#pragma once

#include <list>
#include <map>

#include "blk/io_context.h"
#include "common/buffer.h"

namespace TOPNSPC {

class TransContext;

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

    explicit DeferredBatch(void *priv) : ioc(priv) { ioc.type = 1; }

    void prepare_write(uint64_t seq, uint64_t offset, uint64_t length,
                       const bufferlist &data, uint64_t data_pos);
    void _discard(uint64_t offset, uint64_t length);
};

}  // namespace TOPNSPC
