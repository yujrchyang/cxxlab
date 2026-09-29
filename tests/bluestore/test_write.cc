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
#include "bluestore/trans_context.h"
#include "cxxlab_test.h"

using namespace TOPNSPC;

class WritePathTest : public ::testing::Test {
protected:
    std::string store_path_;
    static constexpr uint64_t kBlockSize = 64 * 1024 * 1024;

    void SetUp() override {
        auto name = std::string("write_") +
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
        cfg.max_blob_size = 65536;
        cfg.allocator_type = "bitmap";
        return cfg;
    }

    bool wait_commit(std::atomic<bool> &flag, int timeout_ms = 2000) {
        for (int i = 0; i < timeout_ms / 10 && !flag; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return flag.load();
    }
};

TEST_F(WritePathTest, WriteSmallData) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 1;
    oid.oid = "write_obj";

    bufferlist data;
    std::string payload(4096, 'A');
    data.append(payload);

    BlueStoreTransaction bt;
    bt.write(oid, 0, 4096, data);

    std::vector<BlueStoreTransaction> tls;
    tls.push_back(std::move(bt));

    std::atomic<bool> committed{false};
    int r = store.queue_transactions(coll, tls, [&committed]() {
        committed = true;
    });
    EXPECT_EQ(r, 0);

    coll->get_osr()->flush();
    EXPECT_TRUE(wait_commit(committed));

    auto on = coll->get_onode(oid, false);
    ASSERT_NE(on, nullptr);
    EXPECT_EQ(on->onode.size, 4096u);
    EXPECT_EQ(on->extent_map.size(), 1u);

    auto it = on->extent_map.begin();
    ASSERT_NE(it, on->extent_map.end());
    EXPECT_EQ(it->logical_offset, 0u);
    EXPECT_EQ(it->blob_offset, 0u);
    EXPECT_EQ(it->length, 4096u);
    EXPECT_NE(it->blob, nullptr);
    EXPECT_FALSE(it->blob->get_blob().get_extents().empty());

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(WritePathTest, WriteAtOffset) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 2;
    oid.oid = "offset_obj";

    bufferlist data;
    std::string payload(4096, 'B');
    data.append(payload);

    BlueStoreTransaction bt;
    bt.write(oid, 8192, 4096, data);

    std::vector<BlueStoreTransaction> tls;
    tls.push_back(std::move(bt));

    std::atomic<bool> committed{false};
    EXPECT_EQ(store.queue_transactions(coll, tls, [&committed]() {
        committed = true;
    }),
              0);

    coll->get_osr()->flush();
    EXPECT_TRUE(wait_commit(committed));

    auto on = coll->get_onode(oid, false);
    ASSERT_NE(on, nullptr);
    EXPECT_EQ(on->onode.size, 12288u);

    auto it = on->extent_map.begin();
    ASSERT_NE(it, on->extent_map.end());
    EXPECT_EQ(it->logical_offset, 8192u);
    EXPECT_EQ(it->length, 4096u);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(WritePathTest, SequentialAppendBlobReuse) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 3;
    oid.oid = "append_obj";

    for (int i = 0; i < 3; ++i) {
        bufferlist data;
        std::string payload(4096, 'C' + i);
        data.append(payload);

        BlueStoreTransaction bt;
        bt.write(oid, i * 4096, 4096, data);

        std::vector<BlueStoreTransaction> tls;
        tls.push_back(std::move(bt));

        std::atomic<bool> committed{false};
        EXPECT_EQ(store.queue_transactions(coll, tls, [&committed]() {
            committed = true;
        }),
                  0);
        coll->get_osr()->flush();
        EXPECT_TRUE(wait_commit(committed));
    }

    auto on = coll->get_onode(oid, false);
    ASSERT_NE(on, nullptr);
    EXPECT_EQ(on->onode.size, 12288u);
    EXPECT_GE(on->extent_map.size(), 1u);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(WritePathTest, OverwriteExtent) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 4;
    oid.oid = "overwrite_obj";

    {
        bufferlist data;
        std::string payload(4096, 'X');
        data.append(payload);

        BlueStoreTransaction bt;
        bt.write(oid, 0, 4096, data);

        std::vector<BlueStoreTransaction> tls;
        tls.push_back(std::move(bt));

        std::atomic<bool> committed{false};
        EXPECT_EQ(store.queue_transactions(coll, tls, [&committed]() {
            committed = true;
        }),
                  0);
        coll->get_osr()->flush();
        EXPECT_TRUE(wait_commit(committed));
    }

