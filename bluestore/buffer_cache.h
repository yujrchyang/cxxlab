#pragma once

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>

#include "common/buffer.h"

namespace TOPNSPC {

class BufferSpace;
class BufferCache;

struct Buffer {
    enum state_t : uint16_t {
        STATE_CLEAN = 0,
        STATE_WRITING = 1,
    };

    enum : uint16_t {
        FLAG_NOCACHE = 1,
    };

    BufferSpace *space = nullptr;
    uint16_t state = STATE_CLEAN;
    uint16_t flags = 0;
    uint64_t seq = 0;
    uint32_t offset = 0;
    uint32_t length = 0;
    bufferlist data;

    std::list<Buffer *>::iterator lru_it;

    Buffer() = default;
    Buffer(BufferSpace *sp, uint16_t st, uint16_t fl, uint64_t s,
           uint32_t off, uint32_t len, bufferlist d)
        : space(sp), state(st), flags(fl), seq(s), offset(off), length(len), data(std::move(d)) {}

    bool is_clean() const { return state == STATE_CLEAN; }
    bool is_writing() const { return state == STATE_WRITING; }
    bool is_nocache() const { return flags & FLAG_NOCACHE; }
};

class BufferSpace {
public:
    BufferSpace() = default;
    ~BufferSpace() = default;

    BufferSpace(const BufferSpace &) = delete;
    BufferSpace &operator=(const BufferSpace &) = delete;

    void write(BufferCache *cache, uint64_t seq, uint32_t offset,
               bufferlist &bl, unsigned flags);
    void finish_write(BufferCache *cache, uint64_t seq);
    void did_read(BufferCache *cache, uint32_t offset, bufferlist &bl);
    bool read(BufferCache *cache, uint32_t offset, uint32_t length,
              bufferlist *res);
    void discard(BufferCache *cache, uint32_t offset, uint32_t length);
    void clear(BufferCache *cache);

    bool is_empty() const { return buffer_map_.empty(); }

private:
    void _discard(BufferCache *cache, uint32_t offset, uint32_t length);

    friend class BufferCache;

    std::map<uint32_t, std::unique_ptr<Buffer>> buffer_map_;
    std::list<Buffer *> writing_;
};

class BufferCache {
public:
    explicit BufferCache(uint64_t max_bytes)
        : max_bytes_(max_bytes) {}

    ~BufferCache() = default;

    BufferCache(const BufferCache &) = delete;
    BufferCache &operator=(const BufferCache &) = delete;

    void add(Buffer *b);
    void rm(Buffer *b);
    void touch(Buffer *b);
    void trim();
    void flush();

    std::mutex &lock() { return lock_; }

    uint64_t get_max_bytes() const { return max_bytes_; }
    uint64_t get_cur_bytes() const { return cur_bytes_; }
    uint64_t get_hit_bytes() const { return hit_bytes_; }
    uint64_t get_miss_bytes() const { return miss_bytes_; }
    size_t get_num_buffers() const { return lru_.size(); }

    void record_hit(uint64_t bytes) { hit_bytes_ += bytes; }
    void record_miss(uint64_t bytes) { miss_bytes_ += bytes; }

private:
    std::mutex lock_;
    uint64_t max_bytes_;
    uint64_t cur_bytes_ = 0;
    std::list<Buffer *> lru_;
    uint64_t hit_bytes_ = 0;
    uint64_t miss_bytes_ = 0;
};

}  // namespace TOPNSPC
