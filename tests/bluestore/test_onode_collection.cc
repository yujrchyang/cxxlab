#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "bluestore/collection.h"
#include "bluestore/onode.h"
#include "kv/key_value_db.h"

using namespace TOPNSPC;

class OnodeCollectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create temporary directory for test database
        char template_path[] = "/tmp/test_onode_XXXXXX";
        char *result = mkdtemp(template_path);
        ASSERT_NE(result, nullptr);
        test_dir_ = result;

        // Create and open RocksDB
        db_ = KeyValueDB::create("rocksdb", test_dir_);
        ASSERT_NE(db_, nullptr);

        std::ostringstream oss;
        int r = db_->create_and_open(oss);
        ASSERT_EQ(r, 0) << "Failed to create and open database: " << oss.str();
    }

    void TearDown() override {
        if (db_) {
            db_->close();
            db_.reset();
        }

        // Clean up temporary directory
        if (!test_dir_.empty()) {
            std::string rm_cmd = "rm -rf " + test_dir_;
            int r = system(rm_cmd.c_str());
            (void)r;
        }
    }

    std::string test_dir_;
    std::unique_ptr<KeyValueDB> db_;
};

// Onode basic tests

TEST_F(OnodeCollectionTest, OnodeConstruction) {
    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 0x12345678;
    oid.oid = "test_object";

    std::string key = "test_key";
    Onode on(oid, key);

    EXPECT_EQ(on.oid, oid);
    EXPECT_EQ(on.key, key);
    EXPECT_FALSE(on.exists);
    EXPECT_EQ(on.onode.size, 0u);
}

TEST_F(OnodeCollectionTest, OnodeReferenceCounting) {
    ghobject_t oid;
    oid.oid = "test_obj";

    Onode *on = new Onode(oid, "key");
    EXPECT_EQ(on->nref.load(), 0);

    on->get();
    EXPECT_EQ(on->nref.load(), 1);

    on->get();
    EXPECT_EQ(on->nref.load(), 2);

    bool deleted = on->put();
    EXPECT_FALSE(deleted);
    EXPECT_EQ(on->nref.load(), 1);

    deleted = on->put();
    EXPECT_TRUE(deleted);
}

TEST_F(OnodeCollectionTest, OnodeEncodeDecode) {
    ghobject_t oid;
    oid.pool = 5;
    oid.hash = 0xABCD1234;
    oid.oid = "encoded_object";

    Onode on(oid, "test_key");
    on.exists = true;
    on.onode.size = 1024;
    on.onode.nid = 42;

    // Add some extents without blobs first
    on.extent_map.add(0, 0, 4096, nullptr);
    on.extent_map.add(8192, 0, 4096, nullptr);

    // Encode
    bufferlist bl;
    on.encode(bl);
    EXPECT_GT(bl.length(), 0u);

    // Decode
    ghobject_t oid2;
    Onode on2(oid2, "test_key2");
    auto p = bl.cbegin();
    on2.decode(p);

    EXPECT_EQ(on2.exists, true);
    EXPECT_EQ(on2.onode.size, 1024u);
    EXPECT_EQ(on2.onode.nid, 42u);
    EXPECT_EQ(on2.extent_map.size(), 2u);
}

TEST_F(OnodeCollectionTest, OnodeAttributes) {
    ghobject_t oid;
    oid.oid = "attr_object";

    Onode on(oid, "key");

    // Set attributes
    std::map<std::string, bufferptr> attrs;

    bufferlist bl1;
    bl1.append("value1");
    attrs["attr1"] = buffer::create(bl1.length());
    bl1.begin().copy(bl1.length(), attrs["attr1"].c_str());

    bufferlist bl2;
    bl2.append("value2");
    attrs["attr2"] = buffer::create(bl2.length());
    bl2.begin().copy(bl2.length(), attrs["attr2"].c_str());

    on.set_attrs(attrs);

    // Get single attribute
    bufferptr value;
    on.get_attr("attr1", &value);
    EXPECT_EQ(value.length(), 6u);
    EXPECT_EQ(std::string(value.c_str(), value.length()), "value1");

    // Get non-existent attribute
    on.get_attr("nonexistent", &value);
    EXPECT_EQ(value.length(), 0u);

    // Get all attributes
    std::map<std::string, bufferptr> all_attrs;
    on.get_all_attrs(&all_attrs);
    EXPECT_EQ(all_attrs.size(), 2u);
    EXPECT_TRUE(all_attrs.find("attr1") != all_attrs.end());
    EXPECT_TRUE(all_attrs.find("attr2") != all_attrs.end());
}

