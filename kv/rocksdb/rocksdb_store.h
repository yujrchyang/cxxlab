#pragma once

#include <map>
#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/merge_operator.h>

#include "kv/key_value_db.h"

namespace TOPNSPC {

class RocksDBStore : public KeyValueDB {
public:
    RocksDBStore(const std::string &dir,
                 std::map<std::string, std::string> options);

    int init(const std::string &options_str) override;
    int open(std::ostream &out) override;
    int create_and_open(std::ostream &out) override;
    int open_read_only(std::ostream &out) override;
    int repair(std::ostream &out) override;
    void close() override;

    Transaction get_transaction() override;
    int submit_transaction(Transaction t) override;
    int submit_transaction_sync(Transaction t) override;

    int get(const std::string &prefix,
            const std::set<std::string> &keys,
            std::map<std::string, bufferlist> *out) override;

    WholeSpaceIterator get_wholespace_iterator(
        IteratorOpts opts) override;

    int set_merge_operator(
        const std::string &prefix,
        std::shared_ptr<MergeOperator> mop) override;

    void compact() override;
    void compact_prefix(const std::string &prefix) override;
    void compact_range(const std::string &prefix,
                       const std::string &start,
                       const std::string &end) override;

    uint64_t get_estimated_size(
        std::map<std::string, uint64_t> &extra) override;

private:
    // ── Nested types ────────────────────────────────────────
    class RDBTransactionImpl;
    class RDBWholeSpaceIteratorImpl;
    class RocksDBMergeAdapter;

    // ── Helpers ─────────────────────────────────────────────
    int open_db(rocksdb::Options opts, std::ostream &out);
    void setup_merge_adapter(::rocksdb::Options &opts);

    // ── Members ─────────────────────────────────────────────
    rocksdb::DB *db_ = nullptr;
    std::string dir_;
    std::map<std::string, std::string> options_;
    std::shared_ptr<RocksDBMergeAdapter> adapter_;
    uint64_t delete_range_threshold_ = 0;
    rocksdb::Options cached_opts_;
};

}  // namespace TOPNSPC
