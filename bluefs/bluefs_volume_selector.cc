#include "bluefs/bluefs_volume_selector.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>

namespace TOPNSPC {

// =========================================================================
// BlueFSVolumeSelector — convenience methods
// =========================================================================

void BlueFSVolumeSelector::add_usage(void *hint, const bluefs_fnode_t &fnode) {
    for (auto &e : fnode.extents) {
        add_usage(hint, e);
    }
    add_usage(hint, fnode.size, true);
}

void BlueFSVolumeSelector::sub_usage(void *hint, const bluefs_fnode_t &fnode) {
    for (auto &e : fnode.extents) {
        sub_usage(hint, e);
    }
    sub_usage(hint, fnode.size, true);
}

// =========================================================================
// OriginalVolumeSelector
// =========================================================================

OriginalVolumeSelector::OriginalVolumeSelector(uint64_t wal_total,
                                               uint64_t db_total,
                                               uint64_t slow_total)
    : wal_total_(wal_total), db_total_(db_total), slow_total_(slow_total) {}

void *OriginalVolumeSelector::get_hint_for_log() const {
    return reinterpret_cast<void *>(static_cast<uintptr_t>(BDEV_WAL));
}

void *OriginalVolumeSelector::get_hint_by_dir(
    const std::string &dirname) const {
    uint8_t res = BDEV_DB;
    if (dirname.size() > 5) {
        if (dirname.size() >= 5 &&
            dirname.compare(dirname.size() - 5, 5, ".slow") == 0 &&
            slow_total_ > 0) {
            res = BDEV_SLOW;
        } else if (dirname.size() >= 4 &&
                   dirname.compare(dirname.size() - 4, 4, ".wal") == 0 &&
                   wal_total_ > 0) {
            res = BDEV_WAL;
        }
    }
    return reinterpret_cast<void *>(static_cast<uintptr_t>(res));
}

uint8_t OriginalVolumeSelector::select_prefer_bdev(void *hint) {
    return static_cast<uint8_t>(reinterpret_cast<uintptr_t>(hint));
}

void OriginalVolumeSelector::get_paths(
    const std::string &base,
    std::vector<BlueFSPathEntry> *paths) const {
    paths->emplace_back(base, db_total_);
    paths->emplace_back(base + ".slow",
                        slow_total_ ? slow_total_ : db_total_);
}

uint64_t OriginalVolumeSelector::get_avail_by_bdev(uint8_t bdev) const {
    switch (bdev) {
    case BDEV_WAL:
        return wal_total_;
    case BDEV_DB:
        return db_total_;
    case BDEV_SLOW:
        return slow_total_;
    default:
        return 0;
    }
}

// =========================================================================
// FitToFastVolumeSelector
// =========================================================================

FitToFastVolumeSelector::FitToFastVolumeSelector(uint64_t wal_total,
                                                 uint64_t db_total,
                                                 uint64_t slow_total)
    : OriginalVolumeSelector(wal_total, db_total, slow_total) {}

void FitToFastVolumeSelector::get_paths(
    const std::string &base,
    std::vector<BlueFSPathEntry> *paths) const {
    // single path with dummy size 1 → RocksDB puts everything in base/
    // BlueFS _allocate() fallback handles spillover when DB is full
    paths->emplace_back(base, 1);
}

// =========================================================================
// RocksDBBlueFSVolumeSelector
// =========================================================================

RocksDBBlueFSVolumeSelector::RocksDBBlueFSVolumeSelector(
    uint64_t wal_total, uint64_t db_total, uint64_t slow_total,
    uint64_t level0_size, uint64_t level_base, uint64_t level_multiplier,
    double reserved_factor, uint64_t reserved, bool use_db_extra) {
    l_totals_[RDB_LEVEL_LOG - RDB_LEVEL_FIRST] = 0;
    l_totals_[RDB_LEVEL_WAL - RDB_LEVEL_FIRST] = wal_total;
    l_totals_[RDB_LEVEL_DB - RDB_LEVEL_FIRST] = db_total;
    l_totals_[RDB_LEVEL_SLOW - RDB_LEVEL_FIRST] = slow_total;

    if (!use_db_extra) {
        return;
    }

    if (reserved == 0) {
        level0_size_ = level0_size;
        level_base_ = level_base;
        level_multiplier_ = level_multiplier;
        // simulate RocksDB level growth to find how much DB to reserve
        uint64_t prev_levels = level0_size;
        uint64_t cur_level = level_base;
        uint64_t cur_threshold = prev_levels + cur_level;
        while (true) {
            uint64_t next_level = cur_level * level_multiplier;
            uint64_t next_threshold = prev_levels + cur_level + next_level;
            if (db_total <= next_threshold) {
                cur_threshold =
                    static_cast<uint64_t>(cur_threshold * reserved_factor);
                db_avail4slow_ =
                    cur_threshold < db_total ? db_total - cur_threshold : 0;
                break;
            }
            prev_levels += cur_level;
            cur_level = next_level;
            cur_threshold = next_threshold;
        }
    } else {
        db_avail4slow_ =
            reserved < db_total ? db_total - reserved : 0;
    }
}

void *RocksDBBlueFSVolumeSelector::get_hint_for_log() const {
    return reinterpret_cast<void *>(static_cast<uintptr_t>(RDB_LEVEL_LOG));
}

void *RocksDBBlueFSVolumeSelector::get_hint_by_dir(
    const std::string &dirname) const {
    uint8_t res = RDB_LEVEL_DB;
    if (dirname.size() > 5) {
        if (dirname.size() >= 5 &&
            dirname.compare(dirname.size() - 5, 5, ".slow") == 0) {
            res = RDB_LEVEL_SLOW;
        } else if (dirname.size() >= 4 &&
                   dirname.compare(dirname.size() - 4, 4, ".wal") == 0) {
            res = RDB_LEVEL_WAL;
        }
    }
    return reinterpret_cast<void *>(static_cast<uintptr_t>(res));
}

uint8_t RocksDBBlueFSVolumeSelector::hint_to_level(void *hint) const {
    return static_cast<uint8_t>(reinterpret_cast<uintptr_t>(hint));
}

void RocksDBBlueFSVolumeSelector::update_max(uint8_t bdev, uint8_t level) {
    uint64_t v = usage_[bdev][level].load();
    uint64_t m = max_usage_[bdev][level].load();
    while (v > m) {
        max_usage_[bdev][level].compare_exchange_weak(m, v);
    }
}

void RocksDBBlueFSVolumeSelector::add_usage(void *hint,
                                            const bluefs_extent_t &extent) {
    if (hint == nullptr) return;
    size_t pos = reinterpret_cast<size_t>(hint) - RDB_LEVEL_FIRST;
    usage_[extent.bdev][pos].fetch_add(extent.length);
    update_max(extent.bdev, pos);
    // per-device totals
    usage_[extent.bdev][LEVELS - 1].fetch_add(extent.length);
    update_max(extent.bdev, LEVELS - 1);
}

void RocksDBBlueFSVolumeSelector::sub_usage(void *hint,
                                            const bluefs_extent_t &extent) {
    if (hint == nullptr) return;
    size_t pos = reinterpret_cast<size_t>(hint) - RDB_LEVEL_FIRST;
    auto &cur = usage_[extent.bdev][pos];
    cur -= extent.length;
    // per-device totals
    usage_[extent.bdev][LEVELS - 1] -= extent.length;
}

void RocksDBBlueFSVolumeSelector::add_usage(void *hint, uint64_t fsize,
                                            bool upd_files) {
    if (hint == nullptr) return;
    size_t pos = reinterpret_cast<size_t>(hint) - RDB_LEVEL_FIRST;
    uint64_t v = usage_[MAX_BDEV][pos].fetch_add(fsize) + fsize;
    uint64_t m = max_usage_[MAX_BDEV][pos].load();
    while (v > m) {
        max_usage_[MAX_BDEV][pos].compare_exchange_weak(m, v);
    }
    if (upd_files) {
        ++per_level_files_[pos];
        ++per_level_files_[LEVELS - 1];
    }
}

void RocksDBBlueFSVolumeSelector::sub_usage(void *hint, uint64_t fsize,
                                            bool upd_files) {
    if (hint == nullptr) return;
    size_t pos = reinterpret_cast<size_t>(hint) - RDB_LEVEL_FIRST;
    usage_[MAX_BDEV][pos] -= fsize;
    if (upd_files) {
        --per_level_files_[pos];
        --per_level_files_[LEVELS - 1];
    }
}

uint8_t RocksDBBlueFSVolumeSelector::select_prefer_bdev(void *h) {
    uint8_t level = hint_to_level(h);

    switch (level) {
    case RDB_LEVEL_SLOW: {
        uint8_t res = BDEV_SLOW;
        if (db_avail4slow_ > 0) {
            // calculate max observed db usage for non-SLOW data
            uint64_t max_db_use = 0;
            max_db_use += max_usage_[BDEV_DB][RDB_LEVEL_LOG - RDB_LEVEL_FIRST].load();
            max_db_use += max_usage_[BDEV_DB][RDB_LEVEL_WAL - RDB_LEVEL_FIRST].load();
            max_db_use += max_usage_[BDEV_DB][RDB_LEVEL_DB - RDB_LEVEL_FIRST].load();
            // DB data that spilled to SLOW
            max_db_use += max_usage_[BDEV_SLOW][RDB_LEVEL_DB - RDB_LEVEL_FIRST].load();

            uint64_t db_total = l_totals_[RDB_LEVEL_DB - RDB_LEVEL_FIRST];
            uint64_t avail = std::min(
                db_avail4slow_,
                max_db_use < db_total ? db_total - max_db_use : 0);

            if (avail >
                usage_[BDEV_DB][RDB_LEVEL_SLOW - RDB_LEVEL_FIRST].load()) {
                res = BDEV_DB;
            }
        }
        return res;
    }
    case RDB_LEVEL_LOG:
    case RDB_LEVEL_WAL:
        return BDEV_WAL;
    case RDB_LEVEL_DB:
    default:
        return BDEV_DB;
    }
}

void RocksDBBlueFSVolumeSelector::get_paths(
    const std::string &base,
    std::vector<BlueFSPathEntry> *paths) const {
    auto db_size = l_totals_[RDB_LEVEL_DB - RDB_LEVEL_FIRST];
    auto slow_size = l_totals_[RDB_LEVEL_SLOW - RDB_LEVEL_FIRST];
    if (slow_size == 0) {
        slow_size = db_size;
    }
    paths->emplace_back(base, db_size);
    paths->emplace_back(base + ".slow", slow_size);
}

uint64_t RocksDBBlueFSVolumeSelector::get_avail_by_bdev(
    uint8_t bdev) const {
    uint64_t total = 0;
    switch (bdev) {
    case BDEV_WAL:
        total = l_totals_[RDB_LEVEL_WAL - RDB_LEVEL_FIRST];
        break;
    case BDEV_DB:
        total = l_totals_[RDB_LEVEL_DB - RDB_LEVEL_FIRST];
        break;
    case BDEV_SLOW:
        total = l_totals_[RDB_LEVEL_SLOW - RDB_LEVEL_FIRST];
        break;
    default:
        return 0;
    }
    uint64_t used = usage_[bdev][LEVELS - 1].load();
    return total > used ? total - used : 0;
}

uint64_t RocksDBBlueFSVolumeSelector::get_level_usage(
    uint8_t level, uint8_t bdev) const {
    return usage_[bdev][level - RDB_LEVEL_FIRST].load();
}

}  // namespace TOPNSPC
