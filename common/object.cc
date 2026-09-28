#include "object.h"

#include <cstring>

namespace TOPNSPC {

uint32_t ghobject_t::reverse_bits(uint32_t v) {
    v = ((v >> 1) & 0x55555555) | ((v & 0x55555555) << 1);
    v = ((v >> 2) & 0x33333333) | ((v & 0x33333333) << 2);
    v = ((v >> 4) & 0x0f0f0f0f) | ((v & 0x0f0f0f0f) << 4);
    v = ((v >> 8) & 0x00ff00ff) | ((v & 0x00ff00ff) << 8);
    v = (v >> 16) | (v << 16);
    return v;
}

uint32_t ghobject_t::reverse_nibbles(uint32_t v) {
    v = ((v >> 4) & 0x0f0f0f0f) | ((v & 0x0f0f0f0f) << 4);
    v = ((v >> 8) & 0x00ff00ff) | ((v & 0x00ff00ff) << 8);
    v = (v >> 16) | (v << 16);
    return v;
}

bool ghobject_t::match(uint32_t bits, uint32_t match_val) const {
    return (match_val & ~((~0u) << bits)) == (hash & ~((~0u) << bits));
}

bool operator==(const ghobject_t &l, const ghobject_t &r) {
    return l.pool == r.pool && l.hash == r.hash && l.nspace == r.nspace &&
        l.key == r.key && l.oid == r.oid && l.snap == r.snap &&
        l.generation == r.generation && l.shard_id == r.shard_id;
}

bool operator!=(const ghobject_t &l, const ghobject_t &r) {
    return !(l == r);
}

bool operator<(const ghobject_t &l, const ghobject_t &r) {
    if (l.shard_id != r.shard_id) return l.shard_id < r.shard_id;
    if (l.pool != r.pool) return l.pool < r.pool;
    if (l.get_bitwise_key_u32() != r.get_bitwise_key_u32())
        return l.get_bitwise_key_u32() < r.get_bitwise_key_u32();
    if (l.nspace != r.nspace) return l.nspace < r.nspace;
    auto lk = l.get_key();
    auto rk = r.get_key();
    if (lk != rk) return lk < rk;
    if (l.oid != r.oid) return l.oid < r.oid;
    if (l.snap != r.snap) return l.snap < r.snap;
    if (l.generation != r.generation) return l.generation < r.generation;
    return false;
}

bool operator<=(const ghobject_t &l, const ghobject_t &r) {
    return !(r < l);
}

bool operator>(const ghobject_t &l, const ghobject_t &r) {
    return r < l;
}

bool operator>=(const ghobject_t &l, const ghobject_t &r) {
    return !(l < r);
}

void append_escaped(const std::string &in, std::string *out) {
    out->reserve(out->size() + in.size() * 3 + 1);
    for (auto c : in) {
        if (c <= '#') {
            out->push_back('#');
            out->push_back("0123456789abcdef"[(c >> 4) & 0x0f]);
            out->push_back("0123456789abcdef"[c & 0x0f]);
        } else if (c >= '~') {
            out->push_back('~');
            out->push_back("0123456789abcdef"[(c >> 4) & 0x0f]);
            out->push_back("0123456789abcdef"[c & 0x0f]);
        } else {
            out->push_back(c);
        }
    }
    out->push_back('!');
}

void append_escaped(const std::string &in, bufferlist *out) {
    std::string tmp;
    append_escaped(in, &tmp);
    out->append(tmp);
}

void key_encode_u32(uint32_t u, std::string *key) {
    uint32_t bu = swab(u);
    key->append(reinterpret_cast<const char *>(&bu), sizeof(bu));
}

void key_encode_u64(uint64_t u, std::string *key) {
    uint64_t bu = swab(u);
    key->append(reinterpret_cast<const char *>(&bu), sizeof(bu));
}

const char *key_decode_u32(const char *key, uint32_t *pu) {
    uint32_t bu;
    std::memcpy(&bu, key, sizeof(bu));
    *pu = swab(bu);
    return key + sizeof(bu);
}

const char *key_decode_u64(const char *key, uint64_t *pu) {
    uint64_t bu;
    std::memcpy(&bu, key, sizeof(bu));
    *pu = swab(bu);
    return key + sizeof(bu);
}

void key_encode_shard(uint8_t shard, std::string *key) {
    key->push_back(static_cast<char>(shard + 0x80));
}

const char *key_decode_shard(const char *key, uint8_t *pshard) {
    *pshard = static_cast<uint8_t>(key[0]) - 0x80;
    return key + 1;
}

void key_encode_prefix(const ghobject_t &oid, std::string *key) {
    key_encode_shard(oid.shard_id, key);
    key_encode_u64(oid.pool + 0x8000000000000000ull, key);
    key_encode_u32(oid.get_bitwise_key_u32(), key);
}

const char *key_decode_prefix(const char *p, ghobject_t *oid) {
    p = key_decode_shard(p, &oid->shard_id);
    uint64_t pool;
    p = key_decode_u64(p, &pool);
    oid->pool = pool - 0x8000000000000000ull;
    uint32_t hash;
    p = key_decode_u32(p, &hash);
    oid->set_bitwise_key_u32(hash);
    return p;
}

static int decode_escaped(const char *p, std::string *out) {
    const char *orig_p = p;
    while (*p && *p != '!') {
        if (*p == '#' || *p == '~') {
            ++p;
            unsigned hex = 0;
            char c = *p++;
            if (c >= '0' && c <= '9')
                hex = (c - '0') << 4;
            else if (c >= 'a' && c <= 'f')
                hex = (c - 'a' + 10) << 4;
            else if (c >= 'A' && c <= 'F')
                hex = (c - 'A' + 10) << 4;
            else
                return -1;

            c = *p++;
            if (c >= '0' && c <= '9')
                hex |= (c - '0');
            else if (c >= 'a' && c <= 'f')
                hex |= (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                hex |= (c - 'A' + 10);
            else
                return -1;

            out->push_back(static_cast<char>(hex));
        } else {
            out->push_back(*p++);
        }
    }
    return p - orig_p;
}

void key_encode_object(const ghobject_t &oid, std::string *key) {
    key->clear();
    size_t max_len = ENCODED_KEY_PREFIX_LEN + (oid.nspace.size() * 3 + 1) +
        (oid.get_key().size() * 3 + 1) + 1 +
        (oid.oid.size() * 3 + 1) + 8 + 8 + 1;
    key->reserve(max_len);

    key_encode_prefix(oid, key);
    append_escaped(oid.nspace, key);

    std::string k = oid.get_key();
    if (k != oid.oid) {
        append_escaped(k, key);
        int r = k.compare(oid.oid);
        if (r > 0) {
            key->push_back('>');
        } else {
            key->push_back('<');
        }
        append_escaped(oid.oid, key);
    } else {
        append_escaped(oid.oid, key);
        key->push_back('=');
    }

    key_encode_u64(oid.snap, key);
    key_encode_u64(oid.generation, key);
    key->push_back(ONODE_KEY_SUFFIX);
}

int key_decode_object(const std::string &key, ghobject_t *oid) {
    if (key.length() < ENCODED_KEY_PREFIX_LEN) return -1;

    const char *p = key.c_str();
    p = key_decode_prefix(p, oid);

    int r = decode_escaped(p, &oid->nspace);
    if (r < 0) return -2;
    p += r + 1;

    std::string k;
    r = decode_escaped(p, &k);
    if (r < 0) return -3;
    p += r + 1;

    if (*p == '=') {
        ++p;
        oid->key.clear();
        oid->oid = k;
    } else if (*p == '<' || *p == '>') {
        ++p;
        r = decode_escaped(p, &oid->oid);
        if (r < 0) return -5;
        p += r + 1;
        oid->key = k;
    } else {
        return -6;
    }

    p = key_decode_u64(p, &oid->snap);
    p = key_decode_u64(p, &oid->generation);

    if (*p != ONODE_KEY_SUFFIX) return -7;
    ++p;
    if (*p) return -8;

    return 0;
}

void key_encode_extent_shard(const std::string &onode_key, uint32_t offset,
                             std::string *key) {
    key->clear();
    key->reserve(onode_key.size() + 4 + 1);
    key->append(onode_key);
    key_encode_u32(offset, key);
    key->push_back(EXTENT_SHARD_KEY_SUFFIX);
}

}  // namespace TOPNSPC
