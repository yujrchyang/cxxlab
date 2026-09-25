#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "btier/btier_types.h"
#include "btier/config.h"
#include "common/buffer_fwd.h"
#include "common/common_fwd.h"

namespace TOPNSPC {

class BlockDevice;

namespace btier {

class ExtentMap;
class KeyMap;
class ScoringEngine;
class BtierObserver;

enum class MigrationResult {
    COMMITTED,
    INTERRUPTED,
    FAILED,
};

// ── MigrationEngine ─────────────────────────────────────────────
// Background thread that:
//   1. Promotes hot extents from SLOW → FAST
//   2. Demotes cold extents from FAST → SLOW
//   3. Compacts extents with high dead-space ratio (stub in C1)
//
// 3-step protocol (lock-free during data copy):
//   Step 1: begin_migration() → claim extent, get MigrationHandle
//   Step 2: Copy data (NO locks held — I/O path runs concurrently)
//   Step 3: Commit or abort
//
// The engine never touches generation directly — all gen logic is
// hidden inside ExtentMap's MigrationHandle methods.
class MigrationEngine {
public:
    MigrationEngine(ExtentMap *extent_map,
                    KeyMap *key_map,
                    ScoringEngine *scoring_engine,
                    BlockDevice *fast_dev,
                    BlockDevice *slow_dev,
                    const BtierConfig &cfg,
                    BtierObserver *observer = nullptr);
    ~MigrationEngine();

    void start();
    void stop();

    // Tier migration: move extent data from one tier to another.
    void enqueue_migrate(uint64_t extent_id, Tier from, Tier to, float score);

    // Compaction: copy live data to a new extent with compacted offsets.
    void enqueue_compact(uint64_t extent_id);

    size_t pending() const;

    using Stats = MigrationStats;
    MigrationStats get_stats() const;

    // Run a single cycle synchronously (for testing without background thread).
    void run_cycle();

    // Manually compact a single extent (for testing).
    // Returns the result of the compaction operation.
    MigrationResult compact_extent(uint64_t extent_id);

    // Manually migrate a single extent (for testing).
    MigrationResult migrate_extent(uint64_t extent_id, Tier from, Tier to);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace btier
}  // namespace TOPNSPC
