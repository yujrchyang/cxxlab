#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <mutex>
#include <set>
#include <vector>

#include "blk/io_context.h"
#include "bluestore/bluestore_types.h"
#include "bluestore/onode.h"
#include "common/interval_set.h"
#include "kv/key_value_db.h"

namespace TOPNSPC {

class Collection;
class OpSequencer;
class Blob;
class DeferredBatch;

struct TransContext {
    enum state_t : uint8_t {
        STATE_PREPARE,
        STATE_AIO_WAIT,
        STATE_IO_DONE,
        STATE_KV_QUEUED,
        STATE_KV_SUBMITTED,
        STATE_KV_DONE,
        STATE_DEFERRED_QUEUED,
        STATE_DEFERRED_CLEANUP,
        STATE_DEFERRED_DONE,
        STATE_FINISHING,
        STATE_DONE,
    };

    static const char *state_name(state_t s) {
        switch (s) {
        case STATE_PREPARE:
            return "PREPARE";
        case STATE_AIO_WAIT:
            return "AIO_WAIT";
        case STATE_IO_DONE:
            return "IO_DONE";
        case STATE_KV_QUEUED:
            return "KV_QUEUED";
        case STATE_KV_SUBMITTED:
            return "KV_SUBMITTED";
        case STATE_KV_DONE:
            return "KV_DONE";
        case STATE_DEFERRED_QUEUED:
            return "DEFERRED_QUEUED";
        case STATE_DEFERRED_CLEANUP:
            return "DEFERRED_CLEANUP";
        case STATE_DEFERRED_DONE:
            return "DEFERRED_DONE";
        case STATE_FINISHING:
            return "FINISHING";
        case STATE_DONE:
            return "DONE";
        default:
            return "UNKNOWN";
        }
    }

    Collection *ch = nullptr;
    OpSequencer *osr = nullptr;

    uint64_t seq = 0;
    uint64_t bytes = 0;
    uint64_t ios = 0;

    std::set<OnodeRef> onodes;
    Transaction t;
    std::list<std::function<void()>> on_commits;

    interval_set<uint64_t> allocated;
    interval_set<uint64_t> released;

    bluestore_deferred_transaction_t *deferred_txn = nullptr;

    IOContext ioc;
    bool had_ios = false;

    std::set<Blob *> blobs_written;

    std::chrono::steady_clock::time_point start;
    std::chrono::steady_clock::time_point last_stamp;

    TransContext(Collection *c, OpSequencer *o)
        : ch(c),
          osr(o),
          ioc(this),
          start(std::chrono::steady_clock::now()),
          last_stamp(start) {}

    ~TransContext() {
        delete deferred_txn;
    }

    TransContext(const TransContext &) = delete;
    TransContext &operator=(const TransContext &) = delete;

    void set_state(state_t s) { state_ = s; }
    state_t get_state() const { return state_; }

    void write_onode(OnodeRef &o) { onodes.insert(o); }
    void note_removed_object(OnodeRef &o) { onodes.erase(o); }

private:
    state_t state_ = STATE_PREPARE;
};

class OpSequencer {
public:
    OpSequencer() = default;
    ~OpSequencer() = default;

    OpSequencer(const OpSequencer &) = delete;
    OpSequencer &operator=(const OpSequencer &) = delete;

    void queue_new(TransContext *txc) {
        std::lock_guard<std::mutex> lg(qlock);
        txc->seq = ++last_seq;
        q.push_back(txc);
    }

    void drain() {
        std::unique_lock<std::mutex> lk(qlock);
        while (!q.empty()) {
            qcond.wait(lk);
        }
    }

    void flush() {
        std::unique_lock<std::mutex> lk(qlock);
        while (!q.empty()) {
            bool all_submitted = true;
            for (auto *txc : q) {
                if (txc->get_state() < TransContext::STATE_KV_SUBMITTED) {
                    all_submitted = false;
                    break;
                }
            }
            if (all_submitted) break;
            qcond.wait(lk);
        }
    }

    bool empty() {
        std::lock_guard<std::mutex> lg(qlock);
        return q.empty();
    }

    size_t size() {
        std::lock_guard<std::mutex> lg(qlock);
        return q.size();
    }

    std::mutex qlock;
    std::condition_variable qcond;
    std::deque<TransContext *> q;

    uint64_t last_seq = 0;
    std::atomic_int txc_with_unstable_io{0};

    DeferredBatch *deferred_pending = nullptr;
    DeferredBatch *deferred_running = nullptr;
    std::mutex deferred_lock;
};

struct BlueStoreTransaction {
    struct Op {
        enum Type : uint8_t {
            OP_NOP = 0,
            OP_TOUCH = 1,
            OP_CREATE = 2,
            OP_WRITE = 3,
            OP_ZERO = 4,
            OP_REMOVE = 5,
            OP_SETATTR = 6,
            OP_SETATTRS = 7,
            OP_OMAP_SETKEYS = 8,
            OP_OMAP_SETHEADER = 9,
            OP_OMAP_RMKEYS = 10,
            OP_OMAP_CLEAR = 11,
        };
        Type type = OP_NOP;
        ghobject_t oid;
        uint64_t offset = 0;
        uint64_t length = 0;
        bufferlist data;
        std::string attr_name;
        bufferptr attr_value;
        std::map<std::string, bufferptr> attrs;
        std::map<std::string, bufferlist> omap_keys;
        std::set<std::string> omap_rmkeys;
    };

