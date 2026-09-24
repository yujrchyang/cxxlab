#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "blk/extent_types.h"
#include "common/interval_set.h"

namespace TOPNSPC {

class Allocator {
public:
    Allocator(std::string_view name, int64_t capacity, int64_t block_size);
    virtual ~Allocator();

    virtual const char *get_type() const = 0;

    virtual int64_t allocate(uint64_t want, uint64_t block_size,
                             uint64_t max_alloc_size, int64_t hint,
                             PExtentVector *extents) = 0;

    int64_t allocate(uint64_t want, uint64_t block_size,
                     int64_t hint, PExtentVector *extents);

    virtual void release(const interval_set<uint64_t> &release_set) = 0;
    void release(const PExtentVector &release_set);

    virtual void dump() = 0;
    virtual void foreach (
        std::function<void(uint64_t offset, uint64_t length)> notify) = 0;

    virtual void init_add_free(uint64_t offset, uint64_t length) = 0;
    virtual void init_rm_free(uint64_t offset, uint64_t length) = 0;

    virtual uint64_t get_free() = 0;
    virtual double get_fragmentation() { return 0.0; }
    virtual double get_fragmentation_score();

    virtual void shutdown() = 0;

    static Allocator *create(
        const std::string &type,
        int64_t size,
        int64_t block_size,
        std::string_view name = "");

    const std::string &get_name() const { return name_; }
    int64_t get_capacity() const { return device_size; }
    int64_t get_block_size() const { return block_size_; }

protected:
    const int64_t device_size;
    const int64_t block_size_;

private:
    std::string name_;
};

}  // namespace TOPNSPC
