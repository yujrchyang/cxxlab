#pragma once

#include <deque>
#include <list>
#include <map>
#include <mutex>

#include "blk/io_context.h"
#include "common/buffer.h"

namespace TOPNSPC {

class TransContext;
class OpSequencer;
class BlockDevice;

class DeferredBatch {
public:
    struct deferred_io {
        bufferlist bl;
        uint64_t seq = 0;
    };

    std::map<uint64_t, deferred_io> iomap;
    std::list<TransContext *> txcs;
    IOContext ioc;
    std::map<uint64_t, int> seq_bytes;
    OpSequencer *osr = nullptr;

    DeferredBatch(OpSequencer *o = nullptr) : ioc(this), osr(o) {
        ioc.type = 1;
    }

    void prepare_write(uint64_t seq, uint64_t offset, uint64_t length,
                       const bufferlist &data, uint64_t data_pos);
    void _discard(uint64_t offset, uint64_t length);
};

class DeferredWriter {
public:
    explicit DeferredWriter(BlockDevice *bdev);
    void queue(TransContext *txc);
    void try_submit();
    void flush_done(DeferredBatch *b);

    std::mutex lock_;
    std::deque<OpSequencer *> deferred_queue_;
    std::deque<DeferredBatch *> deferred_done_queue_;

private:
    BlockDevice *bdev_;
};

}  // namespace TOPNSPC
