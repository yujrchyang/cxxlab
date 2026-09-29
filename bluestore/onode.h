#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "bluestore/bluestore_types.h"
#include "bluestore/extent_map.h"
#include "common/object.h"

namespace TOPNSPC {

class KeyValueDB;
struct TransactionImpl;

struct Onode {
    std::atomic_int nref{0};
    ghobject_t oid;
    std::string key;
    bluestore_onode_t onode;
    ExtentMap extent_map;
    bool exists{false};

    Onode(const ghobject_t &o, const std::string &k)
        : oid(o), key(k), exists(false) {}

    void get() { ++nref; }
    bool put() {
        if (--nref == 0) {
            delete this;
            return true;
        }
        return false;
    }

    void encode(bufferlist &bl) const;
    void decode(bufferlist::const_iterator &p);

    void write_to_kv(KeyValueDB *db, std::shared_ptr<TransactionImpl> &txn);
    static std::unique_ptr<Onode> read_from_kv(KeyValueDB *db,
                                               const ghobject_t &oid,
                                               const std::string &key,
                                               bool *found);

    void update_object_size(uint64_t size) { onode.size = size; }
    uint64_t get_object_size() const { return onode.size; }

    void set_attrs(const std::map<std::string, bufferptr> &attrs);
    void set_attr(const std::string &name, const bufferptr &val);
    void remove_attr(const std::string &name);
    void get_attr(const std::string &name, bufferptr *value) const;
    void get_all_attrs(std::map<std::string, bufferptr> *attrs) const;

    // OMap helpers
    const std::string &get_omap_prefix() const;
    void get_omap_header(std::string *out) const;
    void get_omap_key(const std::string &key, std::string *out) const;
    void get_omap_tail(std::string *out) const;
    void decode_omap_key(const std::string &key, std::string *user_key) const;
};

using OnodeRef = std::shared_ptr<Onode>;

struct OnodeDeleter {
    void operator()(Onode *o) {
        if (o) {
            o->put();
        }
    }
};

}  // namespace TOPNSPC
