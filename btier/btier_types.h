#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "common/common_fwd.h"
#include "common/denc.h"

namespace TOPNSPC::btier {

// ── Tier identifiers ────────────────────────────────────────────
enum class Tier : uint8_t { FAST = 0,
                            SLOW = 1 };

// ── Physical location of an extent ──────────────────────────────
// Protected by ExtentEntry::struct_lock (shared for read, exclusive for
// commit_migration / compaction commit).
struct DiskLocation {
    uint64_t offset = 0;
    uint32_t length = 0;
    Tier tier = Tier::FAST;

    DENC(DiskLocation, v, p) {
        DENC_START(1, 1, p);
        denc(v.offset, p);
        denc(v.length, p);
        denc((uint8_t &)v.tier, p);
        DENC_FINISH(p);
    }
};

// ── Extent operational state (2-bit, fits in metrics word) ──────
// Two states only — simplicity. When an extent is full, append_slot()
// returns UINT32_MAX and the caller finds/creates another extent.
// When an extent is being freed, it is simply removed from ExtentMap.
enum ExtentState : uint32_t {
    ACTIVE = 0,
    MIGRATING = 1,
};

// ── 16-byte extent metrics ───────────────────────────────────────
// raw word (64 bits) layout:
//   bit 0-11:   access_count (12 bits, max 4095)
//   bit 12-23:  write_count  (12 bits, max 4095)
//   bit 24-29:  randomness   (6 bits, 0-63)
//   bit 30-31:  state        (2 bits)
//   bit 32-63:  generation   (32 bits — no practical wrap-around)
//
// Memory: 8 bytes (raw) + 4 bytes (last_access_time) = 12 bytes per extent.
// For 1TB / 4MB = 262,144 extents → ~3MB total metrics memory.
struct ExtentMetrics {
    std::atomic<uint32_t> last_access_time{0};

    std::atomic<uint64_t> raw{0};

    static constexpr uint64_t MASK_ACCESS = 0xFFF;
    static constexpr uint64_t MASK_WRITE = 0xFFFULL << 12;
    static constexpr uint64_t MASK_RANDOM = 0x3FULL << 24;
    static constexpr uint64_t MASK_STATE = 0x3ULL << 30;
    static constexpr uint64_t MASK_GEN = 0xFFFFFFFFULL << 32;

    static uint64_t pack(uint32_t access, uint32_t write, uint32_t random,
                         uint32_t state, uint64_t gen) {
        return (uint64_t)(access & 0xFFF) | ((uint64_t)(write & 0xFFF) << 12) | ((uint64_t)(random & 0x3F) << 24) | ((uint64_t)(state & 0x3) << 30) | ((gen & 0xFFFFFFFF) << 32);
    }

    static uint32_t access_count(uint64_t v) { return v & 0xFFF; }
    static uint32_t write_count(uint64_t v) { return (v >> 12) & 0xFFF; }
    static uint32_t randomness(uint64_t v) { return (v >> 24) & 0x3F; }
    static uint32_t state(uint64_t v) { return (v >> 30) & 0x3; }
    static uint64_t generation(uint64_t v) { return v >> 32; }

    static bool is_migrating(uint64_t v) { return state(v) == MIGRATING; }
};

// ── On-disk extent header (4KB, at start of each extent) ────────
// Written when extent is created, updated on used_bytes / live_bytes
// changes (via fsync/sync). Verified on read and during recovery.
struct ExtentHeader {
    static constexpr uint64_t MAGIC = 0x4254494552535445ULL;  // "BTIERSTE"
    static constexpr uint32_t HEADER_SIZE = 4096;

    uint64_t magic;
    uint64_t extent_id;
    uint32_t length;
    uint32_t used_bytes;
    uint32_t live_bytes;
    uint32_t reserved;
    uint64_t generation;
    uint32_t crc;
    uint32_t pad[1013];
};
static_assert(sizeof(ExtentHeader) == 4096, "ExtentHeader must be 4KB");
static_assert(offsetof(ExtentHeader, crc) == 40, "crc must be at offset 40");

// ── Result of a key lookup ─────────────────────────────────────
struct KeyLocation {
    uint64_t extent_id = 0;
    uint32_t offset = 0;
    uint32_t length = 0;

    DENC(KeyLocation, v, p) {
        DENC_START(1, 1, p);
        denc(v.extent_id, p);
        denc(v.offset, p);
        denc(v.length, p);
        DENC_FINISH(p);
    }
};

// ── I/O operation type (for metrics collection) ─────────────────
enum class IoOp { READ,
                  WRITE };

// ── Migration statistics ────────────────────────────────────────
struct MigrationStats {
    uint64_t promotions_committed = 0;
    uint64_t demotions_committed = 0;
    uint64_t compactions_committed = 0;
    uint64_t interruptions = 0;
    uint64_t io_errors = 0;
};

}  // namespace TOPNSPC::btier

// ── DENC traits specialization (must be in namespace TOPNSPC) ──
namespace TOPNSPC {
WRITE_CLASS_DENC(btier::DiskLocation);
WRITE_CLASS_DENC(btier::KeyLocation);
}  // namespace TOPNSPC
