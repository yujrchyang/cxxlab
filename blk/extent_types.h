#pragma once

#include <cstdint>
#include <vector>

#include "common/common_fwd.h"
#include "common/denc.h"

namespace TOPNSPC {

struct pextent_t {
    static constexpr uint64_t INVALID_OFFSET = ~0ull;

    uint64_t offset = 0;
    uint32_t length = 0;

    pextent_t() = default;
    pextent_t(uint64_t o, uint64_t l)
        : offset(o), length(static_cast<uint32_t>(l)) {}

    bool is_valid() const { return offset != INVALID_OFFSET; }
    uint64_t end() const {
        return offset != INVALID_OFFSET ? offset + length : INVALID_OFFSET;
    }
    bool operator==(const pextent_t &o) const {
        return offset == o.offset && length == o.length;
    }

    DENC(pextent_t, v, p) {
        denc_lba(v.offset, p);
        denc_varint_lowz(v.length, p);
    }
};

using PExtentVector = std::vector<pextent_t>;

WRITE_CLASS_DENC(pextent_t);

template <>
struct denc_traits<PExtentVector> {
    static constexpr bool supported = true;
    static constexpr bool bounded = false;
    static constexpr bool featured = false;
    static constexpr bool need_contiguous = true;

    static void bound_encode(const PExtentVector &v, size_t &p) {
        denc_varint(static_cast<uint32_t>(v.size()), p);
        if (!v.empty()) {
            size_t elem_size = 0;
            denc(v.front(), elem_size);
            p += elem_size * v.size();
        }
    }

    static void encode(const PExtentVector &v,
                       buffer::list::contiguous_appender &p) {
        denc_varint(static_cast<uint32_t>(v.size()), p);
        for (const auto &e : v) {
            denc(e, p);
        }
    }

    static void decode(PExtentVector &v, buffer::ptr::const_iterator &p) {
        uint32_t num;
        denc_varint(num, p);
        v.clear();
        v.resize(num);
        for (uint32_t i = 0; i < num; ++i) {
            denc(v[i], p);
        }
    }
};

}  // namespace TOPNSPC
