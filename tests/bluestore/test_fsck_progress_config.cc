#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bluestore/bluestore.h"
#include "bluestore/bluestore_config.h"
#include "common/buffer.h"
#include "cxxlab_test.h"

using namespace TOPNSPC;

class FsckProgressConfigTest : public ::testing::Test {
protected:
    std::string store_path_;
    static constexpr uint64_t kBlockSize = 64 * 1024 * 1024;

    void SetUp() override {
        auto name = std::string("fpc_") +
            ::testing::UnitTest::GetInstance()
                ->current_test_info()
                ->name();
        auto tmpl = cxxlab_tmp_path(name.c_str());
        store_path_ = std::string(tmpl.c_str()) + "_dir";

        std::string rm_cmd = "rm -rf " + store_path_;
        ::system(rm_cmd.c_str());

        int r = ::mkdir(store_path_.c_str(), 0755);
        ASSERT_EQ(r, 0) << "mkdir failed: " << strerror(errno);

        std::string db_path = store_path_ + "/db";
        r = ::mkdir(db_path.c_str(), 0755);
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
        cfg.allocator_type = "bitmap";
        cfg.buffer_cache_size = 64 * 1024 * 1024;
        cfg.onode_cache_size = 1000;
        return cfg;
    }

    bool wait_commit(std::atomic<bool> &committed) {
        for (int i = 0; i < 100 && !committed.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return committed.load();
    }

    void write_object(BlueStore &store, CollectionRef coll,
                      const ghobject_t &oid, uint64_t offset,
                      const std::string &data) {
        BlueStoreTransaction bt;
        bufferlist bl;
        bl.append(data);
        bt.write(oid, offset, data.size(), bl);

        std::vector<BlueStoreTransaction> txcs;
        txcs.push_back(std::move(bt));

        std::atomic<bool> committed{false};
        ASSERT_EQ(store.queue_transactions(coll, txcs, [&committed]() {
            committed = true;
        }),
                  0);
        coll->get_osr()->flush();
        ASSERT_TRUE(wait_commit(committed));
    }
};

TEST_F(FsckProgressConfigTest, FsckProgressCallback) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 0);
    ASSERT_NE(coll, nullptr);

    for (int i = 0; i < 5; i++) {
        ghobject_t oid;
        oid.pool = 1;
        oid.hash = static_cast<uint32_t>(i * 100);
        oid.oid = "obj" + std::to_string(i);
        write_object(store, coll, oid, 0, std::string(8192, 'A' + i));
    }

    ASSERT_EQ(store.umount(), 0);

    BlueStore fsck_store;
    fsck_store.set_config(cfg);

    std::vector<BlueStore::FsckProgress> progresses;
    int errors = fsck_store.fsck(false,
                                 [&progresses](const BlueStore::FsckProgress &p) {
                                     progresses.push_back(p);
                                 });
    EXPECT_EQ(errors, 0);
    EXPECT_FALSE(progresses.empty());

    bool has_collections = false;
    bool has_objects = false;
    for (const auto &p : progresses) {
        if (p.phase == "collections") has_collections = true;
        if (p.phase == "objects") has_objects = true;
    }
    EXPECT_TRUE(has_collections);
    EXPECT_TRUE(has_objects);

    uint64_t last_processed = 0;
    uint64_t last_total = 0;
    for (const auto &p : progresses) {
        if (p.phase == "objects") {
            last_processed = p.processed;
            last_total = p.total;
        }
    }
    EXPECT_GT(last_total, 0u);
    EXPECT_EQ(last_processed, last_total);
}

TEST_F(FsckProgressConfigTest, ReloadBufferCacheSize) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    BufferCache *cache = store.get_buffer_cache();
    ASSERT_NE(cache, nullptr);
    EXPECT_EQ(cache->get_max_bytes(), cfg.buffer_cache_size);

    auto cfg2 = cfg;
    cfg2.buffer_cache_size = 128 * 1024 * 1024;
    store.reload_config(cfg2);
    EXPECT_EQ(cache->get_max_bytes(), cfg2.buffer_cache_size);

    auto cfg3 = cfg;
    cfg3.buffer_cache_size = 16 * 1024 * 1024;
    store.reload_config(cfg3);
    EXPECT_EQ(cache->get_max_bytes(), cfg3.buffer_cache_size);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(FsckProgressConfigTest, ReloadOnodeCacheSize) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 0);
    ASSERT_NE(coll, nullptr);
    EXPECT_EQ(coll->get_onode_cache_max(), cfg.onode_cache_size);

    auto cfg2 = cfg;
    cfg2.onode_cache_size = 2000;
    store.reload_config(cfg2);
    EXPECT_EQ(coll->get_onode_cache_max(), cfg2.onode_cache_size);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(FsckProgressConfigTest, ReloadInjectRate) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto cfg2 = cfg;
    cfg2.inject_read_err_rate = 0.5;
    cfg2.inject_write_err_rate = 0.3;
    cfg2.inject_kv_err_rate = 0.1;
    store.reload_config(cfg2);

    auto cfg3 = cfg;
    store.reload_config(cfg3);

    ASSERT_EQ(store.umount(), 0);
}
