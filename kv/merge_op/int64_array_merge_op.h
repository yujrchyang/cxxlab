#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

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
        size_t count = std::min(llen, rlen) / sizeof(int64_t);
        std::vector<int64_t> result(count);
        for (size_t i = 0; i < count; i++) {
            int64_t existing, delta;
            memcpy(&existing, ldata + i * sizeof(int64_t), sizeof(int64_t));
            memcpy(&delta, rdata + i * sizeof(int64_t), sizeof(int64_t));
            result[i] = existing + delta;
        }
        new_value->assign(reinterpret_cast<char *>(result.data()),
                          count * sizeof(int64_t));
    }
};

}  // namespace TOPNSPC
