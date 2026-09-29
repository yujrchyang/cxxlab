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

class ZeroRemoveAttrsTest : public ::testing::Test {
protected:
    std::string store_path_;
    static constexpr uint64_t kBlockSize = 64 * 1024 * 1024;

    void SetUp() override {
        auto name = std::string("zra_") +
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

TEST_F(ZeroRemoveAttrsTest, ZeroAndRead) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 1;
    oid.oid = "zero_obj";

    std::string write_data(8192, 'D');
    for (size_t i = 0; i < write_data.size(); ++i) {
        write_data[i] = 'A' + (i % 26);
    }
    bufferlist bl;
    bl.append(write_data);

    BlueStoreTransaction bt;
    bt.write(oid, 0, bl.length(), bl);
    submit_and_wait(store, coll, bt);

    BlueStoreTransaction bt2;
    bt2.zero(oid, 2048, 4096);
    submit_and_wait(store, coll, bt2);

    bufferlist read_bl;
    int r = store.read(coll, oid, 0, 8192, read_bl);
    EXPECT_EQ(r, 8192);

    std::string result = read_bl.to_str();
    EXPECT_EQ(result.substr(0, 2048), write_data.substr(0, 2048));
    EXPECT_EQ(result.substr(2048, 4096), std::string(4096, '\0'));
    EXPECT_EQ(result.substr(6144, 2048), write_data.substr(6144, 2048));

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(ZeroRemoveAttrsTest, ZeroExtendsSize) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 2;
    oid.oid = "zero_extend";

    bufferlist bl;
    bl.append(std::string(4096, 'X'));

    BlueStoreTransaction bt;
    bt.write(oid, 0, bl.length(), bl);
    submit_and_wait(store, coll, bt);

    BlueStoreTransaction bt2;
    bt2.zero(oid, 4096, 4096);
    submit_and_wait(store, coll, bt2);

    auto on = coll->get_onode(oid, false);
    ASSERT_NE(on, nullptr);
    EXPECT_EQ(on->onode.size, 8192u);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(ZeroRemoveAttrsTest, RemoveObject) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 3;
    oid.oid = "remove_obj";

    bufferlist bl;
    bl.append(std::string(8192, 'R'));

    BlueStoreTransaction bt;
    bt.write(oid, 0, bl.length(), bl);
    submit_and_wait(store, coll, bt);

    auto on = coll->get_onode(oid, false);
    ASSERT_NE(on, nullptr);

    BlueStoreTransaction bt2;
    bt2.remove(oid);
    submit_and_wait(store, coll, bt2);

    bufferlist read_bl;
    int r = store.read(coll, oid, 0, 8192, read_bl);
    EXPECT_EQ(r, -ENOENT);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(ZeroRemoveAttrsTest, RemoveReleasesSpace) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 4;
    oid.oid = "release_space";

    bufferlist bl;
    bl.append(std::string(131072, 'S'));

    BlueStoreTransaction bt;
    bt.write(oid, 0, bl.length(), bl);
    submit_and_wait(store, coll, bt);

    BlueStoreTransaction bt2;
    bt2.remove(oid);
    submit_and_wait(store, coll, bt2);

    coll->get_osr()->drain();

    bufferlist read_bl;
    int r = store.read(coll, oid, 0, 131072, read_bl);
    EXPECT_EQ(r, -ENOENT);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(ZeroRemoveAttrsTest, SetAttrAndGetAttr) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 5;
    oid.oid = "attr_obj";

    BlueStoreTransaction bt;
    bt.touch(oid);
    submit_and_wait(store, coll, bt);

    BlueStoreTransaction bt2;
    bt2.setattr(oid, "name", bufferptr("value", 5));
    submit_and_wait(store, coll, bt2);

    bufferptr val;
    int r = store.getattr(coll, oid, "name", &val);
    EXPECT_EQ(r, 0);
    EXPECT_EQ(std::string(val.c_str(), val.length()), "value");

    r = store.getattr(coll, oid, "nonexist", &val);
    EXPECT_EQ(r, -ENODATA);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(ZeroRemoveAttrsTest, SetAttrsAndGetAttrs) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 6;
    oid.oid = "attrs_obj";

    BlueStoreTransaction bt;
    bt.touch(oid);
    submit_and_wait(store, coll, bt);

    std::map<std::string, bufferptr> attrs;
    attrs["key1"] = bufferptr("val1", 4);
    attrs["key2"] = bufferptr("val2", 4);

    BlueStoreTransaction bt2;
    bt2.setattrs(oid, attrs);
    submit_and_wait(store, coll, bt2);

    std::map<std::string, bufferptr> read_attrs;
    int r = store.getattrs(coll, oid, &read_attrs);
    EXPECT_EQ(r, 0);
    EXPECT_EQ(read_attrs.size(), 2u);
    EXPECT_EQ(std::string(read_attrs["key1"].c_str(),
                          read_attrs["key1"].length()),
              "val1");
    EXPECT_EQ(std::string(read_attrs["key2"].c_str(),
                          read_attrs["key2"].length()),
              "val2");

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(ZeroRemoveAttrsTest, AttrsPersistAcrossRemount) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 7;
    oid.oid = "persist_attr";

    BlueStoreTransaction bt;
    bt.touch(oid);
    bt.setattr(oid, "persistent", bufferptr("data", 4));
    submit_and_wait(store, coll, bt);

    ASSERT_EQ(store.umount(), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll2 = store.get_collection(1);
    ASSERT_NE(coll2, nullptr);

    bufferptr val;
    int r = store.getattr(coll2, oid, "persistent", &val);
    EXPECT_EQ(r, 0);
    EXPECT_EQ(std::string(val.c_str(), val.length()), "data");

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(ZeroRemoveAttrsTest, GetAttrNonExistentObject) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 8;
    oid.oid = "nonexist";

    bufferptr val;
    int r = store.getattr(coll, oid, "anything", &val);
    EXPECT_EQ(r, -ENOENT);

    std::map<std::string, bufferptr> attrs;
    r = store.getattrs(coll, oid, &attrs);
    EXPECT_EQ(r, -ENOENT);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(ZeroRemoveAttrsTest, ZeroThenRemove) {
    BlueStore store;
    auto cfg = make_config();
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 9;
    oid.oid = "zero_remove";

    bufferlist bl;
    bl.append(std::string(8192, 'Z'));

    BlueStoreTransaction bt;
    bt.write(oid, 0, bl.length(), bl);
    submit_and_wait(store, coll, bt);

    BlueStoreTransaction bt2;
    bt2.zero(oid, 0, 4096);
    submit_and_wait(store, coll, bt2);

    BlueStoreTransaction bt3;
    bt3.remove(oid);
    submit_and_wait(store, coll, bt3);

    bufferlist read_bl;
    int r = store.read(coll, oid, 0, 8192, read_bl);
    EXPECT_EQ(r, -ENOENT);

    ASSERT_EQ(store.umount(), 0);
}
