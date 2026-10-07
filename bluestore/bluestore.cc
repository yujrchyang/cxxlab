#include "bluestore/bluestore.h"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <chrono>
#include <random>
#include <sstream>

#include "blk/allocator.h"
#include "blk/block_device.h"
#include "bluestore/bitmap_freelist_manager.h"
#include "bluestore/bluestore_constants.h"
#include "bluestore/deferred_writer.h"
#include "bluestore/freelist_manager.h"
#include "common/denc.h"
#include "common/object.h"
#include "common/scope_guard.h"
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

    buffer_cache_ = std::make_unique<BufferCache>(cfg_.buffer_cache_size);

    r = _open_collections();
    if (r < 0)
        goto out_close_alloc;

    _finisher_start();
    _kv_start();

    r = _deferred_replay();
    if (r < 0)
        goto out_close_alloc;

    _init_logger();
    _refresh_perf_counters();

    error_injector_ = std::make_unique<ErrorInjector>();
    error_injector_->bind(alloc_, fm_, db_.get(), min_alloc_size_);

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

    if (buffer_cache_) {
        buffer_cache_->flush();
    }

    {
        std::lock_guard<std::mutex> lg(coll_lock_);
        coll_map_.clear();
    }

    buffer_cache_.reset();
    perf_.reset();
    error_injector_.reset();

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

        auto coll = std::make_shared<Collection>(db_.get(), coll_id,
                                                 cfg_.onode_cache_size);
        coll->get_cnode() = cnode;
        coll->set_store(this);
        coll->set_cache(buffer_cache_.get());

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
    auto coll = std::make_shared<Collection>(db_.get(), coll_id,
                                             cfg_.onode_cache_size);
    coll->get_cnode() = bluestore_cnode_t(bits);
    coll->set_store(this);
    coll->set_cache(buffer_cache_.get());

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
    auto *ioc = static_cast<IOContext *>(priv);
    if (ioc->type == 0) {
        store->txc_aio_finish(ioc->priv);
    } else {
        store->_deferred_batch_aio_finish(
            static_cast<DeferredBatch *>(ioc->priv));
    }
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
            _log_state_latency(txc, l_bluestore_state_prepare_lat);
            if (txc->ioc.has_pending_aios()) {
                txc->set_state(TransContext::STATE_AIO_WAIT);
                txc->had_ios = true;
                _txc_aio_submit(txc);
                return;
            }
            [[fallthrough]];

        case TransContext::STATE_AIO_WAIT:
            _log_state_latency(txc, l_bluestore_state_aio_wait_lat);
            _txc_finish_io(txc);
            return;

        case TransContext::STATE_IO_DONE: {
            _log_state_latency(txc, l_bluestore_state_io_done_lat);
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
            _log_state_latency(txc, l_bluestore_state_kv_done_lat);
            if (txc->deferred_txn) {
                txc->set_state(TransContext::STATE_DEFERRED_QUEUED);
                _log_state_latency(txc, l_bluestore_state_deferred_queued_lat);
                _deferred_queue(txc);
                return;
            }
            txc->set_state(TransContext::STATE_FINISHING);
            [[fallthrough]];

        case TransContext::STATE_DEFERRED_CLEANUP:
            _log_state_latency(txc, l_bluestore_state_deferred_cleanup_lat);
            txc->set_state(TransContext::STATE_FINISHING);
            [[fallthrough]];

        case TransContext::STATE_FINISHING:
            _log_state_latency(txc, l_bluestore_state_finishing_lat);
            _txc_finish(txc);
            return;

        default:
            return;
        }
    }
}

void BlueStore::_assign_nid(TransContext *txc, OnodeRef o) {
    if (o->onode.nid == 0) {
        o->onode.nid = ++nid_last_;
        if (nid_last_ > nid_max_) {
            nid_max_ = nid_last_ + 1024;
            bufferlist bl;
            uint64_t nid_max_val = nid_max_.load();
            cxxlab::encode(nid_max_val, bl);
            txc->t->set(PREFIX_SUPER, "nid_max", bl);
        }
        txc->write_onode(o);
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
                _assign_nid(txc, on);
                txc->write_onode(on);
            }
            break;
        }
        case BlueStoreTransaction::Op::OP_WRITE: {
            auto on = txc->ch->get_onode(op.oid, true);
            if (on) {
                _assign_nid(txc, on);
                bufferlist data = op.data;
                _do_write(txc, txc->ch, on, op.offset, op.length, data);
                txc->bytes += op.length;
                txc->write_onode(on);
            }
            break;
        }
        case BlueStoreTransaction::Op::OP_ZERO: {
            auto on = txc->ch->get_onode(op.oid, false);
            if (on) {
                _do_zero(txc, txc->ch, on, op.offset, op.length);
            }
            break;
        }
        case BlueStoreTransaction::Op::OP_REMOVE: {
            auto on = txc->ch->get_onode(op.oid, false);
            if (on) {
                _do_remove(txc, txc->ch, on);
            }
            break;
        }
        case BlueStoreTransaction::Op::OP_SETATTR: {
            auto on = txc->ch->get_onode(op.oid, true);
            if (on) {
                _assign_nid(txc, on);
                _do_setattr(txc, on, op.attr_name, op.attr_value);
            }
            break;
        }
        case BlueStoreTransaction::Op::OP_SETATTRS: {
            auto on = txc->ch->get_onode(op.oid, true);
            if (on) {
                _assign_nid(txc, on);
                on->set_attrs(op.attrs);
                txc->write_onode(on);
            }
            break;
        }
        case BlueStoreTransaction::Op::OP_OMAP_SETKEYS: {
            auto on = txc->ch->get_onode(op.oid, true);
            if (on) {
                _assign_nid(txc, on);
                bufferlist bl;
                uint32_t num = op.omap_keys.size();
                cxxlab::encode(num, bl);
                for (const auto &[key, value] : op.omap_keys) {
                    cxxlab::encode(key, bl);
                    cxxlab::encode(value, bl);
                }
                _omap_setkeys(txc, on, bl);
                txc->write_onode(on);
            }
            break;
        }
        case BlueStoreTransaction::Op::OP_OMAP_SETHEADER: {
            auto on = txc->ch->get_onode(op.oid, true);
            if (on) {
                _assign_nid(txc, on);
                bufferlist bl = op.data;
                _omap_setheader(txc, on, bl);
                txc->write_onode(on);
            }
            break;
        }
        case BlueStoreTransaction::Op::OP_OMAP_RMKEYS: {
            auto on = txc->ch->get_onode(op.oid, false);
            if (on) {
                bufferlist bl;
                uint32_t num = op.omap_rmkeys.size();
                cxxlab::encode(num, bl);
                for (const auto &key : op.omap_rmkeys) {
                    cxxlab::encode(key, bl);
                }
                _omap_rmkeys(txc, on, bl);
                txc->write_onode(on);
            }
            break;
        }
        case BlueStoreTransaction::Op::OP_OMAP_CLEAR: {
            auto on = txc->ch->get_onode(op.oid, false);
            if (on) {
                _omap_clear(txc, on);
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
    if (!should_inject(cfg_.inject_kv_err_rate)) {
        int r = db_->submit_transaction_sync(txc->t);
        cxxlab_assert(r == 0);
    }
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

    _log_state_latency(txc, l_bluestore_state_kv_committing_lat);
    auto now = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now -
                                                                   txc->start)
                  .count();
    _log_latency(l_bluestore_commit_lat, static_cast<uint64_t>(ns));
    if (perf_) {
        perf_->inc(l_bluestore_txc);
        if (txc->bytes > 0) {
            perf_->inc(l_bluestore_stored, txc->bytes);
        }
    }
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
        _finish_write(t);
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

    // Write deferred transaction to WAL before entering state machine
    if (txc->deferred_txn) {
        txc->deferred_txn->seq = ++deferred_seq_;
        bufferlist bl;
        cxxlab::encode(*txc->deferred_txn, bl);
        std::string key;
        key_encode_u64(txc->deferred_txn->seq, &key);
        txc->t->set(PREFIX_DEFERRED, key, bl);
    }

    if (on_commit) {
        txc->on_commits.push_back(std::move(on_commit));
    }

    auto submit_start = std::chrono::steady_clock::now();
    _txc_state_proc(txc);
    if (perf_) {
        auto now = std::chrono::steady_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      now - submit_start)
                      .count();
        perf_->tinc(l_bluestore_submit_lat, static_cast<uint64_t>(ns));
    }
    return 0;
}

void BlueStore::_kv_start() {
    kv_stop_ = false;
    kv_finalize_stop_ = false;
    kv_sync_in_progress_ = false;
    kv_finalize_in_progress_ = false;
    deferred_writer_ = std::make_unique<DeferredWriter>(bdev_.get());

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
    deferred_writer_.reset();
}

