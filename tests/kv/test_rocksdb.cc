#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "common/buffer.h"
#include "cxxlab_test.h"
#include "kv/key_value_db.h"
#include "kv/merge_op/int64_array_merge_op.h"
#include "kv/merge_op/xor_merge_op.h"

using namespace TOPNSPC;

namespace {

static std::string tmpdir() {
    auto tmpl = cxxlab_tmp_dir("rocksdb");
    char *buf = tmpl.data();
    if (!mkdtemp(buf))
        return {};
    return buf;
}

bufferlist to_bl(const std::string &s) {
    bufferlist bl;
    bl.append(s.data(), static_cast<unsigned>(s.size()));
    return bl;
}

class RocksDBStoreTest : public ::testing::Test {
protected:
    std::unique_ptr<KeyValueDB> db;
    std::string dbpath_;

    void SetUp() override {
        dbpath_ = tmpdir();
        ASSERT_FALSE(dbpath_.empty()) << "mkdtemp failed";
        db = KeyValueDB::create("rocksdb", dbpath_);
        ASSERT_NE(db, nullptr);
    }

    void TearDown() override {
        db->close();
        std::filesystem::remove_all(dbpath_);
    }

    void set(const std::string &prefix, const std::string &key,
             const std::string &value) {
        auto t = db->get_transaction();
        t->set(prefix, key, to_bl(value));
        ASSERT_EQ(db->submit_transaction_sync(t), 0);
    }

    std::optional<std::string> get(const std::string &prefix,
                                   const std::string &key) {
        bufferlist bl;
        int r = db->get(prefix, key, &bl);
        if (r != 0) return std::nullopt;
        return bl.to_str();
    }
};

// ── Lifecycle ──────────────────────────────────────────────────

TEST_F(RocksDBStoreTest, CreateAndOpen) {
    ASSERT_EQ(db->create_and_open(std::cerr), 0);
}

TEST_F(RocksDBStoreTest, ReopenPersistence) {
    ASSERT_EQ(db->create_and_open(std::cerr), 0);
    set("S", "key", "persist-data");
    db->close();

    auto db2 = KeyValueDB::create("rocksdb", dbpath_);
    ASSERT_NE(db2, nullptr);
    ASSERT_EQ(db2->open(std::cerr), 0);
    auto val = [&]() -> std::optional<std::string> {
        bufferlist bl;
        int r = db2->get("S", "key", &bl);
        if (r != 0) return std::nullopt;
        return bl.to_str();
    }();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "persist-data");
    db2->close();
}

// ── Point Read / Write ────────────────────────────────────────

TEST_F(RocksDBStoreTest, PutAndGet) {
    ASSERT_EQ(db->create_and_open(std::cerr), 0);
    set("O", "obj1", "onode-data");
    EXPECT_EQ(get("O", "obj1"), "onode-data");
}

TEST_F(RocksDBStoreTest, GetNotFound) {
    ASSERT_EQ(db->create_and_open(std::cerr), 0);
    EXPECT_FALSE(get("X", "nonexistent").has_value());
}

TEST_F(RocksDBStoreTest, Overwrite) {
    ASSERT_EQ(db->create_and_open(std::cerr), 0);
    set("O", "k", "v1");
    set("O", "k", "v2");
    EXPECT_EQ(get("O", "k"), "v2");
}

TEST_F(RocksDBStoreTest, BatchGet) {
    ASSERT_EQ(db->create_and_open(std::cerr), 0);
    set("O", "a", "1");
    set("O", "b", "2");
    set("O", "c", "3");

    std::set<std::string> keys{"a", "b", "c", "z"};
    std::map<std::string, bufferlist> result;
    ASSERT_EQ(db->get("O", keys, &result), 0);
    EXPECT_EQ(result.size(), 3);
    EXPECT_EQ(result["a"].to_str(), "1");
    EXPECT_EQ(result["b"].to_str(), "2");
    EXPECT_EQ(result["c"].to_str(), "3");
}

// ── Delete ─────────────────────────────────────────────────────

TEST_F(RocksDBStoreTest, Delete) {
    ASSERT_EQ(db->create_and_open(std::cerr), 0);
    set("O", "k", "v");
    {
        auto t = db->get_transaction();
        t->rmkey("O", "k");
        ASSERT_EQ(db->submit_transaction_sync(t), 0);
    }
    EXPECT_FALSE(get("O", "k").has_value());
}

TEST_F(RocksDBStoreTest, DeleteNonExistent) {
    ASSERT_EQ(db->create_and_open(std::cerr), 0);
    auto t = db->get_transaction();
    t->rmkey("O", "nonexistent");
    ASSERT_EQ(db->submit_transaction_sync(t), 0);
}

