#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

#include "blk/allocator.h"
#include "blk/extent_types.h"
#include "common/interval_set.h"

namespace TOPNSPC {

class AllocatorLevel02;

class BitmapAllocator : public Allocator {
public:
    BitmapAllocator(int64_t capacity, int64_t alloc_unit,
                    std::string_view name);
    ~BitmapAllocator() override;

    const char *get_type() const override { return "bitmap"; }

    using Allocator::allocate;
    using Allocator::release;

    int64_t allocate(uint64_t want, uint64_t unit,
                     uint64_t max_alloc_size, int64_t hint,
                     PExtentVector *extents) override;

    void release(const interval_set<uint64_t> &release_set) override;

    uint64_t get_free() override;
    double get_fragmentation() override;

    void dump() override;
    void foreach(
        std::function<void(uint64_t offset, uint64_t length)> notify) override;

    void init_add_free(uint64_t offset, uint64_t length) override;
    void init_rm_free(uint64_t offset, uint64_t length) override;

    void shutdown() override;

    uint64_t claim_free_to_left(uint64_t offset);
    uint64_t claim_free_to_right(uint64_t offset);

private:
    std::unique_ptr<AllocatorLevel02> impl_;
};

}  // namespace TOPNSPC
