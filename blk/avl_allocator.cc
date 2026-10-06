#include <algorithm>
#include <cerrno>
#include <iostream>
#include <limits>

#include "blk/avl_allocator.h"
#include "common/cassert.h"
#include "common/intarith.h"

namespace TOPNSPC {

namespace {
struct range_t {
    uint64_t start;
    uint64_t end;
};
}  // namespace

AvlAllocator::AvlAllocator(int64_t device_size, int64_t block_size,
                           std::string_view name)
    : AvlAllocator(device_size, block_size, 0, name) {}

AvlAllocator::AvlAllocator(int64_t device_size, int64_t block_size,
                           uint64_t max_mem, std::string_view name)
    : Allocator(name, device_size, block_size),
      range_size_alloc_threshold_(128 * 1024),
      range_size_alloc_free_pct_(4),
      max_search_count_(100),
      max_search_bytes_(16 * 1024 * 1024),
      range_count_cap_(max_mem / sizeof(range_seg_t)) {}

AvlAllocator::~AvlAllocator() {
    shutdown();
}

uint64_t AvlAllocator::_pick_block_after(uint64_t *cursor,
                                         uint64_t size,
                                         uint64_t align) {
    const auto compare = range_tree_.key_comp();
    uint32_t search_count = 0;
    uint64_t search_bytes = 0;

    auto rs_start = range_tree_.lower_bound(range_t{*cursor, size}, compare);
    for (auto rs = rs_start; rs != range_tree_.end(); ++rs) {
        uint64_t offset = p2roundup(rs->start, align);
        if (offset + size <= rs->end) {
            *cursor = offset + size;
            return offset;
        }
        if (max_search_count_ > 0 && ++search_count > max_search_count_)
            return -1ULL;
        if (search_bytes = rs->start - rs_start->start;
            max_search_bytes_ > 0 && search_bytes > max_search_bytes_)
            return -1ULL;
    }

    if (*cursor == 0)
        return -1ULL;

    for (auto rs = range_tree_.begin(); rs != rs_start; ++rs) {
        uint64_t offset = p2roundup(rs->start, align);
        if (offset + size <= rs->end) {
            *cursor = offset + size;
            return offset;
        }
        if (max_search_count_ > 0 && ++search_count > max_search_count_)
            return -1ULL;
        if (max_search_bytes_ > 0 &&
            search_bytes + (rs_start->start - rs->start) > max_search_bytes_)
            return -1ULL;
    }
    return -1ULL;
}

uint64_t AvlAllocator::_pick_block_fits(uint64_t size, uint64_t align) {
    const auto compare = range_size_tree_.key_comp();
    auto rs_start = range_size_tree_.lower_bound(range_t{0, size}, compare);
    uint32_t search_count = 0;
    for (auto rs = rs_start; rs != range_size_tree_.end(); ++rs) {
        if (max_search_count_ > 0 && ++search_count > max_search_count_)
            return -1ULL;
        uint64_t offset = p2roundup(rs->start, align);
        if (offset + size <= rs->end)
            return offset;
    }
    return -1ULL;
}

void AvlAllocator::_add_to_tree(uint64_t start, uint64_t size) {
    cxxlab_assert(size != 0);
    uint64_t end = start + size;

    auto rs_after = range_tree_.upper_bound(
        range_t{start, end}, range_tree_.key_comp());

    auto rs_before = range_tree_.end();
    if (rs_after != range_tree_.begin())
        rs_before = std::prev(rs_after);

    bool merge_before = (rs_before != range_tree_.end() &&
                         rs_before->end == start);
    bool merge_after = (rs_after != range_tree_.end() &&
                        rs_after->start == end);

    if (merge_before && merge_after) {
        _range_size_tree_rm(*rs_before);
        _range_size_tree_rm(*rs_after);
        rs_after->start = rs_before->start;
        range_tree_.erase_and_dispose(rs_before, dispose_rs{});
        _range_size_tree_try_insert(*rs_after);
    } else if (merge_before) {
        _range_size_tree_rm(*rs_before);
        rs_before->end = end;
        _range_size_tree_try_insert(*rs_before);
    } else if (merge_after) {
        _range_size_tree_rm(*rs_after);
        rs_after->start = start;
        _range_size_tree_try_insert(*rs_after);
    } else {
        _try_insert_range(start, end, &rs_after);
    }
}

void AvlAllocator::_spillover_range(uint64_t start, uint64_t end) {
    (void)start;
    (void)end;
    // No-op in the base class: overflows are silently dropped.
    // Subclasses (HybridAllocator) redirect to a fallback allocator.
}

void AvlAllocator::_process_range_removal(uint64_t start, uint64_t end,
                                          range_tree_t::iterator &rs) {
    bool left_over = (rs->start != start);
    bool right_over = (rs->end != end);

    _range_size_tree_rm(*rs);

    if (left_over && right_over) {
        auto old_right_end = rs->end;
        auto insert_pos = rs;
        cxxlab_assert(insert_pos != range_tree_.end());
        ++insert_pos;
        rs->end = start;
        _try_insert_range(end, old_right_end, &insert_pos);
        _range_size_tree_try_insert(*rs);
    } else if (left_over) {
        rs->end = start;
        _range_size_tree_try_insert(*rs);
    } else if (right_over) {
        rs->start = end;
        _range_size_tree_try_insert(*rs);
    } else {
        range_tree_.erase_and_dispose(rs, dispose_rs{});
    }
}

bool AvlAllocator::_remove_from_tree(uint64_t start, uint64_t size) {
    uint64_t end = start + size;
    cxxlab_assert(size != 0);

    auto rs = range_tree_.find(range_t{start, end}, range_tree_.key_comp());
    if (rs == range_tree_.end() || rs->start > start || rs->end < end)
        return false;
    cxxlab_assert(size <= num_free_);

    _process_range_removal(start, end, rs);
    return true;
}

void AvlAllocator::_range_size_tree_rm(range_seg_t &r) {
    cxxlab_assert(num_free_ >= r.length());
    num_free_ -= r.length();
    range_size_tree_.erase(r);
}

void AvlAllocator::_range_size_tree_try_insert(range_seg_t &r) {
    if (_try_insert_range(r.start, r.end)) {
        range_size_tree_.insert(r);
        num_free_ += r.length();
    } else {
        range_tree_.erase_and_dispose(r, dispose_rs{});
    }
}

bool AvlAllocator::_try_insert_range(uint64_t start, uint64_t end,
                                     range_tree_t::iterator *insert_pos) {
    bool res = !range_count_cap_ || range_size_tree_.size() < range_count_cap_;
    bool remove_lowest = false;
    if (!res) {
        if (end - start > _lowest_size_available()) {
            remove_lowest = true;
            res = true;
        }
    }
    if (!res) {
        _spillover_range(start, end);
    } else {
        if (insert_pos) {
            auto new_rs = new range_seg_t{start, end};
            range_tree_.insert_before(*insert_pos, *new_rs);
            range_size_tree_.insert(*new_rs);
            num_free_ += new_rs->length();
        }
        if (remove_lowest) {
            auto r = range_size_tree_.begin();
            _range_size_tree_rm(*r);
            _spillover_range(r->start, r->end);
            range_tree_.erase_and_dispose(*r, dispose_rs{});
        }
    }
    return res;
}

void AvlAllocator::_try_remove_from_tree(
    uint64_t start, uint64_t size,
    std::function<void(uint64_t, uint64_t, bool)> cb) {
    uint64_t end = start + size;
    cxxlab_assert(size != 0);

    auto rs = range_tree_.find(range_t{start, end}, range_tree_.key_comp());
    if (rs == range_tree_.end() || rs->start >= end) {
        cb(start, size, false);
        return;
    }

    do {
        auto next_rs = rs;
        ++next_rs;

        if (start < rs->start) {
            cb(start, rs->start - start, false);
            start = rs->start;
        }
        auto range_end = std::min(rs->end, end);
        _process_range_removal(start, range_end, rs);
        cb(start, range_end - start, true);
        start = range_end;

        rs = next_rs;
    } while (rs != range_tree_.end() && rs->start < end && start < end);
    if (start < end) {
        cb(start, end - start, false);
    }
}

int AvlAllocator::_allocate_single(uint64_t size, uint64_t unit,
                                   uint64_t *offset, uint64_t *length) {
    uint64_t max_size = 0;
    if (auto p = range_size_tree_.rbegin(); p != range_size_tree_.rend())
        max_size = p->end - p->start;

    bool force_range_size_alloc = false;
    if (max_size < size) {
        if (max_size < unit)
            return -ENOSPC;
        size = p2align(max_size, unit);
        cxxlab_assert(size > 0);
        force_range_size_alloc = true;
    }

    int free_pct = static_cast<int>(num_free_ * 100 / device_size);
    uint64_t start = 0;

    if (force_range_size_alloc ||
        max_size < range_size_alloc_threshold_ ||
        free_pct < range_size_alloc_free_pct_) {
        start = -1ULL;
    } else {
        uint64_t align = size & -size;
        cxxlab_assert(align != 0);
        uint64_t *cursor = &lbas_[cbits(align) - 1];
        start = _pick_block_after(cursor, size, unit);
    }

    if (start == -1ULL) {
        do {
            start = _pick_block_fits(size, unit);
            if (start != uint64_t(-1ULL))
                break;
            size = p2align(size >> 1, unit);
        } while (size >= unit);
    }

    if (start == -1ULL)
        return -ENOSPC;

    _remove_from_tree(start, size);
    *offset = start;
    *length = size;
    return 0;
}

int64_t AvlAllocator::_allocate(uint64_t want, uint64_t unit,
                                uint64_t max_alloc_size, int64_t hint,
                                PExtentVector *extents) {
    (void)hint;
    uint64_t allocated = 0;
    while (allocated < want) {
        uint64_t offset, length;
        int r = _allocate_single(
            std::min(max_alloc_size, want - allocated),
            unit, &offset, &length);
        if (r < 0)
            break;
        extents->emplace_back(offset, length);
        allocated += length;
    }
    return allocated ? static_cast<int64_t>(allocated) : -ENOSPC;
}

int64_t AvlAllocator::allocate(uint64_t want, uint64_t unit,
                               uint64_t max_alloc_size, int64_t hint,
                               PExtentVector *extents) {
    cxxlab_assert(isp2(unit));
    cxxlab_assert(want % unit == 0);

    if (max_alloc_size == 0)
        max_alloc_size = want;

    constexpr auto cap = std::numeric_limits<decltype(pextent_t::length)>::max();
    if (max_alloc_size >= cap)
        max_alloc_size = p2align(uint64_t(cap), uint64_t(block_size_));

    std::lock_guard l(lock_);
    return _allocate(want, unit, max_alloc_size, hint, extents);
}

void AvlAllocator::_release(const interval_set<uint64_t> &release_set) {
    for (auto p = release_set.begin(); p != release_set.end(); ++p) {
        auto offset = p.get_start();
        auto length = p.get_len();
        cxxlab_assert(offset + length <= uint64_t(device_size));
        _add_to_tree(offset, length);
    }
}

void AvlAllocator::release(const interval_set<uint64_t> &release_set) {
    std::lock_guard l(lock_);
    _release(release_set);
}

uint64_t AvlAllocator::get_free() {
    std::lock_guard l(lock_);
    return num_free_;
}

double AvlAllocator::_get_fragmentation() const {
    auto free_blocks = p2align(num_free_, uint64_t(block_size_)) / block_size_;
    if (free_blocks <= 1)
        return 0.0;
    return static_cast<double>(range_tree_.size() - 1) / (free_blocks - 1);
}

double AvlAllocator::get_fragmentation() {
    std::lock_guard l(lock_);
    return _get_fragmentation();
}

void AvlAllocator::_dump() const {
    std::cerr << "AvlAllocator: " << range_tree_.size() << " ranges, "
              << num_free_ << " bytes free\n";
    for (const auto &rs : range_tree_) {
        std::cerr << "  [0x" << std::hex << rs.start << ", 0x" << rs.end
                  << ") len=0x" << (rs.end - rs.start) << std::dec << "\n";
    }
}

void AvlAllocator::dump() {
    std::lock_guard l(lock_);
    _dump();
}

void AvlAllocator::_foreach(
    std::function<void(uint64_t offset, uint64_t length)> notify) const {
    for (auto &rs : range_tree_)
        notify(rs.start, rs.end - rs.start);
}

void AvlAllocator::foreach (
    std::function<void(uint64_t offset, uint64_t length)> notify) {
    std::lock_guard l(lock_);
    _foreach(notify);
}

void AvlAllocator::init_add_free(uint64_t offset, uint64_t length) {
    if (!length) return;
    std::lock_guard l(lock_);
    cxxlab_assert(offset + length <= uint64_t(device_size));
    _add_to_tree(offset, length);
}

void AvlAllocator::init_rm_free(uint64_t offset, uint64_t length) {
    if (!length) return;
    std::lock_guard l(lock_);
    cxxlab_assert(offset + length <= uint64_t(device_size));
    _remove_from_tree(offset, length);
}

void AvlAllocator::_shutdown() {
    range_size_tree_.clear();
    range_tree_.clear_and_dispose(dispose_rs{});
    num_free_ = 0;
}

void AvlAllocator::shutdown() {
    std::lock_guard l(lock_);
    _shutdown();
}

}  // namespace TOPNSPC
