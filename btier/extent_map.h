#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "blk/allocator.h"
#include "blk/block_device.h"
#include "blk/extent_types.h"
#include "btier/btier_types.h"

namespace TOPNSPC::btier {

class BtierConfig;
class KeyMap;

// ── ExtentEntry (held via shared_ptr in the map) ────────────────
struct ExtentEntry {
    ExtentMetrics metrics;

    std::shared_mutex struct_lock;
    DiskLocation location;
    uint32_t used_bytes = 0;
    uint32_t live_bytes = 0;

    std::atomic<uint32_t> io_refs{0};

    ExtentEntry() = default;
    explicit ExtentEntry(const DiskLocation &loc) : location(loc) {}
};

// ── ExtentMap ────────────────────────────────────────────────────
class ExtentMap {
public:
    explicit ExtentMap(const BtierConfig &cfg);
    ~ExtentMap();

    // ── Initialization ───────────────────────────────────────────
    void add_allocator(Tier tier, Allocator *alloc);
    void init_free_space();

    // ── Location ────────────────────────────────────────────────
    std::optional<DiskLocation> get_location(uint64_t extent_id) const;

    // ── Metrics ────────────────────────────────────────────────
    uint64_t get_raw_metrics(uint64_t extent_id) const;
    void record_io(uint64_t extent_id, IoOp op, uint32_t current_time);
    void set_randomness(uint64_t extent_id, uint32_t randomness);
    void refresh_randomness(const KeyMap &key_map);

    // ── Multi-key packing ────────────────────────────────────────
    uint64_t find_extent_with_space(Tier tier, uint32_t needed_bytes) const;
    uint32_t append_slot(uint64_t extent_id, uint32_t size);
    void mark_dead_slot(uint64_t extent_id, uint32_t length);

    uint32_t get_live_bytes(uint64_t extent_id) const;
    uint32_t get_used_bytes(uint64_t extent_id) const;

    // ── Allocation ───────────────────────────────────────────────
    struct AllocResult {
        uint64_t extent_id;
        DiskLocation location;
    };
    std::optional<AllocResult> allocate_extent(Tier tier, uint64_t size);
    std::optional<DiskLocation> allocate_raw(Tier tier, uint64_t size);

    // ── Migration handle ──────────────────────────────────────────
    struct MigrationHandle {
        uint64_t extent_id = 0;
        DiskLocation src_loc;

    private:
        friend class ExtentMap;
        uint64_t gen_before = 0;
    };

    std::unique_ptr<MigrationHandle> begin_migration(uint64_t extent_id);
    bool commit_migration(MigrationHandle *h, const DiskLocation &new_loc);
    void abort_migration(MigrationHandle *h);
    bool check_migration(const MigrationHandle &h) const;

    void release_source(const DiskLocation &loc);

    // ── Lifecycle ────────────────────────────────────────────────
    void free(uint64_t extent_id);
    void process_deferred_free();
    void clear_deferred_free();

    // ── Introspection ────────────────────────────────────────────
    size_t size() const;
    double fast_watermark() const;

    struct SnapshotEntry {
        uint64_t extent_id;
        DiskLocation location;
        uint64_t raw_metrics;
        uint32_t last_access_time;
        uint32_t used_bytes;
        uint32_t live_bytes;
    };
    std::vector<SnapshotEntry> snapshot() const;

    // ── I/O reference counting ───────────────────────────────────
    void io_ref_inc(uint64_t extent_id);
    void io_ref_dec(uint64_t extent_id);

    // ── ExtentHeader persistence ─────────────────────────────────
    // Write updated ExtentHeaders for all dirty extents to their devices.
    // Called by BtierEngine::sync(). Clears the dirty set.
    void flush_dirty_headers();

    // ── Recovery helpers ────────────────────────────────────────
    // Create an entry from journal replay (bypasses allocation).
    void create_entry_from_journal(uint64_t extent_id,
                                   const DiskLocation &loc);

    // Restore used_bytes/live_bytes for an extent during recovery.
    // Called after all keys have been replayed into KeyMap.
    void restore_extent_state(uint64_t extent_id,
                              uint32_t used_bytes,
                              uint32_t live_bytes);

    // Set block device for a tier (used for writing extent headers).
    void add_block_device(Tier tier, BlockDevice *dev);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace TOPNSPC::btier
