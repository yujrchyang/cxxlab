#pragma once

#include <memory>
#include <string>
#include <vector>

#include "btier/btier_types.h"
#include "btier/config.h"
#include "common/buffer_fwd.h"
#include "common/common_fwd.h"

namespace TOPNSPC {

class BlockDevice;
class Allocator;

namespace btier {

class ExtentMap;
class KeyMap;
class Journal;
class ScoringEngine;
class MigrationEngine;
class BtierObserver;

// ── Scoring result entry ────────────────────────────────────────
struct ScoredExtent {
    uint64_t extent_id;
    float score;
    Tier tier;
};

class BtierEngine {
public:
    BtierEngine();
    ~BtierEngine();

    int init(const BtierConfig &config);
    int recover();
    void shutdown();

    int sync();

    int put(const std::string &key, const bufferlist &value);
    int get(const std::string &key, bufferlist &value);
    int del(const std::string &key);

    // Run a full scoring pass: refresh per-extent randomness from KeyMap
    // per-key stride, adapt weights based on FAST watermark, then score
    // all extents. Returns scored extents sorted by score descending.
    // No background thread — caller invokes this explicitly.
    std::vector<ScoredExtent> run_scoring_pass();

    // Manually compact a specific extent (for testing/admin).
    // Copies live data to a new extent with compacted offsets.
    // Returns true if compaction was committed.
    bool compact_extent(uint64_t extent_id);

    // Manually run one migration cycle (for testing without background thread).
    void run_migration_cycle();

    // Get migration engine stats (for testing).
    using MigrationStats = ::TOPNSPC::btier::MigrationStats;
    MigrationStats get_migration_stats() const;

    struct Stats {
        uint64_t num_keys = 0;
        uint64_t num_extents = 0;
        uint64_t fast_extents = 0;
        uint64_t slow_extents = 0;
        double fast_watermark = 0.0;
        uint64_t journal_bytes = 0;
        uint64_t migrations_pending = 0;
        MigrationStats migration;
    };
    Stats get_stats() const;

    void set_weights(const WeightSet &w);
    void set_watermarks(double low, double high);
    void set_scan_interval(uint32_t ms);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace btier
}  // namespace TOPNSPC
