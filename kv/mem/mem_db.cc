#include "kv/mem/mem_db.h"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/buffer.h"
#include "common/common_fwd.h"

namespace TOPNSPC {

// =====================================================================
// MDBTransactionImpl
// =====================================================================

enum class OpType { SET,
                    RMKEY,
                    RMKEY_BY_PREFIX,
                    RM_RANGE_KEYS,
                    MERGE };

struct MemDB::MDBTransactionImpl : public TransactionImpl {
    struct Op {
        OpType type;
        std::string prefix;
        std::string key;
        std::string end;
        bufferlist value;
    };
    std::vector<Op> ops;

    void set(const std::string &prefix, const std::string &k,
             const bufferlist &bl) override {
        ops.push_back({OpType::SET, prefix, k, {}, bl});
    }

    void rmkey(const std::string &prefix,
               const std::string &k) override {
        ops.push_back({OpType::RMKEY, prefix, k, {}, {}});
    }

    void rmkeys_by_prefix(
        const std::string &prefix) override {
        ops.push_back({OpType::RMKEY_BY_PREFIX, prefix, {}, {}, {}});
    }

    void rm_range_keys(const std::string &prefix,
                       const std::string &start,
                       const std::string &end) override {
        ops.push_back({OpType::RM_RANGE_KEYS, prefix, start, end, {}});
    }

    void merge(const std::string &prefix, const std::string &k,
               const bufferlist &value) override {
        ops.push_back({OpType::MERGE, prefix, k, {}, value});
    }
};

// =====================================================================
// MDBWholeSpaceIteratorImpl
// =====================================================================

class MemDB::MDBWholeSpaceIteratorImpl : public WholeSpaceIteratorImpl {
public:
    using Items = std::vector<std::pair<std::string, std::string>>;

    MDBWholeSpaceIteratorImpl(const MemDB *db, Items items,
                              uint64_t seqno)
        : db_(db), items_(std::move(items)), seqno_(seqno), pos_(-1) {}

    int seek_to_first() override {
        refresh();
        pos_ = items_.empty() ? -1 : 0;
        return 0;
    }

    int seek_to_last() override {
        refresh();
        if (items_.empty()) {
            pos_ = -1;
        } else {
            pos_ = static_cast<ptrdiff_t>(items_.size() - 1);
        }
        return 0;
    }

    int lower_bound(const std::string &to) override {
        refresh();
        auto it = std::lower_bound(
            items_.begin(), items_.end(), to,
            [](const auto &pair, const std::string &key) {
                return pair.first < key;
            });
        auto idx = static_cast<size_t>(it - items_.begin());
        if (idx >= items_.size()) {
            pos_ = -1;
        } else {
            pos_ = static_cast<ptrdiff_t>(idx);
        }
        return 0;
    }

    int upper_bound(const std::string &after) override {
        refresh();
        auto it = std::upper_bound(
            items_.begin(), items_.end(), after,
            [](const std::string &key, const auto &pair) {
                return key < pair.first;
            });
        auto idx = static_cast<size_t>(it - items_.begin());
        if (idx >= items_.size()) {
            pos_ = -1;
        } else {
            pos_ = static_cast<ptrdiff_t>(idx);
        }
        return 0;
    }

    bool valid() const override {
        return pos_ >= 0 &&
            pos_ < static_cast<ptrdiff_t>(items_.size());
    }

    int next() override {
        if (pos_ >= 0) {
            auto next = pos_ + 1;
            if (static_cast<size_t>(next) >= items_.size())
                pos_ = -1;
            else
                pos_ = next;
        }
        return 0;
    }

    int prev() override {
        if (pos_ > 0) {
            --pos_;
        } else if (pos_ == 0) {
            pos_ = -1;
        }
        return 0;
    }

    std::string key() const override {
        return items_[static_cast<size_t>(pos_)].first;
    }

    bufferlist value() const override {
        bufferlist bl;
        const auto &v = items_[static_cast<size_t>(pos_)].second;
        bl.append(v.data(), static_cast<unsigned>(v.size()));
        return bl;
    }

    int status() const override { return 0; }

    std::pair<std::string, std::string> raw_key() const override {
        return decode_key(
            items_[static_cast<size_t>(pos_)].first);
    }

    bool raw_key_is_prefixed(
        const std::string &prefix) const override {
        auto [pre, inner] = decode_key(
            items_[static_cast<size_t>(pos_)].first);
        (void)inner;
        return pre == prefix;
    }

private:
    void refresh() {
        uint64_t cur = db_->seqno_;
        if (cur == seqno_)
            return;
        seqno_ = cur;
        std::string current_key;
        if (pos_ >= 0 &&
            static_cast<size_t>(pos_) < items_.size())
            current_key = items_[pos_].first;
        {
            std::lock_guard<std::mutex> lock(db_->m_lock_);
            items_.clear();
            items_.reserve(db_->db_.size());
            for (auto &[k, v] : db_->db_)
                items_.emplace_back(k, v);
        }
        if (!current_key.empty()) {
            auto it = std::lower_bound(
                items_.begin(), items_.end(), current_key,
                [](const auto &pair, const std::string &key) {
                    return pair.first < key;
                });
            pos_ = static_cast<ptrdiff_t>(it - items_.begin());
            if (static_cast<size_t>(pos_) >= items_.size())
                pos_ = -1;
        }
    }