TEST_F(RocksDBStoreTest, DeleteByPrefix) {
    ASSERT_EQ(db->create_and_open(std::cerr), 0);
    set("O", "a", "1");
    set("O", "b", "2");
    set("M", "k", "keep");
    {
        auto t = db->get_transaction();
        t->rmkeys_by_prefix("O");
        ASSERT_EQ(db->submit_transaction_sync(t), 0);
    }
    EXPECT_FALSE(get("O", "a").has_value());
    EXPECT_FALSE(get("O", "b").has_value());
    EXPECT_EQ(get("M", "k"), "keep");
}

TEST_F(RocksDBStoreTest, DeleteRange) {
    ASSERT_EQ(db->create_and_open(std::cerr), 0);
    set("O", "a", "1");
    set("O", "b", "2");
    set("O", "c", "3");
    set("O", "d", "4");
    {
        auto t = db->get_transaction();
        t->rm_range_keys("O", "b", "d");
        ASSERT_EQ(db->submit_transaction_sync(t), 0);
    }
    EXPECT_EQ(get("O", "a"), "1");
    EXPECT_FALSE(get("O", "b").has_value());
    EXPECT_FALSE(get("O", "c").has_value());
    EXPECT_EQ(get("O", "d"), "4");
}

// ── Transaction ────────────────────────────────────────────────

TEST_F(RocksDBStoreTest, TransactionBatch) {
    ASSERT_EQ(db->create_and_open(std::cerr), 0);
    auto t = db->get_transaction();
    t->set("O", "k1", to_bl("v1"));
    t->set("O", "k2", to_bl("v2"));
    t->rmkey("O", "k1");
    ASSERT_EQ(db->submit_transaction_sync(t), 0);

    EXPECT_FALSE(get("O", "k1").has_value());
    EXPECT_EQ(get("O", "k2"), "v2");
}

// ── Iterator ───────────────────────────────────────────────────

TEST_F(RocksDBStoreTest, IteratorSeekToFirst) {
    ASSERT_EQ(db->create_and_open(std::cerr), 0);
    set("O", "a", "1");
    set("O", "b", "2");

    auto it = db->get_wholespace_iterator();
    ASSERT_NE(it, nullptr);
    std::vector<std::string> keys;
    for (it->seek_to_first(); it->valid(); it->next())
        keys.push_back(it->key());
    ASSERT_EQ(keys.size(), 2);
}

TEST_F(RocksDBStoreTest, IteratorEmpty) {
    ASSERT_EQ(db->create_and_open(std::cerr), 0);
    auto it = db->get_wholespace_iterator();
    it->seek_to_first();
    EXPECT_FALSE(it->valid());
}

TEST_F(RocksDBStoreTest, IteratorUpperBound) {
    ASSERT_EQ(db->create_and_open(std::cerr), 0);
    set("O", "a", "1");
    set("O", "b", "2");
    set("O", "c", "3");
    set("O", "d", "4");

    IteratorBounds opts;
    opts.upper_bound = "d";
    auto it = db->get_iterator("O", 0, opts);
    ASSERT_NE(it, nullptr);

    it->lower_bound("a");
    std::vector<std::string> keys;
    for (; it->valid(); it->next())
        keys.push_back(it->key());
    ASSERT_EQ(keys.size(), 3);
    EXPECT_EQ(keys[0], "a");
    EXPECT_EQ(keys[1], "b");
    EXPECT_EQ(keys[2], "c");
}

TEST_F(RocksDBStoreTest, IteratorNocache) {
    ASSERT_EQ(db->create_and_open(std::cerr), 0);
    set("O", "a", "1");
    set("O", "b", "2");

    auto it = db->get_wholespace_iterator(ITERATOR_NOCACHE);
    ASSERT_NE(it, nullptr);
    int count = 0;
    for (it->seek_to_first(); it->valid(); it->next())
        count++;
    EXPECT_EQ(count, 2);
}

// ── Merge Operator ─────────────────────────────────────────────

TEST_F(RocksDBStoreTest, MergeInt64) {
    db->set_merge_operator(
        "T", std::make_shared<Int64ArrayMergeOperator>());
    ASSERT_EQ(db->create_and_open(std::cerr), 0);

    auto write_delta = [&](int64_t v) {
        auto t = db->get_transaction();
        bufferlist bl;
        bl.append(reinterpret_cast<char *>(&v), sizeof(v));
        t->merge("T", "counter", bl);
        ASSERT_EQ(db->submit_transaction_sync(t), 0);
    };

    write_delta(10);
    write_delta(20);
    write_delta(30);

    bufferlist result;
    ASSERT_EQ(db->get("T", "counter", &result), 0);
    ASSERT_EQ(result.length(), sizeof(int64_t));
    auto *sum = reinterpret_cast<const int64_t *>(result.c_str());
    EXPECT_EQ(*sum, 60);
}

