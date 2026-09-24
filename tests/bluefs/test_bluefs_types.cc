#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "bluefs/bluefs_types.h"
#include "common/denc.h"

using namespace TOPNSPC;

namespace {

bool extent_eq(const bluefs_extent_t &a, const bluefs_extent_t &b) {
    return a.bdev == b.bdev && a.offset == b.offset && a.length == b.length;
}

bool extents_eq(const std::vector<bluefs_extent_t> &a,
                const std::vector<bluefs_extent_t> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!extent_eq(a[i], b[i])) return false;
    }
    return true;
}

TEST(BlueFSExtent, DencRoundtrip) {
    bluefs_extent_t e{1, 0x1000, 0x2000};
    bufferlist bl;
    encode(e, bl);
    auto p = bl.cbegin();
    bluefs_extent_t f;
    decode(f, p);
    EXPECT_EQ(e.bdev, f.bdev);
    EXPECT_EQ(e.offset, f.offset);
    EXPECT_EQ(e.length, f.length);
}

TEST(BlueFSExtent, DefaultValues) {
    bluefs_extent_t e;
    EXPECT_EQ(e.bdev, 0);
    EXPECT_EQ(e.offset, 0ULL);
    EXPECT_EQ(e.length, 0U);
}

TEST(BlueFSExtent, End) {
    bluefs_extent_t e{0, 100, 50};
    EXPECT_EQ(e.end(), 150ULL);
}

TEST(BlueFSFnodeDelta, DencRoundtrip) {
    bluefs_fnode_delta_t d;
    d.ino = 42;
    d.size = 1000;
    d.mtime = 12345;
    d.offset = 500;
    d.extents.push_back({0, 0x1000, 0x2000});
    d.extents.push_back({1, 0x4000, 0x1000});

    bufferlist bl;
    encode(d, bl);
    auto p = bl.cbegin();
    bluefs_fnode_delta_t e;
    decode(e, p);
    EXPECT_EQ(d.ino, e.ino);
    EXPECT_EQ(d.size, e.size);
    EXPECT_EQ(d.mtime, e.mtime);
    EXPECT_EQ(d.offset, e.offset);
    ASSERT_EQ(d.extents.size(), e.extents.size());
    EXPECT_TRUE(extents_eq(d.extents, e.extents));
}

TEST(BlueFSFnode, DefaultValues) {
    bluefs_fnode_t f;
    EXPECT_EQ(f.ino, 0ULL);
    EXPECT_EQ(f.size, 0ULL);
    EXPECT_EQ(f.mtime, 0ULL);
    EXPECT_EQ(f.allocated, 0ULL);
    EXPECT_EQ(f.allocated_committed, 0ULL);
    EXPECT_TRUE(f.extents.empty());
    EXPECT_TRUE(f.extents_index.empty());
}

TEST(BlueFSFnode, Constructor) {
    bluefs_fnode_t f{100, 5000, 98765};
    EXPECT_EQ(f.ino, 100ULL);
    EXPECT_EQ(f.size, 5000ULL);
    EXPECT_EQ(f.mtime, 98765ULL);
    EXPECT_EQ(f.get_allocated(), 0ULL);
    EXPECT_TRUE(f.extents.empty());
}

TEST(BlueFSFnode, AppendExtentCoalesce) {
    bluefs_fnode_t f;
    f.append_extent({0, 0, 0x1000});
    f.append_extent({0, 0x1000, 0x2000});  // adjacent, same bdev -> coalesce
    ASSERT_EQ(f.extents.size(), 1U);
    EXPECT_EQ(f.extents[0].offset, 0ULL);
    EXPECT_EQ(f.extents[0].length, 0x3000U);
    EXPECT_EQ(f.extents[0].bdev, 0);
    EXPECT_EQ(f.allocated, 0x3000ULL);
    // first append's else branch pushes index entry
    EXPECT_EQ(f.extents_index.size(), 1U);
    EXPECT_EQ(f.extents_index[0], 0ULL);
}

TEST(BlueFSFnode, AppendExtentNoCoalesceDiffBdev) {
    bluefs_fnode_t f;
    f.append_extent({0, 0, 0x1000});
    f.append_extent({1, 0x1000, 0x2000});  // diff bdev -> no coalesce
    ASSERT_EQ(f.extents.size(), 2U);
    EXPECT_EQ(f.extents_index.size(), 2U);
    EXPECT_EQ(f.extents_index[0], 0ULL);
    EXPECT_EQ(f.extents_index[1], 0x1000ULL);
    EXPECT_EQ(f.allocated, 0x3000ULL);
}

