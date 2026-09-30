#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bluestore/bluestore.h"
#include "bluestore/bluestore_config.h"
#include "bluestore/buffer_cache.h"
#include "bluestore/trans_context.h"
#include "cxxlab_test.h"

using namespace TOPNSPC;

class BufferCacheUnitTest : public ::testing::Test {
protected:
    void SetUp() override {
        cache_ = std::make_unique<BufferCache>(4096);
    }

    std::unique_ptr<BufferCache> cache_;
    BufferSpace bs_;
};

TEST_F(BufferCacheUnitTest, WriteAndFinishWriteHitCache) {
    bufferlist bl;
    bl.append("hello world!", 12);

    bs_.write(cache_.get(), 1, 0, bl, 0);

    bufferlist res;
    EXPECT_TRUE(bs_.read(cache_.get(), 0, 12, &res));
    EXPECT_EQ(res.length(), 12u);

    bs_.finish_write(cache_.get(), 1);

    res.clear();
    EXPECT_TRUE(bs_.read(cache_.get(), 0, 12, &res));
    EXPECT_EQ(res.length(), 12u);

    std::string s(res.c_str(), res.length());
    EXPECT_EQ(s, "hello world!");
}

TEST_F(BufferCacheUnitTest, DidReadHitCache) {
    bufferlist bl;
    bl.append("cached data", 11);

    bs_.did_read(cache_.get(), 100, bl);

    bufferlist res;
    EXPECT_TRUE(bs_.read(cache_.get(), 100, 11, &res));
    EXPECT_EQ(res.length(), 11u);

    std::string s(res.c_str(), res.length());
    EXPECT_EQ(s, "cached data");
}

TEST_F(BufferCacheUnitTest, CacheMissReturnsFalse) {
    bufferlist res;
    EXPECT_FALSE(bs_.read(cache_.get(), 0, 100, &res));
    EXPECT_EQ(cache_->get_miss_bytes(), 100u);
}

TEST_F(BufferCacheUnitTest, LruEviction) {
    auto small_cache = std::make_unique<BufferCache>(100);

    bufferlist bl1;
    bl1.append(std::string(50, 'A'));
    bufferlist bl2;
    bl2.append(std::string(50, 'B'));
    bufferlist bl3;
    bl3.append(std::string(50, 'C'));

    bs_.did_read(small_cache.get(), 0, bl1);
    bs_.did_read(small_cache.get(), 100, bl2);
    EXPECT_EQ(small_cache->get_cur_bytes(), 100u);

    bs_.did_read(small_cache.get(), 200, bl3);

    EXPECT_LE(small_cache->get_cur_bytes(), 100u);

    bufferlist res;
    EXPECT_FALSE(bs_.read(small_cache.get(), 0, 50, &res));
}

TEST_F(BufferCacheUnitTest, DiscardInvalidatesCachedData) {
    bufferlist bl;
    bl.append("to be discarded", 15);

    bs_.did_read(cache_.get(), 0, bl);

    bs_.discard(cache_.get(), 0, 15);

    bufferlist res;
    EXPECT_FALSE(bs_.read(cache_.get(), 0, 15, &res));
}

TEST_F(BufferCacheUnitTest, ClearRemovesAllCachedData) {
    bufferlist bl1, bl2;
    bl1.append("data1", 5);
    bl2.append("data2", 5);

    bs_.did_read(cache_.get(), 0, bl1);
    bs_.did_read(cache_.get(), 100, bl2);
    EXPECT_EQ(cache_->get_cur_bytes(), 10u);

    bs_.clear(cache_.get());
    EXPECT_EQ(cache_->get_cur_bytes(), 0u);
    EXPECT_TRUE(bs_.is_empty());
}

