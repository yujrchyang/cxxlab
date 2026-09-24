#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "bluefs/bluefs_volume_selector.h"

using namespace TOPNSPC;

namespace {

// =========================================================================
// OriginalVolumeSelector tests
// =========================================================================

TEST(OriginalVolumeSelector, HintForLogReturnsWal) {
    OriginalVolumeSelector vs(256 << 20, 1ULL << 30, 512 << 20);
    void *hint = vs.get_hint_for_log();
    EXPECT_EQ(vs.select_prefer_bdev(hint), BDEV_WAL);
}

TEST(OriginalVolumeSelector, HintByDirDbReturnsDb) {
    OriginalVolumeSelector vs(256 << 20, 1ULL << 30, 512 << 20);
    void *hint = vs.get_hint_by_dir("db");
    EXPECT_EQ(vs.select_prefer_bdev(hint), BDEV_DB);
}

TEST(OriginalVolumeSelector, HintByDirSlowReturnsSlow) {
    OriginalVolumeSelector vs(256 << 20, 1ULL << 30, 512 << 20);
    void *hint = vs.get_hint_by_dir("db.slow");
    EXPECT_EQ(vs.select_prefer_bdev(hint), BDEV_SLOW);
}

TEST(OriginalVolumeSelector, HintByDirWalReturnsWal) {
    OriginalVolumeSelector vs(256 << 20, 1ULL << 30, 512 << 20);
    void *hint = vs.get_hint_by_dir("block.wal");
    EXPECT_EQ(vs.select_prefer_bdev(hint), BDEV_WAL);
}

TEST(OriginalVolumeSelector, HintByDirNoSlowDeviceFallsbackToDb) {
    OriginalVolumeSelector vs(256 << 20, 1ULL << 30, 0);
    void *hint = vs.get_hint_by_dir("db.slow");
    EXPECT_EQ(vs.select_prefer_bdev(hint), BDEV_DB);
}

TEST(OriginalVolumeSelector, GetAvailReturnsCapacities) {
    OriginalVolumeSelector vs(100, 200, 300);
    EXPECT_EQ(vs.get_avail_by_bdev(BDEV_WAL), 100ULL);
    EXPECT_EQ(vs.get_avail_by_bdev(BDEV_DB), 200ULL);
    EXPECT_EQ(vs.get_avail_by_bdev(BDEV_SLOW), 300ULL);
}

TEST(OriginalVolumeSelector, GetPathsReturnsTwoPaths) {
    OriginalVolumeSelector vs(256 << 20, 1ULL << 30, 512 << 20);
    std::vector<BlueFSPathEntry> paths;
    vs.get_paths("/data", &paths);
    ASSERT_EQ(paths.size(), 2U);
    EXPECT_EQ(paths[0].first, "/data");
    EXPECT_EQ(paths[0].second, 1ULL << 30);
    EXPECT_EQ(paths[1].first, "/data.slow");
    EXPECT_EQ(paths[1].second, 512ULL << 20);
}

TEST(OriginalVolumeSelector, GetPathsNoSlowUsesDbCapacity) {
    OriginalVolumeSelector vs(0, 1ULL << 30, 0);
    std::vector<BlueFSPathEntry> paths;
    vs.get_paths("/data", &paths);
    ASSERT_EQ(paths.size(), 2U);
    EXPECT_EQ(paths[1].first, "/data.slow");
    EXPECT_EQ(paths[1].second, 1ULL << 30);
}

// =========================================================================
// FitToFastVolumeSelector tests
// =========================================================================

TEST(FitToFastVolumeSelector, GetPathsReturnsSinglePath) {
    FitToFastVolumeSelector vs(256 << 20, 1ULL << 30, 512 << 20);
    std::vector<BlueFSPathEntry> paths;
    vs.get_paths("/data", &paths);
    ASSERT_EQ(paths.size(), 1U);
    EXPECT_EQ(paths[0].first, "/data");
    EXPECT_EQ(paths[0].second, 1ULL);  // dummy size
}

TEST(FitToFastVolumeSelector, HintByDirDbReturnsDb) {
    FitToFastVolumeSelector vs(256 << 20, 1ULL << 30, 512 << 20);
    void *hint = vs.get_hint_by_dir("db");
    EXPECT_EQ(vs.select_prefer_bdev(hint), BDEV_DB);
}

TEST(FitToFastVolumeSelector, HintByDirSlowReturnsSlow) {
    FitToFastVolumeSelector vs(256 << 20, 1ULL << 30, 512 << 20);
    void *hint = vs.get_hint_by_dir("db.slow");
    EXPECT_EQ(vs.select_prefer_bdev(hint), BDEV_SLOW);
}

// =========================================================================
// RocksDBBlueFSVolumeSelector tests
// =========================================================================

class RdbVolSelectorTest : public ::testing::Test {
protected:
    void SetUp() override {
        vs_no_extra_.reset(new RocksDBBlueFSVolumeSelector(
            256ULL << 20, 1ULL << 30, 512ULL << 20));

        vs_extra_.reset(new RocksDBBlueFSVolumeSelector(
            256ULL << 20, 1ULL << 30, 512ULL << 20,
            64ULL << 20,   // level0_size = 64MB
            256ULL << 20,  // level_base = 256MB
            10,            // level_multiplier = 10
            0.9,           // reserved_factor = 0.9
            0,             // reserved = 0
            true));        // use_db_extra = true
    }

