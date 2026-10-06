#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "common/byteorder.h"
#include "common/cassert.h"
#include "kv/merge_op/merge_op.h"

namespace TOPNSPC {

class Int64ArrayMergeOperator : public MergeOperator {
public:
    const char *name() const override { return "int64_array"; }

    void merge_nonexistent(const char *rdata, size_t rlen,
                           std::string *new_value) override {
        _record_merge(rlen);
        new_value->assign(rdata, rlen);
    }

    void merge(const char *ldata, size_t llen,
               const char *rdata, size_t rlen,
               std::string *new_value) override {
        _record_merge(llen + rlen);
        cxxlab_assert(llen == rlen && llen % sizeof(cxxlab_le64) == 0);
        *new_value = std::string(ldata, llen);
        auto *out = reinterpret_cast<cxxlab_le64 *>(new_value->data());
        const auto *in = reinterpret_cast<const cxxlab_le64 *>(rdata);
        size_t count = llen / sizeof(cxxlab_le64);
        for (size_t i = 0; i < count; i++) {
            out[i] = static_cast<uint64_t>(out[i]) +
                     static_cast<uint64_t>(in[i]);
        }
    }
};

}  // namespace TOPNSPC
