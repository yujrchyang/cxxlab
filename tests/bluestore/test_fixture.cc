#include "test_fixture.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <vector>

namespace TOPNSPC {

void BlueStoreTestFixture::SetUp() {
    auto name = std::string("integ_") +
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
                     std::min<uint64_t>(zeros.size(), kBlockSize - off), off);
        }
    }
    ::close(fd);
}

void BlueStoreTestFixture::TearDown() {
    std::string rm_cmd = "rm -rf " + store_path_;
    ::system(rm_cmd.c_str());
}

BlueStoreConfig BlueStoreTestFixture::make_config(uint64_t min_alloc,
                                                  uint64_t cache_size) {
    BlueStoreConfig cfg;
    cfg.path = store_path_;
    cfg.db_path = store_path_ + "/db";
    cfg.bdev_path = store_path_ + "/block";
    cfg.min_alloc_size = min_alloc;
    cfg.block_size = 4096;
    cfg.max_blob_size = 262144;
    cfg.allocator_type = "bitmap";
    cfg.buffer_cache_size = cache_size;
    return cfg;
}

void BlueStoreTestFixture::submit_and_wait(BlueStore &store,
                                           CollectionRef coll,
                                           BlueStoreTransaction &bt) {
    std::vector<BlueStoreTransaction> tls;
    tls.push_back(std::move(bt));

    std::atomic<bool> committed{false};
    ASSERT_EQ(store.queue_transactions(coll, tls,
                                       [&committed]() { committed = true; }),
              0);
    coll->get_osr()->flush();
    ASSERT_TRUE(wait_commit(committed));
}

void BlueStoreTestFixture::close_and_reopen(std::unique_ptr<BlueStore> &store,
                                            const BlueStoreConfig &cfg,
                                            CollectionRef &coll) {
    coll.reset();
    ASSERT_EQ(store->umount(), 0);
    store.reset();

    store = std::make_unique<BlueStore>();
    ASSERT_EQ(store->mount(cfg), 0);
    coll = store->get_collection(0);
    ASSERT_NE(coll, nullptr);
}

void BlueStoreTestFixture::write_object(BlueStore &store, CollectionRef coll,
                                        const ghobject_t &oid,
                                        uint64_t offset,
                                        const std::string &data) {
    BlueStoreTransaction bt;
    bufferlist bl;
    bl.append(data);
    bt.write(oid, offset, data.size(), bl);
    submit_and_wait(store, coll, bt);
}

void BlueStoreTestFixture::read_and_verify(BlueStore &store,
                                           CollectionRef coll,
                                           const ghobject_t &oid,
                                           uint64_t offset,
                                           const std::string &expected) {
    bufferlist bl;
    int r = store.read(coll, oid, offset, expected.size(), bl);
    ASSERT_GT(r, 0);
    ASSERT_EQ(bl.length(), expected.size());
    std::string result(bl.c_str(), bl.length());
    ASSERT_EQ(result, expected);
}

ghobject_t BlueStoreTestFixture::make_oid(int64_t pool, uint32_t hash,
                                          const std::string &name) {
    ghobject_t oid;
    oid.pool = pool;
    oid.hash = hash;
    oid.oid = name;
    return oid;
}

bool BlueStoreTestFixture::wait_commit(std::atomic<bool> &flag,
                                       int timeout_ms) {
    for (int i = 0; i < timeout_ms / 10 && !flag; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return flag.load();
}

}  // namespace TOPNSPC
