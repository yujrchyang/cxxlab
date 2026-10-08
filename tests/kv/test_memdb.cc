#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "common/buffer.h"
#include "kv/key_value_db.h"
#include "kv/merge_op/int64_array_merge_op.h"
#include "kv/merge_op/xor_merge_op.h"

using namespace TOPNSPC;

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bufferlist to_bl(const std::string &s) {
    bufferlist bl;
    bl.append(s);
    return bl;
}

std::string from_bl(const bufferlist &bl) { return bl.to_str(); }

int64_t read_i64(const bufferlist &bl) {
    if (bl.length() < sizeof(int64_t)) return 0;
    auto s = bl.to_str();
    int64_t v;
    memcpy(&v, s.data(), sizeof(int64_t));
    return v;
}

bufferlist i64_bl(int64_t v) {
    bufferlist bl;
    bl.append(reinterpret_cast<const char *>(&v),
              static_cast<unsigned>(sizeof(v)));
    return bl;
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class MemDBTest : public ::testing::Test {
protected:
    std::unique_ptr<KeyValueDB> db;

    void SetUp() override {
        db = KeyValueDB::create("memdb", "");
        ASSERT_NE(db, nullptr);
        db->set_merge_operator("T",
                               std::make_shared<Int64ArrayMergeOperator>());
        db->set_merge_operator("b",
                               std::make_shared<XorMergeOperator>());
        int r = db->create_and_open(std::cerr);
        ASSERT_EQ(r, 0);
    }

    void TearDown() override { db->close(); }

    void set(const std::string &prefix, const std::string &key,
             const std::string &val) {
        auto t = db->get_transaction();
        t->set(prefix, key, to_bl(val));
        ASSERT_EQ(db->submit_transaction(t), 0);
    }

    void rmkey(const std::string &prefix, const std::string &key) {
        auto t = db->get_transaction();
        t->rmkey(prefix, key);
        ASSERT_EQ(db->submit_transaction(t), 0);
    }

    std::optional<std::string> get(const std::string &prefix,
                                   const std::string &key) {
        bufferlist out;
        int r = db->get(prefix, key, &out);
        if (r != 0) return std::nullopt;
        return from_bl(out);
    }
};

// =====================================================================
// Lifecycle
// =====================================================================

TEST_F(MemDBTest, CreateAndOpen) {
    SUCCEED();
}

TEST_F(MemDBTest, ReopenNoPersistence) {
    set("O", "key", "val");
    db->close();

    db = KeyValueDB::create("memdb", "");
    ASSERT_NE(db, nullptr);
    ASSERT_EQ(db->create_and_open(std::cerr), 0);
    EXPECT_FALSE(get("O", "key").has_value());
}

// =====================================================================
// Point Read / Write
// =====================================================================

TEST_F(MemDBTest, PutAndGet) {
    set("O", "obj1", "hello memdb");
    auto val = get("O", "obj1");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "hello memdb");
}

TEST_F(MemDBTest, GetNotFound) {
    auto val = get("O", "nonexistent");
    EXPECT_FALSE(val.has_value());
}

TEST_F(MemDBTest, Overwrite) {
    set("O", "k", "v1");
    set("O", "k", "v2");
    auto val = get("O", "k");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "v2");
}

TEST_F(MemDBTest, BinaryValue) {
    std::string bin;
    bin.push_back('\0');
    bin.push_back('\xff');
    bin.push_back('\n');
    set("O", "bin", bin);
    auto val = get("O", "bin");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, bin);
}

TEST_F(MemDBTest, EmptyKey) {
    set("O", "", "empty-key-val");
    auto val = get("O", "");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "empty-key-val");
}

// =====================================================================
// Batch get
// =====================================================================

TEST_F(MemDBTest, BatchGet) {
    set("O", "a", "1");
    set("O", "b", "2");
    set("O", "c", "3");

    std::set<std::string> keys = {"a", "b", "c", "d"};
    std::map<std::string, bufferlist> out;
    int r = db->get("O", keys, &out);
    ASSERT_EQ(r, 0);
    EXPECT_EQ(out.size(), 3);
    EXPECT_EQ(from_bl(out["a"]), "1");
    EXPECT_EQ(from_bl(out["b"]), "2");
    EXPECT_EQ(from_bl(out["c"]), "3");
    EXPECT_EQ(out.find("d"), out.end());
}

// =====================================================================
// Delete
// =====================================================================

TEST_F(MemDBTest, Delete) {
    set("O", "k", "v");
    rmkey("O", "k");
    EXPECT_FALSE(get("O", "k").has_value());
}

TEST_F(MemDBTest, DeleteNonExistent) {
    rmkey("O", "nonexistent");
    SUCCEED();
}

// =====================================================================
// Prefix Delete
// =====================================================================

TEST_F(MemDBTest, DeleteByPrefix) {
    set("O", "a", "1");
    set("O", "b", "2");
    set("O", "c", "3");
    set("M", "x", "9");

    auto t = db->get_transaction();
    t->rmkeys_by_prefix("O");
    ASSERT_EQ(db->submit_transaction(t), 0);

    EXPECT_FALSE(get("O", "a").has_value());
    EXPECT_FALSE(get("O", "b").has_value());
    EXPECT_FALSE(get("O", "c").has_value());
    EXPECT_TRUE(get("M", "x").has_value());
}

