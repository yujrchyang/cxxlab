#ifndef COMMON_SPINLOCK_H
#define COMMON_SPINLOCK_H

#include <atomic>

#include "common_fwd.h"

namespace TOPNSPC {

class spinlock {
public:
    void lock();
    bool try_lock();
    void unlock() noexcept;

private:
    std::atomic_flag lock_;
};

}  // namespace TOPNSPC

#endif  // COMMON_SPINLOCK_H
