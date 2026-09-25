// Note: Must include merge_operator.h BEFORE the header to get
// MergeOperationInput / MergeOperationOutput nested types.
#include <rocksdb/merge_operator.h>

#include "kv/rocksdb/rocksdb_store.h"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <deque>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/write_batch.h>

#include "common/buffer.h"

namespace TOPNSPC {

// =====================================================================
// RocksDBMergeAdapter — dispatches kv::MergeOperator calls by prefix
// =====================================================================

class RocksDBStore::RocksDBMergeAdapter : public ::rocksdb::MergeOperator {
    using KvMergeOp = TOPNSPC::MergeOperator;

public:
    explicit RocksDBMergeAdapter(
        std::vector<std::pair<std::string,
                              std::shared_ptr<KvMergeOp>>>
            ops)
        : merge_ops_(std::move(ops)) {}

    const char *Name() const override { return "cxxlab_kv_merge_adapter"; }

    bool FullMergeV2(const MergeOperationInput &merge_in,
                     MergeOperationOutput *merge_out) const override {
        auto key_str = merge_in.key.ToString();
        auto null_pos = key_str.find('\0');
        if (null_pos == std::string::npos)
            return false;
        std::string prefix = key_str.substr(0, null_pos);

        auto mop = find_op(prefix);
        if (!mop)
            return false;

        const ::rocksdb::Slice *existing = merge_in.existing_value;
        const auto &operands = merge_in.operand_list;

        if (operands.empty())
            return false;

        if (!existing) {
            mop->merge_nonexistent(operands[0].data(), operands[0].size(),
                                   &merge_out->new_value);
            for (size_t i = 1; i < operands.size(); i++) {
                std::string tmp;
                mop->merge(merge_out->new_value.data(),
                           merge_out->new_value.size(),
                           operands[i].data(), operands[i].size(), &tmp);
                merge_out->new_value = std::move(tmp);
            }
        } else {
            mop->merge(existing->data(), existing->size(),
                       operands[0].data(), operands[0].size(),
                       &merge_out->new_value);
            for (size_t i = 1; i < operands.size(); i++) {
                std::string tmp;
                mop->merge(merge_out->new_value.data(),
                           merge_out->new_value.size(),
                           operands[i].data(), operands[i].size(), &tmp);
                merge_out->new_value = std::move(tmp);
            }
        }
        return true;
    }

    bool PartialMergeMulti(const ::rocksdb::Slice &key,
                           const std::deque<::rocksdb::Slice> &operand_list,
                           std::string *new_value,
                           ::rocksdb::Logger *logger) const override {
        (void)key;
        (void)operand_list;
        (void)new_value;
        (void)logger;
        return false;
    }

private:
    std::shared_ptr<KvMergeOp> find_op(const std::string &prefix) const {
        for (auto &[p, op] : merge_ops_) {
            if (p == prefix)
                return op;
        }
        return nullptr;
    }

    std::vector<std::pair<std::string, std::shared_ptr<KvMergeOp>>>
        merge_ops_;
};

// =====================================================================
// RDBTransactionImpl
// =====================================================================

struct RocksDBStore::RDBTransactionImpl : public TransactionImpl {
    ::rocksdb::WriteBatch batch;
    ::rocksdb::DB *db = nullptr;
    uint64_t delete_range_threshold = 0;

    explicit RDBTransactionImpl(::rocksdb::DB *db_,
                                uint64_t dr_threshold)
        : db(db_), delete_range_threshold(dr_threshold) {}

    void set(const std::string &prefix, const std::string &k,
             const bufferlist &bl) override {
        auto s = bl.to_str();
        batch.Put(encode_key(prefix, k), ::rocksdb::Slice(s));
    }

    void rmkey(const std::string &prefix,
               const std::string &k) override {
        batch.Delete(encode_key(prefix, k));
    }

    void rm_single_key(const std::string &prefix,
                       const std::string &k) override {
        batch.SingleDelete(encode_key(prefix, k));
    }