TEST_F(MemDBTest, DeleteByPrefixEmpty) {
    auto t = db->get_transaction();
    t->rmkeys_by_prefix("X");
    ASSERT_EQ(db->submit_transaction(t), 0);
}

// =====================================================================
// Range Delete
// =====================================================================

TEST_F(MemDBTest, DeleteRange) {
    set("O", "a", "1");
    set("O", "b", "2");
    set("O", "c", "3");
    set("O", "d", "4");

    auto t = db->get_transaction();
    t->rm_range_keys("O", "b", "d");
    ASSERT_EQ(db->submit_transaction(t), 0);

    EXPECT_TRUE(get("O", "a").has_value());
    EXPECT_FALSE(get("O", "b").has_value());
    EXPECT_FALSE(get("O", "c").has_value());
    EXPECT_TRUE(get("O", "d").has_value());
}

TEST_F(MemDBTest, DeleteRangeNonExistent) {
    auto t = db->get_transaction();
    t->rm_range_keys("O", "x", "z");
    ASSERT_EQ(db->submit_transaction(t), 0);
}

// =====================================================================
// Transaction (batch)
// =====================================================================

TEST_F(MemDBTest, TransactionBatch) {
    auto t = db->get_transaction();
    t->set("O", "k1", to_bl("v1"));
    t->set("O", "k2", to_bl("v2"));
    t->rmkey("O", "k1");
    ASSERT_EQ(db->submit_transaction(t), 0);

    EXPECT_FALSE(get("O", "k1").has_value());
    EXPECT_TRUE(get("O", "k2").has_value());
}

TEST_F(MemDBTest, TransactionAtomicity) {
    set("O", "k", "original");

    auto t = db->get_transaction();
    t->set("O", "k", to_bl("updated"));
    t->rmkey("O", "nonexistent");
    ASSERT_EQ(db->submit_transaction(t), 0);

    auto val = get("O", "k");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "updated");
}

// =====================================================================
// Iterator (whole-space)
// =====================================================================

TEST_F(MemDBTest, IteratorSeekToFirst) {
    set("O", "a", "1");
    set("O", "b", "2");
    set("O", "c", "3");

    auto it = db->get_wholespace_iterator();
    it->seek_to_first();
    std::vector<std::pair<std::string, std::string>> got;
    for (; it->valid(); it->next()) {
        auto [pre, inner] = it->raw_key();
        got.emplace_back(inner, from_bl(it->value()));
    }

    ASSERT_EQ(got.size(), 3);
    EXPECT_EQ(got[0].first, "a");
    EXPECT_EQ(got[1].first, "b");
    EXPECT_EQ(got[2].first, "c");
}

TEST_F(MemDBTest, IteratorSeekToLast) {
    set("O", "a", "1");
    set("O", "b", "2");
    set("O", "c", "3");

    auto it = db->get_wholespace_iterator();
    it->seek_to_last();
    std::vector<std::string> got;
    for (; it->valid(); it->prev())
        got.push_back(it->raw_key().second);

    ASSERT_EQ(got.size(), 3);
    EXPECT_EQ(got[0], "c");
    EXPECT_EQ(got[1], "b");
    EXPECT_EQ(got[2], "a");
}

TEST_F(MemDBTest, IteratorEmpty) {
    auto it = db->get_wholespace_iterator();
    it->seek_to_first();
    EXPECT_FALSE(it->valid());
}

TEST_F(MemDBTest, IteratorLowerBound) {
    set("O", "a", "1");
    set("O", "b", "2");
    set("O", "c", "3");

    auto it = db->get_wholespace_iterator();
    it->lower_bound(std::string("O\0b", 3));

    ASSERT_TRUE(it->valid());
    EXPECT_EQ(it->raw_key().second, "b");

    it->next();
    ASSERT_TRUE(it->valid());
    EXPECT_EQ(it->raw_key().second, "c");
}

TEST_F(MemDBTest, IteratorUpperBound) {
    set("O", "a", "1");
    set("O", "b", "2");
    set("O", "c", "3");

    auto it = db->get_wholespace_iterator();
    it->upper_bound(std::string("O\0b", 3));

    ASSERT_TRUE(it->valid());
    EXPECT_EQ(it->raw_key().second, "c");
}

TEST_F(MemDBTest, IteratorPastEnd) {
    set("O", "a", "1");

    auto it = db->get_wholespace_iterator();
    it->lower_bound("z");
    EXPECT_FALSE(it->valid());
}

// =====================================================================
// Prefix Iterator (via get_iterator)
// =====================================================================

TEST_F(MemDBTest, PrefixIterator) {
    set("O", "obj1", "onode1");
    set("O", "obj2", "onode2");
    set("M", "k1", "v1");
    set("M", "k2", "v2");

    auto it = db->get_iterator("O");
    it->seek_to_first();
    std::vector<std::string> keys;
    for (; it->valid(); it->next())
        keys.push_back(it->key());

    ASSERT_EQ(keys.size(), 2);
    EXPECT_EQ(keys[0], "obj1");
    EXPECT_EQ(keys[1], "obj2");
}

