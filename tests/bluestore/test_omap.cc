#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

#include "bluestore/bluestore.h"
#include "bluestore/bluestore_config.h"
#include "common/object.h"
#include "cxxlab_test.h"

using namespace cxxlab;

class OMapTest : public ::testing::Test {
protected:
    std::string store_path_;
    static constexpr uint64_t kBlockSize = 64 * 1024 * 1024;

    void SetUp() override {
        auto name = std::string("omap_") +
            ::testing::UnitTest::GetInstance()->current_test_info()->name();
        auto tmpl = cxxlab_tmp_path(name.c_str());
        char *dir = ::mkdtemp(const_cast<char *>(tmpl.c_str()));
        ASSERT_NE(dir, nullptr) << "mkdtemp failed: " << strerror(errno);
        store_path_ = std::string(dir);

        std::string db_path = store_path_ + "/db";
        int r = ::mkdir(db_path.c_str(), 0755);
        ASSERT_EQ(r, 0) << "mkdir db failed: " << strerror(errno);

        std::string block_path = store_path_ + "/block";
        int fd = ::open(block_path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
        ASSERT_GE(fd, 0) << "open failed: " << strerror(errno);

        r = ::fallocate(fd, 0, 0, kBlockSize);
        if (r < 0) {
            std::vector<char> zeros(65536, 0);
            for (uint64_t off = 0; off < kBlockSize; off += zeros.size()) {
                ::pwrite(fd, zeros.data(),
                         std::min<uint64_t>(zeros.size(), kBlockSize - off),
                         off);
            }
        }
        ::close(fd);
    }

    void TearDown() override {
        std::string rm_cmd = "rm -rf " + store_path_;
        ::system(rm_cmd.c_str());
    }

    BlueStoreConfig make_config() {
        BlueStoreConfig cfg;
        cfg.path = store_path_;
        cfg.db_path = store_path_ + "/db";
        cfg.bdev_path = store_path_ + "/block";
        cfg.min_alloc_size = 65536;
        cfg.block_size = 4096;
        cfg.max_blob_size = 262144;
        cfg.allocator_type = "bitmap";
        return cfg;
    }

    bool wait_commit(std::atomic<bool> &committed) {
        for (int i = 0; i < 100 && !committed.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return committed.load();
    }
};

TEST_F(OMapTest, SetAndGetKeys) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 0);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 0;
    oid.oid = "test_obj";

