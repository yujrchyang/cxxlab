#include "bluestore/onode.h"
#include "bluestore/bluestore_constants.h"
#include "common/denc.h"
#include "kv/key_value_db.h"

namespace TOPNSPC {

void Onode::encode(bufferlist &bl) const {
    cxxlab::encode(onode, bl);

    uint32_t num_extents = extent_map.size();
    cxxlab::encode(num_extents, bl);

    for (const auto &ext : extent_map) {
        cxxlab::encode(ext.logical_offset, bl);
        cxxlab::encode(ext.blob_offset, bl);
        cxxlab::encode(ext.length, bl);

        if (ext.blob) {
            uint8_t has_blob = 1;
            cxxlab::encode(has_blob, bl);
            cxxlab::encode(ext.blob->get_blob(), bl);
        } else {
            uint8_t has_blob = 0;
            cxxlab::encode(has_blob, bl);
        }
    }
}

void Onode::decode(bufferlist::const_iterator &p) {
    cxxlab::decode(onode, p);

    uint32_t num_extents;
    cxxlab::decode(num_extents, p);

    extent_map.clear();
    for (uint32_t i = 0; i < num_extents; ++i) {
        uint32_t lo, bo, len;
        cxxlab::decode(lo, p);
        cxxlab::decode(bo, p);
        cxxlab::decode(len, p);

        uint8_t has_blob;
        cxxlab::decode(has_blob, p);

        BlobRef blob = nullptr;
        if (has_blob) {
            blob = new Blob();
            bluestore_blob_t temp_blob;
            cxxlab::decode(temp_blob, p);
            blob->dirty_blob() = std::move(temp_blob);
            blob->get();
        }

        extent_map.add(lo, bo, len, blob);
    }

    exists = true;
}

void Onode::write_to_kv(KeyValueDB *db, std::shared_ptr<TransactionImpl> &txn) {
    bufferlist bl;
    encode(bl);
    txn->set(PREFIX_OBJ, key, bl);
}

std::unique_ptr<Onode> Onode::read_from_kv(KeyValueDB *db,
                                           const ghobject_t &oid,
                                           const std::string &key,
                                           bool *found) {
    bufferlist bl;
    int r = db->get(PREFIX_OBJ, key, &bl);

    if (r < 0 || bl.length() == 0) {
        if (found) *found = false;
        return nullptr;
    }

    auto on = std::make_unique<Onode>(oid, key);
    auto p = bl.cbegin();
    on->decode(p);

    if (found) *found = true;
    return on;
}

void Onode::set_attrs(const std::map<std::string, bufferptr> &attrs) {
    onode.attrs = attrs;
}

void Onode::set_attr(const std::string &name, const bufferptr &val) {
    onode.attrs[name] = val;
}

void Onode::remove_attr(const std::string &name) {
    onode.attrs.erase(name);
}

void Onode::get_attr(const std::string &name, bufferptr *value) const {
    auto it = onode.attrs.find(name);
    if (it != onode.attrs.end()) {
        *value = it->second;
    } else {
        *value = bufferptr();
    }
}

void Onode::get_all_attrs(std::map<std::string, bufferptr> *attrs) const {
    *attrs = onode.attrs;
}

// OMap helpers

const std::string &Onode::get_omap_prefix() const {
    if (onode.is_pgmeta_omap()) {
        static const std::string pgmeta_prefix = PREFIX_PGMETA_OMAP;
        return pgmeta_prefix;
    }
    static const std::string default_prefix = PREFIX_OMAP;
    return default_prefix;
}

void Onode::get_omap_header(std::string *out) const {
    out->clear();
    key_encode_u64(onode.nid, out);
    out->push_back('-');  // Header delimiter: '-' < '.' < '~'
}

void Onode::get_omap_key(const std::string &key, std::string *out) const {
    out->clear();
    key_encode_u64(onode.nid, out);
    out->push_back('.');  // Entry delimiter
    out->append(key);     // User key appended directly
}

void Onode::get_omap_tail(std::string *out) const {
    out->clear();
    key_encode_u64(onode.nid, out);
    out->push_back('~');  // Tail delimiter: '-' < '.' < '~'
}

void Onode::decode_omap_key(const std::string &key, std::string *user_key) const {
    // Skip nid (8 bytes) + delimiter (1 byte)
    size_t pos = sizeof(uint64_t) + 1;
    if (key.length() > pos) {
        *user_key = key.substr(pos);
    } else {
        user_key->clear();
    }
}

}  // namespace TOPNSPC
