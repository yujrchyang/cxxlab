#pragma once

#include <cstdint>
#include <mutex>

#include <boost/intrusive/avl_set.hpp>

#include "blk/allocator.h"
#include "common/interval_set.h"

namespace TOPNSPC {

struct range_seg_t {
    uint64_t start;
    uint64_t end;

    range_seg_t(uint64_t start, uint64_t end)
        : start{start}, end{end} {}

    struct before_t {
        template <typename KeyLeft, typename KeyRight>
        bool operator()(const KeyLeft &lhs, const KeyRight &rhs) const {
            return lhs.end <= rhs.start;
        }
    };
    boost::intrusive::avl_set_member_hook<> offset_hook;

    struct shorter_t {
        template <typename KeyType>
        bool operator()(const range_seg_t &lhs, const KeyType &rhs) const {
            auto lhs_size = lhs.end - lhs.start;
            auto rhs_size = rhs.end - rhs.start;
            if (lhs_size < rhs_size) return true;
            if (lhs_size > rhs_size) return false;
            return lhs.start < rhs.start;
        }
    };
    boost::intrusive::avl_set_member_hook<> size_hook;

    uint64_t length() const { return end - start; }
};

class AvlAllocator : public Allocator {
    struct dispose_rs {
        void operator()(range_seg_t *p) { delete p; }
    };

    using range_tree_t =
        boost::intrusive::avl_set<
            range_seg_t,
            boost::intrusive::compare<range_seg_t::before_t>,
            boost::intrusive::member_hook<
                range_seg_t,
                boost::intrusive::avl_set_member_hook<>,
                &range_seg_t::offset_hook>>;

    using range_size_tree_t =
        boost::intrusive::avl_multiset<
            range_seg_t,
            boost::intrusive::compare<range_seg_t::shorter_t>,
            boost::intrusive::member_hook<
                range_seg_t,
                boost::intrusive::avl_set_member_hook<>,
                &range_seg_t::size_hook>,
            boost::intrusive::constant_time_size<true>>;

public:
    AvlAllocator(int64_t device_size, int64_t block_size,
                 std::string_view name);

    /// Constructor for subclasses (HybridAllocator) that need range_count_cap.
    AvlAllocator(int64_t device_size, int64_t block_size,
                 uint64_t max_mem, std::string_view name);

    ~AvlAllocator() override;

    const char *get_type() const override { return "avl"; }

    using Allocator::allocate;
    using Allocator::release;

    int64_t allocate(uint64_t want, uint64_t unit,
                     uint64_t max_alloc_size, int64_t hint,
                     PExtentVector *extents) override;

    void release(const interval_set<uint64_t> &release_set) override;

    uint64_t get_free() override;
    double get_fragmentation() override;

    void dump() override;
    void foreach (
        std::function<void(uint64_t offset, uint64_t length)> notify) override;

    void init_add_free(uint64_t offset, uint64_t length) override;
    void init_rm_free(uint64_t offset, uint64_t length) override;

    void shutdown() override;

protected:
    /// Hook for HybridAllocator to claim adjacent free space from bitmap
    /// before inserting into the AVL tree.
    virtual void _add_to_tree(uint64_t start, uint64_t size);

    /// Called when range_count_cap is exceeded and the smallest node
    /// needs to be spilled over to a fallback allocator.
    virtual void _spillover_range(uint64_t start, uint64_t end);

    range_tree_t range_tree_;
    range_size_tree_t range_size_tree_;

    uint64_t num_free_ = 0;
    static constexpr unsigned MAX_LBAS = 64;
    uint64_t lbas_[MAX_LBAS] = {0};

    uint64_t range_size_alloc_threshold_ = 0;
    int range_size_alloc_free_pct_ = 0;
    uint32_t max_search_count_ = 0;
    uint32_t max_search_bytes_ = 0;
    uint64_t range_count_cap_ = 0;

    std::mutex lock_;

protected:
    /// Subclass-accessible allocation/release/shutdown
    int64_t _allocate(uint64_t want, uint64_t unit,
                      uint64_t max_alloc_size, int64_t hint,
                      PExtentVector *extents);
    void _release(const interval_set<uint64_t> &release_set);
    void _shutdown();

    uint64_t _get_free() const { return num_free_; }
    double _get_fragmentation() const;

    uint64_t _lowest_size_available() {
        auto rs = range_size_tree_.begin();
        return rs != range_size_tree_.end() ? rs->length() : 0;
    }

    void _try_remove_from_tree(
        uint64_t start, uint64_t size,
        std::function<void(uint64_t, uint64_t, bool)> cb);

    void _dump() const;
    void _foreach(
        std::function<void(uint64_t offset, uint64_t length)> notify) const;

    /// Internal helpers (also accessible to subclasses for customization)
    /// Returns false if the range is not present in the AVL tree.
    bool _remove_from_tree(uint64_t start, uint64_t size);
    void _process_range_removal(uint64_t start, uint64_t end,
                                range_tree_t::iterator &rs);
    void _range_size_tree_rm(range_seg_t &r);
    void _range_size_tree_try_insert(range_seg_t &r);
    bool _try_insert_range(uint64_t start, uint64_t end,
                           range_tree_t::iterator *insert_pos = nullptr);

private:
    uint64_t _pick_block_after(uint64_t *cursor, uint64_t size, uint64_t align);
    uint64_t _pick_block_fits(uint64_t size, uint64_t align);
    int _allocate_single(uint64_t size, uint64_t unit,
                         uint64_t *offset, uint64_t *length);
};

}  // namespace TOPNSPC
