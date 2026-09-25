#include "kv/key_value_db.h"
#include "kv/mem/mem_db.h"
#include "kv/rocksdb/rocksdb_store.h"

#include <cerrno>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include "common/buffer.h"

namespace TOPNSPC {

// ===================================================================
// PrefixIteratorImpl
// ===================================================================

PrefixIteratorImpl::PrefixIteratorImpl(WholeSpaceIterator w_iter,
                                       std::string prefix,
                                       IteratorBounds bounds)
    : w_iter_(std::move(w_iter)),
      prefix_(std::move(prefix)),
      prefix_start_(KeyValueDB::encode_key(prefix_, "")),
      prefix_next_(prefix_ + static_cast<char>(0xff)) {
    seek_lower_bound_ = prefix_start_;
    seek_upper_bound_ = prefix_next_;
    if (bounds.lower_bound) {
        seek_lower_bound_ = prefix_start_ + *bounds.lower_bound;
        w_iter_->lower_bound(seek_lower_bound_);
    }
    if (bounds.upper_bound) {
        upper_bound_ = *bounds.upper_bound;
        upper_bound_set_ = true;
        seek_upper_bound_ = prefix_start_ + upper_bound_;
    }
    w_iter_->set_iterate_lower_bound(&seek_lower_bound_);
    w_iter_->set_iterate_upper_bound(&seek_upper_bound_);
}

int PrefixIteratorImpl::seek_to_first() {
    return w_iter_->lower_bound(prefix_start_);
}

int PrefixIteratorImpl::seek_to_last() {
    int r = w_iter_->seek_to_last();
    if (r != 0) return r;
    while (w_iter_->valid() &&
           !w_iter_->raw_key_is_prefixed(prefix_))
        w_iter_->prev();
    return 0;
}

int PrefixIteratorImpl::lower_bound(const std::string &to) {
    return w_iter_->lower_bound(prefix_start_ + to);
}

int PrefixIteratorImpl::upper_bound(const std::string &after) {
    upper_bound_ = after;
    upper_bound_set_ = true;
    int r = w_iter_->lower_bound(prefix_start_ + after);
    if (r != 0) return r;
    if (w_iter_->valid()) {
        auto [pre, inner] = w_iter_->raw_key();
        if (inner == after)
            w_iter_->next();
    }
    return 0;
}

bool PrefixIteratorImpl::valid() const {
    return w_iter_->valid() &&
        w_iter_->raw_key_is_prefixed(prefix_) &&
        (!upper_bound_set_ || w_iter_->raw_key().second < upper_bound_);
}

int PrefixIteratorImpl::next() {
    if (!w_iter_->valid()) return -EINVAL;
    w_iter_->next();
    return 0;
}

int PrefixIteratorImpl::prev() {
    if (!w_iter_->valid()) return -EINVAL;
    w_iter_->prev();
    return 0;
}

std::string PrefixIteratorImpl::key() const {
    return w_iter_->raw_key().second;
}

bufferlist PrefixIteratorImpl::value() const {
    return w_iter_->value();
}

int PrefixIteratorImpl::status() const {
    return w_iter_->status();
}

// ===================================================================
// KeyValueDB
// ===================================================================

int KeyValueDB::get(const std::string &prefix,
                    const std::string &key,
                    bufferlist *out) {
    std::set<std::string> keys = {key};
    std::map<std::string, bufferlist> result;
    int r = get(prefix, keys, &result);
    if (r != 0) return r;
    auto it = result.find(key);
    if (it == result.end())
        return -ENOENT;
    *out = it->second;
    return 0;
}

Iterator KeyValueDB::get_iterator(const std::string &prefix,
                                  IteratorOpts opts,
                                  IteratorBounds bounds) {
    auto w_iter = get_wholespace_iterator(opts);
    if (!w_iter)
        return nullptr;
    return make_iterator(prefix, std::move(w_iter), bounds);
}

Iterator KeyValueDB::make_iterator(const std::string &prefix,
                                   WholeSpaceIterator w_iter,
                                   IteratorBounds bounds) {
    return std::make_shared<PrefixIteratorImpl>(
        std::move(w_iter), prefix, std::move(bounds));
}

// ===================================================================
// Factory
// ===================================================================

std::unique_ptr<KeyValueDB> KeyValueDB::create(
    const std::string &type,
    const std::string &dir,
    std::map<std::string, std::string> options) {
    if (type == "rocksdb") {
        return std::make_unique<RocksDBStore>(dir, options);
    } else if (type == "memdb") {
        return std::make_unique<MemDB>();
    }
    return nullptr;
}

}  // namespace TOPNSPC
