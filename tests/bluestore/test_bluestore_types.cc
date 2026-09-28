#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "bluestore/bluestore_types.h"
#include "common/denc.h"

using namespace TOPNSPC;

TEST(PExtent, DencRoundtrip) {
    pextent_t e{0x10000, 0x4000};
    bufferlist bl;
    encode(e, bl);
    auto p = bl.cbegin();
    pextent_t f;
    decode(f, p);
    EXPECT_EQ(e.offset, f.offset);
    EXPECT_EQ(e.length, f.length);
}

TEST(PExtent, DefaultValues) {
    pextent_t e;
    EXPECT_EQ(e.offset, 0ULL);
    EXPECT_EQ(e.length, 0U);
}

TEST(PExtentVector, DencRoundtrip) {
    PExtentVector v;
    v.push_back({0x1000, 0x2000});
    v.push_back({0x5000, 0x3000});
    v.push_back({0x9000, 0x1000});

    bufferlist bl;
    encode(v, bl);
    auto p = bl.cbegin();
    PExtentVector w;
    decode(w, p);

    ASSERT_EQ(v.size(), w.size());
    for (size_t i = 0; i < v.size(); ++i) {
        EXPECT_EQ(v[i].offset, w[i].offset);
        EXPECT_EQ(v[i].length, w[i].length);
    }
}

TEST(PExtentVector, EmptyRoundtrip) {
    PExtentVector v;
    bufferlist bl;
    encode(v, bl);
    auto p = bl.cbegin();
    PExtentVector w;
    decode(w, p);
    EXPECT_TRUE(w.empty());
}

TEST(IntervalSet, DencRoundtrip) {
    interval_set<uint64_t> s;
    s.insert(0x1000, 0x2000);
    s.insert(0x5000, 0x3000);
    s.insert(0x9000, 0x1000);

    bufferlist bl;
    encode(s, bl);
    auto p = bl.cbegin();
    interval_set<uint64_t> t;
    decode(t, p);

    EXPECT_EQ(s.size(), t.size());
    auto si = s.begin();
    auto ti = t.begin();
    while (si != s.end()) {
        EXPECT_EQ(si.get_start(), ti.get_start());
        EXPECT_EQ(si.get_len(), ti.get_len());
        ++si;
        ++ti;
    }
}

TEST(IntervalSet, EmptyRoundtrip) {
    interval_set<uint64_t> s;
    bufferlist bl;
    encode(s, bl);
    auto p = bl.cbegin();
    interval_set<uint64_t> t;
    decode(t, p);
    EXPECT_TRUE(t.empty());
}

TEST(BluestoreBdevLabel, DencRoundtrip) {
    bluestore_bdev_label_t label;
    label.osd_uuid.generate();
    label.size = 1073741824;
    label.description = "test device";
    label.meta["min_alloc_size"] = "65536";
    label.meta["mkfs_done"] = "true";

    bufferlist bl;
    encode(label, bl);
    auto p = bl.cbegin();
    bluestore_bdev_label_t label2;
    decode(label2, p);

    EXPECT_EQ(label.osd_uuid, label2.osd_uuid);
    EXPECT_EQ(label.size, label2.size);
    EXPECT_EQ(label.description, label2.description);
    EXPECT_EQ(label.meta, label2.meta);
}

TEST(BluestoreBdevLabel, DefaultValues) {
    bluestore_bdev_label_t label;
    EXPECT_EQ(label.size, 0ULL);
    EXPECT_TRUE(label.description.empty());
    EXPECT_TRUE(label.meta.empty());
}

TEST(BluestoreCnode, DencRoundtrip) {
    bluestore_cnode_t c(7);
    bufferlist bl;
    encode(c, bl);
    auto p = bl.cbegin();
    bluestore_cnode_t c2;
    decode(c2, p);
    EXPECT_EQ(c.bits, c2.bits);
}

TEST(BluestoreCnode, DefaultConstructor) {
    bluestore_cnode_t c;
    EXPECT_EQ(c.bits, 0U);
}

TEST(BluestoreBlobUseTracker, DefaultState) {
    bluestore_blob_use_tracker_t t;
    EXPECT_EQ(t.au_size, 0U);
    EXPECT_EQ(t.num_au, 0U);
    EXPECT_TRUE(t.is_empty());
    EXPECT_EQ(t.get_referenced_bytes(), 0U);
}

