// Copyright 2024 cxxlab authors
// SPDX-License-Identifier: Apache-2.0

#include "error_injector.h"

#include "blk/allocator.h"
#include "blk/extent_types.h"
#include "bluestore/freelist_manager.h"
#include "kv/key_value_db.h"

namespace TOPNSPC {

ErrorInjector::ErrorInjector() = default;

ErrorInjector::~ErrorInjector() = default;

void ErrorInjector::bind(Allocator *alloc, FreelistManager *fm,
                         KeyValueDB *db, uint64_t min_alloc) {
    alloc_ = alloc;
    fm_ = fm;
    db_ = db;
    min_alloc_ = min_alloc;
}

void ErrorInjector::inject_data_error(const ghobject_t &oid) {
    std::lock_guard<std::mutex> l(lock_);
    data_error_objects_.insert(oid);
}

void ErrorInjector::inject_mdata_error(const ghobject_t &oid) {
    std::lock_guard<std::mutex> l(lock_);
    mdata_error_objects_.insert(oid);
}

bool ErrorInjector::check_data_error(const ghobject_t &oid) const {
    std::lock_guard<std::mutex> l(lock_);
    return data_error_objects_.count(oid) > 0;
}

bool ErrorInjector::check_mdata_error(const ghobject_t &oid) const {
    std::lock_guard<std::mutex> l(lock_);
    return mdata_error_objects_.count(oid) > 0;
}

void ErrorInjector::inject_leaked(uint64_t len) {
    if (!alloc_ || !fm_ || !db_ || min_alloc_ == 0) return;

    PExtentVector exts;
    int64_t alloc_len =
        alloc_->allocate(len, min_alloc_, min_alloc_ * 256, 0, &exts);
    if (alloc_len < 0) return;

    auto txn = db_->get_transaction();
    for (auto &p : exts) {
        fm_->allocate(p.offset, p.length, txn);
    }
    db_->submit_transaction_sync(txn);
}

void ErrorInjector::inject_false_free(CollectionRef coll,
                                      const ghobject_t &oid) {
    if (!fm_ || !db_ || !coll) return;

    auto o = coll->get_onode(oid, false);
    if (!o || o->extent_map.empty()) return;

    auto &em = o->extent_map;
    const PExtentVector *exts_ptr = nullptr;

    for (auto &ep : em) {
        if (!ep.blob) continue;
        const auto &blob_exts = ep.blob->get_blob().get_extents();
        for (auto &e : blob_exts) {
            if (e.is_valid()) {
                exts_ptr = &blob_exts;
                break;
            }
        }
        if (exts_ptr) break;
    }

    if (!exts_ptr || exts_ptr->empty()) return;

    auto txn = db_->get_transaction();
    for (auto &p : *exts_ptr) {
        if (p.is_valid()) {
            fm_->release(p.offset, p.length, txn);
            break;
        }
    }
    db_->submit_transaction_sync(txn);
}

void ErrorInjector::clear() {
    std::lock_guard<std::mutex> l(lock_);
    data_error_objects_.clear();
    mdata_error_objects_.clear();
}

void ErrorInjector::clear_errors_for(const ghobject_t &oid) {
    std::lock_guard<std::mutex> l(lock_);
    data_error_objects_.erase(oid);
    mdata_error_objects_.erase(oid);
}

}  // namespace TOPNSPC
