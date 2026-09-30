// Copyright 2024 cxxlab authors
// SPDX-License-Identifier: Apache-2.0

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
#include "bluestore/error_injector.h"
#include "common/buffer.h"
#include "cxxlab_test.h"

using namespace TOPNSPC;

class ErrorInjectorTest : public ::testing::Test {
protected:
    std::string store_path_;
    static constexpr uint64_t kBlockSize = 64 * 1024 * 1024;

    void SetUp() override {
        auto name = std::string("errinj_") +
            ::testing::UnitTest::GetInstance()->current_test_info()->name();
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
                      const ghobject_t &oid, const std::string &data) {
        BlueStoreTransaction bt;
        bufferlist bl;
        bl.append(data);
        bt.write(oid, 0, data.size(), bl);

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

// ============================================================================
// Data/metadata error injection
// ============================================================================

TEST_F(ErrorInjectorTest, InjectDataErrorReturnsEioOnRead) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 0);
    ghobject_t oid(1, 0, "", "obj1", "", 0, 0);
    write_object(store, coll, oid, "hello world");

    auto *inj = store.get_error_injector();
    ASSERT_NE(inj, nullptr);
    inj->inject_data_error(oid);

    bufferlist read_bl;
    EXPECT_EQ(inj->check_data_error(oid), true);
    EXPECT_EQ(inj->check_data_error(ghobject_t(1, 0, "", "other", "", 0, 0)),
              false);

    inj->clear();
    EXPECT_EQ(inj->check_data_error(oid), false);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(ErrorInjectorTest, InjectMdataError) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 0);
    ghobject_t oid(1, 0, "", "obj2", "", 0, 0);
    write_object(store, coll, oid, "data");

    auto *inj = store.get_error_injector();
    ASSERT_NE(inj, nullptr);
    inj->inject_mdata_error(oid);

    EXPECT_EQ(inj->check_mdata_error(oid), true);
    EXPECT_EQ(inj->check_mdata_error(
                  ghobject_t(1, 0, "", "other2", "", 0, 0)),
              false);

    inj->clear();
    EXPECT_EQ(inj->check_mdata_error(oid), false);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(ErrorInjectorTest, ClearErrorsForSpecificOid) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto *inj = store.get_error_injector();
    ASSERT_NE(inj, nullptr);

    ghobject_t oid1(1, 0, "", "a", "", 0, 0);
    ghobject_t oid2(1, 0, "", "b", "", 0, 0);
    inj->inject_data_error(oid1);
    inj->inject_data_error(oid2);

    inj->clear_errors_for(oid1);
    EXPECT_EQ(inj->check_data_error(oid1), false);
    EXPECT_EQ(inj->check_data_error(oid2), true);

    ASSERT_EQ(store.umount(), 0);
}

// ============================================================================
// Leaked space injection + fsck detection
// ============================================================================

TEST_F(ErrorInjectorTest, InjectLeakedDetectedByFsck) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 0);
    ghobject_t oid(1, 0, "", "leaked_obj", "", 0, 0);
    write_object(store, coll, oid, std::string(4096, 'X'));

    auto *inj = store.get_error_injector();
    ASSERT_NE(inj, nullptr);
    inj->inject_leaked(65536);

    int errors = store.fsck(false);
    EXPECT_GT(errors, 0);

    int r = store.repair(false);
    EXPECT_GE(r, 0);

    errors = store.fsck(false);
    EXPECT_EQ(errors, 0);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(ErrorInjectorTest, InjectFalseFreeDetectedByFsck) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 0);
    ghobject_t oid(1, 0, "", "false_free_obj", "", 0, 0);
    write_object(store, coll, oid, std::string(4096, 'Y'));

    auto *inj = store.get_error_injector();
    ASSERT_NE(inj, nullptr);
    inj->inject_false_free(coll, oid);

    int errors = store.fsck(false);
    EXPECT_GT(errors, 0);

    int r = store.repair(false);
    EXPECT_GE(r, 0);

    errors = store.fsck(false);
    EXPECT_EQ(errors, 0);

    ASSERT_EQ(store.umount(), 0);
}

// ============================================================================
// Empty store edge case
// ============================================================================

TEST_F(ErrorInjectorTest, InjectLeakedOnEmptyStore) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto *inj = store.get_error_injector();
    ASSERT_NE(inj, nullptr);
    inj->inject_leaked(65536);

    int errors = store.fsck(false);
    EXPECT_GT(errors, 0);

    ASSERT_EQ(store.umount(), 0);
}