TEST(BlueFSFnode, AppendExtentNoCoalesceNonAdjacent) {
    bluefs_fnode_t f;
    f.append_extent({0, 0, 0x1000});
    f.append_extent({0, 0x3000, 0x1000});  // gap -> no coalesce
    ASSERT_EQ(f.extents.size(), 2U);
    EXPECT_EQ(f.extents_index.size(), 2U);
    EXPECT_EQ(f.extents_index[0], 0ULL);
    EXPECT_EQ(f.extents_index[1], 0x1000ULL);
    EXPECT_EQ(f.allocated, 0x2000ULL);
}

TEST(BlueFSFnode, AppendExtentCoalesceOverflow) {
    bluefs_fnode_t f;
    f.append_extent({0, 0, 0xFFFFFFFF});
    f.append_extent({0, 0xFFFFFFFF, 1});  // would overflow uint32 -> no coalesce
    ASSERT_EQ(f.extents.size(), 2U);
}

TEST(BlueFSFnode, RecalcAllocated) {
    bluefs_fnode_t f;
    f.extents.push_back({0, 0, 0x1000});
    f.extents.push_back({1, 0x2000, 0x2000});
    f.extents.push_back({0, 0x4000, 0x500});
    f.recalc_allocated();
    EXPECT_EQ(f.allocated, 0x3500ULL);
    EXPECT_EQ(f.allocated_committed, 0x3500ULL);
    ASSERT_EQ(f.extents_index.size(), 3U);
    EXPECT_EQ(f.extents_index[0], 0ULL);
    EXPECT_EQ(f.extents_index[1], 0x1000ULL);
    EXPECT_EQ(f.extents_index[2], 0x3000ULL);
}

TEST(BlueFSFnode, Seek) {
    bluefs_fnode_t f;
    f.append_extent({0, 0, 0x1000});
    f.append_extent({1, 0x1000, 0x2000});
    f.append_extent({0, 0x3000, 0x500});

    uint64_t x_off;
    auto it = f.seek(0, &x_off);
    ASSERT_NE(it, f.extents.end());
    EXPECT_EQ(it->bdev, 0);
    EXPECT_EQ(it->offset, 0ULL);
    EXPECT_EQ(x_off, 0ULL);

    it = f.seek(0x800, &x_off);
    ASSERT_NE(it, f.extents.end());
    EXPECT_EQ(it->bdev, 0);
    EXPECT_EQ(x_off, 0x800ULL);

    it = f.seek(0x1000, &x_off);
    ASSERT_NE(it, f.extents.end());
    EXPECT_EQ(it->bdev, 1);
    EXPECT_EQ(x_off, 0ULL);

    it = f.seek(0x3000, &x_off);
    ASSERT_NE(it, f.extents.end());
    EXPECT_EQ(it->bdev, 0);
    EXPECT_EQ(x_off, 0ULL);

    it = f.seek(0x4000, &x_off);  // past end
    EXPECT_EQ(it, f.extents.end());
}

TEST(BlueFSFnode, SeekEmpty) {
    bluefs_fnode_t f;
    uint64_t x_off;
    auto it = f.seek(0, &x_off);
    EXPECT_EQ(it, f.extents.end());
}

TEST(BlueFSFnode, MakeDelta) {
    bluefs_fnode_t f;
    f.ino = 1;
    f.size = 0x5000;
    f.mtime = 100;
    f.append_extent({0, 0, 0x1000});
    f.append_extent({1, 0x1000, 0x2000});
    f.append_extent({0, 0x3000, 0x500});
    f.reset_delta();  // committed = allocated

    // delta should be empty since all committed
    auto d = f.make_delta();
    EXPECT_EQ(d.offset, f.allocated);
    EXPECT_TRUE(d.extents.empty());

    // add a new extent (size is separate from allocated)
    f.append_extent({0, 0x4000, 0x1000});
    d = f.make_delta();
    ASSERT_EQ(d.extents.size(), 1U);
    EXPECT_TRUE(extent_eq(d.extents[0], {0, 0x4000, 0x1000}));
    EXPECT_EQ(d.ino, 1ULL);
    EXPECT_EQ(d.size, 0x5000ULL);
    EXPECT_EQ(d.mtime, 100ULL);
}

TEST(BlueFSFnode, MakeDeltaPartialCommit) {
    bluefs_fnode_t f;
    f.ino = 1;
    f.append_extent({0, 0, 0x1000});
    f.append_extent({1, 0x1000, 0x2000});
    f.allocated_committed = 0x800;  // partially committed

    auto d = f.make_delta();
    ASSERT_EQ(d.extents.size(), 2U);
    EXPECT_TRUE(extent_eq(d.extents[0], {0, 0x800, 0x800}));
    EXPECT_TRUE(extent_eq(d.extents[1], {1, 0x1000, 0x2000}));
}

