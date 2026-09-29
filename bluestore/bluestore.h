#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bluestore/bluestore_config.h"
#include "bluestore/bluestore_constants.h"
#include "bluestore/bluestore_types.h"
#include "bluestore/collection.h"
#include "bluestore/trans_context.h"
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

    int queue_transactions(CollectionRef ch,
                           std::vector<BlueStoreTransaction> &tls,
                           std::function<void()> on_commit = nullptr);

    int read(CollectionRef c, const ghobject_t &oid, uint64_t offset,
             uint64_t length, bufferlist &bl);

    void txc_aio_finish(void *p);

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

    static void _aio_callback(void *handle, void *priv);

    TransContext *_txc_create(Collection *c);
    void _txc_state_proc(TransContext *txc);
    void _txc_add_transaction(TransContext *txc,
                              BlueStoreTransaction *bt);
    void _txc_finish_io(TransContext *txc);
    void _txc_write_nodes(TransContext *txc, Transaction t);
    void _txc_finalize_kv(TransContext *txc, Transaction t);
    void _txc_apply_kv(TransContext *txc);
    void _txc_committed_kv(TransContext *txc);
    void _txc_finish(TransContext *txc);
    void _txc_release_alloc(TransContext *txc);
    void _txc_aio_submit(TransContext *txc);

    int _do_write(TransContext *txc, Collection *ch, OnodeRef o,
                  uint64_t offset, uint64_t length, bufferlist &bl);
    void _do_write_data(TransContext *txc, Collection *ch, OnodeRef o,
                        uint64_t offset, uint64_t length, bufferlist &bl,
                        WriteContext *wctx);
    void _do_write_small(TransContext *txc, Collection *ch, OnodeRef o,
                         uint64_t offset, uint64_t length, bufferlist &bl,
                         WriteContext *wctx);
    void _do_write_big(TransContext *txc, Collection *ch, OnodeRef o,
                       uint64_t offset, uint64_t length, bufferlist &bl,
                       uint64_t bl_start, WriteContext *wctx);
    int _do_alloc_write(TransContext *txc, OnodeRef o, WriteContext *wctx);
    void _wctx_finish(TransContext *txc, WriteContext *wctx);
    void _choose_write_options(WriteContext *wctx);
    void _pad_zeros(bufferlist *bl, uint64_t *offset, uint64_t chunk_size);

    int _do_read(OnodeRef o, uint64_t offset, uint64_t length, bufferlist &bl);

    void _kv_start();
    void _kv_stop();
    void _kv_sync_thread_main();
    void _kv_finalize_thread_main();

    void _finisher_start();
    void _finisher_stop();
    void _finisher_thread_main();
    void _queue_finisher(std::function<void()> fn);

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

    std::thread kv_sync_thread_;
    std::thread kv_finalize_thread_;
    std::mutex kv_lock_;
    std::condition_variable kv_cond_;
    bool kv_sync_in_progress_ = false;
    std::atomic<bool> kv_stop_{false};
    std::deque<TransContext *> kv_queue_;
    std::deque<TransContext *> kv_queue_unsubmitted_;

    std::mutex kv_finalize_lock_;
    std::condition_variable kv_finalize_cond_;
    bool kv_finalize_in_progress_ = false;
    std::atomic<bool> kv_finalize_stop_{false};
    std::deque<TransContext *> kv_committing_to_finalize_;

    std::thread finisher_thread_;
    std::mutex finisher_lock_;
    std::condition_variable finisher_cond_;
    std::deque<std::function<void()>> finisher_queue_;
    std::atomic<bool> finisher_stop_{false};
};

}  // namespace TOPNSPC