TEST(BluestoreBlobUseTracker, InitSingleUnit) {
    bluestore_blob_use_tracker_t t;
    t.init(4096, 4096);
    EXPECT_EQ(t.au_size, 4096U);
    EXPECT_EQ(t.num_au, 0U);
    EXPECT_TRUE(t.is_empty());
}

TEST(BluestoreBlobUseTracker, InitMultiUnit) {
    bluestore_blob_use_tracker_t t;
    t.init(65536, 4096);
    EXPECT_EQ(t.au_size, 4096U);
    EXPECT_EQ(t.num_au, 16U);
    EXPECT_TRUE(t.is_empty());
}

TEST(BluestoreBlobUseTracker, GetAndPutSingleUnit) {
    bluestore_blob_use_tracker_t t;
    t.init(4096, 4096);
    t.get(0, 4096);
    EXPECT_FALSE(t.is_empty());
    EXPECT_EQ(t.get_referenced_bytes(), 4096U);
    bool empty = t.put(0, 4096, nullptr);
    EXPECT_TRUE(empty);
    EXPECT_TRUE(t.is_empty());
}

TEST(BluestoreBlobUseTracker, GetAndPutMultiUnit) {
    bluestore_blob_use_tracker_t t;
    t.init(16384, 4096);
    t.get(0, 4096);
    t.get(4096, 4096);
    EXPECT_FALSE(t.is_empty());
    EXPECT_EQ(t.get_referenced_bytes(), 8192U);
    t.put(0, 4096, nullptr);
    EXPECT_FALSE(t.is_empty());
    EXPECT_EQ(t.get_referenced_bytes(), 4096U);
    bool empty = t.put(4096, 4096, nullptr);
    EXPECT_TRUE(empty);
}

TEST(BluestoreBlobUseTracker, CopyConstructor) {
    bluestore_blob_use_tracker_t t;
    t.init(16384, 4096);
    t.get(0, 4096);

    bluestore_blob_use_tracker_t t2(t);
    EXPECT_EQ(t2.au_size, t.au_size);
    EXPECT_EQ(t2.num_au, t.num_au);
    EXPECT_EQ(t2.get_referenced_bytes(), t.get_referenced_bytes());
}

TEST(BluestoreBlobUseTracker, Assignment) {
    bluestore_blob_use_tracker_t t;
    t.init(16384, 4096);
    t.get(0, 4096);

    bluestore_blob_use_tracker_t t2;
    t2 = t;
    EXPECT_EQ(t2.au_size, t.au_size);
    EXPECT_EQ(t2.num_au, t.num_au);
    EXPECT_EQ(t2.get_referenced_bytes(), t.get_referenced_bytes());
}

TEST(BluestoreBlobUseTracker, DencRoundtripSingleUnit) {
    bluestore_blob_use_tracker_t t;
    t.init(4096, 4096);
    t.get(0, 1024);

    bufferlist bl;
    encode(t, bl);
    auto p = bl.cbegin();
    bluestore_blob_use_tracker_t t2;
    decode(t2, p);

    EXPECT_EQ(t.au_size, t2.au_size);
    EXPECT_EQ(t.num_au, t2.num_au);
    EXPECT_EQ(t.get_referenced_bytes(), t2.get_referenced_bytes());
}

TEST(BluestoreBlobUseTracker, DencRoundtripMultiUnit) {
    bluestore_blob_use_tracker_t t;
    t.init(16384, 4096);
    t.get(0, 1024);
    t.get(4096, 2048);
    t.get(8192, 512);

    bufferlist bl;
    encode(t, bl);
    auto p = bl.cbegin();
    bluestore_blob_use_tracker_t t2;
    decode(t2, p);

    EXPECT_EQ(t.au_size, t2.au_size);
    EXPECT_EQ(t.num_au, t2.num_au);
    EXPECT_EQ(t.get_referenced_bytes(), t2.get_referenced_bytes());
}

TEST(BluestoreBlobUseTracker, DencRoundtripUninitialized) {
    bluestore_blob_use_tracker_t t;

    bufferlist bl;
    encode(t, bl);
    auto p = bl.cbegin();
    bluestore_blob_use_tracker_t t2;
    decode(t2, p);

    EXPECT_EQ(t2.au_size, 0U);
    EXPECT_EQ(t2.num_au, 0U);
    EXPECT_TRUE(t2.is_empty());
}