TEST(BlueFSFnode, CloneExtents) {
    bluefs_fnode_t f;
    f.append_extent({0, 0, 0x1000});
    f.append_extent({1, 0x1000, 0x2000});

    bluefs_fnode_t g;
    g.clone_extents(f);
    EXPECT_EQ(f.allocated, g.allocated);
    ASSERT_EQ(f.extents.size(), g.extents.size());
    EXPECT_TRUE(extents_eq(f.extents, g.extents));
}

TEST(BlueFSFnode, Swap) {
    bluefs_fnode_t f{1, 100, 10};
    f.append_extent({0, 0, 0x1000});
    bluefs_fnode_t g{2, 200, 20};
    g.append_extent({1, 0, 0x2000});

    f.swap(g);
    EXPECT_EQ(f.ino, 2ULL);
    EXPECT_EQ(g.ino, 1ULL);
    EXPECT_EQ(f.size, 200ULL);
    EXPECT_EQ(g.size, 100ULL);
    ASSERT_EQ(f.extents.size(), 1U);
    EXPECT_EQ(f.extents[0].bdev, 1);
    ASSERT_EQ(g.extents.size(), 1U);
    EXPECT_EQ(g.extents[0].bdev, 0);
}

TEST(BlueFSFnode, ClearExtents) {
    bluefs_fnode_t f;
    f.append_extent({0, 0, 0x1000});
    EXPECT_GT(f.allocated, 0ULL);
    f.clear_extents();
    EXPECT_TRUE(f.extents.empty());
    EXPECT_TRUE(f.extents_index.empty());
    EXPECT_EQ(f.allocated, 0ULL);
    EXPECT_EQ(f.allocated_committed, 0ULL);
}

TEST(BlueFSFnode, PopFrontExtent) {
    bluefs_fnode_t f;
    f.append_extent({0, 0, 0x1000});
    f.append_extent({1, 0x1000, 0x2000});
    f.append_extent({0, 0x3000, 0x500});

    uint64_t before = f.allocated;
    uint64_t front_len = f.extents[0].length;
    f.pop_front_extent();
    EXPECT_EQ(f.allocated, before - front_len);
    ASSERT_EQ(f.extents.size(), 2U);
    // remaining extents_index should be shifted
    ASSERT_EQ(f.extents_index.size(), 2U);
    EXPECT_EQ(f.extents_index[0], 0ULL);
    EXPECT_EQ(f.extents_index[1], 0x2000ULL);
    // seek should now refer to the new first extent
    uint64_t x_off;
    auto it = f.seek(0, &x_off);
    ASSERT_NE(it, f.extents.end());
    EXPECT_EQ(it->bdev, 1);
}

TEST(BlueFSFnode, DencRoundtrip) {
    bluefs_fnode_t f{42, 0x5000, 12345};
    f.append_extent({0, 0, 0x1000});
    f.append_extent({1, 0x1000, 0x2000});
    f.reset_delta();

    bufferlist bl;
    encode(f, bl);
    auto p = bl.cbegin();
    bluefs_fnode_t g;
    decode(g, p);
    EXPECT_EQ(f.ino, g.ino);
    EXPECT_EQ(f.size, g.size);
    EXPECT_EQ(f.mtime, g.mtime);
    EXPECT_EQ(f.allocated, g.allocated);
    EXPECT_EQ(f.allocated_committed, g.allocated_committed);
    ASSERT_EQ(f.extents.size(), g.extents.size());
    EXPECT_TRUE(extents_eq(f.extents, g.extents));
    // extents_index should be rebuilt by recalc_allocated
    ASSERT_EQ(g.extents_index.size(), g.extents.size());
    EXPECT_EQ(g.extents_index[0], 0ULL);
    EXPECT_EQ(g.extents_index[1], 0x1000ULL);
}

TEST(BlueFSFnode, DencRoundtripAllocatedNotCommitted) {
    bluefs_fnode_t f{1, 0x3000, 0};
    f.append_extent({0, 0, 0x1000});
    f.allocated_committed = 0;
    EXPECT_NE(f.allocated, f.allocated_committed);

    bufferlist bl;
    encode(f, bl);
    auto p = bl.cbegin();
    bluefs_fnode_t g;
    decode(g, p);
    EXPECT_EQ(g.allocated, g.allocated_committed);
}

