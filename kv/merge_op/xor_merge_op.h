#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "common/cassert.h"
#include "kv/merge_op/merge_op.h"

namespace TOPNSPC {

class XorMergeOperator : public MergeOperator {
public:
    const char *name() const override { return "bitwise_xor"; }

    void merge_nonexistent(const char *rdata, size_t rlen,
                           std::string *new_value) override {
        _record_merge(rlen);
        new_value->assign(rdata, rlen);
    }

    void merge(const char *ldata, size_t llen,
               const char *rdata, size_t rlen,
               std::string *new_value) override {
        _record_merge(llen + rlen);
        cxxlab_assert(llen == rlen);
        *new_value = std::string(ldata, llen);
        char *out = new_value->data();
        for (size_t i = 0; i < llen; i++)
            out[i] ^= rdata[i];
    }
};

}  // namespace TOPNSPC