// OnodeSpace tests

TEST_F(OnodeCollectionTest, OnodeSpaceAddAndLookup) {
    OnodeSpace space(100);

    ghobject_t oid;
    oid.oid = "test_obj";

    Onode *raw_on = new Onode(oid, "key");
    raw_on->get();
    OnodeRef on(raw_on, OnodeDeleter{});

    // Add to cache
    space.add(oid, on);
    EXPECT_EQ(space.size(), 1u);

    // Lookup
    OnodeRef found = space.lookup(oid);
    EXPECT_NE(found, nullptr);
    EXPECT_EQ(found->oid, oid);

    // Lookup non-existent
    ghobject_t oid2;
    oid2.oid = "other_obj";
    OnodeRef not_found = space.lookup(oid2);
    EXPECT_EQ(not_found, nullptr);
}

TEST_F(OnodeCollectionTest, OnodeSpaceRemove) {
    OnodeSpace space(100);

    ghobject_t oid;
    oid.oid = "test_obj";

    Onode *raw_on = new Onode(oid, "key");
    raw_on->get();
    OnodeRef on(raw_on, OnodeDeleter{});

    space.add(oid, on);
    EXPECT_EQ(space.size(), 1u);

    space.remove(oid);
    EXPECT_EQ(space.size(), 0u);

    OnodeRef found = space.lookup(oid);
    EXPECT_EQ(found, nullptr);
}

TEST_F(OnodeCollectionTest, OnodeSpaceLRUEviction) {
    OnodeSpace space(3);  // Max 3 entries

    // Add 3 entries
    for (int i = 0; i < 3; ++i) {
        ghobject_t oid;
        oid.oid = "obj" + std::to_string(i);

        Onode *raw_on = new Onode(oid, "key" + std::to_string(i));
        raw_on->get();
        OnodeRef on(raw_on, OnodeDeleter{});

        space.add(oid, on);
    }

    EXPECT_EQ(space.size(), 3u);

    // Add 4th entry, should evict oldest
    ghobject_t oid4;
    oid4.oid = "obj3";
    Onode *raw_on4 = new Onode(oid4, "key3");
    raw_on4->get();
    OnodeRef on4(raw_on4, OnodeDeleter{});

    space.add(oid4, on4);

    EXPECT_EQ(space.size(), 3u);

    // First entry should be evicted
    ghobject_t oid0;
    oid0.oid = "obj0";
    OnodeRef found = space.lookup(oid0);
    EXPECT_EQ(found, nullptr);

    // Fourth entry should exist
    OnodeRef found4 = space.lookup(oid4);
    EXPECT_NE(found4, nullptr);
}

TEST_F(OnodeCollectionTest, OnodeSpaceClear) {
    OnodeSpace space(100);

    for (int i = 0; i < 5; ++i) {
        ghobject_t oid;
        oid.oid = "obj" + std::to_string(i);

        Onode *raw_on = new Onode(oid, "key" + std::to_string(i));
        raw_on->get();
        OnodeRef on(raw_on, OnodeDeleter{});

        space.add(oid, on);
    }

    EXPECT_EQ(space.size(), 5u);

    space.clear();

    EXPECT_EQ(space.size(), 0u);
    EXPECT_TRUE(space.empty());
}

// Collection tests

TEST_F(OnodeCollectionTest, CollectionConstruction) {
    Collection coll(db_.get(), 1);

    EXPECT_EQ(coll.get_coll_id(), 1u);
    EXPECT_EQ(coll.get_db(), db_.get());
}

TEST_F(OnodeCollectionTest, CollectionCreateOnode) {
    Collection coll(db_.get(), 1);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 0x1234;
    oid.oid = "new_object";

    OnodeRef on = coll.create_onode(oid);

    EXPECT_NE(on, nullptr);
    EXPECT_TRUE(on->exists);
    EXPECT_EQ(on->oid, oid);
    EXPECT_EQ(on->onode.nid, 0x1234u);
}

TEST_F(OnodeCollectionTest, CollectionGetOnodeCacheHit) {
    Collection coll(db_.get(), 1);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 0x5678;
    oid.oid = "cached_object";

    // Create onode
    OnodeRef on1 = coll.create_onode(oid);
    ASSERT_NE(on1, nullptr);

    // Get same onode (should hit cache)
    OnodeRef on2 = coll.get_onode(oid, false);

    EXPECT_NE(on2, nullptr);
    EXPECT_EQ(on1.get(), on2.get());  // Same pointer
}

