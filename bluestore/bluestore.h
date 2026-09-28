#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "bluestore_config.h"
#include "bluestore_constants.h"
#include "bluestore_types.h"
#include "collection.h"
#include "kv/key_value_db.h"

namespace TOPNSPC {

class KeyValueDB;
class BlockDevice;
class FreelistManager;
class Allocator;

class BlueStore {
public:
    BlueStore();
    ~BlueStore();

    BlueStore(const BlueStore &) = delete;
    BlueStore &operator=(const BlueStore &) = delete;

    int mkfs(const BlueStoreConfig &cfg);
    int mount(const BlueStoreConfig &cfg);
    int umount();

    bool is_mounted() const { return mounted_; }

    CollectionRef get_collection(uint64_t coll_id);
    CollectionRef create_collection(uint64_t coll_id, uint32_t bits);
    int remove_collection(uint64_t coll_id);

    KeyValueDB *get_db() const { return db_.get(); }

private:
    int _open_bdev(const std::string &path);
    void _close_bdev();

    int _open_db(bool create);
    void _close_db();

    int _open_fm(Transaction txn, bool create);
    void _close_fm();

    int _init_alloc();
    void _close_alloc();

    int _open_collections();

    int _read_super_meta();

    BlueStoreConfig cfg_;
    bool mounted_ = false;

    std::unique_ptr<BlockDevice> bdev_;
    std::unique_ptr<KeyValueDB> db_;
    FreelistManager *fm_ = nullptr;
    Allocator *alloc_ = nullptr;

    uint64_t min_alloc_size_ = 0;
    uint64_t block_size_ = 0;

    std::atomic<uint64_t> nid_last_{0};
    std::atomic<uint64_t> nid_max_{0};
    std::atomic<uint64_t> blobid_last_{0};
    std::atomic<uint64_t> blobid_max_{0};

    std::string freelist_type_;

    std::map<uint64_t, CollectionRef> coll_map_;
    std::mutex coll_lock_;
};

}  // namespace TOPNSPC
