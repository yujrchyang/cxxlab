#include "test_fixture.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace TOPNSPC;

// Full path sequence: mkfs → mount → write → read → zero → remove →
// collection_list → umount → mount → verify persistence
TEST_F(BlueStoreTestFixture, FullPathSequence) {
    auto cfg = make_config();
    BlueStore store;
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(0, 0);
    ASSERT_NE(coll, nullptr);

    std::vector<ghobject_t> oids;
    for (int i = 0; i < 10; ++i) {
        oids.push_back(make_oid(0, i + 1, "obj_" + std::to_string(i)));
    }

    for (int i = 0; i < 10; ++i) {
        std::string data(4096, 'A' + i);
        BlueStoreTransaction bt;
        bt.create(oids[i]);
        bufferlist bl;
        bl.append(data);
        bt.write(oids[i], 0, data.size(), bl);
        submit_and_wait(store, coll, bt);
    }

    for (int i = 0; i < 10; ++i) {
        read_and_verify(store, coll, oids[i], 0, std::string(4096, 'A' + i));
    }

    for (int i = 0; i < 3; ++i) {
        BlueStoreTransaction bt;
        bt.zero(oids[i], 0, 2048);
        submit_and_wait(store, coll, bt);
    }

    for (int i = 3; i < 6; ++i) {
        BlueStoreTransaction bt;
        bt.remove(oids[i]);
        submit_and_wait(store, coll, bt);
    }

    {
        std::vector<ghobject_t> ls;
        ghobject_t next;
        ghobject_t start;
        start.pool = 0;
        start.hash = 0;
        ghobject_t end;
        end.pool = 0;
        end.hash = UINT32_MAX;
        ASSERT_EQ(store.collection_list(coll, start, end, 100, &ls, &next), 0);
        ASSERT_EQ(ls.size(), 7u);
    }

    coll.reset();
    ASSERT_EQ(store.umount(), 0);
    ASSERT_EQ(store.mount(cfg), 0);
    coll = store.get_collection(0);
    ASSERT_NE(coll, nullptr);

    for (int i = 0; i < 3; ++i) {
        bufferlist bl;
        int r = store.read(coll, oids[i], 0, 4096, bl);
        ASSERT_GT(r, 0);
        ASSERT_EQ(bl.length(), 4096u);
        std::string s(bl.c_str(), bl.length());
        ASSERT_EQ(s.substr(0, 2048), std::string(2048, '\0'));
        ASSERT_EQ(s.substr(2048), std::string(2048, 'A' + i));
    }

    for (int i = 3; i < 6; ++i) {
        bufferlist bl;
        int r = store.read(coll, oids[i], 0, 4096, bl);
        ASSERT_EQ(r, -ENOENT);
    }

    for (int i = 6; i < 10; ++i) {
        read_and_verify(store, coll, oids[i], 0, std::string(4096, 'A' + i));
    }

    {
        std::vector<ghobject_t> ls;
        ghobject_t next;
        ghobject_t start;
        start.pool = 0;
        start.hash = 0;
        ghobject_t end;
        end.pool = 0;
        end.hash = UINT32_MAX;
        ASSERT_EQ(store.collection_list(coll, start, end, 100, &ls, &next), 0);
        ASSERT_EQ(ls.size(), 7u);
    }

    ASSERT_EQ(store.umount(), 0);
}

// Persistence tests

TEST_F(BlueStoreTestFixture, TrivialRemount) {
    auto cfg = make_config();
    BlueStore store;
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);
    ASSERT_EQ(store.umount(), 0);
    ASSERT_EQ(store.mount(cfg), 0);
    ASSERT_EQ(store.umount(), 0);
}

TEST_F(BlueStoreTestFixture, WriteAndRemount) {
    auto cfg = make_config();
    auto store = std::make_unique<BlueStore>();
    ASSERT_EQ(store->mkfs(cfg), 0);
    ASSERT_EQ(store->mount(cfg), 0);

    auto coll = store->create_collection(0, 0);
    ASSERT_NE(coll, nullptr);

    auto oid = make_oid(0, 1, "remount_obj");
    write_object(*store, coll, oid, 0, std::string(4096, 'X'));

    close_and_reopen(store, cfg, coll);
    read_and_verify(*store, coll, oid, 0, std::string(4096, 'X'));
    ASSERT_EQ(store->umount(), 0);
}