TEST(BluestoreBlobUseTracker, CanSplit) {
    bluestore_blob_use_tracker_t t;
    EXPECT_FALSE(t.can_split());

    t.init(4096, 4096);
    EXPECT_FALSE(t.can_split());

    t.clear();
    t.init(16384, 4096);
    EXPECT_TRUE(t.can_split());
}

TEST(BluestoreBlobUseTracker, CanSplitAt) {
    bluestore_blob_use_tracker_t t;
    t.init(16384, 4096);
    EXPECT_TRUE(t.can_split_at(4096));
    EXPECT_TRUE(t.can_split_at(8192));
    EXPECT_FALSE(t.can_split_at(1024));
}

TEST(BluestoreBlobUseTracker, Split) {
    bluestore_blob_use_tracker_t t;
    t.init(16384, 4096);
    t.get(0, 1024);
    t.get(4096, 2048);
    t.get(8192, 512);
    t.get(12288, 256);

    bluestore_blob_use_tracker_t r;
    t.split(8192, &r);

    EXPECT_EQ(t.num_au, 2U);
    EXPECT_EQ(t.get_referenced_bytes(), 1024U + 2048U);
    EXPECT_EQ(r.num_au, 2U);
    EXPECT_EQ(r.get_referenced_bytes(), 512U + 256U);
}

TEST(BluestoreBlob, DefaultState) {
    bluestore_blob_t b;
    EXPECT_TRUE(b.get_extents().empty());
    EXPECT_EQ(b.get_logical_length(), 0U);
    EXPECT_EQ(b.flags, 0U);
    EXPECT_EQ(b.csum_type, CSUM_NONE);
    EXPECT_FALSE(b.has_csum());
}

TEST(BluestoreBlob, Flags) {
    bluestore_blob_t b;
    EXPECT_FALSE(b.has_csum());
    b.set_flag(bluestore_blob_t::FLAG_CSUM);
    EXPECT_TRUE(b.has_csum());
    b.clear_flag(bluestore_blob_t::FLAG_CSUM);
    EXPECT_FALSE(b.has_csum());
}

TEST(BluestoreBlob, Allocated) {
    bluestore_blob_t b;
    PExtentVector allocs;
    allocs.push_back({0x10000, 0x4000});
    allocs.push_back({0x20000, 0x4000});
    b.allocated(0, 0x8000, allocs);
    EXPECT_EQ(b.get_logical_length(), 0x8000U);
    EXPECT_EQ(b.get_ondisk_length(), 0x8000U);
    EXPECT_EQ(b.get_extents().size(), 2U);
}

TEST(BluestoreBlob, DencRoundtrip) {
    bluestore_blob_t b;
    PExtentVector allocs;
    allocs.push_back({0x10000, 0x4000});
    allocs.push_back({0x20000, 0x4000});
    b.allocated(0, 0x8000, allocs);

    bufferlist bl;
    encode(b, bl);
    auto p = bl.cbegin();
    bluestore_blob_t b2;
    decode(b2, p);

    EXPECT_EQ(b.get_logical_length(), b2.get_logical_length());
    EXPECT_EQ(b.flags, b2.flags);
    ASSERT_EQ(b.get_extents().size(), b2.get_extents().size());
    for (size_t i = 0; i < b.get_extents().size(); ++i) {
        EXPECT_EQ(b.get_extents()[i].offset, b2.get_extents()[i].offset);
        EXPECT_EQ(b.get_extents()[i].length, b2.get_extents()[i].length);
    }
}

TEST(BluestoreBlob, DencRoundtripWithCsum) {
    bluestore_blob_t b;
    PExtentVector allocs;
    allocs.push_back({0x10000, 0x10000});
    b.allocated(0, 0x10000, allocs);
    b.init_csum(CSUM_CRC32C, 12, 0x10000);

    EXPECT_TRUE(b.has_csum());
    EXPECT_EQ(b.csum_type, CSUM_CRC32C);
    EXPECT_EQ(b.csum_chunk_order, 12U);

    bufferlist bl;
    encode(b, bl);
    auto p = bl.cbegin();
    bluestore_blob_t b2;
    decode(b2, p);

    EXPECT_TRUE(b2.has_csum());
    EXPECT_EQ(b2.csum_type, CSUM_CRC32C);
    EXPECT_EQ(b2.csum_chunk_order, 12U);
    EXPECT_EQ(b2.get_logical_length(), b.get_logical_length());
}

