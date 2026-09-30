#include "bluestore/buffer_cache.h"

namespace TOPNSPC {

// BufferSpace private helper (assumes cache lock held if cache != nullptr)
void BufferSpace::_discard(BufferCache *cache, uint32_t offset,
                           uint32_t length) {
    uint32_t end = offset + length;
    auto it = buffer_map_.begin();
    while (it != buffer_map_.end()) {
        Buffer *b = it->second.get();
        uint32_t b_end = b->offset + b->length;
        if (b->offset >= end) break;
        if (b_end <= offset) {
            ++it;
            continue;
        }
        if (b->is_writing()) {
            writing_.remove(b);
        }
        if (cache) {
            cache->rm(b);
        }
        it = buffer_map_.erase(it);
    }
}

// BufferSpace public methods

void BufferSpace::write(BufferCache *cache, uint64_t seq, uint32_t offset,
                        bufferlist &bl, unsigned flags) {
    std::unique_lock<std::mutex> lk;
    if (cache) lk = std::unique_lock<std::mutex>(cache->lock());

    _discard(cache, offset, bl.length());

    auto b = std::make_unique<Buffer>(this, Buffer::STATE_WRITING,
                                      static_cast<uint16_t>(flags),
                                      seq, offset, bl.length(), bl);
    auto *bp = b.get();
    buffer_map_[offset] = std::move(b);
    writing_.push_back(bp);

    if (cache) cache->add(bp);
}

void BufferSpace::finish_write(BufferCache *cache, uint64_t seq) {
    std::unique_lock<std::mutex> lk;
    if (cache) lk = std::unique_lock<std::mutex>(cache->lock());

    auto it = writing_.begin();
    while (it != writing_.end()) {
        Buffer *b = *it;
        if (b->seq > seq) {
            ++it;
            continue;
        }
        if (b->seq < seq) {
            ++it;
            continue;
        }

        it = writing_.erase(it);

        if (b->is_nocache()) {
            if (cache) cache->rm(b);
            buffer_map_.erase(b->offset);
        } else {
            b->state = Buffer::STATE_CLEAN;
            if (cache) {
                cache->touch(b);
                cache->trim();
            }
        }
    }
}

void BufferSpace::did_read(BufferCache *cache, uint32_t offset,
                           bufferlist &bl) {
    std::unique_lock<std::mutex> lk;
    if (cache) lk = std::unique_lock<std::mutex>(cache->lock());

    _discard(cache, offset, bl.length());

    auto b = std::make_unique<Buffer>(this, Buffer::STATE_CLEAN, 0, 0,
                                      offset, bl.length(), bl);
    auto *bp = b.get();
    buffer_map_[offset] = std::move(b);

    if (cache) {
        cache->add(bp);
        cache->trim();
    }
}

bool BufferSpace::read(BufferCache *cache, uint32_t offset, uint32_t length,
                       bufferlist *res) {
    std::unique_lock<std::mutex> lk;
    if (cache) lk = std::unique_lock<std::mutex>(cache->lock());

    auto it = buffer_map_.find(offset);
    if (it == buffer_map_.end()) {
        if (cache) cache->record_miss(length);
        return false;
    }

    Buffer *b = it->second.get();
    if (!b->is_clean() && !b->is_writing()) {
        if (cache) cache->record_miss(length);
        return false;
    }

    if (offset + length > b->offset + b->length) {
        if (cache) cache->record_miss(length);
        return false;
    }

    uint32_t local_off = offset - b->offset;
    bufferlist tmp;
    tmp.substr_of(b->data, local_off, length);
    res->claim_append(tmp);

    if (b->is_clean() && cache) {
        cache->touch(b);
        cache->record_hit(length);
    }

    return true;
}

void BufferSpace::discard(BufferCache *cache, uint32_t offset,
                          uint32_t length) {
    std::unique_lock<std::mutex> lk;
    if (cache) lk = std::unique_lock<std::mutex>(cache->lock());
    _discard(cache, offset, length);
}

void BufferSpace::clear(BufferCache *cache) {
    std::unique_lock<std::mutex> lk;
    if (cache) lk = std::unique_lock<std::mutex>(cache->lock());

    for (auto &[off, b] : buffer_map_) {
        if (b->is_writing()) {
            writing_.remove(b.get());
        }
        if (cache) cache->rm(b.get());
    }
    buffer_map_.clear();
    writing_.clear();
}

// BufferCache

void BufferCache::add(Buffer *b) {
    lru_.push_front(b);
    b->lru_it = lru_.begin();
    cur_bytes_ += b->length;
}

void BufferCache::rm(Buffer *b) {
    lru_.erase(b->lru_it);
    cur_bytes_ -= b->length;
}

void BufferCache::touch(Buffer *b) {
    lru_.erase(b->lru_it);
    lru_.push_front(b);
    b->lru_it = lru_.begin();
}

void BufferCache::trim() {
    size_t skipped = 0;
    while (cur_bytes_ > max_bytes_ && !lru_.empty() &&
           skipped < lru_.size()) {
        Buffer *b = lru_.back();
        if (!b->is_clean()) {
            ++skipped;
            lru_.pop_back();
            lru_.push_front(b);
            b->lru_it = lru_.begin();
            continue;
        }
        skipped = 0;
        lru_.pop_back();
        cur_bytes_ -= b->length;
        b->space->buffer_map_.erase(b->offset);
    }
}

void BufferCache::flush() {
    std::lock_guard<std::mutex> lg(lock_);
    while (!lru_.empty()) {
        Buffer *b = lru_.back();
        lru_.pop_back();
        cur_bytes_ -= b->length;
        if (b->is_writing()) {
            b->space->writing_.remove(b);
        }
        b->space->buffer_map_.erase(b->offset);
    }
}

}  // namespace TOPNSPC
