#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string_view>
#include <vector>

#include "blk/allocator.h"
#include "blk/extent_types.h"
#include "common/cassert.h"
#include "common/intarith.h"
#include "common/interval_set.h"

namespace TOPNSPC {

using slot_t = uint64_t;
using slot_vector_t = std::vector<slot_t>;

constexpr int slots_per_slotset = 8;
constexpr int bits_per_slot = 64;
constexpr int bits_per_slotset = bits_per_slot * slots_per_slotset;
constexpr slot_t all_slot_set = ~slot_t(0);
constexpr slot_t all_slot_clear = slot_t(0);

inline int64_t find_next_set_bit(slot_t val, int64_t start) {
    if (start >= bits_per_slot) return -1;
    slot_t m = val >> start;
    return m ? int64_t(__builtin_ctzll(m)) + start : -1;
}

inline int64_t count_1s(slot_t val) { return __builtin_popcountll(val); }

inline uint64_t round_up(uint64_t v, uint64_t a) { return p2roundup(v, a); }
inline uint64_t align_down(uint64_t v, uint64_t a) { return p2align(v, a); }

// =====================================================================
// AllocatorLevel01Loose — L0 + L1 (2-bit L1 encoding)
// =====================================================================

class AllocatorLevel01Loose {
public:
    static constexpr int L1_ENTRY_WIDTH = 2;
    static constexpr int L1_ENTRIES_PER_SLOT = bits_per_slot / L1_ENTRY_WIDTH;
    static constexpr slot_t L1_ENTRY_MASK = (slot_t(1) << L1_ENTRY_WIDTH) - 1;

    static constexpr slot_t L1_ENTRY_FULL = 0x0;
    static constexpr slot_t L1_ENTRY_PARTIAL = 0x1;
    static constexpr slot_t L1_ENTRY_FREE = 0x3;

    uint64_t _children_per_slot() const { return L1_ENTRIES_PER_SLOT; }
    uint64_t _level_granularity() const { return l1_granularity; }

    slot_vector_t l0;
    slot_vector_t l1;
    uint64_t l0_granularity = 0;
    uint64_t l1_granularity = 0;
    uint64_t partial_l1_count = 0;
    uint64_t unalloc_l1_count = 0;

    void _init(uint64_t capacity, uint64_t alloc_unit, bool mark_as_free);

    void _mark_alloc_l0(uint64_t l0_start, uint64_t l0_end);
    void _mark_free_l0(uint64_t l0_start, uint64_t l0_end);

    void _mark_l1_on_l0(uint64_t l0_start, uint64_t l0_end);

    void _mark_alloc_l1_l0(uint64_t l0_start, uint64_t l0_end);
    void _mark_free_l1_l0(uint64_t l0_start, uint64_t l0_end);
    uint64_t _free_l1(uint64_t offset, uint64_t length);

    bool _is_slot_fully_allocated(uint64_t idx);
    bool _is_empty_l0(uint64_t l0_start, uint64_t l0_end);
    bool _is_empty_l1(uint64_t l1_start, uint64_t l1_end);

    int _allocate_l0(uint64_t length, uint64_t max_length,
                     uint64_t l0_start, uint64_t l0_end,
                     uint64_t *allocated, PExtentVector *res);

    int _allocate_l1(uint64_t length, uint64_t min_length, uint64_t max_length,
                     uint64_t l1_start, uint64_t l1_end,
                     uint64_t *allocated, PExtentVector *res);

    static void _fragment_and_emplace(uint64_t max_length,
                                      uint64_t offset, uint64_t len,
                                      uint64_t *allocated,
                                      PExtentVector *res);

    void collect_stats(std::map<size_t, size_t> &bins_overall);

    uint64_t claim_free_to_left_l1(uint64_t offs);
    uint64_t claim_free_to_right_l1(uint64_t offs);

private:
    uint64_t _claim_free_to_left_l0(int64_t l0_pos_start);
    uint64_t _claim_free_to_right_l0(int64_t l0_pos_start);
};

// =====================================================================
// AllocatorLevel02 — L2 + orchestration
// =====================================================================

class AllocatorLevel02 {
public:
    AllocatorLevel01Loose l1;
    uint64_t l2_granularity = 0;
    uint64_t available = 0;
    int64_t last_pos = 0;
    std::mutex lock;
    slot_vector_t l2;

    uint64_t get_available() const { return available; }

    void _init(uint64_t capacity, uint64_t alloc_unit, bool mark_as_free);
    int _allocate_l2(uint64_t want, uint64_t min_length, uint64_t max_length,
                     int64_t hint, uint64_t *allocated, PExtentVector *res);
    void _free_l2(const interval_set<uint64_t> &rr);
    void _mark_free(uint64_t offset, uint64_t length);
    void _mark_allocated(uint64_t offset, uint64_t length);
    void _mark_l2_free(uint64_t l2_pos, uint64_t l2_pos_end);
    void _mark_l2_allocated(uint64_t l2_pos, uint64_t l2_pos_end);
    void _mark_l2_on_l1(uint64_t l2_pos, uint64_t l2_pos_end);
    void foreach_internal(std::function<void(uint64_t, uint64_t)> notify);
    void collect_stats(std::map<size_t, size_t> &bins_overall);
    uint64_t claim_free_to_left(uint64_t offset);
    uint64_t claim_free_to_right(uint64_t offset);
    void _shutdown();
};

// =====================================================================
// BitmapAllocator — concrete wrapper
// =====================================================================

class BitmapAllocator : public Allocator,
                        public AllocatorLevel02 {
public:
    BitmapAllocator(int64_t capacity, int64_t alloc_unit,
                    std::string_view name);
    ~BitmapAllocator() override = default;

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
    void foreach (
        std::function<void(uint64_t offset, uint64_t length)> notify) override;

    void init_add_free(uint64_t offset, uint64_t length) override;
    void init_rm_free(uint64_t offset, uint64_t length) override;

    void shutdown() override;
};

}  // namespace TOPNSPC
