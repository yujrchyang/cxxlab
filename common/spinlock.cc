#include "spinlock.h"

namespace TOPNSPC {

void spinlock::lock() {
    while (lock_.test_and_set(std::memory_order_acquire)) {
        lock_.wait(true, std::memory_order_relaxed);
    }
}

bool spinlock::try_lock() {
    return !lock_.test_and_set(std::memory_order_acquire);
}

void spinlock::unlock() noexcept {
    lock_.clear(std::memory_order_release);
    lock_.notify_all();
}

}  // namespace TOPNSPC
