#include "bluestore/bluestore.h"

#include <cerrno>
#include <sstream>

#include "blk/allocator.h"
#include "blk/block_device.h"
#include "bluestore/bitmap_freelist_manager.h"
#include "bluestore/bluestore_constants.h"
#include "bluestore/freelist_manager.h"
#include "common/denc.h"
#include "common/object.h"
#include "kv/key_value_db.h"
#include "kv/merge_op/xor_merge_op.h"

namespace TOPNSPC {

BlueStore::BlueStore() = default;

BlueStore::~BlueStore() {
    if (mounted_) {
        umount();
    }
}

int BlueStore::mkfs(const BlueStoreConfig &cfg) {
    int r;

    cfg_ = cfg;

    r = _open_bdev(cfg.bdev_path);
    if (r < 0) {
        return r;
    }

    block_size_ = bdev_->get_block_size();
    min_alloc_size_ = cfg.min_alloc_size;

    r = _open_db(true);
    if (r < 0) {
        goto out_close_bdev;
    }

    r = _open_fm(db_->get_transaction(), true);
    if (r < 0) {
        goto out_close_db;
    }

    {
        Transaction t = db_->get_transaction();
        if (!t) {
            r = -EIO;
            goto out_close_fm;
        }

        r = fm_->create(bdev_->get_size(), min_alloc_size_, t);
        if (r < 0) {
            goto out_close_fm;
        }

        {
            bufferlist bl;
            cxxlab::encode((uint64_t)0, bl);
            t->set(PREFIX_SUPER, "nid_max", bl);
        }
        {
            bufferlist bl;
            cxxlab::encode((uint64_t)0, bl);
            t->set(PREFIX_SUPER, "blobid_max", bl);
        }
        {
            bufferlist bl;
            cxxlab::encode(min_alloc_size_, bl);
            t->set(PREFIX_SUPER, "min_alloc_size", bl);
        }
        {
            bufferlist bl;
            bl.append("bitmap");
            t->set(PREFIX_SUPER, "freelist_type", bl);
        }
        {
            bufferlist bl;
            cxxlab::encode((uint8_t)CSUM_CRC32C, bl);
            t->set(PREFIX_SUPER, "csum_type", bl);
        }
        {
            bufferlist bl;
            cxxlab::encode((uint8_t)12, bl);
            t->set(PREFIX_SUPER, "csum_order", bl);
        }
        {
            bufferlist bl;
            bl.append("done");
            t->set(PREFIX_SUPER, "mkfs_done", bl);
        }

        r = db_->submit_transaction_sync(t);
        if (r < 0) {
            goto out_close_fm;
        }
    }

    _close_fm();
    _close_db();
    _close_bdev();

    return 0;

out_close_fm:
    _close_fm();
out_close_db:
    _close_db();
out_close_bdev:
    _close_bdev();
    return r;
}

int BlueStore::mount(const BlueStoreConfig &cfg) {
    int r;

    cfg_ = cfg;

    r = _open_bdev(cfg.bdev_path);
    if (r < 0)
        return r;

    r = _open_db(false);
    if (r < 0)
        goto out_close_bdev;

    r = _read_super_meta();
    if (r < 0)
        goto out_close_db;

    r = _open_fm(nullptr, false);
    if (r < 0)
        goto out_close_db;

    r = _init_alloc();
    if (r < 0)
        goto out_close_fm;

    r = _open_collections();
    if (r < 0)
        goto out_close_alloc;

    mounted_ = true;
    return 0;

out_close_alloc:
    _close_alloc();
out_close_fm:
    _close_fm();
out_close_db:
    _close_db();
out_close_bdev:
    _close_bdev();
    return r;
}

int BlueStore::umount() {
    if (!mounted_)
        return -EINVAL;

    {
        std::lock_guard<std::mutex> lg(coll_lock_);
        coll_map_.clear();
    }

    _close_alloc();
    _close_fm();
    _close_db();
    _close_bdev();

    mounted_ = false;
    return 0;
}

int BlueStore::_open_bdev(const std::string &path) {
    bdev_ = BlockDevice::create(path, nullptr, nullptr);
    if (!bdev_)
        return -EIO;

    int r = bdev_->open(path);
    if (r < 0) {
        bdev_.reset();
        return r;
    }

    return 0;
}

void BlueStore::_close_bdev() {
    if (bdev_) {
        bdev_->close();
        bdev_.reset();
    }
}

int BlueStore::_open_db(bool create) {
    db_ = KeyValueDB::create("rocksdb", cfg_.db_path);
    if (!db_) {
        return -EIO;
    }

    // Set up merge operator for bitmap prefix
    db_->set_merge_operator(std::string(PREFIX_ALLOC_BITMAP),
                            std::make_shared<XorMergeOperator>());

    int r = db_->init();
    if (r < 0) {
        return r;
    }

    std::ostringstream oss;
    if (create) {
        r = db_->create_and_open(oss);
    } else {
        r = db_->open(oss);
    }

    if (r < 0) {
        db_.reset();
        return r;
    }

    return 0;
}

void BlueStore::_close_db() {
    if (db_) {
        db_->close();
        db_.reset();
    }
}

int BlueStore::_open_fm(Transaction txn, bool create) {
    fm_ = FreelistManager::create(freelist_type_.empty() ? "bitmap" : freelist_type_,
                                  std::string(PREFIX_SUPER),
                                  std::string(PREFIX_ALLOC_BITMAP));
    if (!fm_)
        return -EIO;

    if (!create) {
        auto cfg_reader = [this](const std::string &key, std::string *value) -> int {
            bufferlist bl;
            int r = db_->get(PREFIX_SUPER, key, &bl);
            if (r < 0)
                return r;
            *value = std::string(bl.c_str(), bl.length());
            return 0;
        };
        int r = fm_->init(db_.get(), false, cfg_reader);
        if (r < 0)
            return r;
    }

    return 0;
}

void BlueStore::_close_fm() {
    if (fm_) {
        fm_->shutdown();
        delete fm_;
        fm_ = nullptr;
    }
}

int BlueStore::_init_alloc() {
    uint64_t dev_size = bdev_->get_size();

    alloc_ = Allocator::create(cfg_.allocator_type, dev_size, min_alloc_size_);
    if (!alloc_)
        return -EIO;

    fm_->enumerate_reset();
    uint64_t offset, length;
    while (fm_->enumerate_next(db_.get(), &offset, &length)) {
        alloc_->init_add_free(offset, length);
    }

    return 0;
}

void BlueStore::_close_alloc() {
    if (alloc_) {
        alloc_->shutdown();
        delete alloc_;
        alloc_ = nullptr;
    }
}

int BlueStore::_open_collections() {
    auto it = db_->get_iterator(PREFIX_COLL);
    if (!it)
        return -EIO;

    it->seek_to_first();
    while (it->valid()) {
        std::string key = it->key();
        bufferlist bl = it->value();

        uint64_t coll_id = 0;
        key_decode_u64(key.c_str(), &coll_id);

        bluestore_cnode_t cnode;
        auto p = bl.cbegin();
        cxxlab::decode(cnode, p);

        auto coll = std::make_shared<Collection>(db_.get(), coll_id);
        coll->get_cnode() = cnode;

        {
            std::lock_guard<std::mutex> lg(coll_lock_);
            coll_map_[coll_id] = coll;
        }

        it->next();
    }

    return 0;
}

int BlueStore::_read_super_meta() {
    {
        bufferlist bl;
        int r = db_->get(TOPNSPC::PREFIX_SUPER, "mkfs_done", &bl);
        if (r < 0 || bl.length() == 0) {
            return -ENODATA;
        }
    }

    {
        bufferlist bl;
        db_->get(PREFIX_SUPER, "min_alloc_size", &bl);
        if (bl.length()) {
            auto p = bl.cbegin();
            cxxlab::decode(min_alloc_size_, p);
        } else {
            min_alloc_size_ = 65536;
        }
    }

    {
        bufferlist bl;
        db_->get(PREFIX_SUPER, "nid_max", &bl);
        if (bl.length()) {
            auto p = bl.cbegin();
            uint64_t v;
            cxxlab::decode(v, p);
            nid_max_ = v;
            nid_last_ = v;
        }
    }

    {
        bufferlist bl;
        db_->get(PREFIX_SUPER, "blobid_max", &bl);
        if (bl.length()) {
            auto p = bl.cbegin();
            uint64_t v;
            cxxlab::decode(v, p);
            blobid_max_ = v;
            blobid_last_ = v;
        }
    }

    {
        bufferlist bl;
        db_->get(PREFIX_SUPER, "freelist_type", &bl);
        if (bl.length()) {
            freelist_type_ = std::string(bl.c_str(), bl.length());
        } else {
            freelist_type_ = "bitmap";
        }
    }

    block_size_ = bdev_->get_block_size();

    return 0;
}

CollectionRef BlueStore::get_collection(uint64_t coll_id) {
    std::lock_guard<std::mutex> lg(coll_lock_);
    auto it = coll_map_.find(coll_id);
    if (it != coll_map_.end())
        return it->second;
    return nullptr;
}

CollectionRef BlueStore::create_collection(uint64_t coll_id, uint32_t bits) {
    auto coll = std::make_shared<Collection>(db_.get(), coll_id);
    coll->get_cnode() = bluestore_cnode_t(bits);

    std::string key;
    key_encode_u64(coll_id, &key);

    bufferlist bl;
    cxxlab::encode(coll->get_cnode(), bl);

    Transaction t = db_->get_transaction();
    t->set(PREFIX_COLL, key, bl);
    db_->submit_transaction_sync(t);

    {
        std::lock_guard<std::mutex> lg(coll_lock_);
        coll_map_[coll_id] = coll;
    }

    return coll;
}

int BlueStore::remove_collection(uint64_t coll_id) {
    std::lock_guard<std::mutex> lg(coll_lock_);
    auto it = coll_map_.find(coll_id);
    if (it == coll_map_.end())
        return -ENOENT;

    std::string key;
    key_encode_u64(coll_id, &key);

    Transaction t = db_->get_transaction();
    t->rmkey(PREFIX_COLL, key);
    db_->submit_transaction_sync(t);

    coll_map_.erase(it);
    return 0;
}

}  // namespace TOPNSPC
