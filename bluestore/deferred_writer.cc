#include "bluestore/deferred_writer.h"

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

}  // namespace TOPNSPC
