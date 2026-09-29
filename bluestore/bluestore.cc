#include "bluestore/bluestore.h"

#include <algorithm>
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

    _finisher_start();
    _kv_start();

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

    for (auto &[id, coll] : coll_map_) {
        coll->get_osr()->drain();
    }

    _kv_stop();
    _finisher_stop();

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
    bdev_ = BlockDevice::create(path, _aio_callback, this);
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
        coll->set_store(this);

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
    coll->set_store(this);

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

void BlueStore::_aio_callback(void *handle, void *priv) {
    auto *store = static_cast<BlueStore *>(handle);
    store->txc_aio_finish(priv);
}

void BlueStore::txc_aio_finish(void *p) {
    auto *txc = static_cast<TransContext *>(p);
    _txc_state_proc(txc);
}

TransContext *BlueStore::_txc_create(Collection *c) {
    auto *txc = new TransContext(c, c->get_osr());
    txc->t = db_->get_transaction();
    c->get_osr()->queue_new(txc);
    return txc;
}

void BlueStore::_txc_state_proc(TransContext *txc) {
    while (true) {
        switch (txc->get_state()) {
        case TransContext::STATE_PREPARE:
            if (txc->ioc.has_pending_aios()) {
                txc->set_state(TransContext::STATE_AIO_WAIT);
                txc->had_ios = true;
                _txc_aio_submit(txc);
                return;
            }
            [[fallthrough]];

        case TransContext::STATE_AIO_WAIT:
            _txc_finish_io(txc);
            return;

        case TransContext::STATE_IO_DONE: {
            if (txc->had_ios) {
                txc->osr->txc_with_unstable_io.fetch_add(1);
            }
            txc->set_state(TransContext::STATE_KV_QUEUED);
            {
                std::lock_guard<std::mutex> lk(kv_lock_);
                kv_queue_.push_back(txc);
                kv_queue_unsubmitted_.push_back(txc);
                if (!kv_sync_in_progress_) {
                    kv_sync_in_progress_ = true;
                    kv_cond_.notify_one();
                }
            }
            return;
        }

        case TransContext::STATE_KV_SUBMITTED:
            _txc_committed_kv(txc);
            [[fallthrough]];

        case TransContext::STATE_KV_DONE:
            if (txc->deferred_txn) {
                txc->set_state(TransContext::STATE_DEFERRED_QUEUED);
                return;
            }
            txc->set_state(TransContext::STATE_FINISHING);
            [[fallthrough]];

        case TransContext::STATE_DEFERRED_CLEANUP:
            txc->set_state(TransContext::STATE_FINISHING);
            [[fallthrough]];

        case TransContext::STATE_FINISHING:
            _txc_finish(txc);
            return;

        default:
            return;
        }
    }
}

void BlueStore::_txc_add_transaction(TransContext *txc,
                                     BlueStoreTransaction *bt) {
    for (auto &op : bt->ops) {
        switch (op.type) {
        case BlueStoreTransaction::Op::OP_NOP:
            break;
        case BlueStoreTransaction::Op::OP_TOUCH:
        case BlueStoreTransaction::Op::OP_CREATE: {
            auto on = txc->ch->get_onode(op.oid, true);
            if (on) {
                txc->write_onode(on);
            }
            break;
        }
        case BlueStoreTransaction::Op::OP_SETATTRS: {
            auto on = txc->ch->get_onode(op.oid, true);
            if (on) {
                on->set_attrs(op.attrs);
                txc->write_onode(on);
            }
            break;
        }
        default:
            break;
        }
    }
}

void BlueStore::_txc_finish_io(TransContext *txc) {
    std::lock_guard<std::mutex> lg(txc->osr->qlock);
    txc->set_state(TransContext::STATE_IO_DONE);
    txc->ioc.release_running_aios();

    auto &q = txc->osr->q;
    auto it = std::find(q.begin(), q.end(), txc);
    if (it == q.end()) return;

    auto earliest = it;
    while (earliest != q.begin()) {
        auto prev = std::prev(earliest);
        auto prev_state = (*prev)->get_state();
        if (prev_state < TransContext::STATE_IO_DONE) {
            return;
        }
        if (prev_state > TransContext::STATE_IO_DONE) {
            break;
        }
        earliest = prev;
    }

    while (earliest != q.end()) {
        if ((*earliest)->get_state() == TransContext::STATE_IO_DONE) {
            _txc_state_proc(*earliest);
            ++earliest;
        } else {
            break;
        }
    }
}

void BlueStore::_txc_write_nodes(TransContext *txc, Transaction t) {
    for (auto &on : txc->onodes) {
        on->write_to_kv(db_.get(), t);
    }
}

void BlueStore::_txc_finalize_kv(TransContext *txc, Transaction t) {
    for (auto it = txc->allocated.begin(); it != txc->allocated.end(); ++it) {
        fm_->allocate(it.get_start(), it.get_len(), t);
    }
    for (auto it = txc->released.begin(); it != txc->released.end(); ++it) {
        fm_->release(it.get_start(), it.get_len(), t);
    }
}

void BlueStore::_txc_apply_kv(TransContext *txc) {
    db_->submit_transaction(txc->t);
    txc->set_state(TransContext::STATE_KV_SUBMITTED);
    {
        std::lock_guard<std::mutex> lg(txc->osr->qlock);
        txc->osr->qcond.notify_all();
    }
}

void BlueStore::_txc_committed_kv(TransContext *txc) {
    {
        std::lock_guard<std::mutex> lg(txc->osr->qlock);
        txc->set_state(TransContext::STATE_KV_DONE);
    }
    for (auto &fn : txc->on_commits) {
        _queue_finisher(std::move(fn));
    }
    txc->on_commits.clear();
}