    std::vector<Op> ops;

    void nop() {
        Op op;
        op.type = Op::OP_NOP;
        ops.push_back(std::move(op));
    }

    void touch(const ghobject_t &oid) {
        Op op;
        op.type = Op::OP_TOUCH;
        op.oid = oid;
        ops.push_back(std::move(op));
    }

    void create(const ghobject_t &oid) {
        Op op;
        op.type = Op::OP_CREATE;
        op.oid = oid;
        ops.push_back(std::move(op));
    }

    void write(const ghobject_t &oid, uint64_t off, uint64_t len,
               bufferlist &bl) {
        Op op;
        op.type = Op::OP_WRITE;
        op.oid = oid;
        op.offset = off;
        op.length = len;
        op.data = bl;
        ops.push_back(std::move(op));
    }

    void zero(const ghobject_t &oid, uint64_t off, uint64_t len) {
        Op op;
        op.type = Op::OP_ZERO;
        op.oid = oid;
        op.offset = off;
        op.length = len;
        ops.push_back(std::move(op));
    }

    void remove(const ghobject_t &oid) {
        Op op;
        op.type = Op::OP_REMOVE;
        op.oid = oid;
        ops.push_back(std::move(op));
    }

    void setattr(const ghobject_t &oid, const std::string &name,
                 const bufferptr &val) {
        Op op;
        op.type = Op::OP_SETATTR;
        op.oid = oid;
        op.attr_name = name;
        op.attr_value = val;
        ops.push_back(std::move(op));
    }

    void setattrs(const ghobject_t &oid,
                  const std::map<std::string, bufferptr> &a) {
        Op op;
        op.type = Op::OP_SETATTRS;
        op.oid = oid;
        op.attrs = a;
        ops.push_back(std::move(op));
    }

    void omap_setkeys(const ghobject_t &oid,
                      const std::map<std::string, bufferlist> &keys) {
        Op op;
        op.type = Op::OP_OMAP_SETKEYS;
        op.oid = oid;
        op.omap_keys = keys;
        ops.push_back(std::move(op));
    }

    void omap_setheader(const ghobject_t &oid, bufferlist &header) {
        Op op;
        op.type = Op::OP_OMAP_SETHEADER;
        op.oid = oid;
        op.data = header;
        ops.push_back(std::move(op));
    }

    void omap_rmkeys(const ghobject_t &oid,
                     const std::set<std::string> &keys) {
        Op op;
        op.type = Op::OP_OMAP_RMKEYS;
        op.oid = oid;
        op.omap_rmkeys = keys;
        ops.push_back(std::move(op));
    }

    void omap_clear(const ghobject_t &oid) {
        Op op;
        op.type = Op::OP_OMAP_CLEAR;
        op.oid = oid;
        ops.push_back(std::move(op));
    }

    bool empty() const { return ops.empty(); }
    size_t num_ops() const { return ops.size(); }
};

struct WriteContext {
    struct write_item {
        uint64_t logical_offset;
        BlobRef b;
        uint64_t blob_length;
        uint64_t b_off;
        bufferlist bl;
        uint64_t b_off0;
        uint64_t length0;
        bool new_blob;
        bool mark_unused;

        write_item(uint64_t loffs, BlobRef blob, uint64_t blen, uint64_t o,
                   bufferlist &data, uint64_t o0, uint64_t len0, bool mu,
                   bool nb)
            : logical_offset(loffs), b(blob), blob_length(blen), b_off(o), bl(data), b_off0(o0), length0(len0), new_blob(nb), mark_unused(mu) {}
    };

    unsigned csum_order = 0;
    uint64_t target_blob_size = 0;

    std::vector<write_item> writes;
    std::vector<OldExtent> old_extents;

    void write(uint64_t loffs, BlobRef b, uint64_t blen, uint64_t o,
               bufferlist &data, uint64_t o0, uint64_t len0, bool mu,
               bool nb) {
        writes.emplace_back(loffs, b, blen, o, data, o0, len0, mu, nb);
    }

    bool has_conflict(BlobRef b, uint64_t b_off, uint64_t b_len) const {
        for (const auto &w : writes) {
            if (w.b == b &&
                b_off < w.b_off + w.blob_length &&
                w.b_off < b_off + b_len)
                return true;
        }
        return false;
    }
};

struct BigDeferredWriteContext {
    uint64_t off = 0;
    uint32_t b_off = 0;
    uint32_t used = 0;
    uint64_t head_read = 0;
    uint64_t tail_read = 0;
    BlobRef blob_ref;
    uint64_t blob_start = 0;
    PExtentVector res_extents;

    uint64_t blob_aligned_len() const { return used + head_read + tail_read; }
};

}  // namespace TOPNSPC
