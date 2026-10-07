#include <cerrno>
#include <limits>

#include "blk/allocator.h"
#include "blk/avl_allocator.h"
#include "blk/bitmap_allocator.h"
#include "blk/hybrid_allocator.h"
#include "common/cassert.h"
#include "common/intarith.h"

namespace TOPNSPC {

Allocator::Allocator(std::string_view name, int64_t capacity, int64_t block_size)
    : device_size(capacity), block_size_(block_size), name_(name) {}

Allocator::~Allocator() = default;

int64_t Allocator::allocate(uint64_t want, uint64_t block_size,
                            int64_t hint, PExtentVector *extents) {
    return allocate(want, block_size, want, hint, extents);
}

void Allocator::release(const PExtentVector &release_vec) {
    interval_set<uint64_t> release_set;
    for (auto &e : release_vec)
        release_set.insert(e.offset, e.length);
    release(release_set);
}

Allocator *Allocator::create(const std::string &type, int64_t size,
                             int64_t block_size, std::string_view name) {
    if (type == "avl" || type == "stupid") {
        return new AvlAllocator(size, block_size, name);
    }
    if (type == "bitmap") {
        return new BitmapAllocator(size, block_size, name);
    }
    if (type == "hybrid") {
        return new HybridAllocator(size, block_size,
                                   sizeof(range_seg_t) * 256, name);
    }
    return nullptr;
}

double Allocator::get_fragmentation_score() {
    static constexpr double double_size_worth = 1.1;
    std::vector<double> scales{1};
    double score_sum = 0;
    size_t sum = 0;

    auto get_score = [&](size_t v) -> double {
        size_t sc = sizeof(v) * 8 - clz(v) - 1;
        while (scales.size() <= sc + 1)
            scales.push_back(scales.back() * double_size_worth);

        size_t sc_shifted = size_t(1) << sc;
        double x = double(v - sc_shifted) / sc_shifted;
        double score = (sc_shifted)*scales[sc] * (1 - x) +
            (sc_shifted * 2) * scales[sc + 1] * x;
        return score;
    };

    foreach ([&](uint64_t off, uint64_t len) {
        (void)off;
        cxxlab_assert(len > 0);
        score_sum += get_score(len);
        sum += len;
    })
        ;

    double ideal = get_score(sum);
    double terrible = sum * get_score(1);
    return (ideal - score_sum) / (ideal - terrible);
}

}  // namespace TOPNSPC
