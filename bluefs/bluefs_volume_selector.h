#pragma once

#include <atomic>
#include <cstdint>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "bluefs/bluefs_types.h"

namespace TOPNSPC {

// =========================================================================
// 设备索引 & 数据级别
// =========================================================================

enum bdev_type_t : uint8_t {
    BDEV_WAL = 0,
    BDEV_DB = 1,
    BDEV_SLOW = 2,
    MAX_BDEV = 3,
};

// RocksDBBlueFSVolumeSelector 使用的逻辑级别
enum rdb_level_t : uint8_t {
    RDB_LEVEL_FIRST = 1,  // 0/nullptr = unset sentinel
    RDB_LEVEL_LOG = RDB_LEVEL_FIRST,
    RDB_LEVEL_WAL,
    RDB_LEVEL_DB,
    RDB_LEVEL_SLOW,
    RDB_LEVEL_MAX,
};

// =========================================================================
// BlueFSPathEntry — {path, size} 对，用于 RocksDB db_paths
// =========================================================================

using BlueFSPathEntry = std::pair<std::string, uint64_t>;

// =========================================================================
// BlueFSVolumeSelector — abstract base
// =========================================================================

class BlueFSVolumeSelector {
public:
    virtual ~BlueFSVolumeSelector() = default;

    virtual void *get_hint_for_log() const = 0;
    virtual void *get_hint_by_dir(const std::string &dirname) const = 0;

    // extent-level tracking
    virtual void add_usage(void *hint, const bluefs_extent_t &extent) = 0;
    virtual void sub_usage(void *hint, const bluefs_extent_t &extent) = 0;

    // size-level tracking (file count + total size per level)
    virtual void add_usage(void *hint, uint64_t fsize, bool upd_files) = 0;
    virtual void sub_usage(void *hint, uint64_t fsize, bool upd_files) = 0;

    // convenience: track an entire fnode
    void add_usage(void *hint, const bluefs_fnode_t &fnode);
    void sub_usage(void *hint, const bluefs_fnode_t &fnode);

    virtual uint8_t select_prefer_bdev(void *hint) = 0;
    virtual void get_paths(const std::string &base,
                           std::vector<BlueFSPathEntry> *paths) const = 0;
    virtual uint64_t get_avail_by_bdev(uint8_t bdev) const = 0;
};

// =========================================================================
// OriginalVolumeSelector — hint = bdev_id, no tracking
// =========================================================================

class OriginalVolumeSelector : public BlueFSVolumeSelector {
public:
    OriginalVolumeSelector(uint64_t wal_total, uint64_t db_total,
                           uint64_t slow_total);

    void *get_hint_for_log() const override;
    void *get_hint_by_dir(const std::string &dirname) const override;

    using BlueFSVolumeSelector::add_usage;
    using BlueFSVolumeSelector::sub_usage;

    void add_usage(void *hint, const bluefs_extent_t &extent) override {}
    void sub_usage(void *hint, const bluefs_extent_t &extent) override {}
    void add_usage(void *hint, uint64_t fsize, bool upd_files) override {}
    void sub_usage(void *hint, uint64_t fsize, bool upd_files) override {}

    uint8_t select_prefer_bdev(void *hint) override;
    void get_paths(const std::string &base,
                   std::vector<BlueFSPathEntry> *paths) const override;
    uint64_t get_avail_by_bdev(uint8_t bdev) const override;

protected:
    uint64_t wal_total_;
    uint64_t db_total_;
    uint64_t slow_total_;
};

// =========================================================================
// FitToFastVolumeSelector — 继承 Original，但只暴露一个路径给 RocksDB
// =========================================================================

class FitToFastVolumeSelector : public OriginalVolumeSelector {
public:
    FitToFastVolumeSelector(uint64_t wal_total, uint64_t db_total,
                            uint64_t slow_total);

    void get_paths(const std::string &base,
                   std::vector<BlueFSPathEntry> *paths) const override;
};

// =========================================================================
// RocksDBBlueFSVolumeSelector — per-level/per-device usage tracking
// =========================================================================

class RocksDBBlueFSVolumeSelector : public BlueFSVolumeSelector {
public:
    RocksDBBlueFSVolumeSelector(uint64_t wal_total, uint64_t db_total,
                                uint64_t slow_total,
                                uint64_t level0_size = 0,
                                uint64_t level_base = 0,
                                uint64_t level_multiplier = 0,
                                double reserved_factor = 0,
                                uint64_t reserved = 0,
                                bool use_db_extra = false);

    void *get_hint_for_log() const override;
    void *get_hint_by_dir(const std::string &dirname) const override;

    using BlueFSVolumeSelector::add_usage;
    using BlueFSVolumeSelector::sub_usage;

    void add_usage(void *hint, const bluefs_extent_t &extent) override;
    void sub_usage(void *hint, const bluefs_extent_t &extent) override;
    void add_usage(void *hint, uint64_t fsize, bool upd_files) override;
    void sub_usage(void *hint, uint64_t fsize, bool upd_files) override;

    uint8_t select_prefer_bdev(void *hint) override;
    void get_paths(const std::string &base,
                   std::vector<BlueFSPathEntry> *paths) const override;
    uint64_t get_avail_by_bdev(uint8_t bdev) const override;

    uint64_t get_level_usage(uint8_t level, uint8_t bdev) const;
    uint64_t get_db_avail4slow() const { return db_avail4slow_; }

private:
    uint8_t hint_to_level(void *hint) const;
    void update_max(uint8_t bdev, uint8_t level);

    // [bdev][level] usage, extra row for per-level totals (index = MAX_BDEV)
    // extra column for per-device totals (index = RDB_LEVEL_MAX - RDB_LEVEL_FIRST)
    static constexpr size_t LEVELS = RDB_LEVEL_MAX - RDB_LEVEL_FIRST + 1;
    static constexpr size_t BDEVS = MAX_BDEV + 1;

    std::array<std::array<std::atomic<uint64_t>, LEVELS>, BDEVS> usage_;
    std::array<std::array<std::atomic<uint64_t>, LEVELS>, BDEVS> max_usage_;
    std::array<std::atomic<uint64_t>, LEVELS> per_level_files_{};

    uint64_t l_totals_[RDB_LEVEL_MAX - RDB_LEVEL_FIRST]{};
    uint64_t db_avail4slow_ = 0;
    uint64_t level0_size_ = 0;
    uint64_t level_base_ = 0;
    uint64_t level_multiplier_ = 0;
};

}  // namespace TOPNSPC
