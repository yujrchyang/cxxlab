#pragma once

#include <cstdint>
#include <vector>

#include "common/common_fwd.h"
#include "common/denc.h"

namespace TOPNSPC {

struct pextent_t {
    uint64_t offset = 0;
    uint32_t length = 0;

    pextent_t() = default;
    pextent_t(uint64_t o, uint32_t l) : offset(o), length(l) {}

    DENC(pextent_t, v, p) {
        DENC_START(1, 1, p);
        denc(v.offset, p);
        denc(v.length, p);
        DENC_FINISH(p);
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
        p += sizeof(uint32_t);
        if (!v.empty()) {
            size_t elem_size = 0;
            denc(v.front(), elem_size);
            p += elem_size * v.size();
        }
    }

    static void encode(const PExtentVector &v,
                       buffer::list::contiguous_appender &p) {
        denc(static_cast<uint32_t>(v.size()), p);
        for (const auto &e : v) {
            denc(e, p);
        }
    }

    static void decode(PExtentVector &v, buffer::ptr::const_iterator &p) {
        uint32_t num;
        denc(num, p);
        v.clear();
        v.resize(num);
        for (uint32_t i = 0; i < num; ++i) {
            denc(v[i], p);
        }
    }
};

}  // namespace TOPNSPC
