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

class ReadPathTest : public ::testing::Test {
protected:
    std::string store_path_;
    static constexpr uint64_t kBlockSize = 64 * 1024 * 1024;

    void SetUp() override {
        auto name = std::string("read_") +
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

    bool wait_commit(std::atomic<bool> &flag, int timeout_ms = 2000) {
        for (int i = 0; i < timeout_ms / 10 && !flag; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return flag.load();
    }
};

TEST_F(ReadPathTest, ReadBasic) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 1;
    oid.oid = "read_basic";

    std::string write_data_str = "Hello, BlueStore Read Path!";
    bufferlist write_data;
    write_data.append(write_data_str);

    BlueStoreTransaction bt;
    bt.write(oid, 0, write_data.length(), write_data);

    std::vector<BlueStoreTransaction> tls;
    tls.push_back(std::move(bt));

    std::atomic<bool> committed{false};
    EXPECT_EQ(store.queue_transactions(coll, tls, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    EXPECT_TRUE(wait_commit(committed));

    bufferlist read_bl;
    int r = store.read(coll, oid, 0, write_data_str.length(), read_bl);
    EXPECT_EQ(r, (int)write_data_str.length());
    EXPECT_EQ(read_bl.to_str(), write_data_str);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(ReadPathTest, ReadPartial) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 2;
    oid.oid = "read_partial";

    std::string write_data_str = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    bufferlist write_data;
    write_data.append(write_data_str);

    BlueStoreTransaction bt;
    bt.write(oid, 0, write_data.length(), write_data);

    std::vector<BlueStoreTransaction> tls;
    tls.push_back(std::move(bt));

    std::atomic<bool> committed{false};
    EXPECT_EQ(store.queue_transactions(coll, tls, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    EXPECT_TRUE(wait_commit(committed));

    bufferlist read_bl;
    int r = store.read(coll, oid, 10, 5, read_bl);
    EXPECT_EQ(r, 5);
    EXPECT_EQ(read_bl.to_str(), "KLMNO");

    bufferlist read_bl2;
    r = store.read(coll, oid, 20, 6, read_bl2);
    EXPECT_EQ(r, 6);
    EXPECT_EQ(read_bl2.to_str(), "UVWXYZ");

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(ReadPathTest, ReadUnaligned) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 3;
    oid.oid = "read_unaligned";

    std::string write_data_str(10000, 'X');
    bufferlist write_data;
    write_data.append(write_data_str);

    BlueStoreTransaction bt;
    bt.write(oid, 0, write_data.length(), write_data);

    std::vector<BlueStoreTransaction> tls;
    tls.push_back(std::move(bt));

    std::atomic<bool> committed{false};
    EXPECT_EQ(store.queue_transactions(coll, tls, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    EXPECT_TRUE(wait_commit(committed));

    bufferlist read_bl;
    int r = store.read(coll, oid, 123, 4567, read_bl);
    EXPECT_EQ(r, 4567);
    EXPECT_EQ(read_bl.length(), 4567u);
    EXPECT_EQ(read_bl.to_str(), std::string(4567, 'X'));

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(ReadPathTest, ReadMultipleExtents) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 4;
    oid.oid = "read_multi_extent";

    std::string data1(2048, 'A');
    std::string data2(2048, 'B');

    {
        bufferlist bl;
        bl.append(data1);
        BlueStoreTransaction bt;
        bt.write(oid, 0, bl.length(), bl);
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
        bufferlist bl;
        bl.append(data2);
        BlueStoreTransaction bt;
        bt.write(oid, 2048, bl.length(), bl);
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

    bufferlist read_bl;
    int r = store.read(coll, oid, 0, 4096, read_bl);
    EXPECT_EQ(r, 4096);
    std::string result = read_bl.to_str();
    EXPECT_EQ(result.substr(0, 2048), data1);
    EXPECT_EQ(result.substr(2048, 2048), data2);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(ReadPathTest, ReadBeyondSize) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 5;
    oid.oid = "read_beyond";

    std::string write_data_str = "Short data";
    bufferlist write_data;
    write_data.append(write_data_str);

    BlueStoreTransaction bt;
    bt.write(oid, 0, write_data.length(), write_data);

    std::vector<BlueStoreTransaction> tls;
    tls.push_back(std::move(bt));

    std::atomic<bool> committed{false};
    EXPECT_EQ(store.queue_transactions(coll, tls, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    EXPECT_TRUE(wait_commit(committed));

    bufferlist read_bl;
    int r = store.read(coll, oid, 0, 1000, read_bl);
    EXPECT_EQ(r, (int)write_data_str.length());
    EXPECT_EQ(read_bl.to_str(), write_data_str);

    bufferlist read_bl2;
    r = store.read(coll, oid, 1000, 100, read_bl2);
    EXPECT_EQ(r, 0);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(ReadPathTest, ReadWithChecksum) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 6;
    oid.oid = "read_csum";

    std::string write_data_str(8192, 'Y');
    bufferlist write_data;
    write_data.append(write_data_str);

    BlueStoreTransaction bt;
    bt.write(oid, 0, write_data.length(), write_data);

    std::vector<BlueStoreTransaction> tls;
    tls.push_back(std::move(bt));

    std::atomic<bool> committed{false};
    EXPECT_EQ(store.queue_transactions(coll, tls, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    EXPECT_TRUE(wait_commit(committed));

    bufferlist read_bl;
    int r = store.read(coll, oid, 0, write_data_str.length(), read_bl);
    EXPECT_EQ(r, (int)write_data_str.length());
    EXPECT_EQ(read_bl.to_str(), write_data_str);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(ReadPathTest, ReadNonExistent) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 7;
    oid.oid = "non_existent";

    bufferlist read_bl;
    int r = store.read(coll, oid, 0, 100, read_bl);
    EXPECT_EQ(r, -ENOENT);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(ReadPathTest, ReadHole) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 8;
    oid.oid = "read_hole";

    std::string write_data_str(4096, 'Z');
    bufferlist write_data;
    write_data.append(write_data_str);

    BlueStoreTransaction bt;
    bt.write(oid, 4096, write_data.length(), write_data);

    std::vector<BlueStoreTransaction> tls;
    tls.push_back(std::move(bt));

    std::atomic<bool> committed{false};
    EXPECT_EQ(store.queue_transactions(coll, tls, [&committed]() {
        committed = true;
    }),
              0);
    coll->get_osr()->flush();
    EXPECT_TRUE(wait_commit(committed));

    bufferlist read_bl;
    int r = store.read(coll, oid, 0, 4096, read_bl);
    EXPECT_EQ(r, 4096);
    EXPECT_EQ(read_bl.to_str(), std::string(4096, '\0'));

    bufferlist read_bl2;
    r = store.read(coll, oid, 2048, 4096, read_bl2);
    EXPECT_EQ(r, 4096);
    std::string result = read_bl2.to_str();
    EXPECT_EQ(result.substr(0, 2048), std::string(2048, '\0'));
    EXPECT_EQ(result.substr(2048, 2048), std::string(2048, 'Z'));

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(ReadPathTest, ReadInterleavedHoleData) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 9;
    oid.oid = "interleaved";

    {
        std::string data_a(4096, 'A');
        bufferlist bl;
        bl.append(data_a);
        BlueStoreTransaction bt;
        bt.write(oid, 0, bl.length(), bl);
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
        std::string data_b(4096, 'B');
        bufferlist bl;
        bl.append(data_b);
        BlueStoreTransaction bt;
        bt.write(oid, 69632, bl.length(), bl);
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

    bufferlist read_bl;
    int r = store.read(coll, oid, 0, 73728, read_bl);
    EXPECT_EQ(r, 73728);

    std::string result = read_bl.to_str();
    EXPECT_EQ(result.length(), 73728u);
    EXPECT_EQ(result.substr(0, 4096), std::string(4096, 'A'));
    EXPECT_EQ(result.substr(4096, 65536), std::string(65536, '\0'));
    EXPECT_EQ(result.substr(69632, 4096), std::string(4096, 'B'));

    ASSERT_EQ(store.umount(), 0);
}
