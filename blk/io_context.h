#ifndef BLK_IO_CONTEXT_H
#define BLK_IO_CONTEXT_H

#include <atomic>
#include <condition_variable>
#include <list>
#include <mutex>

#include "blk/aio.h"
#include "common/common_fwd.h"

namespace TOPNSPC {

/// track in-flight io
struct IOContext {
    void *priv = nullptr;
    uint8_t type = 0;
    int r = 0;

    std::list<aio_t> pending_aios;  ///< not yet submitted
    std::list<aio_t> running_aios;  ///< submitting or submitted
    std::atomic_int num_pending{0};
    std::atomic_int num_running{0};

    std::mutex lock;
    std::condition_variable cond;
    uint32_t flags = 0;

    explicit IOContext(void *p) : priv(p) {}

    IOContext(const IOContext &) = delete;
    IOContext &operator=(const IOContext &) = delete;

    bool has_pending_aios() const { return num_pending.load() > 0; }
    uint64_t get_num_ios() const {
        return num_pending.load() + num_running.load();
    }

    void aio_wait();
    void try_aio_wake();
    void release_running_aios();

    void set_return_value(int _r) { r = _r; }
    int get_return_value() const { return r; }
};

}  // namespace TOPNSPC

#endif  // BLK_IO_CONTEXT_H
