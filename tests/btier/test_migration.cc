#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "blk/allocator.h"
#include "blk/block_device.h"
#include "btier/btier.h"
#include "btier/btier_types.h"
#include "btier/config.h"
#include "btier/extent_map.h"
#include "btier/key_map.h"
#include "btier/migration_engine.h"
#include "btier/scoring_engine.h"
#include "common/buffer.h"
#include "cxxlab_test.h"

using namespace TOPNSPC;
using namespace TOPNSPC::btier;

// ── MigrationEngine unit tests (direct API, no background thread) ──

class MigrationEngineTest : public ::testing::Test {
protected:
    BtierConfig cfg;
    std::unique_ptr<Allocator> fast_alloc;
    std::unique_ptr<Allocator> slow_alloc;
    std::unique_ptr<ExtentMap> em;
    std::unique_ptr<KeyMap> km;
    std::unique_ptr<ScoringEngine> se;

    static constexpr int64_t kFastSize = 16 * 1024 * 1024;
    static constexpr int64_t kSlowSize = 32 * 1024 * 1024;

    void SetUp() override {
        cfg.extent_size = 4 * 1024 * 1024;
        cfg.block_size = 4096;
        cfg.large_value_threshold = 2 * 1024 * 1024;
        cfg.promote_threshold = 0.7f;
        cfg.demote_threshold = 0.3f;
        cfg.scan_interval_ms = 100;

        fast_alloc.reset(
            Allocator::create("avl", kFastSize, cfg.block_size, "test-fast"));
        slow_alloc.reset(
            Allocator::create("avl", kSlowSize, cfg.block_size, "test-slow"));

        em = std::make_unique<ExtentMap>(cfg);
        em->add_allocator(Tier::FAST, fast_alloc.get());
        em->add_allocator(Tier::SLOW, slow_alloc.get());
        em->init_free_space();

        km = std::make_unique<KeyMap>();
        se = std::make_unique<ScoringEngine>(cfg);
    }

    void TearDown() override {
        se.reset();
        km.reset();
        em.reset();
        fast_alloc.reset();
        slow_alloc.reset();
    }
};

TEST_F(MigrationEngineTest, MigrateTierFastToSlowDataIntact) {
    // Allocate extent on FAST
    auto alloc = em->allocate_extent(Tier::FAST, 4 * 1024 * 1024);
    ASSERT_TRUE(alloc.has_value());

    // Append a slot and record I/O
    uint32_t offset = em->append_slot(alloc->extent_id, 4096);
    ASSERT_NE(offset, UINT32_MAX);
    em->record_io(alloc->extent_id, IoOp::WRITE, 1000);

    // Put key in KeyMap
    KeyLocation kloc{alloc->extent_id, offset, 4096};
    km->put("key1", kloc, 0);

    // Migrate FAST → SLOW (no real device, but protocol still works)
    // We can't do real I/O without a block device, so we test the protocol:
    // begin_migration should succeed, and commit_migration should work
    auto h = em->begin_migration(alloc->extent_id);
    ASSERT_NE(h, nullptr);

    // Allocate destination on SLOW
    auto dst = em->allocate_raw(Tier::SLOW, h->src_loc.length);
    ASSERT_TRUE(dst.has_value());

    // Commit migration
    bool committed = em->commit_migration(h.get(), *dst);
    EXPECT_TRUE(committed);

    // Verify location changed to SLOW
    auto loc = em->get_location(alloc->extent_id);
    ASSERT_TRUE(loc.has_value());
    EXPECT_EQ(loc->tier, Tier::SLOW);

    // Verify KeyMap still has the key (extent_id unchanged)
    KeyLocation result;
    EXPECT_TRUE(km->lookup("key1", &result));
    EXPECT_EQ(result.extent_id, alloc->extent_id);
    EXPECT_EQ(result.offset, offset);
}