void BlueStore::_txc_finish(TransContext *txc) {
    std::deque<TransContext *> releasing;
    bool empty = false;

    {
        std::lock_guard<std::mutex> lg(txc->osr->qlock);
        txc->set_state(TransContext::STATE_DONE);

        while (!txc->osr->q.empty()) {
            auto *front = txc->osr->q.front();
            if (front->get_state() == TransContext::STATE_DONE) {
                txc->osr->q.pop_front();
                releasing.push_back(front);
            } else {
                break;
            }
        }
        if (txc->osr->q.empty()) {
            empty = true;
        }
    }

    if (empty) {
        txc->osr->qcond.notify_all();
    }

    for (auto *t : releasing) {
        _txc_release_alloc(t);
        delete t;
    }
}

void BlueStore::_txc_release_alloc(TransContext *txc) {
    if (!txc->released.empty() && alloc_) {
        alloc_->release(txc->released);
    }
    txc->allocated.clear();
    txc->released.clear();
}

void BlueStore::_txc_aio_submit(TransContext *txc) {
    bdev_->aio_submit(&txc->ioc);
}

int BlueStore::queue_transactions(CollectionRef ch,
                                  std::vector<BlueStoreTransaction> &tls,
                                  std::function<void()> on_commit) {
    if (!mounted_)
        return -EINVAL;

    TransContext *txc = _txc_create(ch.get());

    for (auto &bt : tls) {
        _txc_add_transaction(txc, &bt);
    }

    _txc_write_nodes(txc, txc->t);
    _txc_finalize_kv(txc, txc->t);

    if (on_commit) {
        txc->on_commits.push_back(std::move(on_commit));
    }

    _txc_state_proc(txc);
    return 0;
}

void BlueStore::_kv_start() {
    kv_stop_ = false;
    kv_finalize_stop_ = false;
    kv_sync_in_progress_ = false;
    kv_finalize_in_progress_ = false;

    kv_sync_thread_ = std::thread([this] { _kv_sync_thread_main(); });
    kv_finalize_thread_ = std::thread([this] { _kv_finalize_thread_main(); });
}

void BlueStore::_kv_stop() {
    {
        std::lock_guard<std::mutex> lg(kv_lock_);
        kv_stop_ = true;
        kv_cond_.notify_all();
    }
    {
        std::lock_guard<std::mutex> lg(kv_finalize_lock_);
        kv_finalize_stop_ = true;
        kv_finalize_cond_.notify_all();
    }
    if (kv_sync_thread_.joinable()) kv_sync_thread_.join();
    if (kv_finalize_thread_.joinable()) kv_finalize_thread_.join();
}

void BlueStore::_kv_sync_thread_main() {
    std::unique_lock<std::mutex> lk(kv_lock_);
    while (true) {
        while (kv_queue_.empty() && !kv_stop_) {
            kv_sync_in_progress_ = false;
            kv_cond_.wait(lk);
        }
        if (kv_stop_ && kv_queue_.empty()) break;

        std::deque<TransContext *> committing;
        committing.swap(kv_queue_);

        std::deque<TransContext *> submitting;
        submitting.swap(kv_queue_unsubmitted_);

        lk.unlock();

        for (auto *txc : submitting) {
            if (txc->get_state() == TransContext::STATE_KV_QUEUED) {
                _txc_apply_kv(txc);
            }
        }

        if (bdev_) {
            bdev_->flush();
        }

        {
            auto synct = db_->get_transaction();
            db_->submit_transaction_sync(synct);
        }

        {
            std::lock_guard<std::mutex> flk(kv_finalize_lock_);
            for (auto *txc : committing) {
                kv_committing_to_finalize_.push_back(txc);
            }
            if (!kv_finalize_in_progress_) {
                kv_finalize_in_progress_ = true;
                kv_finalize_cond_.notify_one();
            }
        }

        lk.lock();
    }
    kv_sync_in_progress_ = false;
}

void BlueStore::_kv_finalize_thread_main() {
    std::unique_lock<std::mutex> lk(kv_finalize_lock_);
    while (true) {
        while (kv_committing_to_finalize_.empty() && !kv_finalize_stop_) {
            kv_finalize_in_progress_ = false;
            kv_finalize_cond_.wait(lk);
        }
        if (kv_finalize_stop_ && kv_committing_to_finalize_.empty()) break;

        std::deque<TransContext *> committed;
        committed.swap(kv_committing_to_finalize_);

        lk.unlock();

        for (auto *txc : committed) {
            _txc_state_proc(txc);
        }

        lk.lock();
    }
    kv_finalize_in_progress_ = false;
}

void BlueStore::_finisher_start() {
    finisher_stop_ = false;
    finisher_thread_ = std::thread([this] { _finisher_thread_main(); });
}

void BlueStore::_finisher_stop() {
    {
        std::lock_guard<std::mutex> lg(finisher_lock_);
        finisher_stop_ = true;
        finisher_cond_.notify_all();
    }
    if (finisher_thread_.joinable()) finisher_thread_.join();
}

void BlueStore::_finisher_thread_main() {
    std::unique_lock<std::mutex> lk(finisher_lock_);
    while (true) {
        while (finisher_queue_.empty() && !finisher_stop_) {
            finisher_cond_.wait(lk);
        }
        if (finisher_stop_ && finisher_queue_.empty()) break;

        std::deque<std::function<void()>> batch;
        batch.swap(finisher_queue_);

        lk.unlock();

        for (auto &fn : batch) {
            fn();
        }

        lk.lock();
    }
}

void BlueStore::_queue_finisher(std::function<void()> fn) {
    std::lock_guard<std::mutex> lg(finisher_lock_);
    finisher_queue_.push_back(std::move(fn));
    finisher_cond_.notify_one();
}

}  // namespace TOPNSPC
