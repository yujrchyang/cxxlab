#include <gtest/gtest.h>

#include <atomic>
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
#include "common/buffer.h"
#include "cxxlab_test.h"

using namespace TOPNSPC;

class FSCKTest : public ::testing::Test {
protected:
    std::string store_path_;
    static constexpr uint64_t kBlockSize = 64 * 1024 * 1024;

    void SetUp() override {
        auto name = std::string("fsck_") +
            ::testing::UnitTest::GetInstance()->current_test_info()->name();
        auto tmpl = cxxlab_tmp_path(name.c_str());
        store_path_ = std::string(tmpl.c_str()) + "_dir";

        // Clean up any existing directory first
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
                         std::min<uint64_t>(zeros.size(), kBlockSize - off), off);
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

TEST_F(FSCKTest, BasicFSCK) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 0);
    ASSERT_NE(coll, nullptr);

    // Write some objects
    ghobject_t oid1;
    oid1.pool = 1;
    oid1.hash = 100;
    oid1.oid = "obj1";
    write_object(store, coll, oid1, 0, std::string(8192, 'A'));

    ghobject_t oid2;
    oid2.pool = 1;
    oid2.hash = 200;
    oid2.oid = "obj2";
    write_object(store, coll, oid2, 0, std::string(16384, 'B'));

    ASSERT_EQ(store.umount(), 0);

    // Run FSCK - should find no errors
    BlueStore fsck_store;
    fsck_store.set_config(cfg);
    int errors = fsck_store.fsck(false);
    EXPECT_EQ(errors, 0);
}

TEST_F(FSCKTest, DeepFSCK) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 0);
    ASSERT_NE(coll, nullptr);

    // Write objects with data
    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 300;
    oid.oid = "deep_obj";
    write_object(store, coll, oid, 0, std::string(65536, 'C'));

    ASSERT_EQ(store.umount(), 0);

    // Run deep FSCK - should verify data is readable
    BlueStore fsck_store;
    fsck_store.set_config(cfg);
    int errors = fsck_store.fsck(true);
    EXPECT_EQ(errors, 0);
}

TEST_F(FSCKTest, QuickFix) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 0);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 400;
    oid.oid = "quick_obj";
    write_object(store, coll, oid, 0, std::string(4096, 'D'));

    ASSERT_EQ(store.umount(), 0);

    // Run quick_fix - should be fast and fix common issues
    BlueStore fsck_store;
    fsck_store.set_config(cfg);
    int errors = fsck_store.quick_fix();
    EXPECT_GE(errors, 0);
}

TEST_F(FSCKTest, RepairLeakedExtent) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 0);
    ASSERT_NE(coll, nullptr);

    // Write and delete objects to potentially create leaked extents
    for (int i = 0; i < 5; i++) {
        ghobject_t oid;
        oid.pool = 1;
        oid.hash = 500 + i;
        oid.oid = "leak_obj_" + std::to_string(i);

        write_object(store, coll, oid, 0, std::string(65536, 'E'));

        BlueStoreTransaction bt;
        bt.remove(oid);
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

    ASSERT_EQ(store.umount(), 0);

    // Run repair - should fix any leaked extents
    BlueStore fsck_store;
    fsck_store.set_config(cfg);
    int errors = fsck_store.repair(true);
    EXPECT_GE(errors, 0);

    // Verify FSCK passes after repair
    fsck_store.set_config(cfg);
    int errors2 = fsck_store.fsck(true);
    EXPECT_EQ(errors2, 0);
}

TEST_F(FSCKTest, ExtentOverlap) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 0);
    ASSERT_NE(coll, nullptr);

    // Write multiple objects to same offset (should not overlap in practice)
    ghobject_t oid1;
    oid1.pool = 1;
    oid1.hash = 600;
    oid1.oid = "overlap1";
    write_object(store, coll, oid1, 0, std::string(8192, 'F'));

    ghobject_t oid2;
    oid2.pool = 1;
    oid2.hash = 700;
    oid2.oid = "overlap2";
    write_object(store, coll, oid2, 0, std::string(8192, 'G'));

    ASSERT_EQ(store.umount(), 0);

    // FSCK should not report overlap for normal writes
    BlueStore fsck_store;
    fsck_store.set_config(cfg);
    int errors = fsck_store.fsck(false);
    EXPECT_EQ(errors, 0);
}

TEST_F(FSCKTest, EmptyStore) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);

    // FSCK on empty store should pass
    store.set_config(cfg);
    int errors = store.fsck(false);
    EXPECT_EQ(errors, 0);
}

TEST_F(FSCKTest, MultipleCollections) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    // Create multiple collections
    auto coll1 = store.create_collection(1, 0);
    auto coll2 = store.create_collection(2, 0);
    ASSERT_NE(coll1, nullptr);
    ASSERT_NE(coll2, nullptr);

    // Write objects to different collections
    ghobject_t oid1;
    oid1.pool = 1;
    oid1.hash = 800;
    oid1.oid = "coll1_obj";
    write_object(store, coll1, oid1, 0, std::string(4096, 'H'));

    ghobject_t oid2;
    oid2.pool = 2;
    oid2.hash = 900;
    oid2.oid = "coll2_obj";
    write_object(store, coll2, oid2, 0, std::string(4096, 'I'));

    ASSERT_EQ(store.umount(), 0);

    // FSCK should pass
    BlueStore fsck_store;
    fsck_store.set_config(cfg);
    int errors = fsck_store.fsck(false);
    EXPECT_EQ(errors, 0);
}