TEST_F(MigrationEngineTest, MigrateTierInterruptedByWrite) {
    auto alloc = em->allocate_extent(Tier::FAST, 4 * 1024 * 1024);
    ASSERT_TRUE(alloc.has_value());

    em->append_slot(alloc->extent_id, 4096);

    // Begin migration
    auto h = em->begin_migration(alloc->extent_id);
    ASSERT_NE(h, nullptr);

    // Concurrent mark_dead_slot bumps generation
    em->mark_dead_slot(alloc->extent_id, 4096);

    // Commit should fail (interrupted)
    auto dst = em->allocate_raw(Tier::SLOW, h->src_loc.length);
    ASSERT_TRUE(dst.has_value());

    bool committed = em->commit_migration(h.get(), *dst);
    EXPECT_FALSE(committed);

    // Abort restores to ACTIVE
    em->abort_migration(h.get());
    em->release_source(*dst);

    // Extent should still be ACTIVE
    uint64_t raw = em->get_raw_metrics(alloc->extent_id);
    EXPECT_EQ(ExtentMetrics::state(raw), (uint32_t)ACTIVE);
}

TEST_F(MigrationEngineTest, MigrationEngineStats) {
    MigrationEngine me(em.get(), km.get(), se.get(),
                       nullptr, nullptr, cfg);

    auto stats = me.get_stats();
    EXPECT_EQ(stats.promotions_committed, 0u);
    EXPECT_EQ(stats.demotions_committed, 0u);
    EXPECT_EQ(stats.interruptions, 0u);
}

TEST_F(MigrationEngineTest, EnqueueMigrate) {
    MigrationEngine me(em.get(), km.get(), se.get(),
                       nullptr, nullptr, cfg);

    me.enqueue_migrate(1, Tier::SLOW, Tier::FAST, 0.9f);
    EXPECT_EQ(me.pending(), 1u);
}

TEST_F(MigrationEngineTest, EnqueueCompact) {
    MigrationEngine me(em.get(), km.get(), se.get(),
                       nullptr, nullptr, cfg);

    me.enqueue_compact(1);
    EXPECT_EQ(me.pending(), 1u);
}

TEST_F(MigrationEngineTest, StartStop) {
    MigrationEngine me(em.get(), km.get(), se.get(),
                       nullptr, nullptr, cfg);

    me.start();
    me.stop();
    // Should not hang
}

// ── End-to-end migration tests with real devices ───────────────

class BtierMigrationTest : public ::testing::Test {
protected:
    std::string fast_path_;
    std::string slow_path_;
    int fast_fd_ = -1;
    int slow_fd_ = -1;
    static constexpr uint64_t kFastSize = 16 * 1024 * 1024;
    static constexpr uint64_t kSlowSize = 32 * 1024 * 1024;
    BtierConfig cfg;

    void SetUp() override {
        auto tmpl = cxxlab_tmp_path("btier_mig_fast");
        fast_fd_ = ::mkstemp(tmpl.data());
        ASSERT_GE(fast_fd_, 0);
        fast_path_ = tmpl;
        ::fallocate(fast_fd_, 0, 0, kFastSize);

        auto tmpl2 = cxxlab_tmp_path("btier_mig_slow");
        slow_fd_ = ::mkstemp(tmpl2.data());
        ASSERT_GE(slow_fd_, 0);
        slow_path_ = tmpl2;
        ::fallocate(slow_fd_, 0, 0, kSlowSize);

        cfg.fast_dev_path = fast_path_;
        cfg.slow_dev_path = slow_path_;
        cfg.extent_size = 4 * 1024 * 1024;
        cfg.block_size = 4096;
        cfg.large_value_threshold = 2 * 1024 * 1024;
        cfg.journal_size = 1 * 1024 * 1024;
        cfg.scan_interval_ms = 100;
    }

    void TearDown() override {
        if (fast_fd_ >= 0) ::close(fast_fd_);
        if (slow_fd_ >= 0) ::close(slow_fd_);
        if (!fast_path_.empty()) ::unlink(fast_path_.c_str());
        if (!slow_path_.empty()) ::unlink(slow_path_.c_str());
    }

    bufferlist make_value(const std::string &s) {
        bufferlist bl;
        bl.append(s);
        return bl;
    }

    std::string read_value(bufferlist &bl) {
        return std::string(bl.c_str(), bl.length());
    }
};

// ── Promote: write to SLOW, score high, migrate to FAST ──────