void BlueStore::_kv_sync_thread_main() {
    std::unique_lock<std::mutex> lk(kv_lock_);
    while (true) {
        while (kv_queue_.empty() && !deferred_writer_->has_done() &&
               !kv_stop_) {
            kv_sync_in_progress_ = false;
            kv_cond_.wait(lk);
        }
        if (kv_stop_ && kv_queue_.empty() && !deferred_writer_->has_done())
            break;

        std::deque<TransContext *> committing;
        committing.swap(kv_queue_);

        std::deque<TransContext *> submitting;
        submitting.swap(kv_queue_unsubmitted_);

        lk.unlock();

        auto start = std::chrono::steady_clock::now();

        for (auto *txc : submitting) {
            if (txc->get_state() == TransContext::STATE_KV_QUEUED) {
                _log_state_latency(txc, l_bluestore_state_kv_queued_lat);
                _txc_apply_kv(txc);
            }
        }

        if (bdev_) {
            bdev_->flush();
        }

        deferred_writer_->try_submit();
        deferred_writer_->flush_done_to_stable();
        auto deferred_stable = deferred_writer_->swap_stable_queue();

        auto after_flush = std::chrono::steady_clock::now();

        {
            auto synct = db_->get_transaction();
            for (auto *b : deferred_stable) {
                for (auto *txc : b->txcs) {
                    std::string key;
                    key_encode_u64(txc->deferred_txn->seq, &key);
                    synct->rmkey(PREFIX_DEFERRED, key);
                }
            }
            if (!should_inject(cfg_.inject_kv_err_rate)) {
                db_->submit_transaction_sync(synct);
            }
        }

        auto finish = std::chrono::steady_clock::now();
        _log_latency(l_bluestore_kv_flush_lat,
                     static_cast<uint64_t>(
                         std::chrono::duration_cast<std::chrono::nanoseconds>(
                             after_flush - start)
                             .count()));
        _log_latency(l_bluestore_kv_commit_lat,
                     static_cast<uint64_t>(
                         std::chrono::duration_cast<std::chrono::nanoseconds>(
                             finish - after_flush)
                             .count()));
        _log_latency(l_bluestore_kv_sync_lat,
                     static_cast<uint64_t>(
                         std::chrono::duration_cast<std::chrono::nanoseconds>(
                             finish - start)
                             .count()));

        {
            std::lock_guard<std::mutex> flk(kv_finalize_lock_);
            for (auto *txc : committing) {
                kv_committing_to_finalize_.push_back(txc);
            }
            deferred_stable_to_finalize_.insert(
                deferred_stable_to_finalize_.end(), deferred_stable.begin(),
                deferred_stable.end());
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
        while (kv_committing_to_finalize_.empty() &&
               deferred_stable_to_finalize_.empty() && !kv_finalize_stop_) {
            kv_finalize_in_progress_ = false;
            kv_finalize_cond_.wait(lk);
        }
        if (kv_finalize_stop_ && kv_committing_to_finalize_.empty() &&
            deferred_stable_to_finalize_.empty())
            break;

        std::deque<TransContext *> committed;
        committed.swap(kv_committing_to_finalize_);
        std::deque<DeferredBatch *> deferred_stable;
        deferred_stable.swap(deferred_stable_to_finalize_);

        lk.unlock();

        auto start = std::chrono::steady_clock::now();

        for (auto *txc : committed) {
            _txc_state_proc(txc);
        }

        for (auto *b : deferred_stable) {
            auto it = b->txcs.begin();
            while (it != b->txcs.end()) {
                TransContext *txc = *it;
                it = b->txcs.erase(it);
                _finish_write(txc);
                txc->set_state(TransContext::STATE_DEFERRED_CLEANUP);
                _txc_state_proc(txc);
            }
            delete b;
        }

        _refresh_perf_counters();

        auto finish = std::chrono::steady_clock::now();
        _log_latency(l_bluestore_kv_final_lat,
                     static_cast<uint64_t>(
                         std::chrono::duration_cast<std::chrono::nanoseconds>(
                             finish - start)
                             .count()));

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

void BlueStore::_choose_write_options(WriteContext *wctx) {
    unsigned block_size_order = std::countr_zero(block_size_);
    wctx->csum_order = block_size_order;
    wctx->target_blob_size = cfg_.max_blob_size;
    if (wctx->target_blob_size == 0 ||
        wctx->target_blob_size < min_alloc_size_) {
        wctx->target_blob_size = min_alloc_size_;
    }
}

void BlueStore::_pad_zeros(bufferlist *bl, uint64_t *offset,
                           uint64_t chunk_size) {
    uint64_t front_pad = *offset % chunk_size;
    if (front_pad) {
        bufferlist pad;
        pad.append_zero(front_pad);
        pad.claim_append(*bl);
        *bl = std::move(pad);
        *offset -= front_pad;
    }
    uint64_t tail_pad = (*offset + bl->length()) % chunk_size;
    if (tail_pad) {
        tail_pad = chunk_size - tail_pad;
        bl->append_zero(tail_pad);
    }
    if (perf_ && (front_pad || tail_pad)) {
        perf_->inc(l_bluestore_write_pad_bytes, front_pad + tail_pad);
    }
}

void BlueStore::_apply_padding(uint64_t head_pad, uint64_t tail_pad,
                               bufferlist &bl) {
    if (head_pad) {
        bufferlist h;
        h.append_zero(head_pad);
        h.claim_append(bl);
        bl.swap(h);
    }
    if (tail_pad) {
        bl.append_zero(tail_pad);
    }
}

static uint64_t _blob_to_phys(const bluestore_blob_t &blob, uint64_t b_off) {
    uint64_t off = 0;
    for (const auto &e : blob.get_extents()) {
        if (b_off >= off && b_off < off + e.length) {
            return e.offset + (b_off - off);
        }
        off += e.length;
    }
    return 0;
}

int BlueStore::read(CollectionRef c, const ghobject_t &oid, uint64_t offset,
                    uint64_t length, bufferlist &bl) {
    auto read_start = std::chrono::steady_clock::now();

    if (!c) return -ENOENT;

    auto o = c->get_onode(oid, false);
    if (!o || !o->exists) return -ENOENT;

    if (error_injector_ && error_injector_->check_mdata_error(oid)) {
        return -EIO;
    }

    if (offset >= o->onode.size) return 0;

    if (length == 0 || offset + length > o->onode.size) {
        length = o->onode.size - offset;
    }

    int r = _do_read(o, offset, length, bl);

    if (r >= 0 && error_injector_ &&
        error_injector_->check_data_error(oid)) {
        r = -EIO;
    }

    if (r >= 0 && should_inject(cfg_.inject_read_err_rate)) {
        r = -EIO;
    }

    auto read_end = std::chrono::steady_clock::now();
    _log_latency(l_bluestore_read_lat,
                 static_cast<uint64_t>(
                     std::chrono::duration_cast<std::chrono::nanoseconds>(
                         read_end - read_start)
                         .count()));
    if (r == -EIO && perf_) {
        perf_->inc(l_bluestore_read_eio);
    }
    return r;
}

int BlueStore::_do_read(OnodeRef o, uint64_t offset, uint64_t length,
                        bufferlist &bl) {
    bl.clear();

    uint64_t end = offset + length;
    BufferCache *cache = buffer_cache_.get();

    uint64_t pos = offset;
    auto it = o->extent_map.seek_lextent(offset);
    if (it == o->extent_map.end()) {
        it = o->extent_map.begin();
    }

    while (pos < end) {
        if (it == o->extent_map.end() || it->logical_offset > pos) {
            uint64_t hole_end =
                (it != o->extent_map.end()) ? it->logical_offset : end;
            uint64_t hole_len = std::min(hole_end - pos, end - pos);
            bl.append_zero(hole_len);
            pos += hole_len;
            continue;
        }

        uint64_t ext_start = it->logical_offset;
        uint64_t ext_end = ext_start + it->length;

        if (ext_end <= pos) {
            ++it;
            continue;
        }

        uint64_t read_off = std::max(pos, ext_start);
        uint64_t read_end = std::min(end, ext_end);
        uint64_t read_len = read_end - read_off;

        uint64_t blob_off = it->blob_offset + (read_off - ext_start);
        BlobRef blob = it->blob;

        uint64_t chunk_size = blob->get_blob().get_chunk_size(block_size_);
        uint64_t blob_chunk_start = blob_off & ~(chunk_size - 1);
        uint64_t front_pad = blob_off - blob_chunk_start;
        uint64_t aligned_len = (front_pad + read_len + chunk_size - 1) &
            ~(chunk_size - 1);

        uint64_t blob_len = blob->get_blob().get_logical_length();
        if (blob_chunk_start + aligned_len > blob_len) {
            aligned_len = blob_len - blob_chunk_start;
        }

        bufferlist chunk_bl;
        bool cache_hit = false;

        if (cache) {
            cache_hit = blob->bc().read(cache,
                                        static_cast<uint32_t>(blob_chunk_start),
                                        static_cast<uint32_t>(aligned_len),
                                        &chunk_bl);
        }

        if (!cache_hit) {
            uint64_t phys_off =
                _blob_to_phys(blob->get_blob(), blob_chunk_start);

            bufferlist raw_bl;
            int r = bdev_->read(phys_off, aligned_len, &raw_bl, nullptr, true);
            if (r < 0) return r;

            if (blob->get_blob().has_csum()) {
                int bad = blob->get_blob().verify_csum(blob_chunk_start,
                                                       raw_bl, block_size_);
                if (bad >= 0) return -EIO;
            }

            if (cache) {
                blob->bc().did_read(cache,
                                    static_cast<uint32_t>(blob_chunk_start),
                                    raw_bl);
            }

            chunk_bl = std::move(raw_bl);
        }

        bufferlist trimmed;
        trimmed.substr_of(chunk_bl, front_pad, read_len);
        bl.claim_append(trimmed);

        pos = read_end;
        ++it;
    }

    return bl.length();
}

int BlueStore::_do_zero(TransContext *txc, Collection *ch, OnodeRef o,
                        uint64_t offset, uint64_t length) {
    WriteContext wctx;
    o->extent_map.punch_hole(offset, length, &wctx.old_extents);

    BufferCache *cache = buffer_cache_.get();
    if (cache) {
        for (auto &oe : wctx.old_extents) {
            uint32_t cache_off = oe.e.blob_offset;
            oe.e.blob->bc().discard(cache, cache_off, oe.e.length);
        }
    }

    _wctx_finish(txc, &wctx);

    uint64_t end = offset + length;
    if (end > o->onode.size) {
        o->onode.size = end;
    }

    txc->write_onode(o);
    return 0;
}

int BlueStore::_do_remove(TransContext *txc, Collection *ch, OnodeRef o) {
    WriteContext wctx;
    o->extent_map.punch_hole(0, o->onode.size, &wctx.old_extents);

    BufferCache *cache = buffer_cache_.get();
    if (cache) {
        std::set<BlobRef> seen;
        for (auto &oe : wctx.old_extents) {
            if (seen.insert(oe.e.blob).second) {
                oe.e.blob->bc().clear(cache);
            }
        }
    }

    _wctx_finish(txc, &wctx);

    o->exists = false;
    txc->t->rmkey(PREFIX_OBJ, o->key);
    txc->note_removed_object(o);

    o->extent_map.clear();
    o->onode = bluestore_onode_t();

    return 0;
}

void BlueStore::_do_setattr(TransContext *txc, OnodeRef o,
                            const std::string &name, const bufferptr &val) {
    o->set_attr(name, val);
    txc->write_onode(o);
}

int BlueStore::getattr(CollectionRef c, const ghobject_t &oid,
                       const std::string &name, bufferptr *value) {
    if (!c) return -ENOENT;

    auto o = c->get_onode(oid, false);
    if (!o || !o->exists) return -ENOENT;

    auto it = o->onode.attrs.find(name);
    if (it == o->onode.attrs.end()) return -ENODATA;
    *value = it->second;
    return 0;
}

int BlueStore::getattrs(CollectionRef c, const ghobject_t &oid,
                        std::map<std::string, bufferptr> *attrs) {
    if (!c) return -ENOENT;

    auto o = c->get_onode(oid, false);
    if (!o || !o->exists) return -ENOENT;

    o->get_all_attrs(attrs);
    return 0;
}

// OMap operations

int BlueStore::_omap_setkeys(TransContext *txc, OnodeRef o, bufferlist &bl) {
    if (!o->onode.has_omap()) {
        o->onode.set_omap_flags();
        txc->write_onode(o);

        const std::string &prefix = o->get_omap_prefix();
        std::string key_tail;
        bufferlist tail;
        o->get_omap_tail(&key_tail);
        txc->t->set(prefix, key_tail, tail);
    }

    const std::string &prefix = o->get_omap_prefix();
    std::string final_key;
    o->get_omap_key(std::string(), &final_key);
    size_t base_key_len = final_key.size();

    auto p = bl.cbegin();
    uint32_t num;
    cxxlab::decode(num, p);
    while (num--) {
        std::string key;
        bufferlist value;
        cxxlab::decode(key, p);
        cxxlab::decode(value, p);
        final_key.resize(base_key_len);
        final_key += key;
        txc->t->set(prefix, final_key, value);
    }
    return 0;
}

int BlueStore::_omap_setheader(TransContext *txc, OnodeRef o, bufferlist &bl) {
    if (!o->onode.has_omap()) {
        o->onode.set_omap_flags();
        txc->write_onode(o);

        const std::string &prefix = o->get_omap_prefix();
        std::string key_tail;
        bufferlist tail;
        o->get_omap_tail(&key_tail);
        txc->t->set(prefix, key_tail, tail);
    }

    const std::string &prefix = o->get_omap_prefix();
    std::string key;
    o->get_omap_header(&key);
    txc->t->set(prefix, key, bl);
    return 0;
}

int BlueStore::_omap_rmkeys(TransContext *txc, OnodeRef o, bufferlist &bl) {
    if (!o->onode.has_omap()) {
        return 0;
    }

    const std::string &prefix = o->get_omap_prefix();
    std::string final_key;
    o->get_omap_key(std::string(), &final_key);
    size_t base_key_len = final_key.size();

    auto p = bl.cbegin();
    uint32_t num;
    cxxlab::decode(num, p);
    while (num--) {
        std::string key;
        cxxlab::decode(key, p);
        final_key.resize(base_key_len);
        final_key += key;
        txc->t->rmkey(prefix, final_key);
    }
    return 0;
}

void BlueStore::_omap_clear(TransContext *txc, OnodeRef o) {
    if (!o->onode.has_omap()) {
        return;
    }

    const std::string &omap_prefix = o->get_omap_prefix();
    std::string prefix, tail;
    o->get_omap_header(&prefix);
    o->get_omap_tail(&tail);
    txc->t->rm_range_keys(omap_prefix, prefix, tail);
    txc->t->rmkey(omap_prefix, tail);
    o->onode.clear_omap_flag();
}

int BlueStore::_onode_omap_get(const OnodeRef &o, bufferlist *header,
                               std::map<std::string, bufferlist> *out) {
    if (!o->onode.has_omap()) {
        return 0;
    }

    const std::string &prefix = o->get_omap_prefix();
    std::string head, tail;
    o->get_omap_header(&head);
    o->get_omap_tail(&tail);

    IteratorBounds bounds;
    bounds.lower_bound = head;
    bounds.upper_bound = tail;

    auto it = db_->get_iterator(prefix, 0, bounds);
    it->lower_bound(head);

    while (it->valid()) {
        std::string key = it->key();
        if (key == head) {
            if (header) {
                *header = it->value();
            }
        } else if (key >= tail) {
            break;
        } else {
            std::string user_key;
            o->decode_omap_key(key, &user_key);
            if (out) {
                (*out)[user_key] = it->value();
            }
        }
        it->next();
    }

    return 0;
}

int BlueStore::omap_get(CollectionRef c, const ghobject_t &oid,
                        bufferlist *header,
                        std::map<std::string, bufferlist> *out) {
    if (!c) return -ENOENT;

    auto o = c->get_onode(oid, false);
    if (!o || !o->exists) return -ENOENT;

    return _onode_omap_get(o, header, out);
}

int BlueStore::omap_get_header(CollectionRef c, const ghobject_t &oid,
                               bufferlist *header) {
    if (!c) return -ENOENT;

    auto o = c->get_onode(oid, false);
    if (!o || !o->exists) return -ENOENT;

    if (!o->onode.has_omap()) {
        return 0;
    }

    std::string head;
    o->get_omap_header(&head);
    return db_->get(o->get_omap_prefix(), head, header);
}

int BlueStore::omap_get_values(CollectionRef c, const ghobject_t &oid,
                               const std::set<std::string> &keys,
                               std::map<std::string, bufferlist> *out) {
    if (!c) return -ENOENT;

    auto o = c->get_onode(oid, false);
    if (!o || !o->exists) return -ENOENT;

    if (!o->onode.has_omap()) {
        return 0;
    }

    const std::string &prefix = o->get_omap_prefix();
    std::string final_key;
    o->get_omap_key(std::string(), &final_key);
    size_t base_key_len = final_key.size();

    for (const auto &key : keys) {
        final_key.resize(base_key_len);
        final_key += key;
        bufferlist val;
        if (db_->get(prefix, final_key, &val) >= 0) {
            (*out)[key] = val;
        }
    }

    return 0;
}

int BlueStore::omap_check_keys(CollectionRef c, const ghobject_t &oid,
                               const std::set<std::string> &keys,
                               std::set<std::string> *out) {
    if (!c) return -ENOENT;

    auto o = c->get_onode(oid, false);
    if (!o || !o->exists) return -ENOENT;

    if (!o->onode.has_omap()) {
        return 0;
    }

    const std::string &prefix = o->get_omap_prefix();
    std::string final_key;
    o->get_omap_key(std::string(), &final_key);
    size_t base_key_len = final_key.size();

    for (const auto &key : keys) {
        final_key.resize(base_key_len);
        final_key += key;
        bufferlist val;
        if (db_->get(prefix, final_key, &val) >= 0) {
            out->insert(key);
        }
    }

    return 0;
}

int BlueStore::collection_list(CollectionRef c, const ghobject_t &start,
                               const ghobject_t &end, int max,
                               std::vector<ghobject_t> *ls, ghobject_t *next) {
    if (!c) return -ENOENT;
    if (max <= 0) return 0;

    std::lock_guard<std::mutex> lg(c->get_lock());

    std::string start_key, end_key;
    key_encode_object(start, &start_key);
    if (!end.is_max()) {
        key_encode_object(end, &end_key);
    }

    IteratorBounds bounds;
    bounds.lower_bound = start_key;
    if (!end.is_max()) {
        bounds.upper_bound = end_key;
    }

    auto it = db_->get_iterator(PREFIX_OBJ, 0, bounds);
    if (!it) return 0;

    it->seek_to_first();

    int count = 0;
    while (it->valid()) {
        std::string key = it->key();

        if (!key.empty() && key.back() == EXTENT_SHARD_KEY_SUFFIX) {
            it->next();
            continue;
        }

        ghobject_t oid;
        int r = key_decode_object(key, &oid);
        if (r < 0) {
            it->next();
            continue;
        }

        if (oid.pool != (int64_t)c->get_coll_id()) {
            it->next();
            continue;
        }

        if (oid >= end) break;
        if (oid < start) {
            it->next();
            continue;
        }

        if (count >= max) {
            if (next) *next = oid;
            return 0;
        }

        ls->push_back(oid);
        count++;

        it->next();
    }

    if (next) {
        *next = ghobject_t::get_max();
    }

    return 0;
}

int BlueStore::_do_write(TransContext *txc, Collection *ch, OnodeRef o,
                         uint64_t offset, uint64_t length, bufferlist &bl) {
    if (length == 0) return 0;

    WriteContext wctx;
    _choose_write_options(&wctx);

    _do_write_data(txc, ch, o, offset, length, bl, &wctx);

    int r = _do_alloc_write(txc, o, &wctx);
    if (r < 0) return r;

    _wctx_finish(txc, &wctx);

    uint64_t end = offset + length;
    if (end > o->onode.size) {
        o->onode.size = end;
    }

    o->extent_map.compress_extent_map(offset, length);

    return 0;
}

void BlueStore::_do_write_data(TransContext *txc, Collection *ch, OnodeRef o,
                               uint64_t offset, uint64_t length,
                               bufferlist &bl, WriteContext *wctx) {
    uint64_t end = offset + length;

    if (length != min_alloc_size_ &&
        offset / min_alloc_size_ == (end - 1) / min_alloc_size_) {
        _do_write_small(txc, ch, o, offset, length, bl, wctx);
        return;
    }

    uint64_t head_len = (min_alloc_size_ - offset % min_alloc_size_) % min_alloc_size_;
    uint64_t tail_len = end % min_alloc_size_;
    uint64_t mid_off = offset + head_len;
    uint64_t mid_len = length - head_len - tail_len;

    if (head_len > 0) {
        bufferlist head_bl;
        head_bl.substr_of(bl, 0, head_len);
        _do_write_small(txc, ch, o, offset, head_len, head_bl, wctx);
    }

    if (mid_len > 0) {
        _do_write_big(txc, ch, o, mid_off, mid_len, bl, head_len, wctx);
    }

    if (tail_len > 0) {
        bufferlist tail_bl;
        tail_bl.substr_of(bl, head_len + mid_len, tail_len);
        _do_write_small(txc, ch, o, end - tail_len, tail_len, tail_bl, wctx);
    }
}

void BlueStore::_do_write_small(TransContext *txc, Collection *ch,
                                OnodeRef o, uint64_t offset, uint64_t length,
                                bufferlist &bl, WriteContext *wctx) {
    uint64_t alloc_len = min_alloc_size_;
    uint64_t max_bsize = std::max(wctx->target_blob_size, min_alloc_size_);
    uint64_t chunk_size = block_size_;
    uint64_t min_off = offset >= max_bsize ? offset - max_bsize : 0;

    if (bl.is_zero()) {
        o->extent_map.punch_hole(offset, length, &wctx->old_extents);
        return;
    }

    auto it = o->extent_map.seek_lextent(offset);
    auto search_start = (it != o->extent_map.end())
        ? it
        : o->extent_map.begin();

    auto begin = o->extent_map.begin();
    auto end = o->extent_map.end();
    auto ep = search_start;
    auto prev_ep = (search_start != begin) ? std::prev(search_start) : end;

    bool any_change;
    do {
        any_change = false;

        if (ep != end && ep->logical_offset < offset + max_bsize) {
            uint64_t bstart = ep->blob_start();
            if (!(bstart > offset || bstart < min_off) &&
                ep->blob->get_blob().is_mutable()) {
                uint64_t blob_chunk_size =
                    ep->blob->get_blob().get_chunk_size(block_size_);
                uint64_t end_offs = offset + length;
                uint64_t head_pad = p2phase(offset, blob_chunk_size);
                uint64_t tail_pad = p2nphase(end_offs, blob_chunk_size);
                if (head_pad && o->extent_map.has_any_lextents(offset - head_pad, head_pad)) {
                    head_pad = 0;
                }
                if (tail_pad && o->extent_map.has_any_lextents(end_offs, tail_pad)) {
                    tail_pad = 0;
                }
                uint64_t b_off = offset - head_pad - bstart;
                uint64_t b_len = length + head_pad + tail_pad;

                if ((b_off % blob_chunk_size == 0 &&
                     b_len % blob_chunk_size == 0) &&
                    ep->blob->get_blob().get_ondisk_length() >=
                        b_off + b_len &&
                    ep->blob->get_blob().is_unused(b_off, b_len) &&
                    ep->blob->get_blob().is_allocated(b_off, b_len)) {
                    bufferlist padded_bl = bl;
                    _apply_padding(head_pad, tail_pad, padded_bl);
                    _buffer_cache_write(txc, ep->blob, b_off, padded_bl, 0);
                    bluestore_deferred_op_t *op =
                        _get_deferred_op(txc, padded_bl.length());
                    op->op = bluestore_deferred_op_t::OP_WRITE;
                    ep->blob->get_blob().map(
                        b_off, b_len, [&](uint64_t off, uint64_t len) {
                            op->extents.emplace_back(
                                bluestore_pextent_t(off, len));
                            return 0;
                        });
                    op->data = padded_bl;
                    ep->blob->dirty_blob().calc_csum(b_off, padded_bl,
                                                     block_size_);
                    auto le = o->extent_map.set_lextent(
                        static_cast<uint32_t>(offset),
                        static_cast<uint32_t>(b_off + head_pad),
                        static_cast<uint32_t>(length), ep->blob,
                        &wctx->old_extents, min_alloc_size_);
                    ep->blob->dirty_blob().mark_used(le->blob_offset,
                                                     le->length);
                    return;
                }

                uint64_t head_read = p2phase(b_off, blob_chunk_size);
                uint64_t tail_read =
                    p2nphase(b_off + b_len, blob_chunk_size);
                if ((head_read || tail_read) &&
                    ep->blob->get_blob().get_ondisk_length() >=
                        b_off + b_len + tail_read &&
                    head_read + tail_read < min_alloc_size_) {
                    b_off -= head_read;
                    b_len += head_read + tail_read;
                    if (b_off % blob_chunk_size == 0 &&
                        b_len % blob_chunk_size == 0 &&
                        ep->blob->get_blob().is_allocated(b_off, b_len)) {
                        bufferlist padded_bl = bl;
                        _apply_padding(head_pad, tail_pad, padded_bl);
                        if (head_read) {
                            bufferlist head_bl;
                            int r = _do_read(o, offset - head_pad - head_read,
                                             head_read, head_bl);
                            cxxlab_assert(r >= 0);
                            if (head_bl.length() < head_read) {
                                head_bl.append_zero(head_read -
                                                    head_bl.length());
                            }
                            head_bl.claim_append(padded_bl);
                            padded_bl.swap(head_bl);
                        }
                        if (tail_read) {
                            bufferlist tail_bl;
                            int r = _do_read(o, offset + length + tail_pad,
                                             tail_read, tail_bl);
                            cxxlab_assert(r >= 0);
                            if (tail_bl.length() < tail_read) {
                                tail_bl.append_zero(tail_read -
                                                    tail_bl.length());
                            }
                            padded_bl.claim_append(tail_bl);
                        }
                        _buffer_cache_write(txc, ep->blob, b_off,
                                            padded_bl, 0);
                        ep->blob->dirty_blob().calc_csum(b_off,
                                                         padded_bl,
                                                         block_size_);
                        bluestore_deferred_op_t *op =
                            _get_deferred_op(txc, padded_bl.length());
                        op->op = bluestore_deferred_op_t::OP_WRITE;
                        ep->blob->get_blob().map(
                            b_off, b_len,
                            [&](uint64_t off, uint64_t len) {
                                op->extents.emplace_back(
                                    bluestore_pextent_t(off, len));
                                return 0;
                            });
                        op->data = padded_bl;
                        auto le = o->extent_map.set_lextent(
                            static_cast<uint32_t>(offset),
                            static_cast<uint32_t>(offset - bstart),
                            static_cast<uint32_t>(length), ep->blob,
                            &wctx->old_extents, min_alloc_size_);
                        ep->blob->dirty_blob().mark_used(le->blob_offset,
                                                         le->length);
                        return;
                    }
                }

                uint64_t b_off_reuse = offset - bstart;
                uint32_t alloc_len32 = alloc_len;
                if (ep->blob->can_reuse_blob(min_alloc_size_, max_bsize,
                                             b_off_reuse, &alloc_len32) &&
                    !wctx->has_conflict(ep->blob, b_off_reuse, alloc_len32)) {
                    o->extent_map.punch_hole(offset, length,
                                             &wctx->old_extents);

                    uint64_t b_off0 = b_off_reuse;
                    bufferlist padded_bl = bl;
                    _pad_zeros(&padded_bl, &b_off0, chunk_size);

                    alloc_len = alloc_len32;
                    wctx->write(offset, ep->blob, alloc_len, b_off0,
                                padded_bl, b_off_reuse, length, false, false);
                    return;
                }
            }
            ++ep;
            any_change = true;
        }

        if (prev_ep != end && prev_ep->logical_offset >= min_off) {
            uint64_t bstart = prev_ep->blob_start();
            if (bstart <= offset && bstart >= min_off &&
                prev_ep->blob->get_blob().is_mutable()) {
                uint64_t b_off = offset - bstart;
                uint32_t alloc_len32 = alloc_len;
                if (prev_ep->blob->can_reuse_blob(min_alloc_size_, max_bsize,
                                                   b_off, &alloc_len32) &&
                    !wctx->has_conflict(prev_ep->blob, b_off, alloc_len32)) {
                    o->extent_map.punch_hole(offset, length,
                                             &wctx->old_extents);

                    uint64_t b_off0 = b_off;
                    bufferlist padded_bl = bl;
                    _pad_zeros(&padded_bl, &b_off0, chunk_size);

                    alloc_len = alloc_len32;
                    wctx->write(offset, prev_ep->blob, alloc_len, b_off0,
                                padded_bl, b_off, length, false, false);
                    return;
                }
            }
            if (prev_ep != begin) {
                --prev_ep;
                any_change = true;
            } else {
                prev_ep = end;
            }
        }
    } while (any_change);

    uint64_t b_off = offset % alloc_len;
    uint64_t b_off0 = b_off;

    o->extent_map.punch_hole(offset, length, &wctx->old_extents);

    bufferlist padded_bl = bl;
    _pad_zeros(&padded_bl, &b_off0, chunk_size);

    BlobRef b = new Blob();
    b->get();
    b->set_collection(ch);

    wctx->write(offset, b, alloc_len, b_off0, padded_bl, b_off, length,
                min_alloc_size_ != block_size_, true);
    if (perf_) {
        perf_->inc(l_bluestore_write_small);
        perf_->inc(l_bluestore_write_small_bytes, length);
        perf_->inc(l_bluestore_write_new);
    }
}

void BlueStore::_do_write_big(TransContext *txc, Collection *ch, OnodeRef o,
                              uint64_t offset, uint64_t length,
                              bufferlist &bl, uint64_t bl_start,
                              WriteContext *wctx) {
    uint64_t max_bsize = std::max(wctx->target_blob_size, min_alloc_size_);
    uint64_t bl_pos = bl_start;

    while (length > 0) {
        uint64_t min_off = offset >= max_bsize ? offset - max_bsize : 0;
        uint64_t l = max_bsize - offset % max_bsize;
        l = std::min(l, length);

        bufferlist chunk_bl;
        chunk_bl.substr_of(bl, bl_pos, l);

        if (chunk_bl.is_zero()) {
            o->extent_map.punch_hole(offset, l, &wctx->old_extents);
            bl_pos += l;
            offset += l;
            length -= l;
            continue;
        }

        if (cfg_.prefer_deferred_size &&
            l <= cfg_.prefer_deferred_size * 2) {
            auto ep = o->extent_map.seek_lextent(offset);
            BigDeferredWriteContext head_info, tail_info;
            bool will_defer = (ep != o->extent_map.end()) &&
                _can_defer(head_info, ep, offset, l);
            uint64_t offset_next = offset + head_info.used;
            uint64_t remaining = l - head_info.used;
            if (will_defer && remaining &&
                remaining <= cfg_.prefer_deferred_size) {
                auto ep_next = o->extent_map.seek_lextent(offset_next);
                will_defer =
                    (ep_next != o->extent_map.end()) &&
                    _can_defer(tail_info, ep_next, offset_next, remaining) &&
                    remaining == tail_info.used;
            } else if (will_defer && remaining) {
                will_defer = false;
            }
            if (will_defer) {
                will_defer = _apply_defer(head_info);
                if (will_defer && remaining) {
                    will_defer = _apply_defer(tail_info);
                }
            }
            if (will_defer) {
                _do_write_big_apply_deferred(txc, o, head_info, bl, bl_pos,
                                             wctx);
                if (remaining) {
                    _do_write_big_apply_deferred(txc, o, tail_info, bl,
                                                 bl_pos, wctx);
                }
                offset += l;
                length -= l;
                continue;
            }
        }

        o->extent_map.punch_hole(offset, l, &wctx->old_extents);

        auto it = o->extent_map.seek_lextent(offset);
        auto search_start = (it != o->extent_map.end())
            ? it
            : o->extent_map.begin();

        BlobRef b = nullptr;
        uint64_t b_off = 0;

        auto begin = o->extent_map.begin();
        auto end = o->extent_map.end();
        auto ep = search_start;
        auto prev_ep = (search_start != begin) ? std::prev(search_start) : end;

        bool any_change;
        do {
            any_change = false;

            if (ep != end && ep->logical_offset < offset + max_bsize) {
                uint64_t bstart = ep->blob_start();
                if (!(bstart > offset || bstart < min_off) &&
                    ep->blob->get_blob().is_mutable()) {
                    b_off = offset - bstart;
                    uint32_t l32 = l;
                    if (ep->blob->can_reuse_blob(min_alloc_size_, max_bsize,
                                                 b_off, &l32)) {
                        b = ep->blob;
                        l = l32;
                        prev_ep = end;
                    } else {
                        ++ep;
                        any_change = true;
                    }
                } else {
                    ++ep;
                    any_change = true;
                }
            }

            if (b == nullptr && prev_ep != end &&
                prev_ep->logical_offset >= min_off) {
                uint64_t bstart = prev_ep->blob_start();
                if (bstart <= offset && bstart >= min_off &&
                    prev_ep->blob->get_blob().is_mutable()) {
                    b_off = offset - bstart;
                    uint32_t l32 = l;
                    if (prev_ep->blob->can_reuse_blob(min_alloc_size_,
                                                      max_bsize, b_off,
                                                      &l32)) {
                        b = prev_ep->blob;
                        l = l32;
                    } else if (prev_ep != begin) {
                        --prev_ep;
                        any_change = true;
                    } else {
                        prev_ep = end;
                    }
                } else if (prev_ep != begin) {
                    --prev_ep;
                    any_change = true;
                } else {
                    prev_ep = end;
                }
            }
        } while (b == nullptr && any_change);

        bool new_blob = (b == nullptr);
        if (new_blob) {
            b = new Blob();
            b->get();
            b->set_collection(ch);
            b_off = 0;
        }

        wctx->write(offset, b, l, b_off, chunk_bl, b_off, l, false, new_blob);

        if (perf_) {
            perf_->inc(l_bluestore_write_big);
            perf_->inc(l_bluestore_write_big_bytes, l);
            if (new_blob) perf_->inc(l_bluestore_write_new);
        }

        bl_pos += l;
        offset += l;
        length -= l;
    }
}

int BlueStore::_do_alloc_write(TransContext *txc, OnodeRef o,
                               WriteContext *wctx) {
    if (wctx->writes.empty()) return 0;

    uint64_t need = 0;
    for (auto &wi : wctx->writes) {
        uint32_t valid_ondisk = 0;
        for (const auto &e : wi.b->get_blob().get_extents()) {
            if (e.is_valid()) valid_ondisk += e.length;
        }
        uint32_t target_len = wi.new_blob ? wi.blob_length
                                          : wi.b->get_blob().get_logical_length();
        need += target_len - valid_ondisk;
    }

    PExtentVector prealloc;
    if (need > 0) {
        if (should_inject(cfg_.inject_write_err_rate)) {
            return -ENOSPC;
        }
        int64_t r = alloc_->allocate(need, min_alloc_size_, 0, 0, &prealloc);
        if (r < 0) {
            return -ENOSPC;
        }
    }

    size_t prealloc_pos = 0;

    for (auto &wi : wctx->writes) {
        BlobRef b = wi.b;
        auto &dblob = b->dirty_blob();

        uint32_t valid_ondisk = 0;
        for (const auto &e : dblob.get_extents()) {
            if (e.is_valid()) valid_ondisk += e.length;
        }
        uint64_t new_alloc_len;
        if (wi.new_blob) {
            new_alloc_len = wi.blob_length - valid_ondisk;
        } else {
            new_alloc_len = dblob.get_logical_length() - valid_ondisk;
            new_alloc_len = (new_alloc_len + min_alloc_size_ - 1) & ~(min_alloc_size_ - 1);
        }

        PExtentVector new_extents;
        uint64_t remaining = new_alloc_len;
        while (remaining > 0 && prealloc_pos < prealloc.size()) {
            auto &pe = prealloc[prealloc_pos];
            uint64_t take = std::min<uint64_t>(pe.length, remaining);
            new_extents.emplace_back(pe.offset, take);
            txc->allocated.insert(pe.offset, take);
            remaining -= take;
            if (take == pe.length) {
                ++prealloc_pos;
            } else {
                prealloc[prealloc_pos].offset += take;
                prealloc[prealloc_pos].length -= take;
            }
        }

        if (wi.new_blob) {
            dblob.allocated(0, wi.blob_length, new_extents);
            if (!dblob.has_csum() && cfg_.csum_type != CSUM_NONE) {
                dblob.init_csum(cfg_.csum_type, wctx->csum_order,
                                wi.blob_length);
            }
        } else {
            auto &exts = dblob.dirty_extents();
            while (!exts.empty() && !exts.back().is_valid()) {
                exts.pop_back();
            }
            for (auto &ne : new_extents) {
                exts.push_back(ne);
            }

            if (dblob.has_csum()) {
                uint32_t csum_chunk = dblob.get_csum_chunk_size();
                uint32_t needed = dblob.get_logical_length() / csum_chunk;
                uint32_t have = dblob.csum_data.length() /
                    dblob.get_csum_value_size();
                if (needed > have) {
                    buffer::ptr new_csum =
                        buffer::create(dblob.get_csum_value_size() * needed);
                    if (have > 0) {
                        std::memcpy(new_csum.c_str(), dblob.csum_data.c_str(),
                                    have * dblob.get_csum_value_size());
                    }
                    dblob.csum_data = std::move(new_csum);
                }
            }
        }

        if (dblob.has_csum()) {
            dblob.calc_csum(wi.b_off, wi.bl, block_size_);
        }

        b->get_ref(wi.b_off0, wi.length0, min_alloc_size_);

        if (wi.mark_unused) {
            uint64_t b_off = wi.b_off0;
            uint64_t b_end = b_off + wi.length0;
            if (b_off > 0) {
                dblob.add_unused(0, b_off);
            }
            uint64_t llen = dblob.get_logical_length();
            if (b_end < llen) {
                dblob.add_unused(b_end, llen - b_end);
            }
            dblob.mark_used(b_off, wi.length0);
        }

        o->extent_map.set_lextent(wi.logical_offset, wi.b_off0, wi.length0, b,
                                  nullptr);

        _buffer_cache_write(txc, b, wi.b_off0, wi.bl, 0);

        if (wi.bl.length() > 0) {
            uint64_t phys_off = _blob_to_phys(dblob, wi.b_off);

            // Deferred write decision: use deferred path for small writes
            if (wi.bl.length() < cfg_.prefer_deferred_size) {
                bluestore_deferred_op_t *op =
                    _get_deferred_op(txc, wi.bl.length());
                op->op = bluestore_deferred_op_t::OP_WRITE;
                op->extents.emplace_back(phys_off, wi.bl.length());
                op->data = wi.bl;
            } else {
                bdev_->aio_write(phys_off, wi.bl, &txc->ioc, false);
            }
        }
    }

    return 0;
}

void BlueStore::_wctx_finish(TransContext *txc, WriteContext *wctx) {
    for (auto &oe : wctx->old_extents) {
        PExtentVector released;
        oe.e.blob->put_ref(oe.e.blob_offset, oe.e.length, &released);
        for (auto &r : released) {
            txc->released.insert(r.offset, r.length);
        }
    }
    wctx->old_extents.clear();
}

// Deferred write implementation

bluestore_deferred_op_t *BlueStore::_get_deferred_op(TransContext *txc,
                                                     uint64_t len) {
    if (!txc->deferred_txn) {
        txc->deferred_txn = new bluestore_deferred_transaction_t;
    }
    txc->deferred_txn->ops.push_back(bluestore_deferred_op_t());
    return &txc->deferred_txn->ops.back();
}

void BlueStore::_deferred_queue(TransContext *txc) {
    deferred_writer_->queue(txc);
}

void BlueStore::_deferred_batch_aio_finish(DeferredBatch *b) {
    deferred_writer_->flush_done(b);
    std::lock_guard<std::mutex> lg(kv_lock_);
    if (!kv_sync_in_progress_) {
        kv_sync_in_progress_ = true;
        kv_cond_.notify_one();
    }
}

int BlueStore::_deferred_replay() {
    auto it = db_->get_iterator(PREFIX_DEFERRED);
    it->seek_to_first();

    int count = 0;
    while (it->valid()) {
        bluestore_deferred_transaction_t deferred_txn;
        bufferlist bl = it->value();
        auto p = bl.cbegin();
        cxxlab::decode(deferred_txn, p);

        // Create a transaction and process it
        auto coll = get_collection(0);  // Use default collection
        if (!coll) {
            // Create default collection if needed
            coll = create_collection(0, 0);
        }

        TransContext *txc = _txc_create(coll.get());
        txc->deferred_txn = new bluestore_deferred_transaction_t(deferred_txn);
        txc->set_state(TransContext::STATE_KV_DONE);
        _txc_state_proc(txc);

        count++;
        it->next();
    }

    return count;
}

// Buffer cache integration

void BlueStore::_buffer_cache_write(TransContext *txc, BlobRef b,
                                    uint64_t offset, bufferlist &bl,
                                    unsigned flags) {
    BufferCache *cache = buffer_cache_.get();
    if (!cache || !bl.length()) return;

    b->bc().write(cache, txc->seq, static_cast<uint32_t>(offset), bl, flags);
    txc->blobs_written.insert(b);
}

void BlueStore::_finish_write(TransContext *txc) {
    BufferCache *cache = buffer_cache_.get();
    if (!cache) return;

    for (auto *b : txc->blobs_written) {
        b->bc().finish_write(cache, txc->seq);
    }
    txc->blobs_written.clear();
}

bool BlueStore::_can_defer(BigDeferredWriteContext &dctx,
                           ExtentMap::iterator ep,
                           uint64_t offset, uint64_t l) {
    bool res = false;
    auto &blob = ep->blob->get_blob();
    if (offset >= ep->blob_start() && blob.is_mutable()) {
        dctx.off = offset;
        dctx.b_off = offset - ep->blob_start();
        uint64_t chunk_size = blob.get_chunk_size(block_size_);
        uint64_t ondisk = blob.get_ondisk_length();
        if (dctx.b_off >= ondisk) {
            return false;
        }
        dctx.used = std::min(l, ondisk - dctx.b_off);
        dctx.head_read = p2phase<uint64_t>(dctx.b_off, chunk_size);
        dctx.tail_read = p2nphase<uint64_t>(dctx.b_off + dctx.used, chunk_size);
        dctx.b_off -= dctx.head_read;
        cxxlab_assert(dctx.b_off % chunk_size == 0);
        cxxlab_assert(dctx.blob_aligned_len() % chunk_size == 0);
        res = dctx.blob_aligned_len() < cfg_.prefer_deferred_size &&
            dctx.blob_aligned_len() <= ondisk &&
            blob.is_allocated(dctx.b_off, dctx.blob_aligned_len());
        if (res) {
            dctx.blob_ref = ep->blob;
            dctx.blob_start = ep->blob_start();
        }
    }
    return res;
}

bool BlueStore::_apply_defer(BigDeferredWriteContext &dctx) {
    auto &blob = dctx.blob_ref->get_blob();
    uint64_t blob_off = 0;
    uint64_t write_start = dctx.b_off;
    uint64_t write_end = dctx.b_off + dctx.blob_aligned_len();
    for (const auto &ex : blob.get_extents()) {
        uint64_t ext_end = blob_off + ex.length;
        if (write_start < ext_end && write_end > blob_off) {
            uint64_t s = std::max(write_start, blob_off);
            uint64_t e = std::min(write_end, ext_end);
            uint64_t phys_s = ex.offset + (s - blob_off);
            uint64_t phys_len = e - s;
            if (ex.offset < phys_s ||
                ex.offset + ex.length > phys_s + phys_len) {
                dctx.res_extents.emplace_back(
                    bluestore_pextent_t(phys_s, phys_len));
            } else {
                return false;
            }
        }
        blob_off = ext_end;
    }
    return true;
}

void BlueStore::_do_write_big_apply_deferred(
    TransContext *txc, OnodeRef o, BigDeferredWriteContext &dctx,
    bufferlist &bl, uint64_t &bl_pos, WriteContext *wctx) {
    bufferlist out;
    if (dctx.head_read) {
        int r = _do_read(o, dctx.off - dctx.head_read, dctx.head_read, out);
        cxxlab_assert(r >= 0);
        if (out.length() < dctx.head_read) {
            out.append_zero(dctx.head_read - out.length());
        }
    }
    bufferlist data;
    data.substr_of(bl, bl_pos, dctx.used);
    out.claim_append(data);
    if (dctx.tail_read) {
        bufferlist tail_bl;
        int r = _do_read(o, dctx.off + dctx.used, dctx.tail_read, tail_bl);
        cxxlab_assert(r >= 0);
        if (tail_bl.length() < dctx.tail_read) {
            tail_bl.append_zero(dctx.tail_read - tail_bl.length());
        }
        out.claim_append(tail_bl);
    }
    _buffer_cache_write(txc, dctx.blob_ref, dctx.b_off, out, 0);
    dctx.blob_ref->dirty_blob().calc_csum(dctx.b_off, out, block_size_);
    auto le = o->extent_map.set_lextent(
        static_cast<uint32_t>(dctx.off),
        static_cast<uint32_t>(dctx.off - dctx.blob_start),
        static_cast<uint32_t>(dctx.used), dctx.blob_ref,
        &wctx->old_extents, min_alloc_size_);
    dctx.blob_ref->dirty_blob().mark_used(le->blob_offset, le->length);
    bluestore_deferred_op_t *op = _get_deferred_op(txc, out.length());
    op->op = bluestore_deferred_op_t::OP_WRITE;
    op->extents.swap(dctx.res_extents);
    op->data = std::move(out);
    bl_pos += dctx.used;
}

// FSCK implementation

namespace {
void report_fsck(const BlueStore::FsckProgressCallback &cb,
                 BlueStore::FSCKDepth depth, const char *phase,
                 uint64_t processed, uint64_t total, int64_t errors) {
    if (!cb) return;
    BlueStore::FsckProgress p;
    p.depth = depth;
    p.phase = phase;
    p.processed = processed;
    p.total = total;
    p.errors = errors;
    cb(p);
}
}  // namespace

int BlueStore::fsck(bool deep, FsckProgressCallback cb) {
    return _fsck(deep ? FSCK_DEEP : FSCK_REGULAR, false, std::move(cb));
}

int BlueStore::repair(bool deep, FsckProgressCallback cb) {
    return _fsck(deep ? FSCK_DEEP : FSCK_REGULAR, true, std::move(cb));
}

int BlueStore::quick_fix(FsckProgressCallback cb) {
    return _fsck(FSCK_SHALLOW, true, std::move(cb));
}

int BlueStore::_fsck(FSCKDepth depth, bool repair,
                     FsckProgressCallback cb) {
    int64_t errors = 0;
    int64_t repaired = 0;

    bool live_mode = mounted_ && db_ && bdev_ && fm_;

    if (live_mode) {
        errors += _fsck_check_collections(depth, cb);

        std::set<uint64_t> used_blocks;
        errors += _fsck_check_objects(depth, used_blocks, cb);

        if (depth != FSCK_SHALLOW) {
            errors += _fsck_check_freelist(used_blocks, repair, depth, cb);
            if (repair) repaired = errors;
        }

        return repair ? (errors - repaired) : errors;
    }

    // Standalone mode: open separate DB and bdev for fsck
    std::unique_ptr<KeyValueDB> fsck_db;
    std::unique_ptr<BlockDevice> fsck_bdev;
    FreelistManager *original_fm = fm_;
    FreelistManager *fsck_fm = nullptr;

    if (!repair) {
        fsck_db = KeyValueDB::create("rocksdb", cfg_.db_path);
        if (!fsck_db) return -EIO;

        int r = fsck_db->init();
        if (r < 0) return r;

        std::ostringstream oss;
        r = fsck_db->open_read_only(oss);
        if (r < 0) return r;
    } else {
        fsck_db = KeyValueDB::create("rocksdb", cfg_.db_path);
        if (!fsck_db) return -EIO;

        fsck_db->set_merge_operator(std::string(PREFIX_ALLOC_BITMAP),
                                    std::make_shared<XorMergeOperator>());
        int r = fsck_db->init();
        if (r < 0) return r;

        std::ostringstream oss;
        r = fsck_db->open(oss);
        if (r < 0) return r;
    }

    fsck_bdev = BlockDevice::create(cfg_.bdev_path, _aio_callback, this);
    if (!fsck_bdev) {
        return -EIO;
    }
    int r = fsck_bdev->open(cfg_.bdev_path);
    if (r < 0) {
        return r;
    }

    fsck_fm = FreelistManager::create(
        cfg_.freelist_type.empty() ? "bitmap" : cfg_.freelist_type,
        PREFIX_SUPER, PREFIX_ALLOC_BITMAP);
    if (!fsck_fm) {
        fsck_bdev->close();
        return -EIO;
    }

    auto cfg_reader = [&fsck_db](const std::string &key, std::string *value) -> int {
        bufferlist bl;
        int r = fsck_db->get(PREFIX_SUPER, key, &bl);
        if (r < 0)
            return r;
        *value = std::string(bl.c_str(), bl.length());
        return 0;
    };
    r = fsck_fm->init(fsck_db.get(), !repair, cfg_reader);
    if (r < 0) {
        delete fsck_fm;
        fsck_bdev->close();
        return r;
    }

    db_.swap(fsck_db);
    bdev_.swap(fsck_bdev);
    fm_ = fsck_fm;

    auto guard = make_scope_guard([&] {
        db_.swap(fsck_db);
        if (bdev_) bdev_->close();
        bdev_.swap(fsck_bdev);
        if (fm_) delete fm_;
        fm_ = original_fm;
    });

    errors += _fsck_check_collections(depth, cb);

    std::set<uint64_t> used_blocks;
    errors += _fsck_check_objects(depth, used_blocks, cb);

    if (depth != FSCK_SHALLOW && bdev_ && fm_) {
        errors += _fsck_check_freelist(used_blocks, repair, depth, cb);
        if (repair) {
            repaired = errors;
        }
    }

    return repair ? (errors - repaired) : errors;
}

int64_t BlueStore::_fsck_check_collections(FSCKDepth depth,
                                           FsckProgressCallback cb) {
    int64_t errors = 0;

    report_fsck(cb, depth, "collections", 0, 0, 0);

    auto it = db_->get_iterator(PREFIX_COLL);
    if (!it) return -EIO;

    uint64_t processed = 0;
    it->seek_to_first();
    while (it->valid()) {
        std::string key = it->key();
        bufferlist bl = it->value();

        // Try to decode collection
        uint64_t coll_id = 0;
        const char *p = key.c_str();
        key_decode_u64(p, &coll_id);

        if (bl.length() == 0) {
            errors++;
            it->next();
            continue;
        }

        // Try to decode cnode
        auto blp = bl.cbegin();
        bluestore_cnode_t cnode;
        try {
            cxxlab::decode(cnode, blp);
        } catch (...) {
            errors++;
        }

        it->next();
        ++processed;
    }

    report_fsck(cb, depth, "collections", processed, processed, errors);
    return errors;
}

int64_t BlueStore::_fsck_check_objects(FSCKDepth depth,
                                       std::set<uint64_t> &used_blocks,
                                       FsckProgressCallback cb) {
    int64_t errors = 0;

    // Use cfg_.min_alloc_size as fallback if min_alloc_size_ is not set
    uint64_t min_alloc = min_alloc_size_ > 0 ? min_alloc_size_ : cfg_.min_alloc_size;
    if (min_alloc == 0) {
        // Cannot check without min_alloc_size
        return 0;
    }

    report_fsck(cb, depth, "objects", 0, 0, 0);

    auto it = db_->get_iterator(PREFIX_OBJ);
    if (!it) return -EIO;

    // Pre-scan to estimate total object count (count only, no decode)
    uint64_t total = 0;
    if (cb) {
        it->seek_to_first();
        while (it->valid()) {
            ++total;
            it->next();
        }
    }
    report_fsck(cb, depth, "objects", 0, total, 0);

    uint64_t processed = 0;
    uint64_t report_interval = std::max<uint64_t>(100, total / 100);
    it->seek_to_first();
    while (it->valid()) {
        std::string key = it->key();
        bufferlist bl = it->value();

        // Skip extent shard keys
        if (!key.empty() && key.back() == 'x') {
            it->next();
            continue;
        }

        if (bl.length() == 0) {
            errors++;
            it->next();
            continue;
        }

        // Try to decode onode
        auto blp = bl.cbegin();
        Onode on(ghobject_t(), key);
        try {
            on.decode(blp);
        } catch (...) {
            errors++;
            it->next();
            continue;
        }

        // Check extents and track used blocks
        for (const auto &ext : on.extent_map) {
            if (!ext.blob) {
                errors++;
                continue;
            }

            const auto &blob = ext.blob->get_blob();
            for (const auto &pext : blob.get_extents()) {
                if (!pext.is_valid()) continue;

                // Track used blocks (in min_alloc_size units)
                uint64_t start_block = pext.offset / min_alloc;
                uint64_t num_blocks = (pext.length + min_alloc - 1) / min_alloc;

                for (uint64_t i = 0; i < num_blocks; i++) {
                    uint64_t block = start_block + i;
                    auto result = used_blocks.insert(block);
                    if (!result.second) {
                        // Block already used - overlap detected
                        if (depth != FSCK_SHALLOW) {
                            errors++;
                        }
                    }
                }

                // Check extent bounds (REGULAR and DEEP only)
                if (depth != FSCK_SHALLOW) {
                    if (pext.offset + pext.length > bdev_->get_size()) {
                        errors++;
                    }
                }

                // Deep check: try to read data
                if (depth == FSCK_DEEP) {
                    bufferlist data_bl;
                    int r = bdev_->read(pext.offset, pext.length, &data_bl, nullptr, false);
                    if (r < 0) {
                        errors++;
                    }
                }
            }
        }

        it->next();
        ++processed;
        if (cb && (processed % report_interval) == 0) {
            report_fsck(cb, depth, "objects", processed, total, errors);
        }
    }

    report_fsck(cb, depth, "objects", processed, processed, errors);
    return errors;
}

int64_t BlueStore::_fsck_check_freelist(const std::set<uint64_t> &used_blocks,
                                        bool repair, FSCKDepth depth,
                                        FsckProgressCallback cb) {
    int64_t errors = 0;

    report_fsck(cb, depth, "freelist", 0, 0, 0);

    // Use cfg_.min_alloc_size as fallback if min_alloc_size_ is not set
    uint64_t min_alloc = min_alloc_size_ > 0 ? min_alloc_size_ : cfg_.min_alloc_size;
    if (min_alloc == 0) {
        // Cannot check without min_alloc_size
        return 0;
    }

    // Enumerate freelist
    std::set<uint64_t> free_blocks;
    fm_->enumerate_reset();
    uint64_t offset, length;
    while (fm_->enumerate_next(db_.get(), &offset, &length)) {
        uint64_t start_block = offset / min_alloc;
        uint64_t num_blocks = (length + min_alloc - 1) / min_alloc;

        for (uint64_t i = 0; i < num_blocks; i++) {
            free_blocks.insert(start_block + i);
        }
    }

    // Check for conflicts: blocks that are both used and free
    for (uint64_t block : used_blocks) {
        if (free_blocks.count(block) > 0) {
            errors++;
            if (repair) {
                Transaction t = db_->get_transaction();
                fm_->allocate(block * min_alloc, min_alloc, t);
                db_->submit_transaction_sync(t);
            }
        }
    }

    // Check for leaked blocks: allocated (not in free list) but not used
    // by any object. Skip if freelist enumeration returned no data (standalone
    // mode without merge operator can't read bitmap merges).
    if (free_blocks.empty()) {
        return errors;
    }

    uint64_t total_blocks = fm_->get_size() / min_alloc;
    for (uint64_t block = 0; block < total_blocks; ++block) {
        if (used_blocks.count(block) == 0 &&
            free_blocks.count(block) == 0) {
            errors++;
            if (repair) {
                Transaction t = db_->get_transaction();
                fm_->release(block * min_alloc, min_alloc, t);
                db_->submit_transaction_sync(t);
            }
        }
    }

    report_fsck(cb, depth, "freelist", 0, 0, errors);
    return errors;
}

const char **BlueStore::get_tracked_conf_keys() const {
    static const char *keys[] = {
        "buffer_cache_size",
        "onode_cache_size",
        "inject_read_err_rate",
        "inject_write_err_rate",
        "inject_kv_err_rate",
        nullptr,
    };
    return keys;
}

void BlueStore::handle_conf_change(const BlueStoreConfig &cfg,
                                   const std::set<std::string> &changed) {
    if (changed.count("buffer_cache_size")) {
        if (buffer_cache_) {
            buffer_cache_->set_max_bytes(cfg.buffer_cache_size);
        }
    }
    if (changed.count("onode_cache_size")) {
        std::lock_guard<std::mutex> l(coll_lock_);
        for (auto &[id, coll] : coll_map_) {
            coll->set_onode_cache_size(cfg.onode_cache_size);
        }
    }
    // inject_*_err_rate: stored in cfg_ (updated by reload_config);
    // should_inject reads cfg_ directly, no extra action needed.
}

void BlueStore::reload_config(const BlueStoreConfig &new_cfg) {
    std::set<std::string> changed;
    if (new_cfg.buffer_cache_size != cfg_.buffer_cache_size)
        changed.insert("buffer_cache_size");
    if (new_cfg.onode_cache_size != cfg_.onode_cache_size)
        changed.insert("onode_cache_size");
    if (new_cfg.inject_read_err_rate != cfg_.inject_read_err_rate)
        changed.insert("inject_read_err_rate");
    if (new_cfg.inject_write_err_rate != cfg_.inject_write_err_rate)
        changed.insert("inject_write_err_rate");
    if (new_cfg.inject_kv_err_rate != cfg_.inject_kv_err_rate)
        changed.insert("inject_kv_err_rate");
    cfg_ = new_cfg;
    handle_conf_change(cfg_, changed);
}

void BlueStore::_init_logger() {
    PerfCountersBuilder b("bluestore", l_bluestore_first, l_bluestore_last);

    b.add_u64(l_bluestore_allocated, "allocated", "Sum for allocated bytes",
              "al_b", PerfCountersBuilder::PRIO_CRITICAL, UNIT_BYTES);
    b.add_u64(l_bluestore_stored, "stored", "Sum for stored bytes", "st_b",
              PerfCountersBuilder::PRIO_CRITICAL, UNIT_BYTES);
    b.add_u64(l_bluestore_fragmentation, "fragmentation_micros",
              "How fragmented bluestore free space is");
    b.add_u64(l_bluestore_alloc_unit, "alloc_unit",
              "allocation unit size in bytes", "au_b",
              PerfCountersBuilder::PRIO_CRITICAL, UNIT_BYTES);

    b.add_time_avg(l_bluestore_state_prepare_lat, "state_prepare_lat",
                   "Average prepare state latency", "sprl",
                   PerfCountersBuilder::PRIO_USEFUL);
    b.add_time_avg(l_bluestore_state_aio_wait_lat, "state_aio_wait_lat",
                   "Average aio_wait state latency", "sawl",
                   PerfCountersBuilder::PRIO_INTERESTING);
    b.add_time_avg(l_bluestore_state_io_done_lat, "state_io_done_lat",
                   "Average io_done state latency", "sidl",
                   PerfCountersBuilder::PRIO_USEFUL);
    b.add_time_avg(l_bluestore_state_kv_queued_lat, "state_kv_queued_lat",
                   "Average kv_queued state latency", "skql",
                   PerfCountersBuilder::PRIO_USEFUL);
    b.add_time_avg(l_bluestore_state_kv_committing_lat,
                   "state_kv_commiting_lat",
                   "Average kv_commiting state latency", "skcl",
                   PerfCountersBuilder::PRIO_USEFUL);
    b.add_time_avg(l_bluestore_state_kv_done_lat, "state_kv_done_lat",
                   "Average kv_done state latency", "skdl",
                   PerfCountersBuilder::PRIO_USEFUL);
    b.add_time_avg(l_bluestore_state_deferred_queued_lat,
                   "state_deferred_queued_lat",
                   "Average deferred_queued state latency", "sdql",
                   PerfCountersBuilder::PRIO_USEFUL);
    b.add_time_avg(l_bluestore_state_deferred_cleanup_lat,
                   "state_deferred_cleanup_lat",
                   "Average cleanup state latency", "sdcl",
                   PerfCountersBuilder::PRIO_USEFUL);
    b.add_time_avg(l_bluestore_state_finishing_lat, "state_finishing_lat",
                   "Average finishing state latency", "sfnl",
                   PerfCountersBuilder::PRIO_USEFUL);

    b.add_time_avg(l_bluestore_submit_lat, "txc_submit_lat",
                   "Average submit latency", "s_l",
                   PerfCountersBuilder::PRIO_CRITICAL);
    b.add_time_avg(l_bluestore_commit_lat, "txc_commit_lat",
                   "Average commit latency", "c_l",
                   PerfCountersBuilder::PRIO_CRITICAL);
    b.add_u64_counter(l_bluestore_txc, "txc_count", "Transactions committed");

    b.add_time_avg(l_bluestore_read_lat, "read_lat", "Average read latency",
                   "r_l", PerfCountersBuilder::PRIO_CRITICAL);
    b.add_u64_counter(l_bluestore_read_eio, "read_eio",
                      "Read EIO errors propagated to high level callers");

    b.add_time_avg(l_bluestore_kv_flush_lat, "kv_flush_lat",
                   "Average kv_thread flush latency", "kfsl",
                   PerfCountersBuilder::PRIO_INTERESTING);
    b.add_time_avg(l_bluestore_kv_commit_lat, "kv_commit_lat",
                   "Average kv_thread commit latency", "kcol",
                   PerfCountersBuilder::PRIO_USEFUL);
    b.add_time_avg(l_bluestore_kv_sync_lat, "kv_sync_lat",
                   "Average kv_sync thread latency", "kscl",
                   PerfCountersBuilder::PRIO_INTERESTING);
    b.add_time_avg(l_bluestore_kv_final_lat, "kv_final_lat",
                   "Average kv_finalize thread latency", "kfll",
                   PerfCountersBuilder::PRIO_INTERESTING);

    b.add_u64_counter(l_bluestore_write_big, "write_big",
                      "Large aligned writes into fresh blobs");
    b.add_u64_counter(l_bluestore_write_big_bytes, "write_big_bytes",
                      "Large aligned writes into fresh blobs (bytes)", nullptr,
                      PerfCountersBuilder::PRIO_DEBUGONLY, UNIT_BYTES);
    b.add_u64_counter(l_bluestore_write_small, "write_small",
                      "Small writes into existing or sparse small blobs");
    b.add_u64_counter(l_bluestore_write_small_bytes, "write_small_bytes",
                      "Small writes (bytes)", nullptr,
                      PerfCountersBuilder::PRIO_DEBUGONLY, UNIT_BYTES);
    b.add_u64_counter(l_bluestore_write_new, "write_new",
                      "Write into new blob");
    b.add_u64_counter(l_bluestore_write_pad_bytes, "write_pad_bytes",
                      "Sum for write-op padded bytes", nullptr,
                      PerfCountersBuilder::PRIO_DEBUGONLY, UNIT_BYTES);

    b.add_u64(l_bluestore_onodes, "onodes", "Number of onodes in cache");
    b.add_u64(l_bluestore_buffers, "buffers", "Number of buffers in cache");
    b.add_u64(l_bluestore_buffer_bytes, "buffer_bytes",
              "Number of bytes in buffer cache", nullptr,
              PerfCountersBuilder::PRIO_USEFUL, UNIT_BYTES);
    b.add_u64_counter(l_bluestore_buffer_hit_bytes, "buffer_hit_bytes",
                      "Cache hit bytes", nullptr,
                      PerfCountersBuilder::PRIO_USEFUL, UNIT_BYTES);
    b.add_u64_counter(l_bluestore_buffer_miss_bytes, "buffer_miss_bytes",
                      "Cache miss bytes", nullptr,
                      PerfCountersBuilder::PRIO_USEFUL, UNIT_BYTES);

    perf_ = b.create_perf_counters();
}

void BlueStore::_log_state_latency(TransContext *txc, int idx) {
    if (!perf_) return;
    auto now = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  now - txc->last_stamp)
                  .count();
    perf_->tinc(idx, static_cast<uint64_t>(ns));
    txc->last_stamp = now;
}

void BlueStore::_log_latency(int idx, uint64_t nanos) {
    if (!perf_) return;
    perf_->tinc(idx, nanos);
}

bool BlueStore::should_inject(double rate) const {
    if (rate <= 0) return false;
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 999999);
    return dist(rng) < static_cast<int>(rate * 1000000);
}

void BlueStore::_refresh_perf_counters() {
    if (!perf_) return;
    perf_->set(l_bluestore_alloc_unit, min_alloc_size_);

    if (bdev_ && alloc_) {
        perf_->set(l_bluestore_allocated,
                   bdev_->get_size() - alloc_->get_free());
    }

    uint64_t num_onodes = 0;
    {
        std::lock_guard<std::mutex> lg(coll_lock_);
        for (const auto &p : coll_map_) {
            num_onodes += p.second->get_onode_count();
        }
    }
    perf_->set(l_bluestore_onodes, num_onodes);

    if (buffer_cache_) {
        perf_->set(l_bluestore_buffers, buffer_cache_->get_num_buffers());
        perf_->set(l_bluestore_buffer_bytes, buffer_cache_->get_cur_bytes());
        perf_->set(l_bluestore_buffer_hit_bytes,
                   buffer_cache_->get_hit_bytes());
        perf_->set(l_bluestore_buffer_miss_bytes,
                   buffer_cache_->get_miss_bytes());
    }

    if (alloc_) {
        perf_->set(l_bluestore_fragmentation,
                   static_cast<uint64_t>(alloc_->get_fragmentation() * 1000));
    }
}

void BlueStore::dump_perf_counters(Formatter *f) {
    if (!perf_) return;
    perf_tracker_.update_from_perfcounters(*perf_);
    perf_->dump(f, false);
}

void BlueStore::BSPerfTracker::update_from_perfcounters(PerfCounters &perf) {
    commit_latency_ns.consume_next(perf.get_tavg_ns(l_bluestore_commit_lat));
}

uint64_t BlueStore::BSPerfTracker::get_commit_latency_avg() const {
    return commit_latency_ns.current_avg();
}

}  // namespace TOPNSPC
