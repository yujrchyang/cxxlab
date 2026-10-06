#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "common/buffer.h"
#include "common/crc32.h"

using namespace TOPNSPC;

TEST(TestBufferPtr, DefaultConstructor) {
    buffer::ptr bp;
    EXPECT_FALSE(bp.have_raw());
    EXPECT_EQ(0u, bp.length());
    EXPECT_EQ(0u, bp.offset());
}

TEST(TestBufferPtr, Create) {
    buffer::ptr bp(100);
    EXPECT_TRUE(bp.have_raw());
    EXPECT_EQ(100u, bp.length());
    EXPECT_NE(nullptr, bp.c_str());
}

TEST(TestBufferPtr, Copy) {
    const char *data = "hello world";
    buffer::ptr bp(data, strlen(data));
    EXPECT_EQ(strlen(data), bp.length());
    EXPECT_EQ(0, memcmp(data, bp.c_str(), strlen(data)));
}

TEST(TestBufferPtr, CopyConstructor) {
    buffer::ptr bp1("test", 4);
    buffer::ptr bp2(bp1);
    EXPECT_EQ(bp1.length(), bp2.length());
    EXPECT_EQ(bp1.c_str(), bp2.c_str());
}

TEST(TestBufferPtr, MoveConstructor) {
    buffer::ptr bp1("test", 4);
    const char *raw = bp1.c_str();
    buffer::ptr bp2(std::move(bp1));
    EXPECT_FALSE(bp1.have_raw());
    EXPECT_EQ(raw, bp2.c_str());
}

TEST(TestBufferPtr, Assignment) {
    buffer::ptr bp1("hello", 5);
    buffer::ptr bp2;
    bp2 = bp1;
    EXPECT_EQ(bp1.length(), bp2.length());
    EXPECT_EQ(bp1.c_str(), bp2.c_str());
}

TEST(TestBufferPtr, MoveAssignment) {
    buffer::ptr bp1("hello", 5);
    const char *raw = bp1.c_str();
    buffer::ptr bp2;
    bp2 = std::move(bp1);
    EXPECT_FALSE(bp1.have_raw());
    EXPECT_EQ(raw, bp2.c_str());
}

TEST(TestBufferPtr, Clone) {
    buffer::ptr bp1("clone me", 8);
    auto cloned = bp1.clone();
    EXPECT_EQ(bp1.length(), cloned->get_len());
}

TEST(TestBufferPtr, IsAligned) {
    buffer::ptr bp(64);
    EXPECT_TRUE(bp.is_aligned(1));
    EXPECT_TRUE(bp.is_aligned(4));
}

TEST(TestBufferPtr, Cmp) {
    buffer::ptr bp1("abc", 3);
    buffer::ptr bp2("abc", 3);
    buffer::ptr bp3("abd", 3);
    EXPECT_EQ(0, bp1.cmp(bp2));
    EXPECT_GT(0, bp1.cmp(bp3));
    EXPECT_LT(0, bp3.cmp(bp1));
}

TEST(TestBufferPtr, Swap) {
    buffer::ptr bp1("first", 5);
    buffer::ptr bp2("second", 6);
    const char *p1 = bp1.c_str();
    const char *p2 = bp2.c_str();
    bp1.swap(bp2);
    EXPECT_EQ(p1, bp2.c_str());
    EXPECT_EQ(p2, bp1.c_str());
}

TEST(TestBufferList, DefaultConstructor) {
    buffer::list bl;
    EXPECT_EQ(0u, bl.length());
    EXPECT_EQ(0u, bl.get_num_buffers());
}

TEST(TestBufferList, PushBackPtr) {
    buffer::list bl;
    buffer::ptr bp("hello", 5);
    bl.push_back(bp);
    EXPECT_EQ(5u, bl.length());
    EXPECT_EQ(1u, bl.get_num_buffers());
}

TEST(TestBufferList, PushBackMovePtr) {
    buffer::list bl;
    bl.push_back(buffer::ptr("hello", 5));
    EXPECT_EQ(5u, bl.length());
    EXPECT_EQ(1u, bl.get_num_buffers());
}

TEST(TestBufferList, AppendChar) {
    buffer::list bl;
    bl.append('a');
    bl.append('b');
    bl.append('c');
    EXPECT_EQ(3u, bl.length());
    EXPECT_EQ('a', bl[0]);
    EXPECT_EQ('b', bl[1]);
    EXPECT_EQ('c', bl[2]);
}

TEST(TestBufferList, AppendData) {
    buffer::list bl;
    bl.append("hello", 5);
    EXPECT_EQ(5u, bl.length());
    EXPECT_EQ(0, memcmp("hello", bl.c_str(), 5));
}