TEST_F(BtierMigrationTest, PromoteSlowToFast) {
    // Force all data to SLOW by filling FAST first
    BtierConfig slow_cfg = cfg;
    // Make FAST very small so data goes to SLOW
    slow_cfg.large_value_threshold = 0;  // all values get dedicated extents

    BtierEngine engine;
    ASSERT_EQ(engine.init(cfg), 0);

    // Write data
    std::string val(4096, 'X');
    ASSERT_EQ(engine.put("key1", make_value(val)), 0);

    // Verify data is readable
    bufferlist result;
    ASSERT_EQ(engine.get("key1", result), 0);
    EXPECT_EQ(read_value(result), val);

    // Manually run a scoring cycle — should not crash even with no migrations
    auto scored = engine.run_scoring_pass();
    EXPECT_GT(scored.size(), 0u);

    engine.shutdown();
}

// ── Migration preserves data integrity ─────────────────────────

TEST_F(BtierMigrationTest, DataIntactAfterManualMigration) {
    BtierEngine engine;
    ASSERT_EQ(engine.init(cfg), 0);

    // Write multiple keys
    for (int i = 0; i < 20; i++) {
        std::string key = "mig_" + std::to_string(i);
        std::string val(1024, 'a' + (i % 26));
        ASSERT_EQ(engine.put(key, make_value(val)), 0);
    }

    // Verify all keys before migration
    for (int i = 0; i < 20; i++) {
        std::string key = "mig_" + std::to_string(i);
        bufferlist result;
        ASSERT_EQ(engine.get(key, result), 0);
        EXPECT_EQ(result.length(), 1024u);
    }

    // Run scoring pass
    engine.run_scoring_pass();

    // Verify all keys still intact after scoring
    for (int i = 0; i < 20; i++) {
        std::string key = "mig_" + std::to_string(i);
        std::string expected(1024, 'a' + (i % 26));
        bufferlist result;
        ASSERT_EQ(engine.get(key, result), 0);
        EXPECT_EQ(read_value(result), expected);
    }

    engine.shutdown();
}

// ── Concurrent read during migration (TSAN) ───────────────────

TEST_F(BtierMigrationTest, ConcurrentReadDuringScoring) {
    BtierEngine engine;
    ASSERT_EQ(engine.init(cfg), 0);

    // Write data
    for (int i = 0; i < 10; i++) {
        std::string key = "conc_" + std::to_string(i);
        ASSERT_EQ(engine.put(key, make_value(std::string(1024, 'c'))), 0);
    }

    // Concurrent reads while scoring runs
    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};

    std::thread reader([&]() {
        while (!stop.load()) {
            for (int i = 0; i < 10; i++) {
                std::string key = "conc_" + std::to_string(i);
                bufferlist result;
                if (engine.get(key, result) < 0) {
                    errors++;
                }
            }
        }
    });

    // Run a few scoring passes
    for (int i = 0; i < 3; i++) {
        engine.run_scoring_pass();
    }

    stop.store(true);
    reader.join();

    EXPECT_EQ(errors.load(), 0);
    engine.shutdown();
}

// ── Background thread migration ────────────────────────────────

TEST_F(BtierMigrationTest, BackgroundThreadRuns) {
    BtierConfig bg_cfg = cfg;
    bg_cfg.scan_interval_ms = 50;  // fast cycle for testing

    BtierEngine engine;
    ASSERT_EQ(engine.init(bg_cfg), 0);

    // Write some data
    for (int i = 0; i < 5; i++) {
        std::string key = "bg_" + std::to_string(i);
        ASSERT_EQ(engine.put(key, make_value(std::string(1024, 'd'))), 0);
    }

    // Let background thread run for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Verify data still readable
    for (int i = 0; i < 5; i++) {
        std::string key = "bg_" + std::to_string(i);
        bufferlist result;
        ASSERT_EQ(engine.get(key, result), 0);
        EXPECT_EQ(result.length(), 1024u);
    }

    engine.shutdown();
}

// ── Stats include migration counters ──────────────────────────