TEST_F(OnodeCollectionTest, CollectionGetOnodeFromKV) {
    Collection coll(db_.get(), 1);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 0x9ABC;
    oid.oid = "kv_object";

    // Create and write to KV
    OnodeRef on1 = coll.create_onode(oid);
    on1->onode.size = 2048;

    auto txn = db_->get_transaction();
    on1->write_to_kv(db_.get(), txn);
    db_->submit_transaction_sync(txn);

    // Clear cache
    Collection coll2(db_.get(), 1);

    // Get onode from KV (cache miss)
    OnodeRef on2 = coll2.get_onode(oid, false);

    EXPECT_NE(on2, nullptr);
    EXPECT_TRUE(on2->exists);
    EXPECT_EQ(on2->onode.size, 2048u);
}

TEST_F(OnodeCollectionTest, CollectionGetOnodeNotFound) {
    Collection coll(db_.get(), 1);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 0xDEAD;
    oid.oid = "nonexistent";

    // Try to get without create
    OnodeRef on = coll.get_onode(oid, false);

    EXPECT_EQ(on, nullptr);
}

TEST_F(OnodeCollectionTest, CollectionGetOnodeCreate) {
    Collection coll(db_.get(), 1);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 0xBEEF;
    oid.oid = "create_on_get";

    // Get with create=true
    OnodeRef on = coll.get_onode(oid, true);

    EXPECT_NE(on, nullptr);
    EXPECT_TRUE(on->exists);
}

TEST_F(OnodeCollectionTest, CollectionRemoveOnode) {
    Collection coll(db_.get(), 1);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 0xCAFE;
    oid.oid = "remove_object";

    // Create and write to KV
    OnodeRef on1 = coll.create_onode(oid);
    on1->onode.size = 4096;

    auto txn = db_->get_transaction();
    on1->write_to_kv(db_.get(), txn);
    db_->submit_transaction_sync(txn);

    // Remove onode
    coll.remove_onode(oid);

    // Try to get it (should not exist)
    Collection coll2(db_.get(), 1);
    OnodeRef on2 = coll2.get_onode(oid, false);

    EXPECT_EQ(on2, nullptr);
}

TEST_F(OnodeCollectionTest, CollectionCNode) {
    Collection coll(db_.get(), 1);

    bluestore_cnode_t &cnode = coll.get_cnode();
    cnode.bits = 5;

    EXPECT_EQ(coll.get_cnode().bits, 5u);
}

// Integration tests

TEST_F(OnodeCollectionTest, OnodeWithExtentMap) {
    Collection coll(db_.get(), 1);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 0xFACE;
    oid.oid = "extent_object";

    OnodeRef on = coll.create_onode(oid);
    on->onode.size = 16384;

    // Add extents without blobs for simplicity
    on->extent_map.add(0, 0, 4096, nullptr);
    on->extent_map.add(4096, 0, 8192, nullptr);
    on->extent_map.add(12288, 0, 4096, nullptr);

    // Write to KV
    auto txn = db_->get_transaction();
    on->write_to_kv(db_.get(), txn);
    db_->submit_transaction_sync(txn);

    // Read back
    Collection coll2(db_.get(), 1);
    OnodeRef on2 = coll2.get_onode(oid, false);

    ASSERT_NE(on2, nullptr);
    EXPECT_EQ(on2->onode.size, 16384u);
    EXPECT_EQ(on2->extent_map.size(), 3u);
}

TEST_F(OnodeCollectionTest, MultipleObjectsInCollection) {
    Collection coll(db_.get(), 1);

    // Create multiple objects
    for (int i = 0; i < 10; ++i) {
        ghobject_t oid;
        oid.pool = 1;
        oid.hash = i;
        oid.oid = "multi_obj_" + std::to_string(i);

        OnodeRef on = coll.create_onode(oid);
        on->onode.size = (i + 1) * 1024;

        auto txn = db_->get_transaction();
        on->write_to_kv(db_.get(), txn);
        db_->submit_transaction_sync(txn);
    }

    // Read all objects back
    Collection coll2(db_.get(), 1);

    for (int i = 0; i < 10; ++i) {
        ghobject_t oid;
        oid.pool = 1;
        oid.hash = i;
        oid.oid = "multi_obj_" + std::to_string(i);

        OnodeRef on = coll2.get_onode(oid, false);
        ASSERT_NE(on, nullptr) << "Failed to get object " << i;
        EXPECT_EQ(on->onode.size, (uint64_t)(i + 1) * 1024u);
    }
}