    void rmkeys_by_prefix(
        const std::string &prefix) override {
        auto start = encode_key(prefix, "");
        auto end = prefix + static_cast<char>(0xff);
        if (delete_range_threshold == 0) {
            batch.DeleteRange(start, end);
            return;
        }
        uint64_t cnt = delete_range_threshold;
        batch.SetSavePoint();
        ::rocksdb::ReadOptions ropts;
        ropts.fill_cache = false;
        ::rocksdb::Slice lb(start);
        ::rocksdb::Slice ub(end);
        ropts.iterate_lower_bound = &lb;
        ropts.iterate_upper_bound = &ub;
        auto it = db->NewIterator(ropts);
        for (it->SeekToFirst(); it->Valid() && (--cnt) != 0; it->Next()) {
            batch.Delete(it->key());
        }
        if (cnt == 0) {
            batch.RollbackToSavePoint();
            batch.DeleteRange(start, end);
        } else {
            batch.PopSavePoint();
        }
    }

    void rm_range_keys(const std::string &prefix,
                       const std::string &start,
                       const std::string &end) override {
        auto s = encode_key(prefix, start);
        auto e = encode_key(prefix, end);
        if (delete_range_threshold == 0) {
            batch.DeleteRange(s, e);
            return;
        }
        uint64_t cnt = delete_range_threshold;
        batch.SetSavePoint();
        ::rocksdb::ReadOptions ropts;
        ropts.fill_cache = false;
        ::rocksdb::Slice lb(s);
        ::rocksdb::Slice ub(e);
        ropts.iterate_lower_bound = &lb;
        ropts.iterate_upper_bound = &ub;
        auto it = db->NewIterator(ropts);
        for (it->Seek(s); it->Valid() && it->key().compare(e) < 0 && (--cnt) != 0;
             it->Next()) {
            batch.Delete(it->key());
        }
        if (cnt == 0) {
            batch.RollbackToSavePoint();
            batch.DeleteRange(s, e);
        } else {
            batch.PopSavePoint();
        }
    }

