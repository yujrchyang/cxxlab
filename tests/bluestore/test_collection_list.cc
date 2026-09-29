#include <gtest/gtest.h>

#include <algorithm>
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

class CollectionListTest : public ::testing::Test {
protected:
    std::string store_path_;
    static constexpr uint64_t kBlockSize = 64 * 1024 * 1024;

    void SetUp() override {
        auto name = std::string("coll_list_") +
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

    ghobject_t make_oid(int64_t pool, uint32_t hash, const std::string &name) {
        ghobject_t oid;
        oid.pool = pool;
        oid.hash = hash;
        oid.oid = name;
        return oid;
    }

    bool wait_commit(std::atomic<bool> &flag, int timeout_ms = 2000) {
        for (int i = 0; i < timeout_ms / 10 && !flag; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return flag.load();
    }

    void submit_and_wait(BlueStore &store, CollectionRef coll,
                         BlueStoreTransaction &bt) {
        std::vector<BlueStoreTransaction> tls;
        tls.push_back(std::move(bt));

        std::atomic<bool> committed{false};
        ASSERT_EQ(store.queue_transactions(coll, tls, [&committed]() {
            committed = true;
        }),
                  0);
        coll->get_osr()->flush();
        ASSERT_TRUE(wait_commit(committed));
    }
};

TEST_F(CollectionListTest, EmptyCollection) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t start, end;
    start.pool = 1;
    start.hash = 0;
    end.pool = 1;
    end.hash = UINT32_MAX;

    std::vector<ghobject_t> ls;
    ghobject_t next;
    int r = store.collection_list(coll, start, end, 100, &ls, &next);

    EXPECT_EQ(r, 0);
    EXPECT_TRUE(ls.empty());

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(CollectionListTest, SingleObject) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    auto oid = make_oid(1, 100, "obj1");
    BlueStoreTransaction bt;
    bt.touch(oid);
    submit_and_wait(store, coll, bt);

    ghobject_t start, end;
    start.pool = 1;
    start.hash = 0;
    end.pool = 1;
    end.hash = UINT32_MAX;

    std::vector<ghobject_t> ls;
    ghobject_t next;
    int r = store.collection_list(coll, start, end, 100, &ls, &next);

    EXPECT_EQ(r, 0);
    ASSERT_EQ(ls.size(), 1u);
    EXPECT_EQ(ls[0].oid, "obj1");

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(CollectionListTest, MultipleObjects) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    // Create 5 objects with different hashes
    std::vector<std::pair<uint32_t, std::string>> objects = {
        {100, "obj_a"}, {200, "obj_b"}, {300, "obj_c"}, {400, "obj_d"}, {500, "obj_e"}};

    for (const auto &[hash, name] : objects) {
        auto oid = make_oid(1, hash, name);
        BlueStoreTransaction bt;
        bt.touch(oid);
        submit_and_wait(store, coll, bt);
    }

    ghobject_t start, end;
    start.pool = 1;
    start.hash = 0;
    end.pool = 1;
    end.hash = UINT32_MAX;

    std::vector<ghobject_t> ls;
    ghobject_t next;
    int r = store.collection_list(coll, start, end, 100, &ls, &next);

    EXPECT_EQ(r, 0);
    ASSERT_EQ(ls.size(), 5u);

    // Verify objects are sorted by ghobject_t comparison (bitwise key order)
    for (size_t i = 1; i < ls.size(); ++i) {
        EXPECT_LT(ls[i - 1], ls[i]);
    }

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(CollectionListTest, Pagination) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    // Create 10 objects
    for (int i = 0; i < 10; ++i) {
        auto oid = make_oid(1, (i + 1) * 100, "obj" + std::to_string(i));
        BlueStoreTransaction bt;
        bt.touch(oid);
        submit_and_wait(store, coll, bt);
    }

    ghobject_t start, end;
    start.pool = 1;
    start.hash = 0;
    end.pool = 1;
    end.hash = UINT32_MAX;

    std::vector<ghobject_t> all_objects;
    ghobject_t next = start;
    int page_size = 3;

    // Fetch all objects in pages
    while (true) {
        std::vector<ghobject_t> ls;
        int r = store.collection_list(coll, next, end, page_size, &ls, &next);
        EXPECT_EQ(r, 0);

        if (ls.empty()) break;

        all_objects.insert(all_objects.end(), ls.begin(), ls.end());

        if (ls.size() < (size_t)page_size) break;
    }

    EXPECT_EQ(all_objects.size(), 10u);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(CollectionListTest, MaxLimit) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    // Create 10 objects
    for (int i = 0; i < 10; ++i) {
        auto oid = make_oid(1, (i + 1) * 100, "obj" + std::to_string(i));
        BlueStoreTransaction bt;
        bt.touch(oid);
        submit_and_wait(store, coll, bt);
    }

    ghobject_t start, end;
    start.pool = 1;
    start.hash = 0;
    end.pool = 1;
    end.hash = UINT32_MAX;

    std::vector<ghobject_t> ls;
    ghobject_t next;
    int r = store.collection_list(coll, start, end, 5, &ls, &next);

    EXPECT_EQ(r, 0);
    ASSERT_EQ(ls.size(), 5u);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(CollectionListTest, RangeFiltering) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    // Create 5 objects
    std::vector<ghobject_t> created_objects;
    for (int i = 1; i <= 5; ++i) {
        auto oid = make_oid(1, i * 100, "obj" + std::to_string(i));
        BlueStoreTransaction bt;
        bt.touch(oid);
        submit_and_wait(store, coll, bt);
        created_objects.push_back(oid);
    }

    // Sort created objects by ghobject_t comparison
    std::sort(created_objects.begin(), created_objects.end());

    // Query range [second, fourth) - should get 2 objects
    ghobject_t start = created_objects[1];
    ghobject_t end = created_objects[3];

    std::vector<ghobject_t> ls;
    ghobject_t next;
    int r = store.collection_list(coll, start, end, 100, &ls, &next);

    EXPECT_EQ(r, 0);
    // Should include objects at index 1 and 2 (end is exclusive)
    EXPECT_EQ(ls.size(), 2u);

    if (ls.size() >= 2) {
        EXPECT_EQ(ls[0], created_objects[1]);
        EXPECT_EQ(ls[1], created_objects[2]);
    }

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(CollectionListTest, CollectionIsolation) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll1 = store.create_collection(1, 5);
    auto coll2 = store.create_collection(2, 5);
    ASSERT_NE(coll1, nullptr);
    ASSERT_NE(coll2, nullptr);

    // Create objects in collection 1
    for (int i = 0; i < 3; ++i) {
        auto oid = make_oid(1, (i + 1) * 100, "c1_obj" + std::to_string(i));
        BlueStoreTransaction bt;
        bt.touch(oid);
        submit_and_wait(store, coll1, bt);
    }

    // Create objects in collection 2
    for (int i = 0; i < 3; ++i) {
        auto oid = make_oid(2, (i + 1) * 100, "c2_obj" + std::to_string(i));
        BlueStoreTransaction bt;
        bt.touch(oid);
        submit_and_wait(store, coll2, bt);
    }

    // List collection 1
    ghobject_t start1, end1;
    start1.pool = 1;
    start1.hash = 0;
    end1.pool = 1;
    end1.hash = UINT32_MAX;

    std::vector<ghobject_t> ls1;
    ghobject_t next1;
    int r1 = store.collection_list(coll1, start1, end1, 100, &ls1, &next1);

    EXPECT_EQ(r1, 0);
    EXPECT_EQ(ls1.size(), 3u);
    for (const auto &oid : ls1) {
        EXPECT_EQ(oid.pool, 1);
    }

    // List collection 2
    ghobject_t start2, end2;
    start2.pool = 2;
    start2.hash = 0;
    end2.pool = 2;
    end2.hash = UINT32_MAX;

    std::vector<ghobject_t> ls2;
    ghobject_t next2;
    int r2 = store.collection_list(coll2, start2, end2, 100, &ls2, &next2);

    EXPECT_EQ(r2, 0);
    EXPECT_EQ(ls2.size(), 3u);
    for (const auto &oid : ls2) {
        EXPECT_EQ(oid.pool, 2);
    }

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(CollectionListTest, NullCollection) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    ghobject_t start, end;
    start.pool = 1;
    end.pool = 1;

    std::vector<ghobject_t> ls;
    ghobject_t next;
    int r = store.collection_list(nullptr, start, end, 100, &ls, &next);

    EXPECT_EQ(r, -ENOENT);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(CollectionListTest, ZeroMax) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    auto oid = make_oid(1, 100, "obj1");
    BlueStoreTransaction bt;
    bt.touch(oid);
    submit_and_wait(store, coll, bt);

    ghobject_t start, end;
    start.pool = 1;
    start.hash = 0;
    end.pool = 1;
    end.hash = UINT32_MAX;

    std::vector<ghobject_t> ls;
    ghobject_t next;
    int r = store.collection_list(coll, start, end, 0, &ls, &next);

    EXPECT_EQ(r, 0);
    EXPECT_TRUE(ls.empty());

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(CollectionListTest, PersistenceAcrossRemount) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    // Create objects
    for (int i = 0; i < 5; ++i) {
        auto oid = make_oid(1, (i + 1) * 100, "obj" + std::to_string(i));
        BlueStoreTransaction bt;
        bt.touch(oid);
        submit_and_wait(store, coll, bt);
    }

    ASSERT_EQ(store.umount(), 0);

    // Remount and list
    ASSERT_EQ(store.mount(cfg), 0);
    coll = store.get_collection(1);
    ASSERT_NE(coll, nullptr);

    ghobject_t start, end;
    start.pool = 1;
    start.hash = 0;
    end.pool = 1;
    end.hash = UINT32_MAX;

    std::vector<ghobject_t> ls;
    ghobject_t next;
    int r = store.collection_list(coll, start, end, 100, &ls, &next);

    EXPECT_EQ(r, 0);
    EXPECT_EQ(ls.size(), 5u);

    ASSERT_EQ(store.umount(), 0);
}