    {
        bufferlist data;
        std::string payload(4096, 'Y');
        data.append(payload);

        BlueStoreTransaction bt;
        bt.write(oid, 0, 4096, data);

        std::vector<BlueStoreTransaction> tls;
        tls.push_back(std::move(bt));

        std::atomic<bool> committed{false};
        EXPECT_EQ(store.queue_transactions(coll, tls, [&committed]() {
            committed = true;
        }),
                  0);
        coll->get_osr()->flush();
        EXPECT_TRUE(wait_commit(committed));
    }

    auto on = coll->get_onode(oid, false);
    ASSERT_NE(on, nullptr);
    EXPECT_EQ(on->onode.size, 4096u);
    EXPECT_EQ(on->extent_map.size(), 1u);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(WritePathTest, ZeroWriteNoExtent) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 5;
    oid.oid = "zero_obj";

    bufferlist data;
    std::string payload(4096, '\0');
    data.append(payload);

    BlueStoreTransaction bt;
    bt.write(oid, 0, 4096, data);

    std::vector<BlueStoreTransaction> tls;
    tls.push_back(std::move(bt));

    std::atomic<bool> committed{false};
    EXPECT_EQ(store.queue_transactions(coll, tls, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    EXPECT_TRUE(wait_commit(committed));

    auto on = coll->get_onode(oid, false);
    ASSERT_NE(on, nullptr);
    EXPECT_EQ(on->extent_map.size(), 0u);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(WritePathTest, WritePersistence) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 6;
    oid.oid = "persist_write";

    bufferlist data;
    std::string payload(4096, 'P');
    data.append(payload);

    BlueStoreTransaction bt;
    bt.write(oid, 0, 4096, data);

    std::vector<BlueStoreTransaction> tls;
    tls.push_back(std::move(bt));

    std::atomic<bool> committed{false};
    EXPECT_EQ(store.queue_transactions(coll, tls, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    EXPECT_TRUE(wait_commit(committed));

    ASSERT_EQ(store.umount(), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll2 = store.get_collection(1);
    ASSERT_NE(coll2, nullptr);

    auto on = coll2->get_onode(oid, false);
    ASSERT_NE(on, nullptr);
    EXPECT_EQ(on->onode.size, 4096u);
    EXPECT_EQ(on->extent_map.size(), 1u);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(WritePathTest, MultipleWritesToOneObject) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 7;
    oid.oid = "multi_write";

    for (int i = 0; i < 5; ++i) {
        bufferlist data;
        std::string payload(4096, 'M' + i);
        data.append(payload);

        BlueStoreTransaction bt;
        bt.write(oid, i * 4096, 4096, data);

        std::vector<BlueStoreTransaction> tls;
        tls.push_back(std::move(bt));

        std::atomic<bool> committed{false};
        EXPECT_EQ(store.queue_transactions(coll, tls, [&committed]() {
            committed = true;
        }),
                  0);
        coll->get_osr()->flush();
        EXPECT_TRUE(wait_commit(committed));
    }

    auto on = coll->get_onode(oid, false);
    ASSERT_NE(on, nullptr);
    EXPECT_EQ(on->onode.size, 20480u);
    EXPECT_GE(on->extent_map.size(), 1u);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(WritePathTest, WriteBeyondMinAllocSize) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 8;
    oid.oid = "big_write";

    bufferlist data;
    std::string payload(131072, 'Z');
    data.append(payload);

    BlueStoreTransaction bt;
    bt.write(oid, 0, 131072, data);

    std::vector<BlueStoreTransaction> tls;
    tls.push_back(std::move(bt));

    std::atomic<bool> committed{false};
    EXPECT_EQ(store.queue_transactions(coll, tls, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    EXPECT_TRUE(wait_commit(committed));

    auto on = coll->get_onode(oid, false);
    ASSERT_NE(on, nullptr);
    EXPECT_EQ(on->onode.size, 131072u);
    EXPECT_GE(on->extent_map.size(), 1u);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(WritePathTest, TouchAndWrite) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 9;
    oid.oid = "touch_write";

    {
        BlueStoreTransaction bt;
        bt.touch(oid);
        std::vector<BlueStoreTransaction> tls;
        tls.push_back(std::move(bt));
        std::atomic<bool> committed{false};
        EXPECT_EQ(store.queue_transactions(coll, tls, [&committed]() {
            committed = true;
        }),
                  0);
        coll->get_osr()->flush();
        EXPECT_TRUE(wait_commit(committed));
    }

    {
        bufferlist data;
        std::string payload(4096, 'T');
        data.append(payload);

        BlueStoreTransaction bt;
        bt.write(oid, 0, 4096, data);
        std::vector<BlueStoreTransaction> tls;
        tls.push_back(std::move(bt));
        std::atomic<bool> committed{false};
        EXPECT_EQ(store.queue_transactions(coll, tls, [&committed]() {
            committed = true;
        }),
                  0);
        coll->get_osr()->flush();
        EXPECT_TRUE(wait_commit(committed));
    }

    auto on = coll->get_onode(oid, false);
    ASSERT_NE(on, nullptr);
    EXPECT_EQ(on->onode.size, 4096u);
    EXPECT_EQ(on->extent_map.size(), 1u);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(WritePathTest, ChecksumComputed) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 10;
    oid.oid = "csum_obj";

    bufferlist data;
    std::string payload(4096, 'K');
    data.append(payload);

    BlueStoreTransaction bt;
    bt.write(oid, 0, 4096, data);

    std::vector<BlueStoreTransaction> tls;
    tls.push_back(std::move(bt));

    std::atomic<bool> committed{false};
    EXPECT_EQ(store.queue_transactions(coll, tls, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    EXPECT_TRUE(wait_commit(committed));

    auto on = coll->get_onode(oid, false);
    ASSERT_NE(on, nullptr);
    auto it = on->extent_map.begin();
    ASSERT_NE(it, on->extent_map.end());
    EXPECT_TRUE(it->blob->get_blob().has_csum());
    EXPECT_EQ(it->blob->get_blob().csum_type, CSUM_CRC32C);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(WritePathTest, ReverseSearchBlobReuse) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 12;
    oid.oid = "reverse_reuse";

    auto write_and_commit = [&](uint64_t off, char ch) {
        bufferlist data;
        std::string payload(4096, ch);
        data.append(payload);

        BlueStoreTransaction bt;
        bt.write(oid, off, 4096, data);
        std::vector<BlueStoreTransaction> tls;
        tls.push_back(std::move(bt));

        std::atomic<bool> committed{false};
        EXPECT_EQ(store.queue_transactions(coll, tls, [&committed]() {
            committed = true;
        }),
                  0);
        coll->get_osr()->flush();
        EXPECT_TRUE(wait_commit(committed));
    };

    write_and_commit(0, 'R');
    write_and_commit(4096, 'S');
    write_and_commit(8192, 'T');

    auto on = coll->get_onode(oid, false);
    ASSERT_NE(on, nullptr);
    EXPECT_EQ(on->onode.size, 12288u);
    EXPECT_EQ(on->extent_map.size(), 3u);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(WritePathTest, DrainAfterWrites) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 11;
    oid.oid = "drain_write";

    for (int i = 0; i < 5; ++i) {
        bufferlist data;
        std::string payload(4096, 'D');
        data.append(payload);

        BlueStoreTransaction bt;
        bt.write(oid, i * 4096, 4096, data);

        std::vector<BlueStoreTransaction> tls;
        tls.push_back(std::move(bt));
        EXPECT_EQ(store.queue_transactions(coll, tls), 0);
    }

    coll->get_osr()->drain();
    EXPECT_TRUE(coll->get_osr()->empty());

    auto on = coll->get_onode(oid, false);
    ASSERT_NE(on, nullptr);
    EXPECT_EQ(on->onode.size, 20480u);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(WritePathTest, WriteAtOffsetDataIntegrity) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 13;
    oid.oid = "offset_data";

    std::string write_data(4096, 'X');
    bufferlist bl;
    bl.append(write_data);

    BlueStoreTransaction bt;
    bt.write(oid, 8192, 4096, bl);

    std::vector<BlueStoreTransaction> tls;
    tls.push_back(std::move(bt));

    std::atomic<bool> committed{false};
    EXPECT_EQ(store.queue_transactions(coll, tls, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    EXPECT_TRUE(wait_commit(committed));

    auto on = coll->get_onode(oid, false);
    ASSERT_NE(on, nullptr);
    ASSERT_EQ(on->extent_map.size(), 1u);

    auto it = on->extent_map.begin();
    ASSERT_NE(it, on->extent_map.end());
    EXPECT_EQ(it->logical_offset, 8192u);
    EXPECT_EQ(it->blob_offset, 8192u);
    EXPECT_EQ(it->length, 4096u);

    auto &blob_extents = it->blob->get_blob().get_extents();
    ASSERT_EQ(blob_extents.size(), 1u);
    EXPECT_EQ(blob_extents[0].length, 65536u);

    uint64_t phys_off = blob_extents[0].offset + it->blob_offset;

    int fd = ::open(cfg.bdev_path.c_str(), O_RDONLY | O_DIRECT);
    ASSERT_GE(fd, 0);

    void *buf = nullptr;
    ASSERT_EQ(::posix_memalign(&buf, 4096, 4096), 0);

    ssize_t nr = ::pread(fd, buf, 4096, phys_off);
    EXPECT_EQ(nr, 4096);

    std::string read_back(static_cast<char *>(buf), 4096);
    EXPECT_EQ(read_back, write_data);

    ::free(buf);
    ::close(fd);

    ASSERT_EQ(store.umount(), 0);
}