    void merge(const std::string &prefix, const std::string &k,
               const bufferlist &value) override {
        auto s = value.to_str();
        batch.Merge(encode_key(prefix, k), ::rocksdb::Slice(s));
    }
};

// =====================================================================
// RDBWholeSpaceIteratorImpl
// =====================================================================

class RocksDBStore::RDBWholeSpaceIteratorImpl
    : public WholeSpaceIteratorImpl {
public:
    RDBWholeSpaceIteratorImpl(::rocksdb::DB *db,
                              ::rocksdb::ReadOptions ropts)
        : it_(db->NewIterator(ropts)), ropts_(std::move(ropts)) {}

    int seek_to_first() override {
        apply_bounds();
        it_->SeekToFirst();
        return 0;
    }

    int seek_to_last() override {
        apply_bounds();
        it_->SeekToLast();
        return 0;
    }

    int lower_bound(const std::string &to) override {
        apply_bounds();
        it_->Seek(to);
        return 0;
    }

    int upper_bound(const std::string &after) override {
        apply_bounds();
        it_->Seek(after);
        if (it_->Valid() && it_->key() == after)
            it_->Next();
        return 0;
    }

    bool valid() const override {
        return it_->Valid();
    }

    int next() override {
        if (!it_->Valid()) return -EINVAL;
        it_->Next();
        return it_->status().ok() ? 0 : -EIO;
    }

    int prev() override {
        if (!it_->Valid()) return -EINVAL;
        it_->Prev();
        return it_->status().ok() ? 0 : -EIO;
    }

    std::string key() const override {
        return it_->key().ToString();
    }

    bufferlist value() const override {
        bufferlist bl;
        auto v = it_->value();
        bl.append(v.data(), static_cast<unsigned>(v.size()));
        return bl;
    }

    size_t key_size() const override {
        return it_->key().size();
    }

    size_t value_size() const override {
        return it_->value().size();
    }

    int status() const override {
        return it_->status().ok() ? 0 : -EIO;
    }

    std::pair<std::string, std::string> raw_key() const override {
        return decode_key(it_->key().ToString());
    }

    bool raw_key_is_prefixed(
        const std::string &prefix) const override {
        auto [pre, inner] = decode_key(it_->key().ToString());
        (void)inner;
        return pre == prefix;
    }

private:
    void apply_bounds() {
        if (iterate_lower_bound_) {
            lb_slice_ = *iterate_lower_bound_;
            ropts_.iterate_lower_bound = &lb_slice_;
        }
        if (iterate_upper_bound_) {
            ub_slice_ = *iterate_upper_bound_;
            ropts_.iterate_upper_bound = &ub_slice_;
        }
    }

    std::unique_ptr<::rocksdb::Iterator> it_;
    ::rocksdb::ReadOptions ropts_;
    ::rocksdb::Slice lb_slice_;
    ::rocksdb::Slice ub_slice_;
};

// =====================================================================
// RocksDBStore
// =====================================================================

RocksDBStore::RocksDBStore(
    const std::string &dir,
    std::map<std::string, std::string> options)
    : dir_(dir), options_(std::move(options)) {}

static void parse_options(const std::string &str,
                          ::rocksdb::Options &opts) {
    if (str.empty())
        return;
    std::istringstream ss(str);
    std::string kv;
    while (std::getline(ss, kv, ';')) {
        auto eq = kv.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = kv.substr(0, eq);
        std::string value = kv.substr(eq + 1);
        if (key == "write_buffer_size")
            opts.write_buffer_size = std::stoul(value);
        else if (key == "max_write_buffer_number")
            opts.max_write_buffer_number = std::stoul(value);
        else if (key == "min_write_buffer_number_to_merge")
            opts.min_write_buffer_number_to_merge = std::stoul(value);
        else if (key == "max_bytes_for_level_base")
            opts.max_bytes_for_level_base = std::stoull(value);
        else if (key == "target_file_size_base")
            opts.target_file_size_base = std::stoull(value);
        // add more options as needed
    }
}

int RocksDBStore::init(const std::string &options_str) {
    parse_options(options_str, cached_opts_);
    for (auto &[k, v] : options_) {
        if (k == "delete_range_threshold")
            delete_range_threshold_ = std::stoul(v);
    }
    return 0;
}

void RocksDBStore::setup_merge_adapter(::rocksdb::Options &opts) {
    if (!get_merge_ops().empty()) {
        adapter_ = std::make_shared<RocksDBMergeAdapter>(get_merge_ops());
        opts.merge_operator = adapter_;
    }
}

int RocksDBStore::open_db(::rocksdb::Options opts,
                          std::ostream &out) {
    // Apply cached options from init()
    opts.write_buffer_size = cached_opts_.write_buffer_size;
    opts.max_write_buffer_number = cached_opts_.max_write_buffer_number;
    opts.min_write_buffer_number_to_merge =
        cached_opts_.min_write_buffer_number_to_merge;
    opts.max_bytes_for_level_base =
        cached_opts_.max_bytes_for_level_base;
    opts.target_file_size_base = cached_opts_.target_file_size_base;

    setup_merge_adapter(opts);

    ::rocksdb::Status s = ::rocksdb::DB::Open(opts, dir_, &db_);
    if (!s.ok()) {
        out << "RocksDB open failed: " << s.ToString() << std::endl;
        return -EIO;
    }
    return 0;
}

int RocksDBStore::open(std::ostream &out) {
    ::rocksdb::Options opts;
    opts.create_if_missing = false;
    return open_db(std::move(opts), out);
}

int RocksDBStore::create_and_open(std::ostream &out) {
    ::rocksdb::Options opts;
    opts.create_if_missing = true;
    return open_db(std::move(opts), out);
}

int RocksDBStore::open_read_only(std::ostream &out) {
    ::rocksdb::Options opts;
    setup_merge_adapter(opts);
    auto s = ::rocksdb::DB::OpenForReadOnly(opts, dir_, &db_);
    if (!s.ok()) {
        out << "RocksDB open_read_only failed: " << s.ToString()
            << std::endl;
        return -EIO;
    }
    return 0;
}

int RocksDBStore::repair(std::ostream &out) {
    auto s = ::rocksdb::RepairDB(dir_, ::rocksdb::Options());
    if (!s.ok()) {
        out << "RocksDB repair failed: " << s.ToString() << std::endl;
        return -EIO;
    }
    return 0;
}

void RocksDBStore::close() {
    if (db_) {
        delete db_;
        db_ = nullptr;
    }
    adapter_.reset();
}

Transaction RocksDBStore::get_transaction() {
    return std::make_shared<RDBTransactionImpl>(db_, delete_range_threshold_);
}

int RocksDBStore::submit_transaction(Transaction t) {
    auto rdb_t = std::static_pointer_cast<RDBTransactionImpl>(t);
    auto s = db_->Write(::rocksdb::WriteOptions(), &rdb_t->batch);
    return s.ok() ? 0 : -EIO;
}

int RocksDBStore::submit_transaction_sync(Transaction t) {
    auto rdb_t = std::static_pointer_cast<RDBTransactionImpl>(t);
    ::rocksdb::WriteOptions wopts;
    wopts.sync = true;
    auto s = db_->Write(wopts, &rdb_t->batch);
    return s.ok() ? 0 : -EIO;
}

int RocksDBStore::set_merge_operator(
    const std::string &prefix,
    std::shared_ptr<MergeOperator> mop) {
    if (db_)
        return -EROFS;
    KeyValueDB::set_merge_operator(prefix, std::move(mop));
    return 0;
}

int RocksDBStore::get(
    const std::string &prefix,
    const std::set<std::string> &keys,
    std::map<std::string, bufferlist> *out) {
    std::vector<std::string> full_keys;
    full_keys.reserve(keys.size());
    for (auto &k : keys)
        full_keys.push_back(encode_key(prefix, k));

    std::vector<::rocksdb::Slice> slices;
    slices.reserve(full_keys.size());
    for (auto &fk : full_keys)
        slices.push_back(fk);

    std::vector<std::string> values;
    auto statuses = db_->MultiGet(::rocksdb::ReadOptions(), slices, &values);

    size_t i = 0;
    auto kit = keys.begin();
    while (kit != keys.end() && i < statuses.size()) {
        if (statuses[i].ok()) {
            bufferlist bl;
            bl.append(values[i].data(),
                      static_cast<unsigned>(values[i].size()));
            (*out)[*kit] = std::move(bl);
        }
        ++kit;
        ++i;
    }
    return 0;
}

WholeSpaceIterator RocksDBStore::get_wholespace_iterator(
    IteratorOpts opts) {
    ::rocksdb::ReadOptions ropts;
    if (opts & ITERATOR_NOCACHE)
        ropts.fill_cache = false;
    return std::make_unique<RDBWholeSpaceIteratorImpl>(
        db_, std::move(ropts));
}

void RocksDBStore::compact() {
    auto s = db_->CompactRange(::rocksdb::CompactRangeOptions(),
                               nullptr, nullptr);
    (void)s;
}

void RocksDBStore::compact_prefix(const std::string &prefix) {
    auto start = encode_key(prefix, "");
    auto end = prefix + static_cast<char>(0xff);
    ::rocksdb::Slice b(start), e(end);
    auto s = db_->CompactRange(::rocksdb::CompactRangeOptions(), &b, &e);
    (void)s;
}

void RocksDBStore::compact_range(const std::string &prefix,
                                 const std::string &start,
                                 const std::string &end) {
    auto b = encode_key(prefix, start);
    auto e = encode_key(prefix, end);
    ::rocksdb::Slice bs(b), es(e);
    auto s = db_->CompactRange(::rocksdb::CompactRangeOptions(), &bs, &es);
    (void)s;
}

uint64_t RocksDBStore::get_estimated_size(
    std::map<std::string, uint64_t> &extra) {
    (void)extra;
    uint64_t total = 0;
    std::vector<::rocksdb::LiveFileMetaData> files;
    db_->GetLiveFilesMetaData(&files);
    for (auto &f : files)
        total += f.size;
    return total;
}

}  // namespace TOPNSPC