TEST(BluestoreBlob, DencRoundtripEmpty) {
    bluestore_blob_t b;
    bufferlist bl;
    encode(b, bl);
    auto p = bl.cbegin();
    bluestore_blob_t b2;
    decode(b2, p);
    EXPECT_TRUE(b2.get_extents().empty());
    EXPECT_EQ(b2.get_logical_length(), 0U);
}

TEST(BluestoreBlob, Split) {
    bluestore_blob_t b;
    PExtentVector allocs;
    allocs.push_back({0x10000, 0x4000});
    allocs.push_back({0x20000, 0x4000});
    b.allocated(0, 0x8000, allocs);

    bluestore_blob_t rb;
    b.split(0x4000, rb);

    EXPECT_EQ(b.get_logical_length(), 0x4000U);
    EXPECT_EQ(rb.get_logical_length(), 0x4000U);
    EXPECT_EQ(b.get_extents().size(), 1U);
    EXPECT_EQ(rb.get_extents().size(), 1U);
    EXPECT_EQ(b.get_extents()[0].offset, 0x10000ULL);
    EXPECT_EQ(rb.get_extents()[0].offset, 0x20000ULL);
}

TEST(BluestoreBlob, ChunkSize) {
    bluestore_blob_t b;
    EXPECT_EQ(b.get_chunk_size(512), 512ULL);

    b.init_csum(CSUM_CRC32C, 12, 0x10000);
    EXPECT_EQ(b.get_chunk_size(512), 4096ULL);
    EXPECT_EQ(b.get_chunk_size(8192), 8192ULL);
}

TEST(BluestoreOnodeShardInfo, DencRoundtrip) {
    bluestore_onode_t::shard_info si;
    si.offset = 0x1000;
    si.bytes = 0x200;

    bufferlist bl;
    encode(si, bl);
    auto p = bl.cbegin();
    bluestore_onode_t::shard_info si2;
    decode(si2, p);

    EXPECT_EQ(si.offset, si2.offset);
    EXPECT_EQ(si.bytes, si2.bytes);
}

TEST(BluestoreOnode, DefaultValues) {
    bluestore_onode_t o;
    EXPECT_EQ(o.nid, 0ULL);
    EXPECT_EQ(o.size, 0ULL);
    EXPECT_TRUE(o.attrs.empty());
    EXPECT_TRUE(o.extent_map_shards.empty());
    EXPECT_EQ(o.expected_object_size, 0U);
    EXPECT_EQ(o.expected_write_size, 0U);
    EXPECT_EQ(o.alloc_hint_flags, 0U);
    EXPECT_EQ(o.flags, 0U);
}

TEST(BluestoreOnode, Flags) {
    bluestore_onode_t o;
    EXPECT_FALSE(o.has_omap());
    o.set_flag(bluestore_onode_t::FLAG_OMAP);
    EXPECT_TRUE(o.has_omap());
    o.clear_flag(bluestore_onode_t::FLAG_OMAP);
    EXPECT_FALSE(o.has_omap());
}

TEST(BluestoreOnode, DencRoundtrip) {
    bluestore_onode_t o;
    o.nid = 42;
    o.size = 1048576;
    o.attrs["key1"] = buffer::copy("value1", 6);
    o.attrs["key2"] = buffer::copy("value2", 6);
    o.extent_map_shards.push_back({0, 1024});
    o.extent_map_shards.push_back({1024, 2048});
    o.expected_object_size = 4194304;
    o.expected_write_size = 65536;
    o.alloc_hint_flags = 1;
    o.flags = bluestore_onode_t::FLAG_OMAP;

    bufferlist bl;
    encode(o, bl);
    auto p = bl.cbegin();
    bluestore_onode_t o2;
    decode(o2, p);

    EXPECT_EQ(o.nid, o2.nid);
    EXPECT_EQ(o.size, o2.size);
    EXPECT_EQ(o.attrs.size(), o2.attrs.size());
    EXPECT_EQ(o.extent_map_shards.size(), o2.extent_map_shards.size());
    EXPECT_EQ(o.expected_object_size, o2.expected_object_size);
    EXPECT_EQ(o.expected_write_size, o2.expected_write_size);
    EXPECT_EQ(o.alloc_hint_flags, o2.alloc_hint_flags);
    EXPECT_EQ(o.flags, o2.flags);

    for (size_t i = 0; i < o.extent_map_shards.size(); ++i) {
        EXPECT_EQ(o.extent_map_shards[i].offset,
                  o2.extent_map_shards[i].offset);
        EXPECT_EQ(o.extent_map_shards[i].bytes,
                  o2.extent_map_shards[i].bytes);
    }
}