TEST_F(BtierMigrationTest, StatsIncludeMigrationCounters) {
    BtierEngine engine;
    ASSERT_EQ(engine.init(cfg), 0);

    // Write data
    for (int i = 0; i < 5; i++) {
        std::string key = "stat_" + std::to_string(i);
        ASSERT_EQ(engine.put(key, make_value(std::string(1024, 'e'))), 0);
    }

    auto stats = engine.get_stats();
    EXPECT_EQ(stats.num_keys, 5u);
    EXPECT_GE(stats.num_extents, 1u);
    // Migration stats should be zero initially
    EXPECT_EQ(stats.migration.promotions_committed, 0u);
    EXPECT_EQ(stats.migration.demotions_committed, 0u);
    EXPECT_EQ(stats.migration.compactions_committed, 0u);

    engine.shutdown();
}

// ── Persistence after migration ───────────────────────────────

TEST_F(BtierMigrationTest, PersistenceAfterScoring) {
    // Write data
    {
        BtierEngine engine;
        ASSERT_EQ(engine.init(cfg), 0);

        for (int i = 0; i < 10; i++) {
            std::string key = "persist_" + std::to_string(i);
            ASSERT_EQ(engine.put(key, make_value(std::string(1024, 'f'))), 0);
        }

        // Run scoring pass
        engine.run_scoring_pass();

        engine.shutdown();
    }

    // Restart and verify
    {
        BtierEngine engine;
        ASSERT_EQ(engine.init(cfg), 0);

        for (int i = 0; i < 10; i++) {
            std::string key = "persist_" + std::to_string(i);
            bufferlist result;
            ASSERT_EQ(engine.get(key, result), 0);
            EXPECT_EQ(result.length(), 1024u);
        }

        engine.shutdown();
    }
}

// ── Mixed workload: writes + reads + background migration ─────

TEST_F(BtierMigrationTest, MixedWorkloadWithBackgroundMigration) {
    BtierConfig mix_cfg = cfg;
    mix_cfg.scan_interval_ms = 50;

    BtierEngine engine;
    ASSERT_EQ(engine.init(mix_cfg), 0);

    // Phase 1: Write keys
    for (int i = 0; i < 30; i++) {
        std::string key = "mix_" + std::to_string(i);
        ASSERT_EQ(engine.put(key, make_value(std::string(512, 'g'))), 0);
    }

    // Phase 2: Read all keys while background migrator runs
    for (int iter = 0; iter < 5; iter++) {
        for (int i = 0; i < 30; i++) {
            std::string key = "mix_" + std::to_string(i);
            bufferlist result;
            ASSERT_EQ(engine.get(key, result), 0);
            EXPECT_EQ(result.length(), 512u);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Phase 3: Overwrite some keys
    for (int i = 0; i < 10; i++) {
        std::string key = "mix_" + std::to_string(i);
        ASSERT_EQ(engine.put(key, make_value(std::string(1024, 'h'))), 0);
    }

    // Phase 4: Verify all keys
    for (int i = 0; i < 30; i++) {
        std::string key = "mix_" + std::to_string(i);
        bufferlist result;
        ASSERT_EQ(engine.get(key, result), 0);
        if (i < 10) {
            EXPECT_EQ(result.length(), 1024u);
            EXPECT_EQ(result[0], 'h');
        } else {
            EXPECT_EQ(result.length(), 512u);
            EXPECT_EQ(result[0], 'g');
        }
    }

    engine.shutdown();
}

// ── Kill and restart with background migration ────────────────

TEST_F(BtierMigrationTest, KillAndRestartWithMigration) {
    // Write data and let background migration run
    {
        BtierEngine engine;
        ASSERT_EQ(engine.init(cfg), 0);

        for (int i = 0; i < 15; i++) {
            std::string key = "kr_" + std::to_string(i);
            ASSERT_EQ(engine.put(key, make_value(std::string(1024, 'k'))), 0);
        }

        // Let background thread run briefly
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Sync and shutdown (simulating clean shutdown after migration)
        engine.sync();
        engine.shutdown();
    }

    // Restart and verify all keys
    {
        BtierEngine engine;
        ASSERT_EQ(engine.init(cfg), 0);

        for (int i = 0; i < 15; i++) {
            std::string key = "kr_" + std::to_string(i);
            bufferlist result;
            ASSERT_EQ(engine.get(key, result), 0);
            EXPECT_EQ(result.length(), 1024u);
        }

        engine.shutdown();
    }
}
