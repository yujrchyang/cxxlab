#pragma once

#include <memory>

#include "blk/avl_allocator.h"
#include "blk/bitmap_allocator.h"
#include "common/interval_set.h"

namespace TOPNSPC {

class HybridAllocator : public AvlAllocator {
public:
    HybridAllocator(int64_t device_size, int64_t block_size,
                    uint64_t max_mem, std::string_view name);

    ~HybridAllocator() override = default;

    const char *get_type() const override { return "hybrid"; }

    using AvlAllocator::allocate;
    using AvlAllocator::release;

    int64_t allocate(uint64_t want, uint64_t unit,
                     uint64_t max_alloc_size, int64_t hint,
                     PExtentVector *extents) override;

    void release(const interval_set<uint64_t> &release_set) override;

    uint64_t get_free() override;
    double get_fragmentation() override;

    void dump() override;
    void foreach (
        std::function<void(uint64_t offset, uint64_t length)> notify) override;

    void init_add_free(uint64_t offset, uint64_t length) override;
    void init_rm_free(uint64_t offset, uint64_t length) override;

    void shutdown() override;

protected:
    void _add_to_tree(uint64_t start, uint64_t size) override;
    void _spillover_range(uint64_t start, uint64_t end) override;

private:
    std::unique_ptr<BitmapAllocator> child_;
};

}  // namespace TOPNSPC
