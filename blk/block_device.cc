#include "blk/block_device.h"

#include <cerrno>

#include "blk/kernel_device.h"

namespace TOPNSPC {

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------
bool BlockDevice::is_valid_io(uint64_t off, uint64_t len) const {
    if (len == 0)
        return false;
    if (off >= size)
        return false;
    if (off + len < off)
        return false;
    if (off + len > size)
        return false;
    if (off & (block_size - 1))
        return false;
    if (len & (block_size - 1))
        return false;
    return true;
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
std::unique_ptr<BlockDevice> BlockDevice::create(
    const std::string &path, aio_callback_t cb, void *cbpriv) {
    return std::make_unique<KernelDevice>(path, cb, cbpriv);
}

}  // namespace TOPNSPC