TEST_F(BufferCacheUnitTest, NoCacheFlagDiscardsAfterFinishWrite) {
    bufferlist bl;
    bl.append("nocache data", 12);

    bs_.write(cache_.get(), 1, 0, bl, Buffer::FLAG_NOCACHE);
    bs_.finish_write(cache_.get(), 1);

    bufferlist res;
    EXPECT_FALSE(bs_.read(cache_.get(), 0, 12, &res));
    EXPECT_EQ(cache_->get_cur_bytes(), 0u);
}

TEST_F(BufferCacheUnitTest, PartialReadFromCachedBuffer) {
    bufferlist bl;
    bl.append("ABCDEFGHIJ", 10);

    bs_.did_read(cache_.get(), 0, bl);

    bufferlist res;
    EXPECT_TRUE(bs_.read(cache_.get(), 0, 4, &res));
    EXPECT_EQ(res.length(), 4u);

    std::string s(res.c_str(), res.length());
    EXPECT_EQ(s, "ABCD");
}

TEST_F(BufferCacheUnitTest, WritingStateVisibleToReads) {
    bufferlist bl;
    bl.append("in-flight write", 15);

    bs_.write(cache_.get(), 42, 0, bl, 0);

    bufferlist res;
    EXPECT_TRUE(bs_.read(cache_.get(), 0, 15, &res));
    EXPECT_EQ(res.length(), 15u);
}

TEST_F(BufferCacheUnitTest, StatsHitAndMiss) {
    bufferlist bl;
    bl.append("stats test", 10);

    bs_.did_read(cache_.get(), 0, bl);

    {
        bufferlist res;
        bs_.read(cache_.get(), 0, 10, &res);
    }
    EXPECT_EQ(cache_->get_hit_bytes(), 10u);

    {
        bufferlist res;
        bs_.read(cache_.get(), 999, 50, &res);
    }
    EXPECT_EQ(cache_->get_miss_bytes(), 50u);
}

TEST_F(BufferCacheUnitTest, FlushClearsCache) {
    bufferlist bl;
    bl.append("flush me", 8);

    bs_.did_read(cache_.get(), 0, bl);
    EXPECT_EQ(cache_->get_cur_bytes(), 8u);

    cache_->flush();
    EXPECT_EQ(cache_->get_cur_bytes(), 0u);
}

// Integration tests through BlueStore

class BufferCacheIntegrationTest : public ::testing::Test {
protected:
    std::string store_path_;
    static constexpr uint64_t kBlockSize = 64 * 1024 * 1024;

    void SetUp() override {
        auto name = std::string("bcache_") +
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
        cfg.buffer_cache_size = 4 * 1024 * 1024;
        return cfg;
    }

