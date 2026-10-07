#include "blk/io_context.h"

#include "common/cassert.h"

namespace TOPNSPC {

void IOContext::aio_wait() {
    std::unique_lock l(lock);
    while (num_running.load() > 0)
        cond.wait(l);
}

void IOContext::try_aio_wake() {
    std::lock_guard l(lock);
    cxxlab_assert(num_running.load() >= 1);
    if (num_running.fetch_sub(1) == 1)
        cond.notify_all();
}

void IOContext::release_running_aios() {
    cxxlab_assert(num_running.load() == 0);
    running_aios.clear();
}

}  // namespace TOPNSPC