TEST_F(BlueStoreTestFixture, MultiObjectRemount) {
    auto cfg = make_config();
    auto store = std::make_unique<BlueStore>();
    ASSERT_EQ(store->mkfs(cfg), 0);
    ASSERT_EQ(store->mount(cfg), 0);

    auto coll = store->create_collection(0, 0);
    ASSERT_NE(coll, nullptr);

    std::vector<ghobject_t> oids;
    for (int i = 0; i < 100; ++i) {
        auto oid = make_oid(0, i + 1, "multi_" + std::to_string(i));
        oids.push_back(oid);
        std::string data(4096, 'a' + (i % 26));
        write_object(*store, coll, oid, 0, data);
    }

    close_and_reopen(store, cfg, coll);

    for (int i = 0; i < 100; ++i) {
        read_and_verify(*store, coll, oids[i], 0,
                        std::string(4096, 'a' + (i % 26)));
    }
    ASSERT_EQ(store->umount(), 0);
}

TEST_F(BlueStoreTestFixture, WriteOverwriteRemount) {
    auto cfg = make_config();
    auto store = std::make_unique<BlueStore>();
    ASSERT_EQ(store->mkfs(cfg), 0);
    ASSERT_EQ(store->mount(cfg), 0);

    auto coll = store->create_collection(0, 0);
    auto oid = make_oid(0, 1, "overwrite_obj");

    write_object(*store, coll, oid, 0, std::string(4096, 'A'));
    write_object(*store, coll, oid, 0, std::string(4096, 'B'));

    close_and_reopen(store, cfg, coll);
    read_and_verify(*store, coll, oid, 0, std::string(4096, 'B'));
    ASSERT_EQ(store->umount(), 0);
}

TEST_F(BlueStoreTestFixture, ZeroAndRemount) {
    auto cfg = make_config();
    auto store = std::make_unique<BlueStore>();
    ASSERT_EQ(store->mkfs(cfg), 0);
    ASSERT_EQ(store->mount(cfg), 0);

    auto coll = store->create_collection(0, 0);
    auto oid = make_oid(0, 1, "zero_obj");

    write_object(*store, coll, oid, 0, std::string(8192, 'Z'));

    BlueStoreTransaction bt;
    bt.zero(oid, 0, 4096);
    submit_and_wait(*store, coll, bt);

    close_and_reopen(store, cfg, coll);

    bufferlist bl;
    int r = store->read(coll, oid, 0, 8192, bl);
    ASSERT_GT(r, 0);
    ASSERT_EQ(bl.length(), 8192u);
    std::string s(bl.c_str(), bl.length());
    ASSERT_EQ(s.substr(0, 4096), std::string(4096, '\0'));
    ASSERT_EQ(s.substr(4096), std::string(4096, 'Z'));
    ASSERT_EQ(store->umount(), 0);
}

TEST_F(BlueStoreTestFixture, RemoveAndRemount) {
    auto cfg = make_config();
    auto store = std::make_unique<BlueStore>();
    ASSERT_EQ(store->mkfs(cfg), 0);
    ASSERT_EQ(store->mount(cfg), 0);

    auto coll = store->create_collection(0, 0);
    auto oid = make_oid(0, 1, "remove_obj");

    write_object(*store, coll, oid, 0, std::string(4096, 'R'));

    BlueStoreTransaction bt;
    bt.remove(oid);
    submit_and_wait(*store, coll, bt);

    close_and_reopen(store, cfg, coll);

    bufferlist bl;
    int r = store->read(coll, oid, 0, 4096, bl);
    ASSERT_EQ(r, -ENOENT);
    ASSERT_EQ(store->umount(), 0);
}

