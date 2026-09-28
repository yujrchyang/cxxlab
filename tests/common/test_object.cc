#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>
#include <string>
#include <vector>

#include "common/denc.h"
#include "common/object.h"

using namespace TOPNSPC;

TEST(Ghobject, DefaultValues) {
    ghobject_t obj;
    EXPECT_EQ(obj.pool, INT64_MIN);
    EXPECT_EQ(obj.hash, 0U);
    EXPECT_TRUE(obj.nspace.empty());
    EXPECT_TRUE(obj.key.empty());
    EXPECT_TRUE(obj.oid.empty());
    EXPECT_EQ(obj.snap, 0ULL);
    EXPECT_EQ(obj.generation, ghobject_t::NO_GEN);
    EXPECT_EQ(obj.shard_id, ghobject_t::NO_SHARD);
}

TEST(Ghobject, Constructor) {
    ghobject_t obj(1, 0x12345678, "ns", "key", "obj", 100, 200, 0);
    EXPECT_EQ(obj.pool, 1);
    EXPECT_EQ(obj.hash, 0x12345678U);
    EXPECT_EQ(obj.nspace, "ns");
    EXPECT_EQ(obj.key, "key");
    EXPECT_EQ(obj.oid, "obj");
    EXPECT_EQ(obj.snap, 100ULL);
    EXPECT_EQ(obj.generation, 200ULL);
    EXPECT_EQ(obj.shard_id, 0);
}

TEST(Ghobject, ConstructorKeySameAsOid) {
    ghobject_t obj(1, 0, "", "same", "same", 0, 0);
    EXPECT_TRUE(obj.key.empty());
    EXPECT_EQ(obj.get_key(), "same");
}

TEST(Ghobject, ConstructorKeyDifferentFromOid) {
    ghobject_t obj(1, 0, "", "k", "o", 0, 0);
    EXPECT_EQ(obj.key, "k");
    EXPECT_EQ(obj.get_key(), "k");
}

TEST(Ghobject, Equality) {
    ghobject_t a(1, 100, "ns", "key", "obj", 50, 1);
    ghobject_t b(1, 100, "ns", "key", "obj", 50, 1);
    EXPECT_EQ(a, b);
    EXPECT_FALSE(a != b);
}

TEST(Ghobject, Inequality) {
    ghobject_t a(1, 100, "ns", "key", "obj", 50, 1);
    ghobject_t b(2, 100, "ns", "key", "obj", 50, 1);
    EXPECT_NE(a, b);
}

TEST(Ghobject, ReverseBits) {
    EXPECT_EQ(ghobject_t::reverse_bits(0x00000001), 0x80000000U);
    EXPECT_EQ(ghobject_t::reverse_bits(0x80000000), 0x00000001U);
    EXPECT_EQ(ghobject_t::reverse_bits(0x12345678), 0x1E6A2C48U);
    EXPECT_EQ(ghobject_t::reverse_bits(0), 0U);
    EXPECT_EQ(ghobject_t::reverse_bits(0xFFFFFFFF), 0xFFFFFFFFU);
}

TEST(Ghobject, ReverseNibbles) {
    EXPECT_EQ(ghobject_t::reverse_nibbles(0x12345678), 0x87654321U);
    EXPECT_EQ(ghobject_t::reverse_nibbles(0), 0U);
    EXPECT_EQ(ghobject_t::reverse_nibbles(0xFFFFFFFF), 0xFFFFFFFFU);
}

TEST(Ghobject, BitwiseKeyRoundtrip) {
    ghobject_t obj(1, 0xABCD1234, "", "", "test", 0, 0);
    uint32_t bitwise = obj.get_bitwise_key_u32();
    ghobject_t obj2;
    obj2.set_bitwise_key_u32(bitwise);
    EXPECT_EQ(obj.hash, obj2.hash);
}

