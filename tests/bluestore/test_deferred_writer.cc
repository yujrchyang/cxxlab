#include <gtest/gtest.h>

#include <string>

#include "bluestore/deferred_writer.h"
#include "common/buffer.h"

using namespace TOPNSPC;

TEST(DeferredBatch, PrepareWriteSingle) {
    DeferredBatch batch(nullptr);
    std::string data(4096, 'A');
    bufferlist bl;
    bl.append(data);
    batch.prepare_write(1, 0x1000, 4096, bl, 0);
    EXPECT_EQ(batch.iomap.size(), 1u);
    EXPECT_EQ(batch.iomap[0x1000].seq, 1u);
    EXPECT_EQ(batch.iomap[0x1000].bl.length(), 4096u);
    EXPECT_EQ(batch.seq_bytes[1], 4096);
}

TEST(DeferredBatch, PrepareWriteDiscardTail) {
    DeferredBatch batch(nullptr);
    std::string data(8192, 'A');
    bufferlist bl;
    bl.append(data);
    batch.prepare_write(1, 0x1000, 8192, bl, 0);
    EXPECT_EQ(batch.iomap.size(), 1u);

    std::string data2(4096, 'B');
    bufferlist bl2;
    bl2.append(data2);
    batch.prepare_write(2, 0x2000, 4096, bl2, 0);

    EXPECT_EQ(batch.iomap.size(), 2u);
    EXPECT_EQ(batch.iomap[0x1000].seq, 1u);
    EXPECT_EQ(batch.iomap[0x1000].bl.length(), 4096u);
    EXPECT_EQ(batch.iomap[0x2000].seq, 2u);
    EXPECT_EQ(batch.iomap[0x2000].bl.length(), 4096u);
    EXPECT_EQ(batch.seq_bytes[1], 4096);
    EXPECT_EQ(batch.seq_bytes[2], 4096);
}

TEST(DeferredBatch, PrepareWriteFullOverlap) {
    DeferredBatch batch(nullptr);
    std::string data(4096, 'A');
    bufferlist bl;
    bl.append(data);
    batch.prepare_write(1, 0x1000, 4096, bl, 0);
    batch.prepare_write(2, 0x1000, 4096, bl, 0);

    EXPECT_EQ(batch.iomap.size(), 1u);
    EXPECT_EQ(batch.iomap[0x1000].seq, 2u);
    EXPECT_EQ(batch.seq_bytes[1], 0);
    EXPECT_EQ(batch.seq_bytes[2], 4096);
}

TEST(DeferredBatch, PrepareWriteDiscardHead) {
    DeferredBatch batch(nullptr);
    std::string data(8192, 'A');
    bufferlist bl;
    bl.append(data);
    batch.prepare_write(1, 0x1000, 8192, bl, 0);

    std::string data2(4096, 'B');
    bufferlist bl2;
    bl2.append(data2);
    batch.prepare_write(2, 0x1000, 4096, bl2, 0);

    EXPECT_EQ(batch.iomap.size(), 2u);
    EXPECT_EQ(batch.iomap[0x1000].seq, 2u);
    EXPECT_EQ(batch.iomap[0x1000].bl.length(), 4096u);
    EXPECT_EQ(batch.iomap[0x2000].seq, 1u);
    EXPECT_EQ(batch.iomap[0x2000].bl.length(), 4096u);
    EXPECT_EQ(batch.seq_bytes[1], 4096);
    EXPECT_EQ(batch.seq_bytes[2], 4096);
}

TEST(DeferredBatch, PrepareWriteDiscardMiddle) {
    DeferredBatch batch(nullptr);
    std::string data(12288, 'A');
    bufferlist bl;
    bl.append(data);
    batch.prepare_write(1, 0x1000, 12288, bl, 0);

    std::string data2(4096, 'B');
    bufferlist bl2;
    bl2.append(data2);
    batch.prepare_write(2, 0x2000, 4096, bl2, 0);

    EXPECT_EQ(batch.iomap.size(), 3u);
    EXPECT_EQ(batch.iomap[0x1000].seq, 1u);
    EXPECT_EQ(batch.iomap[0x1000].bl.length(), 4096u);
    EXPECT_EQ(batch.iomap[0x2000].seq, 2u);
    EXPECT_EQ(batch.iomap[0x2000].bl.length(), 4096u);
    EXPECT_EQ(batch.iomap[0x3000].seq, 1u);
    EXPECT_EQ(batch.iomap[0x3000].bl.length(), 4096u);
    EXPECT_EQ(batch.seq_bytes[1], 8192);
    EXPECT_EQ(batch.seq_bytes[2], 4096);
}