    bool wait_commit(std::atomic<bool> &flag, int timeout_ms = 2000) {
        for (int i = 0; i < timeout_ms / 10 && !flag; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return flag.load();
    }
};

TEST_F(BufferCacheIntegrationTest, WriteThenReadHitsCache) {
    auto cfg = make_config();
    BlueStore store;
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(0, 0);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 0;
    oid.hash = 1;

    bufferlist wbl;
    std::string data(4096, 'X');
    wbl.append(data);

    std::atomic<bool> committed{false};
    {
        std::vector<BlueStoreTransaction> tls(1);
        tls[0].create(oid);
        tls[0].write(oid, 0, wbl.length(), wbl);
        ASSERT_EQ(store.queue_transactions(coll, tls, [&] { committed = true; }),
                  0);
    }
    ASSERT_TRUE(wait_commit(committed));

    bufferlist rbl;
    ASSERT_GT(store.read(coll, oid, 0, 4096, rbl), 0);
    EXPECT_EQ(rbl.length(), 4096u);

    rbl.clear();
    ASSERT_GT(store.read(coll, oid, 0, 4096, rbl), 0);
    EXPECT_EQ(rbl.length(), 4096u);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(BufferCacheIntegrationTest, OverwriteInvalidatesOldCache) {
    auto cfg = make_config();
    BlueStore store;
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(0, 0);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 0;
    oid.hash = 2;

    bufferlist wbl1;
    wbl1.append(std::string(4096, 'A'));

    std::atomic<bool> committed{false};
    {
        std::vector<BlueStoreTransaction> tls(1);
        tls[0].create(oid);
        tls[0].write(oid, 0, wbl1.length(), wbl1);
        ASSERT_EQ(store.queue_transactions(coll, tls, [&] { committed = true; }),
                  0);
    }
    ASSERT_TRUE(wait_commit(committed));

    bufferlist rbl;
    ASSERT_GT(store.read(coll, oid, 0, 4096, rbl), 0);

    bufferlist wbl2;
    wbl2.append(std::string(4096, 'B'));

    std::atomic<bool> committed2{false};
    {
        std::vector<BlueStoreTransaction> tls(1);
        tls[0].write(oid, 0, wbl2.length(), wbl2);
        ASSERT_EQ(store.queue_transactions(coll, tls, [&] { committed2 = true; }),
                  0);
    }
    ASSERT_TRUE(wait_commit(committed2));

    rbl.clear();
    ASSERT_GT(store.read(coll, oid, 0, 4096, rbl), 0);
    std::string result(rbl.c_str(), rbl.length());
    EXPECT_EQ(result, std::string(4096, 'B'));

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(BufferCacheIntegrationTest, ZeroInvalidatesCache) {
    auto cfg = make_config();
    BlueStore store;
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(0, 0);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 0;
    oid.hash = 3;

    bufferlist wbl;
    wbl.append(std::string(8192, 'Z'));

    std::atomic<bool> committed{false};
    {
        std::vector<BlueStoreTransaction> tls(1);
        tls[0].create(oid);
        tls[0].write(oid, 0, wbl.length(), wbl);
        ASSERT_EQ(store.queue_transactions(coll, tls, [&] { committed = true; }),
                  0);
    }
    ASSERT_TRUE(wait_commit(committed));

    bufferlist rbl;
    ASSERT_GT(store.read(coll, oid, 0, 8192, rbl), 0);

    std::atomic<bool> committed2{false};
    {
        std::vector<BlueStoreTransaction> tls(1);
        tls[0].zero(oid, 0, 4096);
        ASSERT_EQ(store.queue_transactions(coll, tls, [&] { committed2 = true; }),
                  0);
    }
    ASSERT_TRUE(wait_commit(committed2));

    rbl.clear();
    ASSERT_GT(store.read(coll, oid, 0, 4096, rbl), 0);
    EXPECT_TRUE(rbl.is_zero());

    rbl.clear();
    ASSERT_GT(store.read(coll, oid, 4096, 4096, rbl), 0);
    std::string tail(rbl.c_str(), rbl.length());
    EXPECT_EQ(tail, std::string(4096, 'Z'));

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(BufferCacheIntegrationTest, RemoveClearsCache) {
    auto cfg = make_config();
    BlueStore store;
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(0, 0);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 0;
    oid.hash = 4;

    bufferlist wbl;
    wbl.append(std::string(4096, 'R'));

    std::atomic<bool> committed{false};
    {
        std::vector<BlueStoreTransaction> tls(1);
        tls[0].create(oid);
        tls[0].write(oid, 0, wbl.length(), wbl);
        ASSERT_EQ(store.queue_transactions(coll, tls, [&] { committed = true; }),
                  0);
    }
    ASSERT_TRUE(wait_commit(committed));

    bufferlist rbl;
    ASSERT_GT(store.read(coll, oid, 0, 4096, rbl), 0);

    std::atomic<bool> committed2{false};
    {
        std::vector<BlueStoreTransaction> tls(1);
        tls[0].remove(oid);
        ASSERT_EQ(store.queue_transactions(coll, tls, [&] { committed2 = true; }),
                  0);
    }
    ASSERT_TRUE(wait_commit(committed2));

    rbl.clear();
    EXPECT_EQ(store.read(coll, oid, 0, 4096, rbl), -ENOENT);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(BufferCacheIntegrationTest, MultipleReadsHitCache) {
    auto cfg = make_config();
    BlueStore store;
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(0, 0);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 0;
    oid.hash = 5;

    bufferlist wbl;
    wbl.append(std::string(4096, 'M'));

    std::atomic<bool> committed{false};
    {
        std::vector<BlueStoreTransaction> tls(1);
        tls[0].create(oid);
        tls[0].write(oid, 0, wbl.length(), wbl);
        ASSERT_EQ(store.queue_transactions(coll, tls, [&] { committed = true; }),
                  0);
    }
    ASSERT_TRUE(wait_commit(committed));

    for (int i = 0; i < 5; ++i) {
        bufferlist rbl;
        ASSERT_GT(store.read(coll, oid, 0, 4096, rbl), 0);
        EXPECT_EQ(rbl.length(), 4096u);
        std::string s(rbl.c_str(), rbl.length());
        EXPECT_EQ(s, std::string(4096, 'M'));
    }

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(BufferCacheIntegrationTest, OnodeCacheSizeConfigurable) {
    auto cfg = make_config();
    cfg.onode_cache_size = 2;

    BlueStore store;
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(0, 0);
    ASSERT_NE(coll, nullptr);

    for (uint32_t i = 0; i < 5; ++i) {
        ghobject_t oid;
        oid.pool = 0;
        oid.hash = i + 100;

        std::atomic<bool> committed{false};
        std::vector<BlueStoreTransaction> tls(1);
        tls[0].create(oid);
        ASSERT_EQ(
            store.queue_transactions(coll, tls, [&] { committed = true; }), 0);
        ASSERT_TRUE(wait_commit(committed));
    }

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(BufferCacheIntegrationTest, ColdStartCacheEmpty) {
    auto cfg = make_config();
    BlueStore store;
    ASSERT_EQ(store.mkfs(cfg), 0);

    ASSERT_EQ(store.mount(cfg), 0);
    auto coll = store.create_collection(0, 0);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 0;
    oid.hash = 6;

    bufferlist wbl;
    wbl.append(std::string(4096, 'C'));

    std::atomic<bool> committed{false};
    {
        std::vector<BlueStoreTransaction> tls(1);
        tls[0].create(oid);
        tls[0].write(oid, 0, wbl.length(), wbl);
        ASSERT_EQ(store.queue_transactions(coll, tls, [&] { committed = true; }),
                  0);
    }
    ASSERT_TRUE(wait_commit(committed));
    ASSERT_EQ(store.umount(), 0);

    ASSERT_EQ(store.mount(cfg), 0);
    coll = store.get_collection(0);
    ASSERT_NE(coll, nullptr);

    bufferlist rbl;
    ASSERT_GT(store.read(coll, oid, 0, 4096, rbl), 0);
    EXPECT_EQ(rbl.length(), 4096u);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(BufferCacheIntegrationTest, CacheDisabledWhenSizeZero) {
    auto cfg = make_config();
    cfg.buffer_cache_size = 0;

    BlueStore store;
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(0, 0);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 0;
    oid.hash = 7;

    bufferlist wbl;
    wbl.append(std::string(4096, 'D'));

    std::atomic<bool> committed{false};
    {
        std::vector<BlueStoreTransaction> tls(1);
        tls[0].create(oid);
        tls[0].write(oid, 0, wbl.length(), wbl);
        ASSERT_EQ(store.queue_transactions(coll, tls, [&] { committed = true; }),
                  0);
    }
    ASSERT_TRUE(wait_commit(committed));

    bufferlist rbl;
    ASSERT_GT(store.read(coll, oid, 0, 4096, rbl), 0);
    EXPECT_EQ(rbl.length(), 4096u);

    rbl.clear();
    ASSERT_GT(store.read(coll, oid, 0, 4096, rbl), 0);
    EXPECT_EQ(rbl.length(), 4096u);

    ASSERT_EQ(store.umount(), 0);
}
