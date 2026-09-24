#include "blk/bitmap_allocator.h"

#include <algorithm>
#include <cerrno>
#include <limits>

#include "common/intarith.h"

namespace TOPNSPC {

// =====================================================================
// AllocatorLevel01Loose — method bodies
// =====================================================================

void AllocatorLevel01Loose::_init(uint64_t capacity,
                                  uint64_t alloc_unit,
                                  bool mark_as_free) {
    l0_granularity = alloc_unit;
    l1_granularity = alloc_unit * bits_per_slotset;

    uint64_t aligned = round_up(
        capacity, l1_granularity * slots_per_slotset * L1_ENTRIES_PER_SLOT);

    uint64_t l1_cnt = aligned / l1_granularity / L1_ENTRIES_PER_SLOT;
    l1.assign(l1_cnt, mark_as_free ? all_slot_set : all_slot_clear);

    uint64_t l0_cnt = aligned / alloc_unit / bits_per_slot;
    l0.assign(l0_cnt, mark_as_free ? all_slot_set : all_slot_clear);

    if (!mark_as_free) {
        uint64_t l0_no_use = round_up(capacity, l0_granularity) / l0_granularity;
        _mark_alloc_l1_l0(l0_no_use, aligned / l0_granularity);
    }
    partial_l1_count = 0;
    unalloc_l1_count = mark_as_free
        ? l1.size() * L1_ENTRIES_PER_SLOT * slots_per_slotset
        : 0;
}

void AllocatorLevel01Loose::_mark_alloc_l0(
    uint64_t l0_start, uint64_t l0_end) {
    uint64_t p = l0_start;
    while (p < l0_end && (p % bits_per_slot)) {
        l0[p / bits_per_slot] &= ~(slot_t(1) << (p % bits_per_slot));
        ++p;
    }
    while (p + bits_per_slot <= l0_end) {
        l0[p / bits_per_slot] = all_slot_clear;
        p += bits_per_slot;
    }
    while (p < l0_end) {
        l0[p / bits_per_slot] &= ~(slot_t(1) << (p % bits_per_slot));
        ++p;
    }
}

void AllocatorLevel01Loose::_mark_free_l0(
    uint64_t l0_start, uint64_t l0_end) {
    uint64_t p = l0_start;
    while (p < l0_end && (p % bits_per_slot)) {
        l0[p / bits_per_slot] |= (slot_t(1) << (p % bits_per_slot));
        ++p;
    }
    while (p + bits_per_slot <= l0_end) {
        l0[p / bits_per_slot] = all_slot_set;
        p += bits_per_slot;
    }
    while (p < l0_end) {
        l0[p / bits_per_slot] |= (slot_t(1) << (p % bits_per_slot));
        ++p;
    }
}

void AllocatorLevel01Loose::_mark_l1_on_l0(
    uint64_t l0_start, uint64_t l0_end) {
    auto ls = align_down(l0_start, bits_per_slotset);
    auto le = round_up(l0_end, bits_per_slotset);
    for (auto e = ls / bits_per_slotset; e < le / bits_per_slotset; ++e) {
        slot_t agg_and = all_slot_set;
        slot_t agg_or = all_slot_clear;
        for (int i = 0; i < slots_per_slotset; ++i) {
            auto v = l0[e * slots_per_slotset + i];
            agg_and &= v;
            agg_or |= v;
        }

        auto slot = e / L1_ENTRIES_PER_SLOT;
        auto off = e % L1_ENTRIES_PER_SLOT;
        slot_t old = (l1[slot] >> (off * L1_ENTRY_WIDTH)) & L1_ENTRY_MASK;

        slot_t nxt;
        if (agg_or == all_slot_clear)
            nxt = L1_ENTRY_FULL;
        else if (agg_and == all_slot_set)
            nxt = L1_ENTRY_FREE;
        else
            nxt = L1_ENTRY_PARTIAL;

        if (old == nxt) continue;
        if (old == L1_ENTRY_PARTIAL)
            --partial_l1_count;
        else if (old == L1_ENTRY_FREE)
            --unalloc_l1_count;
        if (nxt == L1_ENTRY_PARTIAL)
            ++partial_l1_count;
        else if (nxt == L1_ENTRY_FREE)
            ++unalloc_l1_count;

        l1[slot] &= ~(L1_ENTRY_MASK << (off * L1_ENTRY_WIDTH));
        l1[slot] |= (nxt << (off * L1_ENTRY_WIDTH));
    }
}

void AllocatorLevel01Loose::_mark_alloc_l1_l0(
    uint64_t l0_start, uint64_t l0_end) {
    _mark_alloc_l0(l0_start, l0_end);
    _mark_l1_on_l0(align_down(l0_start, bits_per_slotset),
                   round_up(l0_end, bits_per_slotset));
}

void AllocatorLevel01Loose::_mark_free_l1_l0(
    uint64_t l0_start, uint64_t l0_end) {
    _mark_free_l0(l0_start, l0_end);
    _mark_l1_on_l0(align_down(l0_start, bits_per_slotset),
                   round_up(l0_end, bits_per_slotset));
}

uint64_t AllocatorLevel01Loose::_free_l1(
    uint64_t offset, uint64_t length) {
    uint64_t ls = offset / l0_granularity;
    uint64_t le = round_up(offset + length, l0_granularity) / l0_granularity;
    _mark_free_l1_l0(ls, le);
    return length;
}

bool AllocatorLevel01Loose::_is_slot_fully_allocated(uint64_t idx) {
    for (int i = 0; i < slots_per_slotset; ++i)
        if (l0[idx * slots_per_slotset + i] != 0) return false;
    return true;
}

bool AllocatorLevel01Loose::_is_empty_l0(
    uint64_t l0_start, uint64_t l0_end) {
    for (uint64_t p = l0_start; p < l0_end;) {
        if ((p % bits_per_slot) == 0 && p + bits_per_slot <= l0_end) {
            if (l0[p / bits_per_slot]) return false;
            p += bits_per_slot;
        } else {
            if (l0[p / bits_per_slot] & (slot_t(1) << (p % bits_per_slot)))
                return false;
            ++p;
        }
    }
    return true;
}

bool AllocatorLevel01Loose::_is_empty_l1(
    uint64_t l1_start, uint64_t l1_end) {
    for (uint64_t i = l1_start; i < l1_end; ++i)
        if (!_is_slot_fully_allocated(i)) return false;
    return true;
}

int AllocatorLevel01Loose::_allocate_l0(
    uint64_t length, uint64_t max_length,
    uint64_t l0_start, uint64_t l0_end,
    uint64_t *allocated, PExtentVector *res) {
    uint64_t need = length / l0_granularity;
    uint64_t p = l0_start;

    while (p < l0_end && need > 0) {
        auto &slot = l0[p / bits_per_slot];
        auto boff = p % bits_per_slot;

        if (slot == all_slot_clear) {
            p += bits_per_slot - boff;
            continue;
        }

        int64_t bit = find_next_set_bit(slot, boff);
        if (bit < 0) {
            p += bits_per_slot - boff;
            continue;
        }

        uint64_t run_start = p / bits_per_slot * bits_per_slot + bit;
        uint64_t run_end = run_start;

        while (run_end - run_start < need) {
            auto &s2 = l0[run_end / bits_per_slot];
            int64_t nxt = find_next_set_bit(s2, run_end % bits_per_slot);
            if (nxt == int64_t(run_end % bits_per_slot)) {
                ++run_end;
            } else {
                break;
            }
        }

        uint64_t units = std::min(run_end - run_start, need);
        uint64_t bytes = units * l0_granularity;
        _fragment_and_emplace(max_length, run_start * l0_granularity,
                              bytes, allocated, res);
        _mark_alloc_l0(run_start, run_start + units);
        need -= units;
        p = run_start + units;
    }
    return need ? -ENOSPC : 0;
}

int AllocatorLevel01Loose::_allocate_l1(
    uint64_t length, uint64_t min_length, uint64_t max_length,
    uint64_t l1_start, uint64_t l1_end,
    uint64_t *allocated, PExtentVector *res) {
    (void)min_length;
    uint64_t l0_per_entry = bits_per_slotset;

    for (uint64_t p = l1_start; p < l1_end; ++p) {
        uint64_t slot = p / L1_ENTRIES_PER_SLOT;
        uint64_t off = p % L1_ENTRIES_PER_SLOT;
        if (slot >= l1.size()) break;

        slot_t ent = (l1[slot] >> (off * L1_ENTRY_WIDTH)) & L1_ENTRY_MASK;
        if (ent == L1_ENTRY_FULL) continue;

        uint64_t l0s = p * l0_per_entry;
        uint64_t l0e = l0s + l0_per_entry;
        uint64_t rem = length - *allocated;
        if (rem == 0) return 0;

        if (ent == L1_ENTRY_FREE && rem >= l0_per_entry * l0_granularity) {
            uint64_t alloc_len = l0_per_entry * l0_granularity;
            _fragment_and_emplace(max_length, l0s * l0_granularity,
                                  alloc_len, allocated, res);
            _mark_alloc_l1_l0(l0s, l0e);
            continue;
        }

        _allocate_l0(rem, max_length, l0s, l0e, allocated, res);
    }
    return *allocated >= length ? 0 : -ENOSPC;
}

void AllocatorLevel01Loose::_fragment_and_emplace(
    uint64_t max_length, uint64_t offset, uint64_t len,
    uint64_t *allocated, PExtentVector *res) {
    if (!res->empty()) {
        auto &last = res->back();
        if (last.offset + last.length == offset) {
            uint64_t merged = last.length + len;
            if (max_length == 0 || merged <= max_length) {
                last.length = merged;
                if (allocated) *allocated += len;
                return;
            }
            if (last.length < max_length) {
                uint64_t take = max_length - last.length;
                last.length += take;
                offset += take;
                len -= take;
                if (allocated) *allocated += take;
            }
        }
    }
    while (len > 0) {
        uint64_t chunk = (max_length > 0 && len > max_length) ? max_length : len;
        cxxlab_assert(chunk <= std::numeric_limits<uint32_t>::max());
        res->emplace_back(offset, static_cast<uint32_t>(chunk));
        if (allocated) *allocated += chunk;
        offset += chunk;
        len -= chunk;
    }
}

void AllocatorLevel01Loose::collect_stats(
    std::map<size_t, size_t> &bins_overall) {
    size_t free_seq = 0;
    for (auto slot : l0) {
        if (slot == all_slot_set) {
            free_seq += bits_per_slot;
        } else if (slot != all_slot_clear) {
            size_t pos = 0;
            do {
                auto pos1 = find_next_set_bit(slot, pos);
                if (pos1 == int64_t(pos)) {
                    ++free_seq;
                    pos = pos1 + 1;
                } else {
                    if (free_seq) {
                        ++bins_overall[cbits(free_seq) - 1];
                        free_seq = 0;
                    }
                    if (pos1 >= 0) {
                        free_seq = 1;
                    }
                    pos = pos1 + 1;
                }
            } while (pos < bits_per_slot);
        } else if (free_seq) {
            ++bins_overall[cbits(free_seq) - 1];
            free_seq = 0;
        }
    }
    if (free_seq) {
        ++bins_overall[cbits(free_seq) - 1];
    }
}

// =====================================================================
// AllocatorLevel02 — method bodies
// =====================================================================

void AllocatorLevel02::_init(uint64_t capacity, uint64_t alloc_unit,
                             bool mark_as_free) {
    l1._init(capacity, alloc_unit, mark_as_free);

    uint64_t l1g = l1._level_granularity();
    uint64_t l1c = l1._children_per_slot();
    l2_granularity = l1g * l1c * slots_per_slotset;

    constexpr uint64_t l2eps = bits_per_slot;
    uint64_t aligned = round_up(capacity, l2_granularity * l2eps);
    uint64_t l2cnt = aligned / l2_granularity / l2eps;
    l2.assign(l2cnt, mark_as_free ? all_slot_set : all_slot_clear);

    if (mark_as_free) {
        available = align_down(capacity, alloc_unit);
    } else {
        available = 0;
        uint64_t l2nu = round_up(capacity, l2_granularity) / l2_granularity;
        _mark_l2_allocated(l2nu, aligned / l2_granularity);
    }
    last_pos = 0;
}

void AllocatorLevel02::_mark_l2_free(uint64_t l2_pos, uint64_t l2_pos_end) {
    auto d = bits_per_slot;
    while (l2_pos < l2_pos_end) {
        l2[l2_pos / d] |= (slot_t(1) << (l2_pos % d));
        ++l2_pos;
    }
}

void AllocatorLevel02::_mark_l2_allocated(uint64_t l2_pos,
                                          uint64_t l2_pos_end) {
    auto d = bits_per_slot;
    while (l2_pos < l2_pos_end) {
        l2[l2_pos / d] &= ~(slot_t(1) << (l2_pos % d));
        ++l2_pos;
    }
}

void AllocatorLevel02::_mark_l2_on_l1(uint64_t l2_pos, uint64_t l2_pos_end) {
    auto d = bits_per_slot;
    for (uint64_t p = l2_pos; p < l2_pos_end; ++p) {
        bool free = false;
        uint64_t l1b = p * AllocatorLevel01Loose::L1_ENTRIES_PER_SLOT * slots_per_slotset;
        for (uint64_t i = 0; i < uint64_t(AllocatorLevel01Loose::L1_ENTRIES_PER_SLOT * slots_per_slotset); ++i) {
            auto s = (l1b + i) / AllocatorLevel01Loose::L1_ENTRIES_PER_SLOT;
            auto o = (l1b + i) % AllocatorLevel01Loose::L1_ENTRIES_PER_SLOT;
            if ((l1.l1[s] >> (o * AllocatorLevel01Loose::L1_ENTRY_WIDTH)) & AllocatorLevel01Loose::L1_ENTRY_MASK) {
                free = true;
                break;
            }
        }
        if (free)
            l2[p / d] |= (slot_t(1) << (p % d));
        else
            l2[p / d] &= ~(slot_t(1) << (p % d));
    }
}

void AllocatorLevel02::_mark_free(uint64_t offset, uint64_t length) {
    l1._free_l1(offset, length);
    uint64_t l2p = offset / l2_granularity;
    uint64_t l2e = round_up(offset + length, l2_granularity) / l2_granularity;
    _mark_l2_free(l2p, l2e);
    available += length;
}

void AllocatorLevel02::_mark_allocated(uint64_t offset, uint64_t length) {
    uint64_t l0s = offset / l1.l0_granularity;
    uint64_t l0e = round_up(offset + length, l1.l0_granularity) / l1.l0_granularity;
    l1._mark_alloc_l1_l0(l0s, l0e);
    uint64_t l2p = offset / l2_granularity;
    uint64_t l2e = round_up(offset + length, l2_granularity) / l2_granularity;
    _mark_l2_allocated(l2p, l2e);
    cxxlab_assert(available >= length);
    available -= length;
}

void AllocatorLevel02::_free_l2(const interval_set<uint64_t> &rr) {
    uint64_t released = 0;
    std::lock_guard l(lock);
    for (auto p = rr.begin(); p != rr.end(); ++p) {
        uint64_t o = p.get_start(), len = p.get_len();
        released += l1._free_l1(o, len);
        _mark_l2_free(o / l2_granularity,
                      round_up(o + len, l2_granularity) / l2_granularity);
    }
    available += released;
}

int AllocatorLevel02::_allocate_l2(
    uint64_t want, uint64_t min_length, uint64_t max_length,
    int64_t hint, uint64_t *allocated, PExtentVector *res) {
    std::lock_guard l(lock);
    if (max_length == 0)
        max_length = std::numeric_limits<uint32_t>::max();
    max_length = align_down(max_length, min_length);
    if (max_length < min_length)
        max_length = min_length;

    auto d = bits_per_slot;
    int64_t pos = 0;
    if (hint > 0) {
        pos = align_down(hint / l2_granularity, d);
        if (uint64_t(pos) >= l2.size() * d) pos = 0;
    }

    for (int round = 0; round < 2; ++round) {
        uint64_t idx = round ? 0 : pos;
        uint64_t end = round ? uint64_t(pos) : l2.size() * d;

        while (idx < end) {
            slot_t val = l2[idx / d];
            if (val == all_slot_clear) {
                idx += d;
                continue;
            }

            uint64_t slot_start = idx / d * d;
            uint64_t scan_end = std::min(slot_start + d, end);

            int64_t bit = (val == all_slot_set) ? 0 : find_next_set_bit(val, idx % d);
            while (bit >= 0 && uint64_t(bit) < scan_end) {
                uint64_t b = slot_start + bit;
                uint64_t l1s = b * slots_per_slotset * AllocatorLevel01Loose::L1_ENTRIES_PER_SLOT;
                uint64_t l1e = l1s + slots_per_slotset * AllocatorLevel01Loose::L1_ENTRIES_PER_SLOT;

                l1._allocate_l1(want, min_length, max_length, l1s, l1e, allocated, res);

                if (l1._is_empty_l1(l1s, l1e))
                    l2[idx / d] &= ~(slot_t(1) << bit);

                if (*allocated >= want) {
                    last_pos = b;
                    available -= *allocated;
                    return 0;
                }
                bit = (val == all_slot_set)
                    ? (uint64_t(bit) + 1 < scan_end ? bit + 1 : -1)
                    : find_next_set_bit(l2[idx / d], bit + 1);
            }
            idx = scan_end;
        }
    }
    if (*allocated > 0) available -= *allocated;
    return *allocated > 0 ? 0 : -ENOSPC;
}

void AllocatorLevel02::foreach_internal(
    std::function<void(uint64_t, uint64_t)> notify) {
    std::lock_guard l(lock);
    for (uint64_t i = 0; i < l1.l1.size(); ++i) {
        slot_t val = l1.l1[i];
        if (val == all_slot_clear) continue;
        for (int e = 0; e < AllocatorLevel01Loose::L1_ENTRIES_PER_SLOT; ++e) {
            uint64_t eidx = i * AllocatorLevel01Loose::L1_ENTRIES_PER_SLOT + e;
            slot_t ent = (val >> (e * AllocatorLevel01Loose::L1_ENTRY_WIDTH)) & AllocatorLevel01Loose::L1_ENTRY_MASK;
            if (ent == AllocatorLevel01Loose::L1_ENTRY_FULL) continue;
            if (ent == AllocatorLevel01Loose::L1_ENTRY_FREE) {
                notify(eidx * l1.l1_granularity, l1.l1_granularity);
                continue;
            }
            uint64_t l0b = eidx * bits_per_slotset;
            for (int s = 0; s < slots_per_slotset; ++s) {
                slot_t l0v = l1.l0[l0b / bits_per_slot + s];
                int64_t b = 0;
                while ((b = find_next_set_bit(l0v, b)) >= 0) {
                    uint64_t rs = b;
                    while (++b < bits_per_slot && (l0v & (slot_t(1) << b))) {
                    }
                    notify((l0b / bits_per_slot * bits_per_slot + s * bits_per_slot + rs) * l1.l0_granularity,
                           (b - rs) * l1.l0_granularity);
                }
            }
        }
    }
}

void AllocatorLevel02::collect_stats(
    std::map<size_t, size_t> &bins_overall) {
    std::lock_guard l(lock);
    l1.collect_stats(bins_overall);
}

// =====================================================================
// BitmapAllocator
// =====================================================================

BitmapAllocator::BitmapAllocator(int64_t capacity, int64_t alloc_unit,
                                 std::string_view name)
    : Allocator(name, capacity, alloc_unit) {
    _init(capacity, alloc_unit, false);
}

int64_t BitmapAllocator::allocate(uint64_t want, uint64_t unit,
                                  uint64_t max_alloc_size, int64_t hint,
                                  PExtentVector *extents) {
    cxxlab_assert(isp2(unit));
    cxxlab_assert(want % unit == 0);
    cxxlab_assert(unit >= l1.l0_granularity);
    uint64_t allocated = 0;
    int r = _allocate_l2(want, unit, max_alloc_size, hint,
                         &allocated, extents);
    return (r == 0 && allocated > 0) ? static_cast<int64_t>(allocated) : -ENOSPC;
}

void BitmapAllocator::release(const interval_set<uint64_t> &release_set) {
    _free_l2(release_set);
}

uint64_t BitmapAllocator::get_free() {
    std::lock_guard l(lock);
    return available;
}

double BitmapAllocator::get_fragmentation() {
    std::lock_guard l(lock);
    uint64_t total = l1.unalloc_l1_count + l1.partial_l1_count;
    if (total == 0)
        return 0.0;
    return static_cast<double>(l1.partial_l1_count) / total;
}

void BitmapAllocator::dump() {
    std::map<size_t, size_t> bins;
    collect_stats(bins);
    for ([[maybe_unused]] auto &[bin, count] : bins) {
    }
}

void BitmapAllocator::foreach (
    std::function<void(uint64_t offset, uint64_t length)> notify) {
    foreach_internal(notify);
}

void BitmapAllocator::init_add_free(uint64_t offset, uint64_t length) {
    if (!length) return;
    std::lock_guard l(lock);
    uint64_t off = round_up(offset, l1.l0_granularity);
    length = align_down(offset + length - off, l1.l0_granularity);
    if (length == 0) return;
    _mark_free(off, length);
}

void BitmapAllocator::init_rm_free(uint64_t offset, uint64_t length) {
    if (!length) return;
    std::lock_guard l(lock);
    uint64_t off = round_up(offset, l1.l0_granularity);
    length = align_down(offset + length - off, l1.l0_granularity);
    if (length == 0) return;
    _mark_allocated(off, length);
}

void BitmapAllocator::shutdown() {
    std::lock_guard l(lock);
    _shutdown();
}

// =====================================================================
// AllocatorLevel01Loose — claim methods
// =====================================================================

uint64_t AllocatorLevel01Loose::_claim_free_to_left_l0(int64_t l0_pos_start) {
    auto d0 = bits_per_slot;
    int64_t pos = l0_pos_start - 1;
    slot_t bits = slot_t(1) << (pos % d0);
    int64_t idx = pos / d0;
    slot_t *val_s = l0.data() + idx;
    int64_t pos_e = p2align<int64_t>(pos, d0);

    while (pos >= pos_e) {
        if (0 == ((*val_s) & bits))
            return uint64_t(pos + 1);
        (*val_s) &= ~bits;
        bits >>= 1;
        --pos;
    }

    --idx;
    val_s = l0.data() + idx;
    while (idx >= 0 && (*val_s) == all_slot_set) {
        *val_s = all_slot_clear;
        --idx;
        pos -= d0;
        val_s = l0.data() + idx;
    }

    if (idx >= 0 && (*val_s) != all_slot_set && (*val_s) != all_slot_clear) {
        int64_t pe = p2align<int64_t>(pos, d0);
        bits = slot_t(1) << (pos % d0);
        while (pos >= pe) {
            if (0 == ((*val_s) & bits))
                return uint64_t(pos + 1);
            (*val_s) &= ~bits;
            bits >>= 1;
            --pos;
        }
    }
    return uint64_t(pos + 1);
}

uint64_t AllocatorLevel01Loose::_claim_free_to_right_l0(int64_t l0_pos_start) {
    auto d0 = bits_per_slot;
    int64_t pos = l0_pos_start;
    slot_t bits = slot_t(1) << (pos % d0);
    size_t idx = size_t(pos / d0);
    if (idx >= l0.size())
        return uint64_t(pos);
    slot_t *val_s = l0.data() + idx;
    int64_t pos_e = p2roundup<int64_t>(pos + 1, d0);

    while (pos < pos_e) {
        if (0 == ((*val_s) & bits))
            return uint64_t(pos);
        (*val_s) &= ~bits;
        bits <<= 1;
        ++pos;
    }

    ++idx;
    val_s = l0.data() + idx;
    while (idx < l0.size() && (*val_s) == all_slot_set) {
        *val_s = all_slot_clear;
        ++idx;
        pos += d0;
        val_s = l0.data() + idx;
    }

    if (idx < l0.size() && (*val_s) != all_slot_set && (*val_s) != all_slot_clear) {
        int64_t pe = p2roundup<int64_t>(pos + 1, d0);
        bits = slot_t(1) << (pos % d0);
        while (pos < pe) {
            if (0 == ((*val_s) & bits))
                return uint64_t(pos);
            (*val_s) &= ~bits;
            bits <<= 1;
            ++pos;
        }
    }
    return uint64_t(pos);
}

uint64_t AllocatorLevel01Loose::claim_free_to_left_l1(uint64_t offs) {
    uint64_t l0_pos_end = offs / l0_granularity;
    uint64_t l0_pos_start = _claim_free_to_left_l0(int64_t(l0_pos_end));
    if (l0_pos_start < l0_pos_end) {
        _mark_l1_on_l0(
            p2align(l0_pos_start, uint64_t(bits_per_slotset)),
            p2roundup(l0_pos_end, uint64_t(bits_per_slotset)));
        return l0_granularity * (l0_pos_end - l0_pos_start);
    }
    return 0;
}

uint64_t AllocatorLevel01Loose::claim_free_to_right_l1(uint64_t offs) {
    uint64_t l0_pos_start = offs / l0_granularity;
    uint64_t l0_pos_end = _claim_free_to_right_l0(int64_t(l0_pos_start));
    if (l0_pos_start < l0_pos_end) {
        _mark_l1_on_l0(
            p2align(l0_pos_start, uint64_t(bits_per_slotset)),
            p2roundup(l0_pos_end, uint64_t(bits_per_slotset)));
        return l0_granularity * (l0_pos_end - l0_pos_start);
    }
    return 0;
}

// =====================================================================
// AllocatorLevel02 — claim-free forwarding with L2 update
// =====================================================================

uint64_t AllocatorLevel02::claim_free_to_left(uint64_t offset) {
    std::lock_guard l(lock);
    auto allocated = l1.claim_free_to_left_l1(offset);
    cxxlab_assert(available >= allocated);
    available -= allocated;

    uint64_t l2_pos = (offset - allocated) / l2_granularity;
    uint64_t l2_pos_end =
        p2roundup(int64_t(offset), int64_t(l2_granularity)) / l2_granularity;
    _mark_l2_on_l1(l2_pos, l2_pos_end);
    return allocated;
}

uint64_t AllocatorLevel02::claim_free_to_right(uint64_t offset) {
    std::lock_guard l(lock);
    auto allocated = l1.claim_free_to_right_l1(offset);
    cxxlab_assert(available >= allocated);
    available -= allocated;

    uint64_t l2_pos = offset / l2_granularity;
    int64_t end = int64_t(offset + allocated);
    uint64_t l2_pos_end =
        p2roundup(end, int64_t(l2_granularity)) / l2_granularity;
    _mark_l2_on_l1(l2_pos, l2_pos_end);
    return allocated;
}

void AllocatorLevel02::_shutdown() {
    last_pos = 0;
    available = 0;
}

}  // namespace TOPNSPC