TEST(BluestoreOnode, DencRoundtripMinimal) {
    bluestore_onode_t o;
    o.nid = 1;
    o.size = 0;

    bufferlist bl;
    encode(o, bl);
    auto p = bl.cbegin();
    bluestore_onode_t o2;
    decode(o2, p);

    EXPECT_EQ(o2.nid, 1ULL);
    EXPECT_EQ(o2.size, 0ULL);
    EXPECT_TRUE(o2.attrs.empty());
    EXPECT_TRUE(o2.extent_map_shards.empty());
}

TEST(BluestoreDeferredOp, DencRoundtrip) {
    bluestore_deferred_op_t op;
    op.op = bluestore_deferred_op_t::OP_WRITE;
    op.extents.push_back({0x10000, 0x4000});
    op.extents.push_back({0x20000, 0x4000});
    op.data.append("test data");

    bufferlist bl;
    encode(op, bl);
    auto p = bl.cbegin();
    bluestore_deferred_op_t op2;
    decode(op2, p);

    EXPECT_EQ(op.op, op2.op);
    ASSERT_EQ(op.extents.size(), op2.extents.size());
    for (size_t i = 0; i < op.extents.size(); ++i) {
        EXPECT_EQ(op.extents[i].offset, op2.extents[i].offset);
        EXPECT_EQ(op.extents[i].length, op2.extents[i].length);
    }
    EXPECT_EQ(op.data.length(), op2.data.length());
}

TEST(BluestoreDeferredOp, DefaultValues) {
    bluestore_deferred_op_t op;
    EXPECT_EQ(op.op, 0U);
    EXPECT_TRUE(op.extents.empty());
    EXPECT_EQ(op.data.length(), 0U);
}

TEST(BluestoreDeferredTransaction, DencRoundtrip) {
    bluestore_deferred_transaction_t txn;
    txn.seq = 12345;

    bluestore_deferred_op_t op1;
    op1.op = bluestore_deferred_op_t::OP_WRITE;
    op1.extents.push_back({0x10000, 0x4000});
    op1.data.append("data1");

    bluestore_deferred_op_t op2;
    op2.op = bluestore_deferred_op_t::OP_WRITE;
    op2.extents.push_back({0x20000, 0x4000});
    op2.data.append("data2");

    txn.ops.push_back(op1);
    txn.ops.push_back(op2);
    txn.released.insert(0x30000, 0x4000);

    bufferlist bl;
    encode(txn, bl);
    auto p = bl.cbegin();
    bluestore_deferred_transaction_t txn2;
    decode(txn2, p);

    EXPECT_EQ(txn.seq, txn2.seq);
    ASSERT_EQ(txn.ops.size(), txn2.ops.size());
    for (size_t i = 0; i < txn.ops.size(); ++i) {
        EXPECT_EQ(txn.ops[i].op, txn2.ops[i].op);
        ASSERT_EQ(txn.ops[i].extents.size(), txn2.ops[i].extents.size());
    }
    EXPECT_EQ(txn.released.size(), txn2.released.size());
}

TEST(BluestoreDeferredTransaction, DefaultValues) {
    bluestore_deferred_transaction_t txn;
    EXPECT_EQ(txn.seq, 0ULL);
    EXPECT_TRUE(txn.ops.empty());
    EXPECT_TRUE(txn.released.empty());
}

TEST(BluestoreDeferredTransaction, EmptyRoundtrip) {
    bluestore_deferred_transaction_t txn;
    txn.seq = 1;

    bufferlist bl;
    encode(txn, bl);
    auto p = bl.cbegin();
    bluestore_deferred_transaction_t txn2;
    decode(txn2, p);

    EXPECT_EQ(txn2.seq, 1ULL);
    EXPECT_TRUE(txn2.ops.empty());
    EXPECT_TRUE(txn2.released.empty());
}

TEST(ChecksumType, ValueSize) {
    EXPECT_EQ(csum_value_size(CSUM_NONE), 0U);
    EXPECT_EQ(csum_value_size(CSUM_CRC32C), 4U);
    EXPECT_EQ(csum_value_size(CSUM_XXHASH32), 4U);
    EXPECT_EQ(csum_value_size(CSUM_XXHASH64), 8U);
}
