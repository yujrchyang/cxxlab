#pragma once

#include <cstdint>
#include <mutex>
#include <set>

#include "bluestore/collection.h"
#include "common/object.h"
#include "common_fwd.h"

namespace TOPNSPC {

class Allocator;
class FreelistManager;
class KeyValueDB;

class ErrorInjector {
public:
    ErrorInjector();
    ~ErrorInjector();

    ErrorInjector(const ErrorInjector &) = delete;
    ErrorInjector &operator=(const ErrorInjector &) = delete;

    void bind(Allocator *alloc, FreelistManager *fm, KeyValueDB *db,
              uint64_t min_alloc);

    void inject_data_error(const ghobject_t &oid);
    void inject_mdata_error(const ghobject_t &oid);

    bool check_data_error(const ghobject_t &oid) const;
    bool check_mdata_error(const ghobject_t &oid) const;

    void inject_leaked(uint64_t len);
    void inject_false_free(CollectionRef coll, const ghobject_t &oid);

    void clear();
    void clear_errors_for(const ghobject_t &oid);

private:
    mutable std::mutex lock_;
    std::set<ghobject_t> data_error_objects_;
    std::set<ghobject_t> mdata_error_objects_;

    Allocator *alloc_ = nullptr;
    FreelistManager *fm_ = nullptr;
    KeyValueDB *db_ = nullptr;
    uint64_t min_alloc_ = 0;
};

}  // namespace TOPNSPC