TEST(BlueFSSuper, DencRoundtrip) {
    bluefs_super_t s;
    s.uuid = uuid_d{};
    s.osd_uuid = uuid_d{};
    s.version = 1;
    s.block_size = 8192;

    bluefs_fnode_t log_fnode{0, 0x10000, 0};
    log_fnode.append_extent({0, 0, 0x5000});
    log_fnode.append_extent({1, 0x10000, 0x5000});
    log_fnode.reset_delta();
    s.log_fnode = log_fnode;

    bufferlist bl;
    encode(s, bl);
    auto p = bl.cbegin();
    bluefs_super_t t;
    decode(t, p);
    EXPECT_EQ(s.uuid, t.uuid);
    EXPECT_EQ(s.osd_uuid, t.osd_uuid);
    EXPECT_EQ(s.version, t.version);
    EXPECT_EQ(s.block_size, t.block_size);
    EXPECT_EQ(s.log_fnode.ino, t.log_fnode.ino);
    EXPECT_EQ(s.log_fnode.size, t.log_fnode.size);
    EXPECT_EQ(s.log_fnode.allocated, t.log_fnode.allocated);
    EXPECT_EQ(s.block_mask(), ~((uint64_t)s.block_size - 1));
}

TEST(BlueFSTransaction, DefaultEmpty) {
    bluefs_transaction_t t;
    EXPECT_TRUE(t.empty());
    EXPECT_EQ(t.seq, 0ULL);
    EXPECT_TRUE(t.uuid.is_zero());
}

TEST(BlueFSTransaction, Clear) {
    bluefs_transaction_t t;
    t.uuid = uuid_d{};
    t.seq = 100;
    t.op_init();
    EXPECT_FALSE(t.empty());
    t.clear();
    EXPECT_TRUE(t.empty());
    EXPECT_EQ(t.seq, 0ULL);
    EXPECT_TRUE(t.uuid.is_zero());
}

TEST(BlueFSTransaction, OpInit) {
    bluefs_transaction_t t;
    t.op_init();
    EXPECT_FALSE(t.empty());
}

TEST(BlueFSTransaction, OpDirCreate) {
    bluefs_transaction_t t;
    t.op_dir_create("test_dir");
    EXPECT_FALSE(t.empty());
}

TEST(BlueFSTransaction, OpDirRemove) {
    bluefs_transaction_t t;
    t.op_dir_remove("test_dir");
    EXPECT_FALSE(t.empty());
}

TEST(BlueFSTransaction, OpDirLink) {
    bluefs_transaction_t t;
    t.op_dir_link("dir", "file", 42);
    EXPECT_FALSE(t.empty());
}

TEST(BlueFSTransaction, OpDirUnlink) {
    bluefs_transaction_t t;
    t.op_dir_unlink("dir", "file");
    EXPECT_FALSE(t.empty());
}

TEST(BlueFSTransaction, OpFileUpdate) {
    bluefs_transaction_t t;
    bluefs_fnode_t f{1, 0x1000, 0};
    f.append_extent({0, 0, 0x1000});
    f.reset_delta();
    t.op_file_update(f);
    EXPECT_TRUE(f.allocated == f.allocated_committed);
    EXPECT_FALSE(t.empty());
}

TEST(BlueFSTransaction, OpFileUpdateInc) {
    bluefs_transaction_t t;
    bluefs_fnode_t f{1, 0x1000, 0};
    f.append_extent({0, 0, 0x1000});
    f.reset_delta();
    f.append_extent({0, 0x1000, 0x500});  // uncommitted extent
    t.op_file_update_inc(f);
    EXPECT_TRUE(f.allocated == f.allocated_committed);
    EXPECT_FALSE(t.empty());
}

TEST(BlueFSTransaction, OpFileRemove) {
    bluefs_transaction_t t;
    t.op_file_remove(42);
    EXPECT_FALSE(t.empty());
}

TEST(BlueFSTransaction, OpJump) {
    bluefs_transaction_t t;
    t.op_jump(100, 0x10000);
    EXPECT_FALSE(t.empty());
}

TEST(BlueFSTransaction, OpJumpSeq) {
    bluefs_transaction_t t;
    t.op_jump_seq(200);
    EXPECT_FALSE(t.empty());
}

TEST(BlueFSTransaction, ClaimOps) {
    bluefs_transaction_t a, b;
    a.op_init();
    b.op_dir_create("test");
    a.claim_ops(b);
    EXPECT_FALSE(a.empty());
    EXPECT_TRUE(b.empty());
}

TEST(BlueFSTransaction, DencRoundtrip) {
    bluefs_transaction_t t;
    t.uuid = uuid_d{};
    t.seq = 42;
    t.op_init();
    t.op_dir_create("mydir");
    t.op_dir_link("mydir", "myfile", 100);
    t.op_file_remove(200);
    t.op_jump_seq(300);

    bufferlist bl;
    encode(t, bl);
    auto p = bl.cbegin();
    bluefs_transaction_t u;
    decode(u, p);
    EXPECT_EQ(t.uuid, u.uuid);
    EXPECT_EQ(t.seq, u.seq);
    EXPECT_EQ(t.op_bl.length(), u.op_bl.length());
}

}  // namespace
