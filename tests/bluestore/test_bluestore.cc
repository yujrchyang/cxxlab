#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bluestore/bluestore.h"
#include "bluestore/bluestore_config.h"
#include "cxxlab_test.h"

using namespace TOPNSPC;

class BlueStoreLifecycleTest : public ::testing::Test {
protected:
    std::string store_path_;
    static constexpr uint64_t kBlockSize = 64 * 1024 * 1024;

    void SetUp() override {
        auto tmpl = cxxlab_tmp_path("bluestore_lifecycle");
        store_path_ = std::string(tmpl.c_str()) + "_dir";

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
};

TEST_F(BlueStoreLifecycleTest, MkfsBasic) {
    BlueStore store;
    auto cfg = make_config();

    int r = store.mkfs(cfg);
    EXPECT_EQ(r, 0);
}

TEST_F(BlueStoreLifecycleTest, MkfsMountUmount) {
    BlueStore store;
    auto cfg = make_config();

    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);
    EXPECT_TRUE(store.is_mounted());

    ASSERT_EQ(store.umount(), 0);
    EXPECT_FALSE(store.is_mounted());
}

TEST_F(BlueStoreLifecycleTest, MountWithoutMkfsFails) {
    BlueStore store;
    auto cfg = make_config();

    int r = store.mount(cfg);
    EXPECT_NE(r, 0);
}

TEST_F(BlueStoreLifecycleTest, UmountWithoutMountFails) {
    BlueStore store;
    auto cfg = make_config();

    int r = store.umount();
    EXPECT_NE(r, 0);
}

TEST_F(BlueStoreLifecycleTest, CreateAndMountCollection) {
    BlueStore store;
    auto cfg = make_config();

    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);
    EXPECT_EQ(coll->get_cnode().bits, 5u);

    ASSERT_EQ(store.umount(), 0);

    ASSERT_EQ(store.mount(cfg), 0);

    auto coll2 = store.get_collection(1);
    ASSERT_NE(coll2, nullptr);
    EXPECT_EQ(coll2->get_cnode().bits, 5u);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(BlueStoreLifecycleTest, MultipleCollections) {
    BlueStore store;
    auto cfg = make_config();

    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    for (uint64_t i = 1; i <= 5; ++i) {
        auto coll = store.create_collection(i, i);
        ASSERT_NE(coll, nullptr);
    }

    for (uint64_t i = 1; i <= 5; ++i) {
        auto coll = store.get_collection(i);
        ASSERT_NE(coll, nullptr);
        EXPECT_EQ(coll->get_cnode().bits, i);
    }

    ASSERT_EQ(store.umount(), 0);

    ASSERT_EQ(store.mount(cfg), 0);

    for (uint64_t i = 1; i <= 5; ++i) {
        auto coll = store.get_collection(i);
        ASSERT_NE(coll, nullptr) << "Collection " << i << " not found after remount";
        EXPECT_EQ(coll->get_cnode().bits, i);
    }

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(BlueStoreLifecycleTest, RemoveCollection) {
    BlueStore store;
    auto cfg = make_config();

    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ASSERT_EQ(store.remove_collection(1), 0);

    auto coll2 = store.get_collection(1);
    EXPECT_EQ(coll2, nullptr);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(BlueStoreLifecycleTest, RemoveNonExistentCollectionFails) {
    BlueStore store;
    auto cfg = make_config();

    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    int r = store.remove_collection(999);
    EXPECT_NE(r, 0);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(BlueStoreLifecycleTest, EmptyCollectionsOnMount) {
    BlueStore store;
    auto cfg = make_config();

    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.get_collection(1);
    EXPECT_EQ(coll, nullptr);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(BlueStoreLifecycleTest, DbIsAccessible) {
    BlueStore store;
    auto cfg = make_config();

    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto *db = store.get_db();
    ASSERT_NE(db, nullptr);

    ASSERT_EQ(store.umount(), 0);
}
