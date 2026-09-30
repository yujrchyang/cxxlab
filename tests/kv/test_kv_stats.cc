#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "common/buffer.h"
#include "common/perf_counter.h"
#include "cxxlab_test.h"
#include "kv/key_value_db.h"
#include "kv/merge_op/int64_array_merge_op.h"

using namespace TOPNSPC;

namespace {

bufferlist to_bl(const std::string &s) {
    bufferlist bl;
    bl.append(s.data(), static_cast<unsigned>(s.size()));
    return bl;
}

bufferlist i64_bl(int64_t v) {
    bufferlist bl;
    bl.append(reinterpret_cast<const char *>(&v),
              static_cast<unsigned>(sizeof(v)));
    return bl;
}

std::string tmpdir() {
    auto tmpl = cxxlab_tmp_dir("kvstats");
    char *buf = tmpl.data();
    if (!mkdtemp(buf)) return {};
    return buf;
}

// =========================================================================
// MemDB stats
// =========================================================================

class MemDBStatsTest : public ::testing::Test {
protected:
    std::unique_ptr<KeyValueDB> db;

    void SetUp() override {
        db = KeyValueDB::create("memdb", "");
        ASSERT_NE(db, nullptr);
        db->set_merge_operator(
            "T", std::make_shared<Int64ArrayMergeOperator>());
        std::ostringstream out;
        ASSERT_EQ(db->create_and_open(out), 0);
    }

    void TearDown() override { db->close(); }

    PerfCounters *perf() { return db->get_perf_counters(); }
};

TEST_F(MemDBStatsTest, PutDelSubmitCount) {
    auto p = perf();
    ASSERT_NE(p, nullptr);

    auto t = db->get_transaction();
    t->set("O", "k1", to_bl("v1"));
    t->set("O", "k2", to_bl("v2"));
    t->set("O", "k3", to_bl("v3"));
    t->rmkey("O", "k1");
    t->rmkey("O", "k2");
    ASSERT_EQ(db->submit_transaction(t), 0);

    EXPECT_EQ(p->get(l_kv_put_count), 3);
    EXPECT_EQ(p->get(l_kv_del_count), 2);
    EXPECT_EQ(p->get(l_kv_submit_count), 1);
}

TEST_F(MemDBStatsTest, MergeCountAndOpStats) {
    auto p = perf();
    ASSERT_NE(p, nullptr);

    for (int i = 0; i < 4; i++) {
        auto t = db->get_transaction();
        t->merge("T", "stat", i64_bl(i + 1));
        ASSERT_EQ(db->submit_transaction(t), 0);
    }

    EXPECT_EQ(p->get(l_kv_merge_count), 4);
    EXPECT_EQ(p->get(l_kv_submit_count), 4);

    auto stats = db->get_merge_op_stats();
    ASSERT_EQ(stats.size(), 1u);
    EXPECT_EQ(stats[0].prefix, "T");
    EXPECT_EQ(stats[0].name, "int64_array");
    EXPECT_EQ(stats[0].merge_count, 4u);
    EXPECT_GT(stats[0].merge_bytes, 0u);
}

TEST_F(MemDBStatsTest, GetAndIterCount) {
    auto t = db->get_transaction();
    t->set("O", "k1", to_bl("v1"));
    ASSERT_EQ(db->submit_transaction(t), 0);

    auto p = perf();
    ASSERT_NE(p, nullptr);

    bufferlist bl;
    db->get("O", "k1", &bl);
    EXPECT_EQ(p->get(l_kv_get_count), 1);

    auto it = db->get_iterator("O");
    EXPECT_NE(it, nullptr);
    EXPECT_EQ(p->get(l_kv_iter_count), 1);
}

TEST_F(MemDBStatsTest, LatencyNonZero) {
    auto t = db->get_transaction();
    t->set("O", "k1", to_bl("v1"));
    ASSERT_EQ(db->submit_transaction(t), 0);

    auto p = perf();
    ASSERT_NE(p, nullptr);

    bufferlist bl;
    db->get("O", "k1", &bl);

    auto [gsum, gcount] = p->get_tavg_ns(l_kv_get_lat);
    EXPECT_GT(gcount, 0u);

    auto [ssum, scount] = p->get_tavg_ns(l_kv_submit_lat);
    EXPECT_GT(scount, 0u);
}

TEST_F(MemDBStatsTest, SyncCommitLatency) {
    auto t = db->get_transaction();
    t->set("O", "k1", to_bl("v1"));
    ASSERT_EQ(db->submit_transaction_sync(t), 0);

    auto p = perf();
    ASSERT_NE(p, nullptr);

    EXPECT_EQ(p->get(l_kv_submit_count), 1);
    auto [sum, count] = p->get_tavg_ns(l_kv_commit_lat);
    EXPECT_GT(count, 0u);
}

TEST_F(MemDBStatsTest, ResetMergeStats) {
    for (int i = 0; i < 3; i++) {
        auto t = db->get_transaction();
        t->merge("T", "stat", i64_bl(i + 1));
        ASSERT_EQ(db->submit_transaction(t), 0);
    }

    auto &mops = db->get_merge_ops();
    ASSERT_EQ(mops.size(), 1u);
    auto &mop = mops[0].second;
    EXPECT_EQ(mop->get_merge_count(), 3);
    EXPECT_GT(mop->get_merge_bytes(), 0u);

    mop->reset_merge_stats();
    EXPECT_EQ(mop->get_merge_count(), 0);
    EXPECT_EQ(mop->get_merge_bytes(), 0);
}

// =========================================================================
// RocksDBStore stats
// =========================================================================

class RocksDBStatsTest : public ::testing::Test {
protected:
    std::unique_ptr<KeyValueDB> db;
    std::string dbpath_;

