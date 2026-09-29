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

class TransContextTest : public ::testing::Test {
protected:
    std::string store_path_;
    static constexpr uint64_t kBlockSize = 64 * 1024 * 1024;

    void SetUp() override {
        auto name = std::string("txc_") + ::testing::UnitTest::GetInstance()->current_test_info()->name();
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

TEST_F(TransContextTest, OpSequencerBasic) {
    OpSequencer osr;
    EXPECT_TRUE(osr.empty());
    EXPECT_EQ(osr.size(), 0u);

    Collection coll(nullptr, 1);
    TransContext txc1(&coll, &osr);
    osr.queue_new(&txc1);
    EXPECT_EQ(txc1.seq, 1u);
    EXPECT_FALSE(osr.empty());
    EXPECT_EQ(osr.size(), 1u);
}

TEST_F(TransContextTest, TransContextStates) {
    Collection coll(nullptr, 1);
    OpSequencer osr;
    TransContext txc(&coll, &osr);

    EXPECT_EQ(txc.get_state(), TransContext::STATE_PREPARE);
    EXPECT_STREQ(TransContext::state_name(TransContext::STATE_PREPARE), "PREPARE");
    EXPECT_STREQ(TransContext::state_name(TransContext::STATE_DONE), "DONE");

    txc.set_state(TransContext::STATE_AIO_WAIT);
    EXPECT_EQ(txc.get_state(), TransContext::STATE_AIO_WAIT);

    txc.set_state(TransContext::STATE_KV_DONE);
    EXPECT_EQ(txc.get_state(), TransContext::STATE_KV_DONE);
}

TEST_F(TransContextTest, OpSequencerDrain) {
    OpSequencer osr;
    EXPECT_TRUE(osr.empty());

    Collection coll(nullptr, 1);
    TransContext txc1(&coll, &osr);
    TransContext txc2(&coll, &osr);
    osr.queue_new(&txc1);
    osr.queue_new(&txc2);

    EXPECT_EQ(txc1.seq, 1u);
    EXPECT_EQ(txc2.seq, 2u);
    EXPECT_EQ(osr.size(), 2u);

    std::thread drain_thread([&osr]() {
        osr.drain();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    {
        std::lock_guard<std::mutex> lg(osr.qlock);
        txc1.set_state(TransContext::STATE_DONE);
        txc2.set_state(TransContext::STATE_DONE);
        osr.q.pop_front();
        osr.q.pop_front();
    }
    osr.qcond.notify_all();

    drain_thread.join();
    EXPECT_TRUE(osr.empty());
}

TEST_F(TransContextTest, EmptyTransactionCommit) {
    BlueStore store;
    auto cfg = make_config();

    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    std::atomic<bool> committed{false};

    BlueStoreTransaction bt;
    bt.nop();

    std::vector<BlueStoreTransaction> tls;
    tls.push_back(std::move(bt));

    int r = store.queue_transactions(coll, tls, [&committed]() {
        committed = true;
    });
    EXPECT_EQ(r, 0);

    coll->get_osr()->flush();

    for (int i = 0; i < 100 && !committed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(committed);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(TransContextTest, TouchCreatesOnode) {
    BlueStore store;
    auto cfg = make_config();

    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    std::atomic<bool> committed{false};

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 42;
    oid.oid = "testobj";

    BlueStoreTransaction bt;
    bt.touch(oid);

    std::vector<BlueStoreTransaction> tls;
    tls.push_back(std::move(bt));

    int r = store.queue_transactions(coll, tls, [&committed]() {
        committed = true;
    });
    EXPECT_EQ(r, 0);

    coll->get_osr()->flush();

    for (int i = 0; i < 100 && !committed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(committed);

    auto on = coll->get_onode(oid, false);
    ASSERT_NE(on, nullptr);
    EXPECT_TRUE(on->exists);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(TransContextTest, MultipleTransactionsOrdering) {
    BlueStore store;
    auto cfg = make_config();

    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    std::atomic<int> commit_order{0};
    std::vector<int> order;
    std::mutex order_lock;

    for (int i = 0; i < 5; ++i) {
        ghobject_t oid;
        oid.pool = 1;
        oid.hash = i + 1;
        oid.oid = "obj_" + std::to_string(i);

        BlueStoreTransaction bt;
        bt.touch(oid);

        std::vector<BlueStoreTransaction> tls;
        tls.push_back(std::move(bt));

        int idx = i;
        int r = store.queue_transactions(coll, tls,
                                         [&order, &order_lock, idx]() {
                                             std::lock_guard<std::mutex> lg(order_lock);
                                             order.push_back(idx);
                                         });
        EXPECT_EQ(r, 0);
    }

    coll->get_osr()->flush();

    for (int i = 0; i < 200 && (int)order.size() < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    {
        std::lock_guard<std::mutex> lg(order_lock);
        ASSERT_EQ((int)order.size(), 5);
        for (int i = 0; i < 5; ++i) {
            EXPECT_EQ(order[i], i) << "Transaction " << i << " committed out of order";
        }
    }

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(TransContextTest, SetattrsPersist) {
    BlueStore store;
    auto cfg = make_config();

    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 100;
    oid.oid = "attr_obj";

    std::atomic<bool> committed{false};

    BlueStoreTransaction bt;
    bt.touch(oid);

    std::map<std::string, bufferptr> attrs;
    attrs["key1"] = bufferptr("value1", 6);
    attrs["key2"] = bufferptr("value2", 6);
    bt.setattrs(oid, attrs);

    std::vector<BlueStoreTransaction> tls;
    tls.push_back(std::move(bt));

    int r = store.queue_transactions(coll, tls, [&committed]() {
        committed = true;
    });
    EXPECT_EQ(r, 0);

    coll->get_osr()->flush();

    for (int i = 0; i < 100 && !committed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(committed);

    auto on = coll->get_onode(oid, false);
    ASSERT_NE(on, nullptr);

    bufferptr val;
    on->get_attr("key1", &val);
    ASSERT_TRUE(val.length() > 0);
    EXPECT_EQ(std::string(val.c_str(), val.length()), "value1");

    on->get_attr("key2", &val);
    ASSERT_TRUE(val.length() > 0);
    EXPECT_EQ(std::string(val.c_str(), val.length()), "value2");

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(TransContextTest, PersistenceAcrossRemount) {
    BlueStore store;
    auto cfg = make_config();

    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    ghobject_t oid;
    oid.pool = 1;
    oid.hash = 200;
    oid.oid = "persist_obj";

    std::atomic<bool> committed{false};

    BlueStoreTransaction bt;
    bt.touch(oid);

    std::vector<BlueStoreTransaction> tls;
    tls.push_back(std::move(bt));

    int r = store.queue_transactions(coll, tls, [&committed]() {
        committed = true;
    });
    EXPECT_EQ(r, 0);

    coll->get_osr()->flush();

    for (int i = 0; i < 100 && !committed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(committed);

    ASSERT_EQ(store.umount(), 0);

    ASSERT_EQ(store.mount(cfg), 0);

    auto coll2 = store.get_collection(1);
    ASSERT_NE(coll2, nullptr);

    auto on = coll2->get_onode(oid, false);
    ASSERT_NE(on, nullptr);
    EXPECT_TRUE(on->exists);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(TransContextTest, MultipleCollections) {
    BlueStore store;
    auto cfg = make_config();

    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll1 = store.create_collection(1, 5);
    auto coll2 = store.create_collection(2, 5);
    ASSERT_NE(coll1, nullptr);
    ASSERT_NE(coll2, nullptr);

    ghobject_t oid1, oid2;
    oid1.pool = 1;
    oid1.hash = 1;
    oid1.oid = "c1_obj";
    oid2.pool = 2;
    oid2.hash = 2;
    oid2.oid = "c2_obj";

    std::atomic<int> commits{0};

    BlueStoreTransaction bt1;
    bt1.touch(oid1);
    std::vector<BlueStoreTransaction> tls1;
    tls1.push_back(std::move(bt1));

    BlueStoreTransaction bt2;
    bt2.touch(oid2);
    std::vector<BlueStoreTransaction> tls2;
    tls2.push_back(std::move(bt2));

    EXPECT_EQ(store.queue_transactions(coll1, tls1, [&commits]() { commits++; }), 0);
    EXPECT_EQ(store.queue_transactions(coll2, tls2, [&commits]() { commits++; }), 0);

    coll1->get_osr()->flush();
    coll2->get_osr()->flush();

    for (int i = 0; i < 100 && commits < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(commits, 2);

    auto on1 = coll1->get_onode(oid1, false);
    ASSERT_NE(on1, nullptr);

    auto on2 = coll2->get_onode(oid2, false);
    ASSERT_NE(on2, nullptr);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(TransContextTest, QueueWithoutMountFails) {
    BlueStore store;

    auto coll = std::make_shared<Collection>(nullptr, 1);
    BlueStoreTransaction bt;
    bt.nop();
    std::vector<BlueStoreTransaction> tls;
    tls.push_back(std::move(bt));

    int r = store.queue_transactions(coll, tls);
    EXPECT_NE(r, 0);
}

TEST_F(TransContextTest, DrainAfterTransactions) {
    BlueStore store;
    auto cfg = make_config();

    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(1, 5);
    ASSERT_NE(coll, nullptr);

    for (int i = 0; i < 10; ++i) {
        ghobject_t oid;
        oid.pool = 1;
        oid.hash = i + 1;
        oid.oid = "drain_" + std::to_string(i);

        BlueStoreTransaction bt;
        bt.touch(oid);
        std::vector<BlueStoreTransaction> tls;
        tls.push_back(std::move(bt));

        EXPECT_EQ(store.queue_transactions(coll, tls), 0);
    }

    coll->get_osr()->drain();
    EXPECT_TRUE(coll->get_osr()->empty());

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(TransContextTest, BlueStoreTransactionOps) {
    BlueStoreTransaction bt;
    EXPECT_TRUE(bt.empty());
    EXPECT_EQ(bt.num_ops(), 0u);

    bt.nop();
    EXPECT_EQ(bt.num_ops(), 1u);

    ghobject_t oid;
    oid.oid = "test";
    bt.touch(oid);
    EXPECT_EQ(bt.num_ops(), 2u);

    bt.create(oid);
    EXPECT_EQ(bt.num_ops(), 3u);

    bt.remove(oid);
    EXPECT_EQ(bt.num_ops(), 4u);

    EXPECT_FALSE(bt.empty());
}
