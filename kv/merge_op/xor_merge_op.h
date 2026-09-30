#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "kv/merge_op/merge_op.h"

namespace TOPNSPC {

class XorMergeOperator : public MergeOperator {
public:
    const char *name() const override { return "xor"; }

    void merge_nonexistent(const char *rdata, size_t rlen,
                           std::string *new_value) override {
        _record_merge(rlen);
        new_value->assign(rdata, rlen);
    }

    void merge(const char *ldata, size_t llen,
               const char *rdata, size_t rlen,
               std::string *new_value) override {
        _record_merge(llen + rlen);
        size_t count = std::min(llen, rlen);
        std::vector<char> result(count);
        for (size_t i = 0; i < count; i++)
            result[i] = ldata[i] ^ rdata[i];
        new_value->assign(result.data(), count);
    }
};

}  // namespace TOPNSPC