TEST(Ghobject, NibblewiseKeyRoundtrip) {
    ghobject_t obj(1, 0xABCD1234, "", "", "test", 0, 0);
    uint32_t nibblewise = obj.get_nibblewise_key_u32();
    ghobject_t obj2;
    obj2.set_nibblewise_key_u32(nibblewise);
    EXPECT_EQ(obj.hash, obj2.hash);
}

TEST(Ghobject, Match) {
    ghobject_t obj;
    obj.hash = 0x0F;
    EXPECT_TRUE(obj.match(4, 0x0F));
    EXPECT_TRUE(obj.match(8, 0x0F));
    EXPECT_FALSE(obj.match(4, 0x0E));
}

TEST(Ghobject, DencRoundtrip) {
    ghobject_t obj(42, 0x12345678, "namespace", "mykey", "myoid", 100, 200, 3);

    bufferlist bl;
    encode(obj, bl);
    auto p = bl.cbegin();
    ghobject_t obj2;
    decode(obj2, p);

    EXPECT_EQ(obj, obj2);
}

TEST(Ghobject, DencRoundtripMinimal) {
    ghobject_t obj;

    bufferlist bl;
    encode(obj, bl);
    auto p = bl.cbegin();
    ghobject_t obj2;
    decode(obj2, p);

    EXPECT_EQ(obj, obj2);
}

TEST(Ghobject, Ordering) {
    ghobject_t a(1, 10, "", "", "a", 0, 0);
    ghobject_t b(1, 10, "", "", "b", 0, 0);
    ghobject_t c(2, 10, "", "", "a", 0, 0);

    EXPECT_TRUE(a < b);
    EXPECT_TRUE(a < c);
    EXPECT_TRUE(b < c);

    ghobject_t x(1, 20, "", "", "a", 0, 0);
    EXPECT_TRUE(a != x);
}

TEST(AppendEscaped, EmptyString) {
    std::string out;
    append_escaped("", &out);
    EXPECT_EQ(out, "!");
}

TEST(AppendEscaped, NormalChars) {
    std::string out;
    append_escaped("hello", &out);
    EXPECT_EQ(out, "hello!");
}

TEST(AppendEscaped, SpecialChars) {
    std::string out;
    append_escaped("#!", &out);
    EXPECT_EQ(out, "#23#21!");
}

TEST(AppendEscaped, HighChars) {
    std::string out;
    std::string in;
    in.push_back(static_cast<char>(0xff));
    append_escaped(in, &out);
    EXPECT_EQ(out, "~ff!");
}

TEST(KeyEncoding, EncodeDecodeU32) {
    std::string key;
    key_encode_u32(0x12345678, &key);
    EXPECT_EQ(key.size(), 4U);

    uint32_t val;
    key_decode_u32(key.c_str(), &val);
    EXPECT_EQ(val, 0x12345678U);
}

TEST(KeyEncoding, EncodeDecodeU64) {
    std::string key;
    key_encode_u64(0x123456789ABCDEF0ULL, &key);
    EXPECT_EQ(key.size(), 8U);

    uint64_t val;
    key_decode_u64(key.c_str(), &val);
    EXPECT_EQ(val, 0x123456789ABCDEF0ULL);
}

TEST(KeyEncoding, EncodeDecodeShard) {
    std::string key;
    key_encode_shard(5, &key);
    EXPECT_EQ(key.size(), 1U);

    uint8_t shard;
    key_decode_shard(key.c_str(), &shard);
    EXPECT_EQ(shard, 5);
}

TEST(KeyEncoding, EncodeDecodePrefix) {
    ghobject_t obj(1, 0x12345678, "", "", "test", 0, 0);
    obj.shard_id = 2;

    std::string key;
    key_encode_prefix(obj, &key);
    EXPECT_EQ(key.size(), ENCODED_KEY_PREFIX_LEN);

    ghobject_t obj2;
    key_decode_prefix(key.c_str(), &obj2);

    EXPECT_EQ(obj2.shard_id, 2);
    EXPECT_EQ(obj2.pool, 1);
    EXPECT_EQ(obj2.hash, 0x12345678U);
}

