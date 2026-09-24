#pragma once

#include <cstdint>
#include <vector>

#include "common/common_fwd.h"

namespace TOPNSPC {

struct pextent_t {
    uint64_t offset = 0;
    uint32_t length = 0;

    pextent_t() = default;
    pextent_t(uint64_t o, uint32_t l) : offset(o), length(l) {}
};

using PExtentVector = std::vector<pextent_t>;

}  // namespace TOPNSPC