    std::unique_ptr<RocksDBBlueFSVolumeSelector> vs_no_extra_;
    std::unique_ptr<RocksDBBlueFSVolumeSelector> vs_extra_;
};

TEST_F(RdbVolSelectorTest, HintForLogReturnsWal) {
    void *hint = vs_no_extra_->get_hint_for_log();
    EXPECT_EQ(vs_no_extra_->select_prefer_bdev(hint), BDEV_WAL);
}

TEST_F(RdbVolSelectorTest, HintByDirDbReturnsDb) {
    void *hint = vs_no_extra_->get_hint_by_dir("db");
    EXPECT_EQ(vs_no_extra_->select_prefer_bdev(hint), BDEV_DB);
}

TEST_F(RdbVolSelectorTest, HintByDirSlowReturnsSlow) {
    void *hint = vs_no_extra_->get_hint_by_dir("db.slow");
    EXPECT_EQ(vs_no_extra_->select_prefer_bdev(hint), BDEV_SLOW);
}

TEST_F(RdbVolSelectorTest, HintByDirWalReturnsWal) {
    void *hint = vs_no_extra_->get_hint_by_dir("block.wal");
    EXPECT_EQ(vs_no_extra_->select_prefer_bdev(hint), BDEV_WAL);
}

TEST_F(RdbVolSelectorTest, AddSubUsage) {
    void *hint = vs_no_extra_->get_hint_by_dir("db");
    bluefs_extent_t ext{BDEV_DB, 0, 0x10000};

    uint64_t before = vs_no_extra_->get_avail_by_bdev(BDEV_DB);
    vs_no_extra_->add_usage(hint, ext);
    EXPECT_EQ(vs_no_extra_->get_avail_by_bdev(BDEV_DB), before - 0x10000);

    vs_no_extra_->sub_usage(hint, ext);
    EXPECT_EQ(vs_no_extra_->get_avail_by_bdev(BDEV_DB), before);
}

TEST_F(RdbVolSelectorTest, AddSubLevelUsage) {
    void *hint = vs_no_extra_->get_hint_by_dir("db");
    bluefs_extent_t ext{BDEV_DB, 0, 0x10000};
    vs_no_extra_->add_usage(hint, ext);
    EXPECT_EQ(vs_no_extra_->get_level_usage(RDB_LEVEL_DB, BDEV_DB), 0x10000ULL);
    EXPECT_EQ(vs_no_extra_->get_level_usage(RDB_LEVEL_LOG, BDEV_DB), 0ULL);
}

TEST_F(RdbVolSelectorTest, FnodeUsage) {
    void *hint = vs_no_extra_->get_hint_by_dir("db");
    bluefs_fnode_t fnode{1, 0x5000, 0};
    fnode.append_extent({BDEV_DB, 0, 0x1000});
    fnode.append_extent({BDEV_DB, 0x1000, 0x2000});
    vs_no_extra_->add_usage(hint, fnode);
    EXPECT_EQ(vs_no_extra_->get_level_usage(RDB_LEVEL_DB, BDEV_DB), 0x3000ULL);
}

TEST_F(RdbVolSelectorTest, GetPaths) {
    std::vector<BlueFSPathEntry> paths;
    vs_no_extra_->get_paths("/data", &paths);
    ASSERT_EQ(paths.size(), 2U);
    EXPECT_EQ(paths[0].first, "/data");
    EXPECT_EQ(paths[0].second, 1ULL << 30);
    EXPECT_EQ(paths[1].first, "/data.slow");
    EXPECT_EQ(paths[1].second, 512ULL << 20);
}

TEST_F(RdbVolSelectorTest, LevelDbPrefersDb) {
    void *hint = vs_no_extra_->get_hint_by_dir("db");
    EXPECT_EQ(vs_no_extra_->select_prefer_bdev(hint), BDEV_DB);
}

TEST_F(RdbVolSelectorTest, LevelWalAlwaysWal) {
    void *hint = vs_no_extra_->get_hint_by_dir("block.wal");
    EXPECT_EQ(vs_no_extra_->select_prefer_bdev(hint), BDEV_WAL);
}

TEST_F(RdbVolSelectorTest, LevelLogAlwaysWal) {
    void *hint = vs_no_extra_->get_hint_for_log();
    EXPECT_EQ(vs_no_extra_->select_prefer_bdev(hint), BDEV_WAL);
}

TEST_F(RdbVolSelectorTest, SlowSpillsToDbWithExtra) {
    // Fill SLOW device
    void *hint = vs_extra_->get_hint_by_dir("db.slow");
    vs_extra_->add_usage(hint, bluefs_extent_t{BDEV_SLOW, 0, 512ULL << 20});
    EXPECT_EQ(vs_extra_->get_avail_by_bdev(BDEV_SLOW), 0ULL);
    // with extra enabled, SLOW should spill to DB
    uint8_t bdev = vs_extra_->select_prefer_bdev(hint);
    EXPECT_EQ(bdev, BDEV_DB);
}

TEST_F(RdbVolSelectorTest, SlowStaysOnSlowWithoutExtra) {
    void *hint = vs_no_extra_->get_hint_by_dir("db.slow");
    vs_no_extra_->add_usage(hint, bluefs_extent_t{BDEV_SLOW, 0, 512ULL << 20});
    EXPECT_EQ(vs_no_extra_->get_avail_by_bdev(BDEV_SLOW), 0ULL);
    // without extra, SLOW stays on SLOW even when full
    uint8_t bdev = vs_no_extra_->select_prefer_bdev(hint);
    EXPECT_EQ(bdev, BDEV_SLOW);
}

TEST_F(RdbVolSelectorTest, DbAvail4SlowCalculated) {
    // 1GB DB total, level0=64MB, base=256MB, multiplier=10, factor=0.9
    // Level sizes: L0=64, L1=256, L2=2560 → exceeds 1GB at L2 threshold (64+256+2560=2880)
    // threshold = 64+256 = 320MB
    // reserved_factor=0.9 → 320*0.9 = 288MB reserved
    // db_avail4slow = 1024 - 288 = 736MB
    EXPECT_GT(vs_extra_->get_db_avail4slow(), 0ULL);
    EXPECT_LT(vs_extra_->get_db_avail4slow(), 1ULL << 30);
}

TEST_F(RdbVolSelectorTest, DbAvail4SlowZeroWithoutExtra) {
    EXPECT_EQ(vs_no_extra_->get_db_avail4slow(), 0ULL);
}

TEST_F(RdbVolSelectorTest, LevelUsageTrackingPerLevelPerDev) {
    void *hint_db = vs_no_extra_->get_hint_by_dir("db");
    void *hint_slow = vs_no_extra_->get_hint_by_dir("db.slow");
    vs_no_extra_->add_usage(hint_db, bluefs_extent_t{BDEV_DB, 0, 0x10000});
    vs_no_extra_->add_usage(hint_slow, bluefs_extent_t{BDEV_SLOW, 0, 0x20000});
    EXPECT_EQ(vs_no_extra_->get_level_usage(RDB_LEVEL_DB, BDEV_DB), 0x10000ULL);
    EXPECT_EQ(vs_no_extra_->get_level_usage(RDB_LEVEL_SLOW, BDEV_SLOW), 0x20000ULL);
    EXPECT_EQ(vs_no_extra_->get_level_usage(RDB_LEVEL_DB, BDEV_SLOW), 0ULL);
}

}  // namespace
