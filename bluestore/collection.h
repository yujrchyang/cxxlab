#pragma once

#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "bluestore/bluestore_types.h"
#include "bluestore/onode.h"
#include "common/object.h"

namespace TOPNSPC {

class KeyValueDB;
class OpSequencer;
class BlueStore;
class BufferCache;

class OnodeSpace {
public:
    OnodeSpace(size_t max_size = 1000) : max_size_(max_size) {}

    OnodeRef lookup(const ghobject_t &oid);
    OnodeRef add(const ghobject_t &oid, OnodeRef on);
    void remove(const ghobject_t &oid);
    void clear();
    size_t size() const { return cache_.size(); }
    bool empty() const { return cache_.empty(); }
    void set_max_size(size_t max_size) { max_size_ = max_size; }

private:
    void evict_if_needed();

    std::mutex lock_;
    size_t max_size_;
    std::unordered_map<ghobject_t, OnodeRef> cache_;
    std::list<ghobject_t> lru_;
    std::unordered_map<ghobject_t, std::list<ghobject_t>::iterator> lru_map_;
};

class Collection {
public:
    Collection(KeyValueDB *db, uint64_t coll_id,
               size_t onode_cache_size = 1000);

    ~Collection();

    Collection(const Collection &) = delete;
    Collection &operator=(const Collection &) = delete;

    OnodeRef get_onode(const ghobject_t &oid, bool create);
    OnodeRef create_onode(const ghobject_t &oid);
    void remove_onode(const ghobject_t &oid);

    bluestore_cnode_t &get_cnode() { return cnode_; }
    const bluestore_cnode_t &get_cnode() const { return cnode_; }

    uint64_t get_coll_id() const { return coll_id_; }

    KeyValueDB *get_db() const { return db_; }

    OpSequencer *get_osr() const { return osr_; }

    BlueStore *get_store() const { return store_; }
    void set_store(BlueStore *s) { store_ = s; }

    BufferCache *get_cache() const { return cache_; }
    void set_cache(BufferCache *c) { cache_ = c; }

    size_t get_onode_count() const { return onode_space_.size(); }

    std::mutex &get_lock() { return lock_; }

private:
    std::string encode_onode_key(const ghobject_t &oid);

    KeyValueDB *db_;
    uint64_t coll_id_;
    bluestore_cnode_t cnode_;
    OnodeSpace onode_space_;
    std::mutex lock_;
    OpSequencer *osr_;
    BlueStore *store_ = nullptr;
    BufferCache *cache_ = nullptr;
};

using CollectionRef = std::shared_ptr<Collection>;

}  // namespace TOPNSPC
