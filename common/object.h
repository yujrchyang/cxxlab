#pragma once

#include <cstdint>
#include <string>

#include "common_fwd.h"
#include "denc.h"

namespace TOPNSPC {

struct ghobject_t {
    static constexpr int64_t POOL_META = -1;
    static constexpr int64_t POOL_TEMP_START = -2;
    static constexpr uint8_t NO_SHARD = 0xff;
    static constexpr uint64_t NO_GEN = UINT64_MAX;

    int64_t pool = INT64_MIN;
    uint32_t hash = 0;
    std::string nspace;
    std::string key;
    std::string oid;
    uint64_t snap = 0;
    uint64_t generation = NO_GEN;
    uint8_t shard_id = NO_SHARD;
    bool max = false;

    ghobject_t() = default;

    ghobject_t(int64_t p, uint32_t h, std::string_view ns, std::string_view k,
               std::string_view o, uint64_t s, uint64_t gen, uint8_t sid = NO_SHARD)
        : pool(p), hash(h), nspace(ns), key(k), oid(o), snap(s), generation(gen), shard_id(sid) {
        if (key == oid) {
            key.clear();
        }
    }

    bool is_no_gen() const { return generation == NO_GEN; }
    bool is_no_shard() const { return shard_id == NO_SHARD; }
    bool is_max() const { return max; }

    static ghobject_t get_max();

    std::string get_key() const { return key.empty() ? oid : key; }

    static uint32_t reverse_bits(uint32_t v);
    static uint32_t reverse_nibbles(uint32_t v);

    uint32_t get_bitwise_key_u32() const { return reverse_bits(hash); }
    uint32_t get_nibblewise_key_u32() const { return reverse_nibbles(hash); }

    void set_bitwise_key_u32(uint32_t rev) { hash = reverse_bits(rev); }
    void set_nibblewise_key_u32(uint32_t rev) { hash = reverse_nibbles(rev); }

    bool match(uint32_t bits, uint32_t match_val) const;

    DENC(ghobject_t, v, p) {
        DENC_START(2, 1, p);
        if (struct_v >= 2) {
            denc(v.max, p);
        }
        denc(v.pool, p);
        denc(v.hash, p);
        denc(v.nspace, p);
        denc(v.key, p);
        denc(v.oid, p);
        denc(v.snap, p);
        denc(v.generation, p);
        denc(v.shard_id, p);
        DENC_FINISH(p);
    }

    friend bool operator==(const ghobject_t &l, const ghobject_t &r);
    friend bool operator!=(const ghobject_t &l, const ghobject_t &r);
    friend bool operator<(const ghobject_t &l, const ghobject_t &r);
    friend bool operator<=(const ghobject_t &l, const ghobject_t &r);
    friend bool operator>(const ghobject_t &l, const ghobject_t &r);
    friend bool operator>=(const ghobject_t &l, const ghobject_t &r);
};
WRITE_CLASS_DENC(ghobject_t);

constexpr size_t ENCODED_KEY_PREFIX_LEN = 1 + 8 + 4;
constexpr char ONODE_KEY_SUFFIX = 'o';
constexpr char EXTENT_SHARD_KEY_SUFFIX = 'x';

void append_escaped(const std::string &in, std::string *out);
void append_escaped(const std::string &in, bufferlist *out);

void key_encode_u32(uint32_t u, std::string *key);
void key_encode_u64(uint64_t u, std::string *key);

const char *key_decode_u32(const char *key, uint32_t *pu);
const char *key_decode_u64(const char *key, uint64_t *pu);

void key_encode_shard(uint8_t shard, std::string *key);
const char *key_decode_shard(const char *key, uint8_t *pshard);

void key_encode_prefix(const ghobject_t &oid, std::string *key);
const char *key_decode_prefix(const char *p, ghobject_t *oid);

void key_encode_object(const ghobject_t &oid, std::string *key);
int key_decode_object(const std::string &key, ghobject_t *oid);

void key_encode_extent_shard(const std::string &onode_key, uint32_t offset,
                             std::string *key);

}  // namespace TOPNSPC

namespace std {

template <>
struct hash<TOPNSPC::ghobject_t> {
    size_t operator()(const TOPNSPC::ghobject_t &obj) const {
        size_t h = 0;
        h ^= std::hash<int64_t>{}(obj.pool) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}(obj.hash) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>{}(obj.oid) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint64_t>{}(obj.snap) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint64_t>{}(obj.generation) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint8_t>{}(obj.shard_id) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

}  // namespace std