TEST_F(RocksDBStoreTest, MergeNonExistent) {
    db->set_merge_operator(
        "T", std::make_shared<Int64ArrayMergeOperator>());
    ASSERT_EQ(db->create_and_open(std::cerr), 0);

    int64_t v = 42;
    auto t = db->get_transaction();
    bufferlist bl;
    bl.append(reinterpret_cast<char *>(&v), sizeof(v));
    t->merge("T", "new_key", bl);
    ASSERT_EQ(db->submit_transaction_sync(t), 0);

    bufferlist result;
    ASSERT_EQ(db->get("T", "new_key", &result), 0);
    ASSERT_EQ(result.length(), sizeof(int64_t));
    auto *got = reinterpret_cast<const int64_t *>(result.c_str());
    EXPECT_EQ(*got, 42);
}

TEST_F(RocksDBStoreTest, MergeXor) {
    db->set_merge_operator(
        "b", std::make_shared<XorMergeOperator>());
    ASSERT_EQ(db->create_and_open(std::cerr), 0);

    auto write_xor = [&](const std::string &v) {
        auto t = db->get_transaction();
        t->merge("b", "bitmap", to_bl(v));
        ASSERT_EQ(db->submit_transaction_sync(t), 0);
    };

    write_xor(std::string("\xff\x00", 2));
    write_xor(std::string("\x0f\xf0", 2));

    bufferlist result;
    ASSERT_EQ(db->get("b", "bitmap", &result), 0);
    EXPECT_EQ(result.to_str(), std::string("\xf0\xf0", 2));
}

TEST_F(RocksDBStoreTest, MergeMultiplePrefixes) {
    db->set_merge_operator(
        "T", std::make_shared<Int64ArrayMergeOperator>());
    db->set_merge_operator(
        "b", std::make_shared<XorMergeOperator>());
    ASSERT_EQ(db->create_and_open(std::cerr), 0);

    int64_t v = 10;
    auto t1 = db->get_transaction();
    bufferlist bl;
    bl.append(reinterpret_cast<char *>(&v), sizeof(v));
    t1->merge("T", "stat", bl);
    t1->merge("b", "bmap", to_bl(std::string("\x0f", 1)));
    ASSERT_EQ(db->submit_transaction_sync(t1), 0);

    bufferlist stat_val;
    ASSERT_EQ(db->get("T", "stat", &stat_val), 0);
    ASSERT_EQ(stat_val.length(), sizeof(int64_t));
    EXPECT_EQ(*reinterpret_cast<const int64_t *>(stat_val.c_str()), 10);

    bufferlist bmap_val;
    ASSERT_EQ(db->get("b", "bmap", &bmap_val), 0);
    EXPECT_EQ(bmap_val.to_str(), std::string("\x0f", 1));
}

// ── Compact ────────────────────────────────────────────────────

TEST_F(RocksDBStoreTest, Compact) {
    ASSERT_EQ(db->create_and_open(std::cerr), 0);
    set("O", "a", "1");
    set("O", "b", "2");

    db->compact();
    EXPECT_EQ(get("O", "a"), "1");
    EXPECT_EQ(get("O", "b"), "2");
}

// ── Estimated Size ─────────────────────────────────────────────

TEST_F(RocksDBStoreTest, EstimatedSize) {
    ASSERT_EQ(db->create_and_open(std::cerr), 0);
    set("O", "k", "v");
    db->compact();

    std::map<std::string, uint64_t> extra;
    auto size = db->get_estimated_size(extra);
    EXPECT_GT(size, 0);
}

}  // namespace

// ── Key encoding roundtrip (static, no DB needed) ─────────────

TEST(KeyValueDBEncodeKey, Roundtrip) {
    auto encoded = KeyValueDB::encode_key("S", "min_alloc_size");
    EXPECT_EQ(encoded, std::string("S\0min_alloc_size", 16));
    auto [pre, key] = KeyValueDB::decode_key(encoded);
    EXPECT_EQ(pre, "S");
    EXPECT_EQ(key, "min_alloc_size");
}

TEST(KeyValueDBEncodeKey, EmptyKey) {
    auto encoded = KeyValueDB::encode_key("C", "");
    EXPECT_EQ(encoded, std::string("C\0", 2));
    auto [pre, key] = KeyValueDB::decode_key(encoded);
    EXPECT_EQ(pre, "C");
    EXPECT_TRUE(key.empty());
}

TEST(KeyValueDBEncodeKey, EmptyPrefix) {
    auto encoded = KeyValueDB::encode_key("", "k");
    EXPECT_EQ(encoded, std::string("\0k", 2));
    auto [pre, key] = KeyValueDB::decode_key(encoded);
    EXPECT_TRUE(pre.empty());
    EXPECT_EQ(key, "k");
}

TEST(KeyValueDBEncodeKey, NoSeparatorDecodesAsPrefixOnly) {
    auto [pre, key] = KeyValueDB::decode_key("noseparator");
    EXPECT_EQ(pre, "noseparator");
    EXPECT_TRUE(key.empty());
}