    void SetUp() override {
        dbpath_ = tmpdir();
        ASSERT_FALSE(dbpath_.empty()) << "mkdtemp failed";
        db = KeyValueDB::create("rocksdb", dbpath_);
        ASSERT_NE(db, nullptr);
        db->set_merge_operator(
            "T", std::make_shared<Int64ArrayMergeOperator>());
        std::ostringstream out;
        ASSERT_EQ(db->create_and_open(out), 0);
    }

    void TearDown() override {
        db->close();
        std::filesystem::remove_all(dbpath_);
    }

    PerfCounters *perf() { return db->get_perf_counters(); }
};

TEST_F(RocksDBStatsTest, PutDelSubmitCount) {
    auto p = perf();
    ASSERT_NE(p, nullptr);

    auto t = db->get_transaction();
    t->set("O", "k1", to_bl("v1"));
    t->set("O", "k2", to_bl("v2"));
    t->set("O", "k3", to_bl("v3"));
    t->rmkey("O", "k1");
    t->rmkey("O", "k2");
    ASSERT_EQ(db->submit_transaction(t), 0);

    EXPECT_EQ(p->get(l_kv_put_count), 3);
    EXPECT_EQ(p->get(l_kv_del_count), 2);
    EXPECT_EQ(p->get(l_kv_submit_count), 1);
}

TEST_F(RocksDBStatsTest, MergeCountAndOpStats) {
    auto p = perf();
    ASSERT_NE(p, nullptr);

    for (int i = 0; i < 4; i++) {
        auto t = db->get_transaction();
        t->merge("T", "stat", i64_bl(i + 1));
        ASSERT_EQ(db->submit_transaction(t), 0);
    }

    EXPECT_EQ(p->get(l_kv_merge_count), 4);
    EXPECT_EQ(p->get(l_kv_submit_count), 4);

    bufferlist bl;
    db->get("T", "stat", &bl);

    auto stats = db->get_merge_op_stats();
    ASSERT_EQ(stats.size(), 1u);
    EXPECT_EQ(stats[0].prefix, "T");
    EXPECT_EQ(stats[0].name, "int64_array");
    EXPECT_GE(stats[0].merge_count, 1u);
    EXPECT_GT(stats[0].merge_bytes, 0u);
}

TEST_F(RocksDBStatsTest, GetAndIterCount) {
    auto t = db->get_transaction();
    t->set("O", "k1", to_bl("v1"));
    ASSERT_EQ(db->submit_transaction(t), 0);

    auto p = perf();
    ASSERT_NE(p, nullptr);

    bufferlist bl;
    db->get("O", "k1", &bl);
    EXPECT_EQ(p->get(l_kv_get_count), 1);

    auto it = db->get_iterator("O");
    EXPECT_NE(it, nullptr);
    EXPECT_EQ(p->get(l_kv_iter_count), 1);
}

TEST_F(RocksDBStatsTest, LatencyNonZero) {
    auto t = db->get_transaction();
    t->set("O", "k1", to_bl("v1"));
    ASSERT_EQ(db->submit_transaction(t), 0);

    auto p = perf();
    ASSERT_NE(p, nullptr);

    bufferlist bl;
    db->get("O", "k1", &bl);

    auto [gsum, gcount] = p->get_tavg_ns(l_kv_get_lat);
    EXPECT_GT(gcount, 0u);

    auto [ssum, scount] = p->get_tavg_ns(l_kv_submit_lat);
    EXPECT_GT(scount, 0u);
}

TEST_F(RocksDBStatsTest, SyncCommitLatency) {
    auto t = db->get_transaction();
    t->set("O", "k1", to_bl("v1"));
    ASSERT_EQ(db->submit_transaction_sync(t), 0);

    auto p = perf();
    ASSERT_NE(p, nullptr);

    EXPECT_EQ(p->get(l_kv_submit_count), 1);
    auto [sum, count] = p->get_tavg_ns(l_kv_commit_lat);
    EXPECT_GT(count, 0u);
}

}  // namespace