TEST_F(BlueStoreTestFixture, AttrsRemount) {
    auto cfg = make_config();
    auto store = std::make_unique<BlueStore>();
    ASSERT_EQ(store->mkfs(cfg), 0);
    ASSERT_EQ(store->mount(cfg), 0);

    auto coll = store->create_collection(0, 0);
    auto oid = make_oid(0, 1, "attr_obj");

    {
        BlueStoreTransaction bt;
        bt.create(oid);
        submit_and_wait(*store, coll, bt);
    }

    {
        BlueStoreTransaction bt;
        bufferptr val("hello", 5);
        bt.setattr(oid, "myattr", val);
        submit_and_wait(*store, coll, bt);
    }

    close_and_reopen(store, cfg, coll);

    bufferptr val;
    int r = store->getattr(coll, oid, "myattr", &val);
    ASSERT_EQ(r, 0);
    ASSERT_EQ(val.length(), 5u);
    ASSERT_EQ(std::string(val.c_str(), 5), "hello");
    ASSERT_EQ(store->umount(), 0);
}

TEST_F(BlueStoreTestFixture, OMapRemount) {
    auto cfg = make_config();
    auto store = std::make_unique<BlueStore>();
    ASSERT_EQ(store->mkfs(cfg), 0);
    ASSERT_EQ(store->mount(cfg), 0);

    auto coll = store->create_collection(0, 0);
    auto oid = make_oid(0, 1, "omap_obj");

    {
        BlueStoreTransaction bt;
        bt.create(oid);
        submit_and_wait(*store, coll, bt);
    }

    {
        BlueStoreTransaction bt;
        std::map<std::string, bufferlist> keys;
        bufferlist v1;
        v1.append("value1");
        keys["key1"] = v1;
        bufferlist v2;
        v2.append("value2");
        keys["key2"] = v2;
        bt.omap_setkeys(oid, keys);
        submit_and_wait(*store, coll, bt);
    }

    close_and_reopen(store, cfg, coll);

    bufferlist header;
    std::map<std::string, bufferlist> out;
    ASSERT_EQ(store->omap_get(coll, oid, &header, &out), 0);
    ASSERT_EQ(out.size(), 2u);
    ASSERT_EQ(std::string(out["key1"].c_str(), out["key1"].length()),
              "value1");
    ASSERT_EQ(std::string(out["key2"].c_str(), out["key2"].length()),
              "value2");
    ASSERT_EQ(store->umount(), 0);
}

// Mixed operations with shadow state

namespace {

struct ShadowObject {
    bufferlist data;
    bool exists = true;
};

class MixedWorkloadState {
public:
    MixedWorkloadState(BlueStore *store, CollectionRef coll, uint64_t seed)
        : store_(store), coll_(coll), rng_(seed) {}

    void seed_objects(int count) {
        for (int i = 0; i < count; ++i) {
            ghobject_t oid;
            oid.pool = 0;
            oid.hash = i + 1;
            oid.oid = "mixed_" + std::to_string(i);

            std::string data(65536, 'A' + (i % 26));
            write_object_internal(oid, 0, data);

            available_.push_back(oid);
            shadow_[oid] = ShadowObject{};
            shadow_[oid].data.append(data);
        }
    }

    void run(int num_ops) {
        for (int i = 0; i < num_ops; ++i) {
            if (available_.empty()) break;
            int op = dist_(rng_) % 100;
            if (op < 50) {
                do_write();
            } else if (op < 80) {
                do_read_verify();
            } else if (op < 90) {
                do_remove();
            } else {
                do_setattr();
            }
        }
    }

    void verify_all() {
        for (auto &[oid, obj] : shadow_) {
            if (!obj.exists || obj.data.length() == 0) continue;
            bufferlist bl;
            int r = store_->read(coll_, oid, 0, obj.data.length(), bl);
            ASSERT_GT(r, 0) << "read failed for oid=" << oid.oid
                            << " expected_len=" << obj.data.length();
            ASSERT_EQ(bl.length(), obj.data.length())
                << "length mismatch for " << oid.oid;
            ASSERT_TRUE(bl.contents_equal(obj.data))
                << "data mismatch for " << oid.oid;
        }
    }

