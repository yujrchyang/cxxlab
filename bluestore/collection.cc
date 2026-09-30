#include "bluestore/collection.h"

#include "bluestore/bluestore_constants.h"
#include "bluestore/trans_context.h"
#include "kv/key_value_db.h"

namespace TOPNSPC {

Collection::Collection(KeyValueDB *db, uint64_t coll_id,
                       size_t onode_cache_size)
    : db_(db), coll_id_(coll_id), onode_space_(onode_cache_size), osr_(new OpSequencer()) {}

Collection::~Collection() {
    delete osr_;
}

// OnodeSpace implementation

OnodeRef OnodeSpace::lookup(const ghobject_t &oid) {
    std::lock_guard<std::mutex> lg(lock_);
    auto it = cache_.find(oid);
    if (it == cache_.end()) {
        return nullptr;
    }

    // Move to front of LRU
    auto lru_it = lru_map_.find(oid);
    if (lru_it != lru_map_.end()) {
        lru_.erase(lru_it->second);
        lru_.push_front(oid);
        lru_map_[oid] = lru_.begin();
    }

    return it->second;
}

OnodeRef OnodeSpace::add(const ghobject_t &oid, OnodeRef on) {
    std::lock_guard<std::mutex> lg(lock_);

    if (cache_.find(oid) != cache_.end()) {
        // Already exists, update LRU
        auto lru_it = lru_map_.find(oid);
        if (lru_it != lru_map_.end()) {
            lru_.erase(lru_it->second);
            lru_.push_front(oid);
            lru_map_[oid] = lru_.begin();
        }
        return on;
    }

    evict_if_needed();

    cache_[oid] = on;
    lru_.push_front(oid);
    lru_map_[oid] = lru_.begin();

    return on;
}

void OnodeSpace::remove(const ghobject_t &oid) {
    std::lock_guard<std::mutex> lg(lock_);

    auto it = cache_.find(oid);
    if (it != cache_.end()) {
        cache_.erase(it);

        auto lru_it = lru_map_.find(oid);
        if (lru_it != lru_map_.end()) {
            lru_.erase(lru_it->second);
            lru_map_.erase(lru_it);
        }
    }
}

void OnodeSpace::clear() {
    std::lock_guard<std::mutex> lg(lock_);
    cache_.clear();
    lru_.clear();
    lru_map_.clear();
}

void OnodeSpace::evict_if_needed() {
    while (cache_.size() >= max_size_ && !lru_.empty()) {
        // Evict from back of LRU (least recently used)
        ghobject_t oid = lru_.back();
        lru_.pop_back();
        lru_map_.erase(oid);
        cache_.erase(oid);
    }
}

// Collection implementation

std::string Collection::encode_onode_key(const ghobject_t &oid) {
    std::string key;
    key_encode_object(oid, &key);
    return key;
}

OnodeRef Collection::get_onode(const ghobject_t &oid, bool create) {
    std::lock_guard<std::mutex> lg(lock_);

    // First check cache
    OnodeRef on = onode_space_.lookup(oid);
    if (on) {
        return on;
    }

    // Try to load from KV
    std::string key = encode_onode_key(oid);
    bool found = false;
    auto loaded = Onode::read_from_kv(db_, oid, key, &found);

    if (found) {
        OnodeRef ref(loaded.release(), OnodeDeleter{});
        ref->get();  // Increment reference count
        return onode_space_.add(oid, ref);
    }

    // Not found in KV
    if (!create) {
        return nullptr;
    }

    // Create new onode
    return create_onode(oid);
}

OnodeRef Collection::create_onode(const ghobject_t &oid) {
    std::string key = encode_onode_key(oid);

    Onode *raw_on = new Onode(oid, key);
    raw_on->get();  // Initial reference
    raw_on->exists = true;
    raw_on->onode.nid = oid.hash;  // Use hash as nid for simplicity

    OnodeRef ref(raw_on, OnodeDeleter{});
    return onode_space_.add(oid, ref);
}

void Collection::remove_onode(const ghobject_t &oid) {
    std::lock_guard<std::mutex> lg(lock_);

    // Remove from cache
    onode_space_.remove(oid);

    // Delete from KV
    std::string key = encode_onode_key(oid);
    auto txn = db_->get_transaction();
    txn->rmkey(PREFIX_OBJ, key);
    db_->submit_transaction_sync(txn);
}

}  // namespace TOPNSPC