TEST_F(MemDBTest, PrefixIteratorEmptyPrefix) {
    set("O", "obj1", "v1");
    set("M", "k1", "v2");

    auto it = db->get_iterator("X");
    it->seek_to_first();
    EXPECT_FALSE(it->valid());
}

TEST_F(MemDBTest, PrefixIteratorSeek) {
    set("O", "a", "1");
    set("O", "b", "2");
    set("O", "c", "3");
    set("M", "x", "9");

    auto it = db->get_iterator("O");
    it->lower_bound("b");

    ASSERT_TRUE(it->valid());
    EXPECT_EQ(it->key(), "b");

    it->next();
    ASSERT_TRUE(it->valid());
    EXPECT_EQ(it->key(), "c");

    it->next();
    EXPECT_FALSE(it->valid());
}

// =====================================================================
// Merge Operator
// =====================================================================

TEST_F(MemDBTest, Merge) {
    auto t = db->get_transaction();
    t->merge("T", "counter", i64_bl(10));
    t->merge("T", "counter", i64_bl(20));
    t->merge("T", "counter", i64_bl(30));
    ASSERT_EQ(db->submit_transaction_sync(t), 0);

    bufferlist out;
    int r = db->get("T", "counter", &out);
    ASSERT_EQ(r, 0);
    EXPECT_EQ(read_i64(out), 60);
}

TEST_F(MemDBTest, MergeNonExistent) {
    auto t = db->get_transaction();
    t->merge("T", "new_key", i64_bl(42));
    ASSERT_EQ(db->submit_transaction(t), 0);

    bufferlist out;
    int r = db->get("T", "new_key", &out);
    ASSERT_EQ(r, 0);
    EXPECT_EQ(read_i64(out), 42);
}

TEST_F(MemDBTest, MergeXor) {
    auto t = db->get_transaction();
    t->merge("b", "bitmap", to_bl("\x0f\x0f"));
    t->merge("b", "bitmap", to_bl("\xf0\xf0"));
    ASSERT_EQ(db->submit_transaction(t), 0);

    bufferlist out;
    int r = db->get("b", "bitmap", &out);
    ASSERT_EQ(r, 0);
    EXPECT_EQ(out.to_str(), std::string("\xff\xff", 2));
}

TEST_F(MemDBTest, MergeNoOperatorRegistered) {
    auto t = db->get_transaction();
    t->merge("X", "key", to_bl("data"));
    EXPECT_NE(db->submit_transaction(t), 0);  // Should fail, not crash
}

// =====================================================================
// Multiple prefixes (interleaved)
// =====================================================================

TEST_F(MemDBTest, MultiplePrefixes) {
    set("S", "super", "sb");
    set("C", "coll1", "cnode");
    set("O", "obj1", "onode");
    set("M", "k1", "v1");
    set("B", "alloc", "extent");

    EXPECT_EQ(get("S", "super"), "sb");
    EXPECT_EQ(get("C", "coll1"), "cnode");
    EXPECT_EQ(get("O", "obj1"), "onode");
    EXPECT_EQ(get("M", "k1"), "v1");
    EXPECT_EQ(get("B", "alloc"), "extent");

    auto it = db->get_wholespace_iterator();
    it->seek_to_first();
    int count = 0;
    for (; it->valid(); it->next())
        count++;
    EXPECT_EQ(count, 5);
}

// =====================================================================
// Compact (no-op for MemDB)
// =====================================================================

TEST_F(MemDBTest, Compact) {
    set("O", "a", "1");
    db->compact();
    EXPECT_TRUE(get("O", "a").has_value());
}

TEST_F(MemDBTest, CompactAsync) {
    set("O", "a", "1");
    db->compact_async();
    EXPECT_TRUE(get("O", "a").has_value());
}

// =====================================================================
// Estimated size
// =====================================================================

TEST_F(MemDBTest, EstimatedSize) {
    set("O", "k", "hello");
    std::map<std::string, uint64_t> extra;
    uint64_t size = db->get_estimated_size(extra);
    EXPECT_GE(size, 8);
}

// =====================================================================
// Edge cases
// =====================================================================

TEST_F(MemDBTest, KeyWithNullByte) {
    std::string key_with_null = "key\0with\0null";
    key_with_null += '\0';
    set("O", key_with_null, "val");
    auto val = get("O", key_with_null);
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "val");
}

TEST_F(MemDBTest, ValueWithNullBytes) {
    std::string val_with_null = "he\0llo\0\0wo\0rld";
    val_with_null += '\0';
    set("O", "k", val_with_null);
    auto val = get("O", "k");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, val_with_null);
}

TEST_F(MemDBTest, SubmitTransactionSync) {
    auto t = db->get_transaction();
    t->set("O", "k", to_bl("sync"));
    ASSERT_EQ(db->submit_transaction_sync(t), 0);
    EXPECT_TRUE(get("O", "k").has_value());
}

}  // namespace