    const MemDB *db_;
    Items items_;
    uint64_t seqno_;
    ptrdiff_t pos_;
};

// =====================================================================
// MemDB
// =====================================================================

MemDB::MemDB() {}

int MemDB::init(const std::string &options_str) {
    return 0;
}

int MemDB::open(std::ostream &out) {
    return 0;
}

int MemDB::create_and_open(std::ostream &out) {
    return 0;
}

void MemDB::close() {
    std::lock_guard<std::mutex> lock(m_lock_);
    db_.clear();
}

Transaction MemDB::get_transaction() {
    return std::make_shared<MDBTransactionImpl>();
}

int MemDB::submit_transaction(Transaction t) {
    auto mdb_t = std::static_pointer_cast<MDBTransactionImpl>(t);
    std::lock_guard<std::mutex> lock(m_lock_);
    for (auto &op : mdb_t->ops) {
        switch (op.type) {
        case OpType::SET:
            _set_key(encode_key(op.prefix, op.key), op.value);
            break;
        case OpType::RMKEY:
            _rmkey(encode_key(op.prefix, op.key));
            break;
        case OpType::RMKEY_BY_PREFIX:
            _rmkeys_by_prefix(op.prefix);
            break;
        case OpType::RM_RANGE_KEYS:
            _rm_range_keys(op.prefix, op.key, op.end);
            break;
        case OpType::MERGE: {
            int r = _merge(op.prefix, encode_key(op.prefix, op.key),
                           op.value);
            if (r) return r;
            break;
        }
        }
    }
    return 0;
}

int MemDB::submit_transaction_sync(Transaction t) {
    return submit_transaction(std::move(t));
}

int MemDB::get(
    const std::string &prefix,
    const std::set<std::string> &keys,
    std::map<std::string, bufferlist> *out) {
    std::lock_guard<std::mutex> lock(m_lock_);
    for (auto &k : keys) {
        auto full = encode_key(prefix, k);
        auto it = db_.find(full);
        if (it != db_.end()) {
            bufferlist bl;
            bl.append(it->second.data(), it->second.size());
            (*out)[k] = std::move(bl);
        }
    }
    return 0;
}

WholeSpaceIterator MemDB::get_wholespace_iterator(
    IteratorOpts opts) {
    std::lock_guard<std::mutex> lock(m_lock_);
    MDBWholeSpaceIteratorImpl::Items items;
    items.reserve(db_.size());
    for (auto &[k, v] : db_)
        items.emplace_back(k, v);
    return std::make_unique<MDBWholeSpaceIteratorImpl>(
        this, std::move(items), seqno_);
}

void MemDB::compact() {}

uint64_t MemDB::get_estimated_size(
    std::map<std::string, uint64_t> &extra) {
    std::lock_guard<std::mutex> lock(m_lock_);
    uint64_t total = 0;
    for (auto &[k, v] : db_)
        total += k.size() + v.size();
    return total;
}

// ── Private helpers ────────────────────────────────────────────────

void MemDB::_set_key(const std::string &full_key,
                     const bufferlist &bl) {
    db_[full_key] = bl.to_str();
    ++seqno_;
}

void MemDB::_rmkey(const std::string &full_key) {
    db_.erase(full_key);
    ++seqno_;
}

void MemDB::_rmkeys_by_prefix(const std::string &prefix) {
    auto start = encode_key(prefix, "");
    auto it = db_.lower_bound(start);
    while (it != db_.end() &&
           it->first.compare(0, start.size(), start) == 0)
        it = db_.erase(it);
    ++seqno_;
}

void MemDB::_rm_range_keys(const std::string &prefix,
                           const std::string &start,
                           const std::string &end) {
    auto s = encode_key(prefix, start);
    auto e = encode_key(prefix, end);
    auto it = db_.lower_bound(s);
    while (it != db_.end() && it->first < e)
        it = db_.erase(it);
    ++seqno_;
}

int MemDB::_merge(const std::string &prefix,
                  const std::string &full_key,
                  const bufferlist &bl) {
    auto &mops = get_merge_ops();
    std::shared_ptr<MergeOperator> mop;
    for (auto &[p, op] : mops) {
        if (p == prefix) {
            mop = op;
            break;
        }
    }
    if (!mop) return -ENOENT;

    std::string rdata = bl.to_str();
    auto it = db_.find(full_key);
    if (it == db_.end()) {
        std::string new_value;
        mop->merge_nonexistent(rdata.data(), rdata.size(),
                               &new_value);
        db_[full_key] = std::move(new_value);
    } else {
        std::string new_value;
        mop->merge(it->second.data(), it->second.size(),
                   rdata.data(), rdata.size(), &new_value);
        it->second = std::move(new_value);
    }
    ++seqno_;
    return 0;
}

}  // namespace TOPNSPC