TEST(KeyEncoding, EncodeDecodeObject) {
    ghobject_t obj(42, 0xABCD1234, "myns", "mykey", "myoid", 100, 200, 3);

    std::string key;
    key_encode_object(obj, &key);
    EXPECT_FALSE(key.empty());

    ghobject_t obj2;
    int r = key_decode_object(key, &obj2);
    EXPECT_EQ(r, 0);
    EXPECT_EQ(obj, obj2);
}

TEST(KeyEncoding, EncodeDecodeObjectNoKey) {
    ghobject_t obj(1, 100, "ns", "obj", "obj", 50, 1);

    std::string key;
    key_encode_object(obj, &key);

    ghobject_t obj2;
    int r = key_decode_object(key, &obj2);
    EXPECT_EQ(r, 0);
    EXPECT_EQ(obj, obj2);
    EXPECT_TRUE(obj2.key.empty());
}

TEST(KeyEncoding, EncodeDecodeObjectEmptyNspace) {
    ghobject_t obj(1, 100, "", "", "testobj", 0, ghobject_t::NO_GEN);

    std::string key;
    key_encode_object(obj, &key);

    ghobject_t obj2;
    int r = key_decode_object(key, &obj2);
    EXPECT_EQ(r, 0);
    EXPECT_EQ(obj, obj2);
}

TEST(KeyEncoding, SortOrderMatchesComparison) {
    std::vector<ghobject_t> objects;
    objects.emplace_back(1, 10, "ns", "", "obj1", 0, 0, 0);
    objects.emplace_back(1, 20, "ns", "", "obj2", 0, 0, 0);
    objects.emplace_back(2, 10, "ns", "", "obj3", 0, 0, 0);
    objects.emplace_back(1, 10, "other", "", "obj4", 0, 0, 0);

    std::vector<std::string> keys;
    for (const auto &obj : objects) {
        std::string key;
        key_encode_object(obj, &key);
        keys.push_back(key);
    }

    std::vector<size_t> indices(objects.size());
    std::iota(indices.begin(), indices.end(), 0);

    std::sort(indices.begin(), indices.end(),
              [&objects](size_t a, size_t b) { return objects[a] < objects[b]; });

    std::vector<size_t> key_indices(objects.size());
    std::iota(key_indices.begin(), key_indices.end(), 0);

    std::sort(key_indices.begin(), key_indices.end(),
              [&keys](size_t a, size_t b) { return keys[a] < keys[b]; });

    EXPECT_EQ(indices, key_indices);
}

TEST(KeyEncoding, ExtentShardKey) {
    ghobject_t obj(1, 100, "", "", "testobj", 0, 0);

    std::string onode_key;
    key_encode_object(obj, &onode_key);

    std::string shard_key;
    key_encode_extent_shard(onode_key, 0x1000, &shard_key);

    EXPECT_EQ(shard_key.size(), onode_key.size() + 4 + 1);
    EXPECT_EQ(shard_key.back(), EXTENT_SHARD_KEY_SUFFIX);

    std::string expected_prefix = onode_key;
    EXPECT_EQ(shard_key.substr(0, onode_key.size()), expected_prefix);
}

TEST(KeyEncoding, MultipleObjectsSortOrder) {
    std::vector<ghobject_t> objects;

    objects.emplace_back(1, 0, "", "", "a", 0, 0);
    objects.emplace_back(1, 0, "", "", "b", 0, 0);
    objects.emplace_back(1, 0, "", "", "c", 0, 0);

    std::vector<std::string> keys;
    for (const auto &obj : objects) {
        std::string key;
        key_encode_object(obj, &key);
        keys.push_back(key);
    }

    for (size_t i = 0; i < keys.size() - 1; ++i) {
        EXPECT_LT(keys[i], keys[i + 1])
            << "Key " << i << " should be less than key " << (i + 1);
    }
}
