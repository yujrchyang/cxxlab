#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "common/buffer.h"
#include "kv/merge_op/merge_op.h"

namespace TOPNSPC {

// ---------------------------------------------------------------------------
// TransactionImpl
// ---------------------------------------------------------------------------
struct TransactionImpl {
    virtual void set(const std::string &prefix,
                     const std::string &k,
                     const bufferlist &bl) = 0;

    virtual void set(const std::string &prefix,
                     const char *k, size_t klen,
                     const bufferlist &bl) {
        set(prefix, std::string(k, klen), bl);
    }

    virtual void rmkey(const std::string &prefix,
                       const std::string &k) = 0;

    virtual void rmkey(const std::string &prefix,
                       const char *k, size_t klen) {
        rmkey(prefix, std::string(k, klen));
    }

    /// Single-key delete (LSM-tree stores). Removes only the latest
    /// version; behavior undefined if the key has been overwritten.
    virtual void rm_single_key(const std::string &prefix,
                               const std::string &k) {
        rmkey(prefix, k);
    }

    virtual void rmkeys_by_prefix(const std::string &prefix) = 0;

    virtual void rm_range_keys(const std::string &prefix,
                               const std::string &start,
                               const std::string &end) = 0;

    virtual void merge(const std::string &prefix,
                       const std::string &k,
                       const bufferlist &value) = 0;

    virtual ~TransactionImpl() = default;
};

using Transaction = std::shared_ptr<TransactionImpl>;

// ---------------------------------------------------------------------------
// IteratorBounds
// ---------------------------------------------------------------------------
struct IteratorBounds {
    std::optional<std::string> lower_bound;
    std::optional<std::string> upper_bound;
};

// ---------------------------------------------------------------------------
// IteratorImpl
// ---------------------------------------------------------------------------
class IteratorImpl {
public:
    virtual int seek_to_first() = 0;
    virtual int seek_to_last() = 0;
    virtual int lower_bound(const std::string &to) = 0;
    virtual int upper_bound(const std::string &after) = 0;

    virtual bool valid() const = 0;
    virtual int next() = 0;
    virtual int prev() = 0;

    virtual std::string key() const = 0;
    virtual bufferlist value() const = 0;

    virtual int status() const = 0;

    virtual ~IteratorImpl() = default;
};

using Iterator = std::shared_ptr<IteratorImpl>;

// ---------------------------------------------------------------------------
// WholeSpaceIteratorImpl
// ---------------------------------------------------------------------------
class WholeSpaceIteratorImpl : public IteratorImpl {
public:
    /// Returns the raw (prefix, key) pair decoded from the stored key.
    virtual std::pair<std::string, std::string> raw_key() const = 0;

    /// Returns true if the current key belongs to the given prefix.
    virtual bool raw_key_is_prefixed(const std::string &prefix) const = 0;

    virtual size_t key_size() const { return 0; }
    virtual size_t value_size() const { return 0; }

    /// Set a persistent lower bound hint for the RocksDB iterator.
    /// The pointer must remain valid for the iterator's lifetime.
    void set_iterate_lower_bound(const std::string *b) { iterate_lower_bound_ = b; }
    /// Set a persistent upper bound hint for the RocksDB iterator.
    void set_iterate_upper_bound(const std::string *b) { iterate_upper_bound_ = b; }

protected:
    const std::string *iterate_lower_bound_ = nullptr;
    const std::string *iterate_upper_bound_ = nullptr;
};

using WholeSpaceIterator = std::shared_ptr<WholeSpaceIteratorImpl>;

// ---------------------------------------------------------------------------
// IteratorOpts
// ---------------------------------------------------------------------------
using IteratorOpts = uint32_t;
inline constexpr IteratorOpts ITERATOR_NOCACHE = 1;

// ---------------------------------------------------------------------------
// PrefixIteratorImpl — wraps WholeSpaceIterator, filters by prefix
// ---------------------------------------------------------------------------
class PrefixIteratorImpl : public IteratorImpl {
public:
    PrefixIteratorImpl(WholeSpaceIterator w_iter,
                       std::string prefix,
                       IteratorBounds bounds = IteratorBounds{});