TEST(TestBufferList, AppendString) {
    buffer::list bl;
    std::string s = "world";
    bl.append(buffer::ptr(s.data(), s.size()));
    EXPECT_EQ(5u, bl.length());
    EXPECT_EQ(0, memcmp("world", bl.c_str(), 5));
}

TEST(TestBufferList, CopyAndCompare) {
    buffer::list bl1;
    bl1.append("hello", 5);
    bl1.append(" ", 1);
    bl1.append("world", 5);

    buffer::list bl2(bl1);
    EXPECT_TRUE(bl1.contents_equal(bl2));

    buffer::list bl3;
    bl3.append("hello world", 11);
    EXPECT_TRUE(bl1.contents_equal(bl3));
}

TEST(TestBufferList, Clear) {
    buffer::list bl;
    bl.append("test", 4);
    EXPECT_EQ(4u, bl.length());
    bl.clear();
    EXPECT_EQ(0u, bl.length());
    EXPECT_EQ(0u, bl.get_num_buffers());
}

TEST(TestBufferList, Iterator) {
    buffer::list bl;
    bl.append("abc", 3);
    bl.append("def", 3);

    std::string result;
    for (auto it = bl.begin(); !it.end(); ++it) {
        result += *it;
    }
    EXPECT_EQ("abcdef", result);
}

TEST(TestBufferList, Equality) {
    buffer::list bl1, bl2;
    bl1.append("test", 4);
    bl2.append("test", 4);
    EXPECT_TRUE(bl1 == bl2);
    EXPECT_FALSE(bl1 != bl2);
}

TEST(TestBufferList, Inequality) {
    buffer::list bl1, bl2;
    bl1.append("abc", 3);
    bl2.append("def", 3);
    EXPECT_TRUE(bl1 != bl2);
    EXPECT_TRUE(bl1 < bl2);
    EXPECT_TRUE(bl2 > bl1);
}

TEST(TestBufferList, ToString) {
    buffer::list bl;
    bl.append("hello", 5);
    bl.append(" ", 1);
    bl.append("world", 5);
    EXPECT_EQ("hello world", bl.to_str());
}

TEST(TestBufferList, PrependZero) {
    buffer::list bl;
    bl.append("hello", 5);
    bl.prepend_zero(3);
    EXPECT_EQ(8u, bl.length());
}

TEST(TestBufferHash, Basic) {
    buffer::list bl;
    bl.append("test data", 9);
    buffer::hash h1;
    h1.update(bl);
    buffer::hash h2;
    h2.update(bl);
    EXPECT_EQ(h1.digest(), h2.digest());
}

TEST(TestBufferHash, Accumulate) {
    buffer::list bl;
    bl.append("hello", 5);
    buffer::hash h;
    h.update(bl);
    uint32_t d1 = h.digest();
    bl.append(" world", 6);
    h.update(bl);
    uint32_t d2 = h.digest();
    EXPECT_NE(d1, d2);
    buffer::hash h3;
    h3.update(bl);
    EXPECT_NE(d2, h3.digest());
}

TEST(TestBufferNamedCreate, Create) {
    auto r = buffer::create(100);
    ASSERT_NE(nullptr, r);
    EXPECT_EQ(100u, r->get_len());
}

TEST(TestBufferNamedCreate, Copy) {
    const char *data = "hello world";
    auto r = buffer::copy(data, strlen(data));
    ASSERT_NE(nullptr, r);
    EXPECT_EQ(0, memcmp(data, r->get_data(), strlen(data)));
}

TEST(TestBufferNamedCreate, CreateWithChar) {
    auto r = buffer::create(10, 'A');
    ASSERT_NE(nullptr, r);
    for (unsigned i = 0; i < 10; i++) {
        EXPECT_EQ('A', r->get_data()[i]);
    }
}

TEST(TestBufferNamedCreate, Malloc) {
    auto r = buffer::create_malloc(64);
    ASSERT_NE(nullptr, r);
    EXPECT_EQ(64u, r->get_len());
}

TEST(TestBufferNamedCreate, Static) {
    char data[] = "static buffer";
    auto r = buffer::create_static(strlen(data), data);
    ASSERT_NE(nullptr, r);
    EXPECT_EQ(data, r->get_data());
}

TEST(TestBufferPtr, AppendToPtr) {
    buffer::ptr bp(10);
    bp.set_length(0);
    unsigned pos = bp.append('x');
    EXPECT_EQ(1u, pos);
    EXPECT_EQ(1u, bp.length());
    EXPECT_EQ('x', bp[0]);

    const char *extra = "yz";
    pos = bp.append(extra, 2);
    EXPECT_EQ(3u, pos);
    EXPECT_EQ(3u, bp.length());
    EXPECT_EQ(0, memcmp("xyz", bp.c_str(), 3));
}