    void scan() {
        std::vector<ghobject_t> ls;
        ghobject_t start, end, next;
        start.pool = 0;
        start.hash = 0;
        end.pool = 0;
        end.hash = UINT32_MAX;
        int r = store_->collection_list(coll_, start, end, 1000, &ls, &next);
        ASSERT_EQ(r, 0);

        std::set<ghobject_t> store_oids(ls.begin(), ls.end());
        std::set<ghobject_t> shadow_oids;
        for (auto &[oid, obj] : shadow_) {
            if (obj.exists) shadow_oids.insert(oid);
        }
        ASSERT_EQ(store_oids, shadow_oids);
    }

private:
    ghobject_t pick_random() {
        int idx = dist_(rng_) % available_.size();
        return available_[idx];
    }

    void submit_txn(BlueStoreTransaction &bt) {
        std::vector<BlueStoreTransaction> tls;
        tls.push_back(std::move(bt));
        std::atomic<bool> committed{false};
        store_->queue_transactions(coll_, tls, [&] { committed = true; });
        coll_->get_osr()->flush();
        for (int i = 0; i < 200 && !committed; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    void write_object_internal(const ghobject_t &oid, uint64_t offset,
                               const std::string &data) {
        BlueStoreTransaction bt;
        bufferlist bl;
        bl.append(data);
        bt.write(oid, offset, data.size(), bl);
        submit_txn(bt);
    }

    void do_write() {
        auto oid = pick_random();
        auto &obj = shadow_[oid];

        uint64_t max_offset =
            std::max<uint64_t>(obj.data.length(), 65536) - 65536;
        uint64_t offset = 0;
        if (max_offset > 0) {
            offset = (dist_(rng_) % max_offset) & ~(65536 - 1);
        }
        uint64_t len = 65536;
        std::string data(len, 'a' + (dist_(rng_) % 26));

        write_object_internal(oid, offset, data);

        bufferlist new_bl;
        if (offset > obj.data.length()) {
            new_bl = obj.data;
            new_bl.append_zero(offset - obj.data.length());
            bufferlist wbl;
            wbl.append(data);
            new_bl.claim_append(wbl);
        } else if (offset == obj.data.length()) {
            new_bl = obj.data;
            bufferlist wbl;
            wbl.append(data);
            new_bl.claim_append(wbl);
        } else {
            bufferlist head;
            if (offset > 0) {
                head.substr_of(obj.data, 0, offset);
            }
            bufferlist wbl;
            wbl.append(data);
            uint64_t tail_off = offset + len;
            if (tail_off < obj.data.length()) {
                bufferlist tail;
                tail.substr_of(obj.data, tail_off,
                               obj.data.length() - tail_off);
                head.claim_append(wbl);
                head.claim_append(tail);
                new_bl = std::move(head);
            } else {
                head.claim_append(wbl);
                new_bl = std::move(head);
            }
        }
        obj.data = std::move(new_bl);
    }

    void do_read_verify() {
        auto oid = pick_random();
        auto &obj = shadow_[oid];
        if (obj.data.length() == 0) return;

        uint64_t len = std::min<uint64_t>(obj.data.length(), 65536);
        bufferlist bl;
        int r = store_->read(coll_, oid, 0, len, bl);
        ASSERT_GT(r, 0);

        bufferlist expected;
        expected.substr_of(obj.data, 0, len);
        ASSERT_TRUE(bl.contents_equal(expected));
    }

    void do_remove() {
        if (available_.size() <= 1) return;

        int idx = dist_(rng_) % available_.size();
        auto oid = available_[idx];
        available_.erase(available_.begin() + idx);
        shadow_[oid].exists = false;

        BlueStoreTransaction bt;
        bt.remove(oid);
        submit_txn(bt);
    }

    void do_setattr() {
        auto oid = pick_random();

        std::string name = "attr_" + std::to_string(dist_(rng_) % 10);
        std::string val_str = "val_" + std::to_string(dist_(rng_) % 1000);
        bufferptr val(val_str.c_str(), val_str.size());

        BlueStoreTransaction bt;
        bt.setattr(oid, name, val);
        submit_txn(bt);
    }

    BlueStore *store_;
    CollectionRef coll_;
    std::mt19937 rng_;
    std::uniform_int_distribution<int> dist_;
    std::map<ghobject_t, ShadowObject> shadow_;
    std::vector<ghobject_t> available_;
};

}  // namespace

TEST_F(BlueStoreTestFixture, MixedOpsBasic) {
    auto cfg = make_config();
    BlueStore store;
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(0, 0);
    ASSERT_NE(coll, nullptr);

    MixedWorkloadState state(&store, coll, 12345);
    state.seed_objects(50);
    state.run(1000);
    state.verify_all();

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(BlueStoreTestFixture, MixedOpsWithFsck) {
    auto cfg = make_config();
    BlueStore store;
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(0, 0);
    ASSERT_NE(coll, nullptr);

    MixedWorkloadState state(&store, coll, 54321);
    state.seed_objects(30);

    for (int round = 0; round < 5; ++round) {
        state.run(200);
        state.verify_all();

        coll.reset();
        ASSERT_EQ(store.umount(), 0);
        ASSERT_EQ(store.mount(cfg), 0);
        coll = store.get_collection(0);
        ASSERT_NE(coll, nullptr);
    }

    state.verify_all();
    ASSERT_EQ(store.umount(), 0);
    ASSERT_EQ(store.fsck(true), 0);
}

TEST_F(BlueStoreTestFixture, MixedOpsWithRemount) {
    auto cfg = make_config();
    auto store = std::make_unique<BlueStore>();
    ASSERT_EQ(store->mkfs(cfg), 0);
    ASSERT_EQ(store->mount(cfg), 0);

    auto coll = store->create_collection(0, 0);
    ASSERT_NE(coll, nullptr);

    {
        MixedWorkloadState state(store.get(), coll, 99999);
        state.seed_objects(30);
        state.run(500);
        state.verify_all();
    }

    close_and_reopen(store, cfg, coll);

    {
        MixedWorkloadState state(store.get(), coll, 99999);
        state.run(500);
        state.verify_all();
    }

    ASSERT_EQ(store->umount(), 0);
}

// Stress tests

TEST_F(BlueStoreTestFixture, ManySmallWrites) {
    auto cfg = make_config();
    BlueStore store;
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(0, 0);
    auto oid = make_oid(0, 1, "small_writes");

    {
        BlueStoreTransaction bt;
        bt.create(oid);
        submit_and_wait(store, coll, bt);
    }

    uint64_t write_size = cfg.min_alloc_size;
    bufferlist expected;
    for (int i = 0; i < 50; ++i) {
        uint64_t offset = i * write_size;
        std::string data(write_size, 'a' + (i % 26));
        expected.append(data);

        BlueStoreTransaction bt;
        bufferlist bl;
        bl.append(data);
        bt.write(oid, offset, write_size, bl);
        submit_and_wait(store, coll, bt);
    }

    bufferlist result;
    int r = store.read(coll, oid, 0, expected.length(), result);
    ASSERT_GT(r, 0);
    ASSERT_TRUE(result.contents_equal(expected));
    ASSERT_EQ(store.umount(), 0);
}

TEST_F(BlueStoreTestFixture, ManyObjectsWriteRead) {
    auto cfg = make_config();
    BlueStore store;
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(0, 0);

    std::vector<ghobject_t> oids;
    std::vector<std::string> data_vec;
    for (int i = 0; i < 200; ++i) {
        auto oid = make_oid(0, i + 1, "many_" + std::to_string(i));
        oids.push_back(oid);

        std::string data(65536, 'a' + (i % 26));
        data_vec.push_back(data);

        write_object(store, coll, oid, 0, data);
    }

    for (int i = 0; i < 200; ++i) {
        read_and_verify(store, coll, oids[i], 0, data_vec[i]);
    }

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(BlueStoreTestFixture, LargeObjectChunked) {
    auto cfg = make_config();
    BlueStore store;
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(0, 0);
    auto oid = make_oid(0, 1, "large_obj");

    {
        BlueStoreTransaction bt;
        bt.create(oid);
        submit_and_wait(store, coll, bt);
    }

    bufferlist expected;
    uint64_t chunk_size = 65536;
    uint64_t total_size = 4 * 1024 * 1024;

    for (uint64_t off = 0; off < total_size; off += chunk_size) {
        std::string data(chunk_size, 'A' + (off / chunk_size) % 26);
        expected.append(data);

        BlueStoreTransaction bt;
        bufferlist bl;
        bl.append(data);
        bt.write(oid, off, chunk_size, bl);
        submit_and_wait(store, coll, bt);
    }

    bufferlist result;
    int r = store.read(coll, oid, 0, total_size, result);
    ASSERT_GT(r, 0);
    ASSERT_EQ(result.length(), total_size);
    ASSERT_TRUE(result.contents_equal(expected));
    ASSERT_EQ(store.umount(), 0);
}

TEST_F(BlueStoreTestFixture, CacheEvictionUnderPressure) {
    auto cfg = make_config(65536, 128 * 1024);
    BlueStore store;
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(0, 0);

    std::vector<ghobject_t> oids;
    for (int i = 0; i < 20; ++i) {
        auto oid = make_oid(0, i + 1, "cache_" + std::to_string(i));
        oids.push_back(oid);

        std::string data(65536, 'A' + i);
        write_object(store, coll, oid, 0, data);
    }

    for (int i = 0; i < 20; ++i) {
        read_and_verify(store, coll, oids[i], 0, std::string(65536, 'A' + i));
    }

    ASSERT_EQ(store.umount(), 0);
}

// Boundary conditions

TEST_F(BlueStoreTestFixture, EmptyObjectReadWrite) {
    auto cfg = make_config();
    BlueStore store;
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(0, 0);
    auto oid = make_oid(0, 1, "empty_obj");

    {
        BlueStoreTransaction bt;
        bt.create(oid);
        submit_and_wait(store, coll, bt);
    }

    bufferlist bl;
    int r = store.read(coll, oid, 0, 0, bl);
    ASSERT_EQ(r, 0);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(BlueStoreTestFixture, ZeroLengthWrite) {
    auto cfg = make_config();
    BlueStore store;
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(0, 0);
    auto oid = make_oid(0, 1, "zero_write");

    {
        BlueStoreTransaction bt;
        bt.create(oid);
        submit_and_wait(store, coll, bt);
    }

    {
        BlueStoreTransaction bt;
        bufferlist bl;
        bt.write(oid, 0, 0, bl);
        submit_and_wait(store, coll, bt);
    }

    bufferlist bl;
    int r = store.read(coll, oid, 0, 100, bl);
    ASSERT_EQ(r, 0);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(BlueStoreTestFixture, ReadBeyondSize) {
    auto cfg = make_config();
    BlueStore store;
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(0, 0);
    auto oid = make_oid(0, 1, "beyond_obj");

    write_object(store, coll, oid, 0, std::string(4096, 'B'));

    bufferlist bl;
    int r = store.read(coll, oid, 8192, 4096, bl);
    ASSERT_EQ(r, 0);

    ASSERT_EQ(store.umount(), 0);
}

TEST_F(BlueStoreTestFixture, OverlappingWriteSameTransaction) {
    auto cfg = make_config();
    BlueStore store;
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(0, 0);
    auto oid = make_oid(0, 1, "overlap_obj");

    {
        BlueStoreTransaction bt;
        bt.create(oid);
        bufferlist bl1;
        bl1.append(std::string(4096, 'A'));
        bt.write(oid, 0, 4096, bl1);
        bufferlist bl2;
        bl2.append(std::string(4096, 'B'));
        bt.write(oid, 4096, 4096, bl2);
        submit_and_wait(store, coll, bt);
    }

    bufferlist result;
    int r = store.read(coll, oid, 0, 8192, result);
    ASSERT_GT(r, 0);
    ASSERT_EQ(result.length(), 8192u);
    std::string s(result.c_str(), result.length());
    ASSERT_EQ(s.substr(0, 4096), std::string(4096, 'A'));
    ASSERT_EQ(s.substr(4096, 4096), std::string(4096, 'B'));

    ASSERT_EQ(store.umount(), 0);
}

// Crash recovery (deferred write persistence)

TEST_F(BlueStoreTestFixture, DeferredReplayAfterCrash) {
    auto cfg = make_config();
    cfg.prefer_deferred_size = 65536;
    auto store = std::make_unique<BlueStore>();
    ASSERT_EQ(store->mkfs(cfg), 0);
    ASSERT_EQ(store->mount(cfg), 0);

    auto coll = store->create_collection(0, 0);
    ASSERT_NE(coll, nullptr);

    std::vector<ghobject_t> oids;
    std::vector<std::string> expected_data;
    for (int i = 0; i < 5; ++i) {
        auto oid = make_oid(0, i + 1, "deferred_" + std::to_string(i));
        oids.push_back(oid);

        std::string data(65536, 'D' + i);
        expected_data.push_back(data);
        write_object(*store, coll, oid, 0, data);
    }

    for (int i = 0; i < 5; ++i) {
        std::string small_data(8, 'X' + i);
        BlueStoreTransaction bt;
        bufferlist bl;
        bl.append(small_data);
        bt.write(oids[i], 0, small_data.size(), bl);
        submit_and_wait(*store, coll, bt);

        expected_data[i].replace(0, small_data.size(), small_data);
    }

    close_and_reopen(store, cfg, coll);

    for (int i = 0; i < 5; ++i) {
        read_and_verify(*store, coll, oids[i], 0, expected_data[i]);
    }

    ASSERT_EQ(store->umount(), 0);
}

// FSCK integration

TEST_F(BlueStoreTestFixture, FsckAfterMixedOps) {
    auto cfg = make_config();
    BlueStore store;
    ASSERT_EQ(store.mkfs(cfg), 0);
    ASSERT_EQ(store.mount(cfg), 0);

    auto coll = store.create_collection(0, 0);
    ASSERT_NE(coll, nullptr);

    MixedWorkloadState state(&store, coll, 77777);
    state.seed_objects(30);
    state.run(500);

    coll.reset();
    ASSERT_EQ(store.umount(), 0);
    ASSERT_EQ(store.fsck(true), 0);
    ASSERT_EQ(store.mount(cfg), 0);
    coll = store.get_collection(0);
    ASSERT_NE(coll, nullptr);

    state.verify_all();
    ASSERT_EQ(store.umount(), 0);
}

TEST_F(BlueStoreTestFixture, FsckAfterRemount) {
    auto cfg = make_config();
    auto store = std::make_unique<BlueStore>();
    ASSERT_EQ(store->mkfs(cfg), 0);
    ASSERT_EQ(store->mount(cfg), 0);

    auto coll = store->create_collection(0, 0);
    ASSERT_NE(coll, nullptr);

    for (int i = 0; i < 10; ++i) {
        auto oid = make_oid(0, i + 1, "fsck_" + std::to_string(i));
        write_object(*store, coll, oid, 0, std::string(4096, 'F' + i));
    }

    coll.reset();
    ASSERT_EQ(store->umount(), 0);
    ASSERT_EQ(store->fsck(false), 0);
    ASSERT_EQ(store->mount(cfg), 0);
    coll = store->get_collection(0);
    ASSERT_NE(coll, nullptr);

    for (int i = 0; i < 10; ++i) {
        auto oid = make_oid(0, i + 1, "fsck_" + std::to_string(i));
        read_and_verify(*store, coll, oid, 0, std::string(4096, 'F' + i));
    }

    ASSERT_EQ(store->umount(), 0);
}