    // Create object
    BlueStoreTransaction txc;
    txc.touch(oid);
    std::vector<BlueStoreTransaction> txcs;
    txcs.push_back(std::move(txc));
    std::atomic<bool> committed{false};
    ASSERT_EQ(store.queue_transactions(coll, txcs, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    ASSERT_TRUE(wait_commit(committed));

    // Set OMap keys
    std::map<std::string, bufferlist> keys;
    bufferlist val1, val2;
    val1.append("value1");
    val2.append("value2");
    keys["key1"] = val1;
    keys["key2"] = val2;

    BlueStoreTransaction txc2;
    txc2.omap_setkeys(oid, keys);
    txcs.clear();
    txcs.push_back(std::move(txc2));
    committed = false;
    ASSERT_EQ(store.queue_transactions(coll, txcs, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    ASSERT_TRUE(wait_commit(committed));

    // Get OMap values
    std::set<std::string> query_keys = {"key1", "key2", "key3"};
    std::map<std::string, bufferlist> result;
    ASSERT_EQ(store.omap_get_values(coll, oid, query_keys, &result), 0);

    EXPECT_EQ(result.size(), 2u);
    EXPECT_EQ(result["key1"].to_str(), "value1");
    EXPECT_EQ(result["key2"].to_str(), "value2");
    EXPECT_EQ(result.count("key3"), 0u);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(OMapTest, SetAndGetHeader) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 0);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 0;
    oid.oid = "test_obj";

    BlueStoreTransaction txc;
    txc.touch(oid);
    std::vector<BlueStoreTransaction> txcs;
    txcs.push_back(std::move(txc));
    std::atomic<bool> committed{false};
    ASSERT_EQ(store.queue_transactions(coll, txcs, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    ASSERT_TRUE(wait_commit(committed));

    // Set header
    bufferlist header;
    header.append("test_header");

    BlueStoreTransaction txc2;
    txc2.omap_setheader(oid, header);
    txcs.clear();
    txcs.push_back(std::move(txc2));
    committed = false;
    ASSERT_EQ(store.queue_transactions(coll, txcs, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    ASSERT_TRUE(wait_commit(committed));

    // Get header
    bufferlist result;
    ASSERT_EQ(store.omap_get_header(coll, oid, &result), 0);
    EXPECT_EQ(result.to_str(), "test_header");

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(OMapTest, RemoveKeys) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 0);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 0;
    oid.oid = "test_obj";

    BlueStoreTransaction txc;
    txc.touch(oid);
    std::vector<BlueStoreTransaction> txcs;
    txcs.push_back(std::move(txc));
    std::atomic<bool> committed{false};
    ASSERT_EQ(store.queue_transactions(coll, txcs, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    ASSERT_TRUE(wait_commit(committed));

    // Set keys
    std::map<std::string, bufferlist> keys;
    bufferlist val1, val2, val3;
    val1.append("value1");
    val2.append("value2");
    val3.append("value3");
    keys["key1"] = val1;
    keys["key2"] = val2;
    keys["key3"] = val3;

    BlueStoreTransaction txc2;
    txc2.omap_setkeys(oid, keys);
    txcs.clear();
    txcs.push_back(std::move(txc2));
    committed = false;
    ASSERT_EQ(store.queue_transactions(coll, txcs, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    ASSERT_TRUE(wait_commit(committed));

    // Remove key2
    std::set<std::string> rm_keys = {"key2"};
    BlueStoreTransaction txc3;
    txc3.omap_rmkeys(oid, rm_keys);
    txcs.clear();
    txcs.push_back(std::move(txc3));
    committed = false;
    ASSERT_EQ(store.queue_transactions(coll, txcs, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    ASSERT_TRUE(wait_commit(committed));

    // Verify
    std::set<std::string> query_keys = {"key1", "key2", "key3"};
    std::map<std::string, bufferlist> result;
    ASSERT_EQ(store.omap_get_values(coll, oid, query_keys, &result), 0);

    EXPECT_EQ(result.size(), 2u);
    EXPECT_EQ(result["key1"].to_str(), "value1");
    EXPECT_EQ(result.count("key2"), 0u);
    EXPECT_EQ(result["key3"].to_str(), "value3");

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(OMapTest, Clear) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 0);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 0;
    oid.oid = "test_obj";

    BlueStoreTransaction txc;
    txc.touch(oid);
    std::vector<BlueStoreTransaction> txcs;
    txcs.push_back(std::move(txc));
    std::atomic<bool> committed{false};
    ASSERT_EQ(store.queue_transactions(coll, txcs, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    ASSERT_TRUE(wait_commit(committed));

    // Set keys and header
    std::map<std::string, bufferlist> keys;
    bufferlist val1, header;
    val1.append("value1");
    header.append("header");
    keys["key1"] = val1;

    BlueStoreTransaction txc2;
    txc2.omap_setkeys(oid, keys);
    txc2.omap_setheader(oid, header);
    txcs.clear();
    txcs.push_back(std::move(txc2));
    committed = false;
    ASSERT_EQ(store.queue_transactions(coll, txcs, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    ASSERT_TRUE(wait_commit(committed));

    // Clear
    BlueStoreTransaction txc3;
    txc3.omap_clear(oid);
    txcs.clear();
    txcs.push_back(std::move(txc3));
    committed = false;
    ASSERT_EQ(store.queue_transactions(coll, txcs, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    ASSERT_TRUE(wait_commit(committed));

    // Verify all cleared
    std::set<std::string> query_keys = {"key1"};
    std::map<std::string, bufferlist> result;
    ASSERT_EQ(store.omap_get_values(coll, oid, query_keys, &result), 0);
    EXPECT_EQ(result.size(), 0u);

    bufferlist header_result;
    ASSERT_EQ(store.omap_get_header(coll, oid, &header_result), 0);
    EXPECT_EQ(header_result.length(), 0u);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(OMapTest, GetFull) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 0);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 0;
    oid.oid = "test_obj";

    BlueStoreTransaction txc;
    txc.touch(oid);
    std::vector<BlueStoreTransaction> txcs;
    txcs.push_back(std::move(txc));
    std::atomic<bool> committed{false};
    ASSERT_EQ(store.queue_transactions(coll, txcs, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    ASSERT_TRUE(wait_commit(committed));

    // Set keys and header
    std::map<std::string, bufferlist> keys;
    bufferlist val1, val2, header;
    val1.append("value1");
    val2.append("value2");
    header.append("header");
    keys["key1"] = val1;
    keys["key2"] = val2;

    BlueStoreTransaction txc2;
    txc2.omap_setkeys(oid, keys);
    txc2.omap_setheader(oid, header);
    txcs.clear();
    txcs.push_back(std::move(txc2));
    committed = false;
    ASSERT_EQ(store.queue_transactions(coll, txcs, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    ASSERT_TRUE(wait_commit(committed));

    // Get full
    bufferlist header_result;
    std::map<std::string, bufferlist> keys_result;
    ASSERT_EQ(store.omap_get(coll, oid, &header_result, &keys_result), 0);

    EXPECT_EQ(header_result.to_str(), "header");
    EXPECT_EQ(keys_result.size(), 2u);
    EXPECT_EQ(keys_result["key1"].to_str(), "value1");
    EXPECT_EQ(keys_result["key2"].to_str(), "value2");

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(OMapTest, NonExistentObject) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 0);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 0;
    oid.oid = "nonexistent";

    std::set<std::string> query_keys = {"key1"};
    std::map<std::string, bufferlist> result;
    EXPECT_EQ(store.omap_get_values(coll, oid, query_keys, &result), -ENOENT);

    bufferlist header;
    EXPECT_EQ(store.omap_get_header(coll, oid, &header), -ENOENT);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(OMapTest, EmptyOMap) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 0);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 0;
    oid.oid = "test_obj";

    BlueStoreTransaction txc;
    txc.touch(oid);
    std::vector<BlueStoreTransaction> txcs;
    txcs.push_back(std::move(txc));
    std::atomic<bool> committed{false};
    ASSERT_EQ(store.queue_transactions(coll, txcs, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    ASSERT_TRUE(wait_commit(committed));

    // Get from empty omap
    std::set<std::string> query_keys = {"key1"};
    std::map<std::string, bufferlist> result;
    ASSERT_EQ(store.omap_get_values(coll, oid, query_keys, &result), 0);
    EXPECT_EQ(result.size(), 0u);

    bufferlist header;
    ASSERT_EQ(store.omap_get_header(coll, oid, &header), 0);
    EXPECT_EQ(header.length(), 0u);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(OMapTest, CheckKeys) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 0);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 0;
    oid.oid = "test_obj";

    BlueStoreTransaction txc;
    txc.touch(oid);
    std::vector<BlueStoreTransaction> txcs;
    txcs.push_back(std::move(txc));
    std::atomic<bool> committed{false};
    ASSERT_EQ(store.queue_transactions(coll, txcs, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    ASSERT_TRUE(wait_commit(committed));

    // Set some keys
    std::map<std::string, bufferlist> keys;
    bufferlist val1, val2;
    val1.append("value1");
    val2.append("value2");
    keys["key1"] = val1;
    keys["key2"] = val2;

    BlueStoreTransaction txc2;
    txc2.omap_setkeys(oid, keys);
    txcs.clear();
    txcs.push_back(std::move(txc2));
    committed = false;
    ASSERT_EQ(store.queue_transactions(coll, txcs, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    ASSERT_TRUE(wait_commit(committed));

    // Check keys - some exist, some don't
    std::set<std::string> check_keys = {"key1", "key2", "key3"};
    std::set<std::string> result;
    ASSERT_EQ(store.omap_check_keys(coll, oid, check_keys, &result), 0);
    EXPECT_EQ(result.size(), 2u);
    EXPECT_TRUE(result.count("key1") > 0);
    EXPECT_TRUE(result.count("key2") > 0);
    EXPECT_EQ(result.count("key3"), 0u);

    // Check keys on nonexistent object
    ghobject_t oid2;
    oid2.pool = 1;
    oid2.hash = 0;
    oid2.oid = "nonexistent";
    result.clear();
    EXPECT_EQ(store.omap_check_keys(coll, oid2, check_keys, &result), -ENOENT);

    // Check keys on object with no omap
    ghobject_t oid3;
    oid3.pool = 1;
    oid3.hash = 0;
    oid3.oid = "no_omap_obj";

    BlueStoreTransaction txc3;
    txc3.touch(oid3);
    txcs.clear();
    txcs.push_back(std::move(txc3));
    committed = false;
    ASSERT_EQ(store.queue_transactions(coll, txcs, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    ASSERT_TRUE(wait_commit(committed));

    result.clear();
    ASSERT_EQ(store.omap_check_keys(coll, oid3, check_keys, &result), 0);
    EXPECT_EQ(result.size(), 0u);

    ASSERT_EQ(store.umount(), 0);
}
