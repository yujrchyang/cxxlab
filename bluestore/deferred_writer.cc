#include "bluestore/deferred_writer.h"

#include <algorithm>

#include "blk/block_device.h"
#include "bluestore/bluestore_types.h"
#include "bluestore/trans_context.h"
#include "common/cassert.h"

namespace TOPNSPC {

void DeferredBatch::prepare_write(uint64_t seq, uint64_t offset,
                                  uint64_t length, const bufferlist &data,
                                  uint64_t data_pos) {
    _discard(offset, length);
    auto i = iomap.insert({offset, deferred_io()});
    cxxlab_assert(i.second);
    i.first->second.seq = seq;
    i.first->second.bl.substr_of(data, data_pos, length);
    seq_bytes[seq] += static_cast<int>(length);
}

void DeferredBatch::_discard(uint64_t offset, uint64_t length) {
    auto p = iomap.lower_bound(offset);
    if (p != iomap.begin()) {
        --p;
        auto end = p->first + p->second.bl.length();
        if (end > offset) {
            bufferlist head;
            head.substr_of(p->second.bl, 0, offset - p->first);
            auto i = seq_bytes.find(p->second.seq);
            cxxlab_assert(i != seq_bytes.end());
            if (end > offset + length) {
                bufferlist tail;
                tail.substr_of(p->second.bl, offset + length - p->first,
                               end - (offset + length));
                auto &n = iomap[offset + length];
                n.bl = std::move(tail);
                n.seq = p->second.seq;
                i->second -= static_cast<int>(length);
            } else {
                i->second -= static_cast<int>(end - offset);
            }
            cxxlab_assert(i->second >= 0);
            p->second.bl = std::move(head);
        }
        ++p;
    }
    while (p != iomap.end()) {
        if (p->first >= offset + length) {
            break;
        }
        auto i = seq_bytes.find(p->second.seq);
        cxxlab_assert(i != seq_bytes.end());
        auto end = p->first + p->second.bl.length();
        if (end > offset + length) {
            size_t drop_front = offset + length - p->first;
            size_t keep_tail = end - (offset + length);
            auto &s = iomap[offset + length];
            s.seq = p->second.seq;
            s.bl.substr_of(p->second.bl, drop_front, keep_tail);
            i->second -= static_cast<int>(drop_front);
        } else {
            i->second -= static_cast<int>(p->second.bl.length());
        }
        cxxlab_assert(i->second >= 0);
        p = iomap.erase(p);
    }
}

DeferredWriter::DeferredWriter(BlockDevice *bdev) : bdev_(bdev) {}

void DeferredWriter::queue(TransContext *txc) {
    auto *osr = txc->osr;
    std::unique_lock<std::mutex> l(osr->deferred_lock);
    if (!osr->deferred_pending) {
        osr->deferred_pending = new DeferredBatch(osr);
    }
    auto *b = osr->deferred_pending;
    b->txcs.push_back(txc);
    auto &wt = *txc->deferred_txn;
    for (auto &op : wt.ops) {
        uint64_t data_pos = 0;
        for (auto &e : op.extents) {
            if (e.offset == bluestore_pextent_t::INVALID_OFFSET) continue;
            uint64_t write_len =
                std::min<uint64_t>(e.length, op.data.length() - data_pos);
            if (write_len == 0) break;
            b->prepare_write(wt.seq, e.offset, write_len, op.data, data_pos);
            data_pos += write_len;
        }
    }
    if (!osr->deferred_running) {
        std::lock_guard<std::mutex> l2(lock_);
        deferred_queue_.push_back(osr);
    }
    l.unlock();
    try_submit();
}

void DeferredWriter::try_submit() {
    std::deque<OpSequencer *> osrs;
    {
        std::lock_guard<std::mutex> l(lock_);
        osrs.swap(deferred_queue_);
    }
    for (auto *osr : osrs) {
        std::unique_lock<std::mutex> l(osr->deferred_lock);
        if (!osr->deferred_pending || osr->deferred_running) {
            continue;
        }
        auto *b = osr->deferred_pending;
        osr->deferred_running = b;
        osr->deferred_pending = nullptr;
        l.unlock();

        uint64_t start = 0, pos = 0;
        bufferlist bl;
        auto i = b->iomap.begin();
        while (true) {
            if (i == b->iomap.end() || i->first != pos) {
                if (bl.length()) {
                    bdev_->aio_write(start, bl, &b->ioc, false);
                }
                if (i == b->iomap.end()) break;
                start = 0;
                pos = i->first;
                bl.clear();
            }
            if (!bl.length()) start = pos;
            pos += i->second.bl.length();
            bl.claim_append(i->second.bl);
            ++i;
        }
        if (b->ioc.has_pending_aios()) {
            bdev_->aio_submit(&b->ioc);
        } else {
            flush_done(b);
        }
    }
}

void DeferredWriter::flush_done(DeferredBatch *b) {
    auto *osr = b->osr;
    if (osr) {
        std::lock_guard<std::mutex> l(osr->deferred_lock);
        cxxlab_assert(osr->deferred_running == b);
        osr->deferred_running = nullptr;
    }
    std::lock_guard<std::mutex> l(lock_);
    deferred_done_queue_.push_back(b);
}

}  // namespace TOPNSPC
