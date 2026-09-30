#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "common/common_fwd.h"

namespace TOPNSPC {

class MergeOperator {
public:
    virtual ~MergeOperator() = default;

    virtual void merge_nonexistent(const char *rdata, size_t rlen,
                                   std::string *new_value) = 0;

    virtual void merge(const char *ldata, size_t llen,
                       const char *rdata, size_t rlen,
                       std::string *new_value) = 0;

    virtual const char *name() const = 0;

    // Merge statistics (cxxlab extension, not in Ceph)
    uint64_t get_merge_count() const {
        return merge_count_.load(std::memory_order_relaxed);
    }
    uint64_t get_merge_bytes() const {
        return merge_bytes_.load(std::memory_order_relaxed);
    }
    void reset_merge_stats() {
        merge_count_.store(0, std::memory_order_relaxed);
        merge_bytes_.store(0, std::memory_order_relaxed);
    }

protected:
    void _record_merge(size_t bytes) {
        merge_count_.fetch_add(1, std::memory_order_relaxed);
        merge_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    }

private:
    std::atomic<uint64_t> merge_count_{0};
    std::atomic<uint64_t> merge_bytes_{0};
};

}  // namespace TOPNSPC