    int seek_to_first() override;
    int seek_to_last() override;
    int lower_bound(const std::string &to) override;
    int upper_bound(const std::string &after) override;

    bool valid() const override;
    int next() override;
    int prev() override;

    std::string key() const override;
    bufferlist value() const override;

    int status() const override;

private:
    WholeSpaceIterator w_iter_;
    std::string prefix_;
    std::string prefix_start_;
    std::string prefix_next_;
    std::string seek_lower_bound_;
    std::string seek_upper_bound_;
    std::string upper_bound_;
    bool upper_bound_set_ = false;
};

// ---------------------------------------------------------------------------
// KeyValueDB — abstract base class
// ---------------------------------------------------------------------------
class KeyValueDB {
public:
    virtual ~KeyValueDB() = default;

    // ── Factory ─────────────────────────────────────────────
    static std::unique_ptr<KeyValueDB> create(
        const std::string &type,
        const std::string &dir,
        std::map<std::string, std::string> options = {});

    // ── Lifecycle ───────────────────────────────────────────
    virtual int init(const std::string &options_str = "") = 0;
    virtual int open(std::ostream &out) = 0;
    virtual int create_and_open(std::ostream &out) = 0;
    virtual void close() = 0;

    virtual int open_read_only(std::ostream &out) { return -ENOTSUP; }

    virtual int repair(std::ostream &out) { return 0; }

    // ── Transaction ─────────────────────────────────────────
    virtual Transaction get_transaction() = 0;
    virtual int submit_transaction(Transaction t) = 0;

    virtual int submit_transaction_sync(Transaction t) = 0;

    // ── Point Read ──────────────────────────────────────────
    virtual int get(const std::string &prefix,
                    const std::set<std::string> &keys,
                    std::map<std::string, bufferlist> *out) = 0;

    virtual int get(const std::string &prefix,
                    const std::string &key,
                    bufferlist *out);

    // ── Iterator ────────────────────────────────────────────
    virtual WholeSpaceIterator get_wholespace_iterator(
        IteratorOpts opts = 0) = 0;

    virtual Iterator get_iterator(
        const std::string &prefix,
        IteratorOpts opts = 0,
        IteratorBounds bounds = IteratorBounds{});

    // ── Merge Operator ──────────────────────────────────────
    virtual int set_merge_operator(
        const std::string &prefix,
        std::shared_ptr<MergeOperator> mop) {
        merge_ops_.emplace_back(prefix, std::move(mop));
        return 0;
    }

    // ── Maintenance ─────────────────────────────────────────
    virtual void compact() = 0;
    virtual void compact_async() { compact(); }

    virtual void compact_prefix(const std::string &prefix) { compact(); }
    virtual void compact_prefix_async(const std::string &prefix) {
        compact_async();
    }
    virtual void compact_range(const std::string &prefix,
                               const std::string &start,
                               const std::string &end) {
        (void)prefix;
        (void)start;
        (void)end;
        compact();
    }
    virtual void compact_range_async(const std::string &prefix,
                                     const std::string &start,
                                     const std::string &end) {
        compact_range(prefix, start, end);
    }

    virtual uint64_t get_estimated_size(
        std::map<std::string, uint64_t> &extra) = 0;

    const std::vector<std::pair<std::string,
                                std::shared_ptr<MergeOperator>>> &
    get_merge_ops() const { return merge_ops_; }

    // ── Key encoding ───────────────────────────────────────
    // Ceph-compatible format: prefix + '\0' + inner_key
    static std::string encode_key(const std::string &prefix,
                                  const std::string &key) {
        return prefix + '\0' + key;
    }
    static std::pair<std::string, std::string> decode_key(
        const std::string &full_key) {
        auto pos = full_key.find('\0');
        if (pos == std::string::npos)
            return {full_key, {}};
        return {full_key.substr(0, pos),
                full_key.substr(pos + 1)};
    }

protected:
    /// Wrap a WholeSpaceIterator into a prefix-filtered Iterator.
    Iterator make_iterator(const std::string &prefix,
                           WholeSpaceIterator w_iter,
                           IteratorBounds bounds = IteratorBounds{});

private:
    std::vector<std::pair<std::string,
                          std::shared_ptr<MergeOperator>>>
        merge_ops_;
};

}  // namespace TOPNSPC