TEST(TestBufferPtr, AppendZeros) {
    buffer::ptr bp(10);
    bp.set_length(0);
    bp.append_zeros(5);
    EXPECT_EQ(5u, bp.length());
    for (unsigned i = 0; i < 5; i++) {
        EXPECT_EQ('\0', bp[i]);
    }
}

TEST(TestBufferPtr, CopyInOut) {
    buffer::ptr bp(10);
    bp.set_length(10);
    const char *src = "abcdefghij";
    bp.copy_in(0, 10, src);

    char dest[11] = {};
    bp.copy_out(0, 10, dest);
    EXPECT_EQ(0, memcmp(src, dest, 10));
}

TEST(TestBufferPtr, Zero) {
    buffer::ptr bp(10);
    bp.set_length(10);
    bp.copy_in(0, 10, "abcdefghij");
    bp.zero();
    for (unsigned i = 0; i < 10; i++) {
        EXPECT_EQ('\0', bp[i]);
    }
}

TEST(TestBufferPtr, IsZero) {
    buffer::ptr bp(10);
    bp.set_length(10);
    bp.zero();
    EXPECT_TRUE(bp.is_zero());
    bp[0] = 1;
    EXPECT_FALSE(bp.is_zero());
}

TEST(TestBufferPtr, Partial) {
    buffer::ptr full(100);
    full.set_length(100);
    buffer::ptr partial(full, 10, 20);
    EXPECT_TRUE(partial.is_partial());
    EXPECT_EQ(10u, partial.offset());
    EXPECT_EQ(20u, partial.length());
}

TEST(TestBufferPtr, SubPtr) {
    buffer::ptr bp("hello world", 11);
    buffer::ptr sub(bp, 6, 5);
    EXPECT_EQ(5u, sub.length());
    EXPECT_EQ(0, memcmp("world", sub.c_str(), 5));
}

TEST(TestBufferList, SubstrOf) {
    buffer::list bl;
    bl.append("hello ", 6);
    bl.append("world", 5);

    buffer::list sub;
    sub.substr_of(bl, 6, 5);
    EXPECT_EQ(5u, sub.length());
    EXPECT_EQ(0, memcmp("world", sub.c_str(), 5));
}

TEST(TestBufferList, Rebuild) {
    buffer::list bl;
    bl.append("hello ", 6);
    bl.append("world", 5);

    bl.rebuild();
    EXPECT_EQ(11u, bl.length());
    EXPECT_EQ(0, memcmp("hello world", bl.c_str(), 11));
}

TEST(TestBufferList, MoveAssignment) {
    buffer::list bl1;
    bl1.append("test", 4);
    buffer::list bl2;
    bl2 = std::move(bl1);
    EXPECT_EQ(4u, bl2.length());
    EXPECT_EQ(0u, bl1.length());
}

TEST(TestBufferList, StaticFromMem) {
    char data[] = "static data";
    auto bl = buffer::list::static_from_mem(data, strlen(data));
    EXPECT_EQ(strlen(data), bl.length());
    EXPECT_EQ(0, memcmp(data, bl.c_str(), strlen(data)));
}

TEST(BufferListCrc32c, MultiBufferMatchesSequential) {
    buffer::list bl;
    bl.append("hello ", 6);
    bl.append("world", 5);
    uint32_t whole = bl.crc32c(0);
    uint32_t seq = calc_crc32(
        reinterpret_cast<const uint8_t *>("hello "), 6, 0);
    seq = calc_crc32(
        reinterpret_cast<const uint8_t *>("world"), 5, seq);
    EXPECT_EQ(whole, seq);
}

TEST(BufferListCrc32c, CacheHitBaseMatch) {
    buffer::list bl;
    bl.append("hello ", 6);
    bl.append("world", 5);
    uint32_t crc1 = bl.crc32c(0);
    uint32_t crc2 = bl.crc32c(0);
    EXPECT_EQ(crc1, crc2);
}

TEST(BufferListCrc32c, CacheAdjustCorrectness) {
    buffer::ptr shared("abcdefghij", 10);

    buffer::list bl2;
    bl2.push_back(shared);
    uint32_t crc2 = bl2.crc32c(0);

    buffer::list bl1;
    bl1.append("XX", 2);
    bl1.push_back(shared);
    uint32_t crc1 = bl1.crc32c(0);

    uint32_t expect2 = calc_crc32(
        reinterpret_cast<const uint8_t *>("abcdefghij"), 10, 0);
    EXPECT_EQ(crc2, expect2);

    uint32_t expect1 = calc_crc32(
        reinterpret_cast<const uint8_t *>("XX"), 2, 0);
    expect1 = calc_crc32(
        reinterpret_cast<const uint8_t *>("abcdefghij"), 10, expect1);
    EXPECT_EQ(crc1, expect1);
}
