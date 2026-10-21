#pragma once

#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "buffer_pool_manager.h"
#include "config.h"
#include "page.h"
#include "rid.h"
#include "table_page.h"
#include "tuple.h"

class TableHeap;

/**
 * @brief Forward iterator that traverses all live tuples across an entire TableHeap
 * (across all linked pages, automatically skipping deleted/tombstoned slots).
 */
class TableIterator {
private:
    const TableHeap *table_heap_{nullptr};
    RID rid_{};
    Tuple tuple_{};

    void AdvanceToNextLiveTuple();

public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = Tuple;
    using difference_type = std::ptrdiff_t;
    using pointer = const Tuple *;
    using reference = const Tuple &;

    TableIterator() = default;
    TableIterator(const TableHeap *table_heap, const RID &rid);

    reference operator*() const { return tuple_; }
    pointer operator->() const { return &tuple_; }

    TableIterator &operator++();
    TableIterator operator++(int);

    bool operator==(const TableIterator &other) const;
    bool operator!=(const TableIterator &other) const { return !(*this == other); }

    Tuple GetTuple() const { return tuple_; }
    RID GetRid() const { return rid_; }

    bool IsEnd() const { return !rid_.IsValid(); }
    bool HasNext() const { return !IsEnd(); }
    explicit operator bool() const { return !IsEnd(); }

    bool Next(Tuple *tuple = nullptr, RID *rid = nullptr);
};

/**
 * @brief TableHeap represents a physical relational table stored as a doubly-linked list
 * of TablePages in the BufferPoolManager.
 *
 * It manages cross-page allocation, sequential insertion, tuple fetching,
 * cross-page migration updates (Delete Old + Insert New), and table-level iteration.
 */
class TableHeap {
private:
    BufferPoolManager *bpm_{nullptr};
    std::shared_ptr<BufferPoolManager> bpm_sp_{nullptr};
    page_id_t first_page_id_{INVALID_PAGE_ID};
    mutable std::mutex latch_;

public:
    using Iterator = TableIterator;
    using const_iterator = TableIterator;
    using iterator = TableIterator;

    /**
     * @brief Create or attach to a TableHeap.
     * @param bpm The buffer pool manager.
     * @param first_page_id The root page ID of the table heap. If INVALID_PAGE_ID,
     *                      a new page is automatically allocated and initialized.
     */
    explicit TableHeap(BufferPoolManager *bpm, page_id_t first_page_id = INVALID_PAGE_ID);
    explicit TableHeap(std::shared_ptr<BufferPoolManager> bpm, page_id_t first_page_id = INVALID_PAGE_ID);

    ~TableHeap() = default;

    // Disable copy semantics
    TableHeap(const TableHeap &) = delete;
    TableHeap &operator=(const TableHeap &) = delete;

    // Enable move semantics
    TableHeap(TableHeap &&) noexcept = default;
    TableHeap &operator=(TableHeap &&) noexcept = default;

    // Tuple CRUD Operations
    /**
     * @brief Insert a tuple into the table heap.
     * Searches for space starting from the first page, following next_page links.
     * If all existing pages are full, a new page is allocated and linked.
     */
    bool InsertTuple(const Tuple &tuple, RID *rid);

    /**
     * @brief Read a tuple by its RID.
     */
    bool GetTuple(const RID &rid, Tuple *tuple) const;
    std::optional<Tuple> GetTuple(const RID &rid) const;

    /**
     * @brief Update a tuple in the table heap.
     * 1. Attempts an in-place / in-page update on the current page.
     * 2. If the updated tuple cannot fit on the current page (even after defragmentation),
     *    it executes Strategy 3 (Delete Old + Insert New):
     *    - Marks the old tuple deleted: table_page->MarkDelete(old_rid).
     *    - Inserts the new tuple on an existing or newly allocated page in the heap.
     *    - Sets new_rid to the new location (for secondary index pointer updates).
     *    - If insertion fails, rolls back the delete on the old page.
     */
    bool UpdateTuple(const Tuple &new_tuple, const RID &rid, RID *new_rid = nullptr);
    bool UpdateTuple(const Tuple &new_tuple, Tuple *old_tuple, const RID &rid, RID *new_rid = nullptr);

    /**
     * @brief Mark a tuple as deleted (soft delete via tombstone flag).
     */
    bool MarkDelete(const RID &rid);

    /**
     * @brief Apply a delete (physically clears slot offset and size for slot reuse).
     */
    bool ApplyDelete(const RID &rid);

    /**
     * @brief Rollback a marked delete.
     */
    bool RollbackDelete(const RID &rid);

    // Iteration over entire table
    Iterator begin();
    Iterator end();
    Iterator begin() const;
    Iterator end() const;
    Iterator cbegin() const;
    Iterator cend() const;

    Iterator Begin() { return begin(); }
    Iterator End() { return end(); }
    Iterator Begin() const { return begin(); }
    Iterator End() const { return end(); }

    // Diagnostics & Metadata
    page_id_t GetFirstPageId() const { return first_page_id_; }
    BufferPoolManager *GetBufferPoolManager() const { return bpm_; }
    std::vector<page_id_t> GetPageIds() const;
    size_t GetTupleCount() const;
};
