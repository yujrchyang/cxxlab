#include "blk/hybrid_allocator.h"

#include <cerrno>

#include "common/cassert.h"
#include "common/intarith.h"

namespace TOPNSPC {

HybridAllocator::HybridAllocator(int64_t device_size, int64_t block_size,
                                 uint64_t max_mem, std::string_view name)
    : AvlAllocator(device_size, block_size, max_mem, name) {
    child_ = std::make_unique<BitmapAllocator>(
        device_size, block_size, std::string(name) + ".child");
}

void HybridAllocator::_add_to_tree(uint64_t start, uint64_t size) {
    if (child_) {
        uint64_t head = child_->claim_free_to_left(start);
        uint64_t tail = child_->claim_free_to_right(start + size);
        start -= head;
        size += head + tail;
    }
    AvlAllocator::_add_to_tree(start, size);
}

void HybridAllocator::_spillover_range(uint64_t start, uint64_t end) {
    child_->init_add_free(start, end - start);
}

int64_t HybridAllocator::allocate(uint64_t want, uint64_t unit,
                                  uint64_t max_alloc_size, int64_t hint,
                                  PExtentVector *extents) {
    cxxlab_assert(isp2(unit));
    cxxlab_assert(want % unit == 0);

    if (max_alloc_size == 0)
        max_alloc_size = want;

    constexpr auto cap = std::numeric_limits<decltype(pextent_t::length)>::max();
    if (max_alloc_size >= cap)
        max_alloc_size = p2align(uint64_t(cap), uint64_t(block_size_));

    uint64_t allocated = 0;
    std::lock_guard l(lock_);
    PExtentVector avl_extents;
    int64_t r = _allocate(want, unit, max_alloc_size, hint, &avl_extents);
    if (r > 0) {
        allocated = static_cast<uint64_t>(r);
        extents->insert(extents->end(), avl_extents.begin(), avl_extents.end());
    }
    if (allocated < want) {
        PExtentVector child_extents;
        r = child_->allocate(want - allocated, unit,
                             max_alloc_size, hint, &child_extents);
        if (r > 0) {
            allocated += static_cast<uint64_t>(r);
            extents->insert(extents->end(), child_extents.begin(), child_extents.end());
        }
    }
    return allocated ? static_cast<int64_t>(allocated) : -ENOSPC;
}

void HybridAllocator::release(const interval_set<uint64_t> &release_set) {
    AvlAllocator::release(release_set);
}

uint64_t HybridAllocator::get_free() {
    std::lock_guard l(lock_);
    return _get_free() + child_->get_free();
}

double HybridAllocator::get_fragmentation() {
    std::lock_guard l(lock_);
    return _get_fragmentation();
}

void HybridAllocator::dump() {
    std::lock_guard l(lock_);
    _dump();
    child_->dump();
}

void HybridAllocator::foreach (
    std::function<void(uint64_t offset, uint64_t length)> notify) {
    std::lock_guard l(lock_);
    _foreach(notify);
    child_->foreach (notify);
}

void HybridAllocator::init_add_free(uint64_t offset, uint64_t length) {
    AvlAllocator::init_add_free(offset, length);
}

void HybridAllocator::init_rm_free(uint64_t offset, uint64_t length) {
    if (!length) return;
    std::lock_guard l(lock_);
    cxxlab_assert(offset + length <= uint64_t(device_size));
    _try_remove_from_tree(offset, length,
                          [this](uint64_t off, uint64_t len, bool found) {
                              if (!found) {
                                  child_->init_rm_free(off, len);
                              }
                          });
}

void HybridAllocator::shutdown() {
    AvlAllocator::shutdown();
    child_->shutdown();
}

}  // namespace TOPNSPC
