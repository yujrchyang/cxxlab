#pragma once

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "bluestore/bluestore.h"
#include "bluestore/bluestore_config.h"
#include "bluestore/trans_context.h"
#include "cxxlab_test.h"

namespace TOPNSPC {

class BlueStoreTestFixture : public ::testing::Test {
protected:
    std::string store_path_;
    static constexpr uint64_t kBlockSize = 64 * 1024 * 1024;

    void SetUp() override;
    void TearDown() override;

    BlueStoreConfig make_config(uint64_t min_alloc = 65536,
                                uint64_t cache_size = 0);

    void submit_and_wait(BlueStore &store, CollectionRef coll,
                         BlueStoreTransaction &bt);

    void close_and_reopen(std::unique_ptr<BlueStore> &store,
                          const BlueStoreConfig &cfg, CollectionRef &coll);

    void write_object(BlueStore &store, CollectionRef coll,
                      const ghobject_t &oid, uint64_t offset,
                      const std::string &data);

    void read_and_verify(BlueStore &store, CollectionRef coll,
                         const ghobject_t &oid, uint64_t offset,
                         const std::string &expected);

    ghobject_t make_oid(int64_t pool, uint32_t hash, const std::string &name);

    bool wait_commit(std::atomic<bool> &flag, int timeout_ms = 2000);
};

}  // namespace TOPNSPC
