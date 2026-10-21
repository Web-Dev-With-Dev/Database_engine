#pragma once

#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <span>
#include "config.h"
#include "page.h"
#include "rid.h"
#include "tuple.h"

class TablePage;

/**
 * @brief Forward iterator for traversing live tuples in a TablePage.
 * Automatically skips deleted, empty, and tombstoned slots.
 */
class TablePageIterator {
private:
    const TablePage *page_{nullptr};
    uint32_t slot_id_{0};
    Tuple current_tuple_{};

    void AdvanceToNextLiveSlot();

public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = Tuple;
    using difference_type = std::ptrdiff_t;
    using pointer = const Tuple *;
    using reference = const Tuple &;

    TablePageIterator() = default;
    explicit TablePageIterator(const TablePage *page, uint32_t slot_id = 0);
    TablePageIterator(const TablePage *page, const RID &rid);

    reference operator*() const { return current_tuple_; }
    pointer operator->() const { return &current_tuple_; }

    TablePageIterator &operator++();
    TablePageIterator operator++(int);

    bool operator==(const TablePageIterator &other) const;
    bool operator!=(const TablePageIterator &other) const { return !(*this == other); }

    Tuple GetTuple() const { return current_tuple_; }
    bool GetTuple(Tuple *tuple) const;
    const Tuple &GetTupleRef() const { return current_tuple_; }

    RID GetRid() const;
    uint32_t GetSlotNum() const { return slot_id_; }
    page_id_t GetPageId() const;
    const TablePage *GetPage() const { return page_; }

    bool IsEnd() const;
    bool HasNext() const { return !IsEnd(); }
    bool IsValid() const { return !IsEnd(); }
    explicit operator bool() const { return !IsEnd(); }

    bool Next(Tuple *tuple = nullptr, RID *rid = nullptr);
    void Refresh();
};

/**
 * @brief Slot structure in the slot directory.
 * Each slot is 8 bytes: 4-byte offset + 4-byte size.
 * High-order bit of size is used as the tombstone (delete) flag.
 */
struct TupleSlot {
    uint32_t offset{0};
    uint32_t size{0};
};

/**
 * @brief Slotted-page layout implemented on top of raw Page.
 *
 * Page layout:
 * --------------------------------------------------------------------------------------
 * | PageId (4B) | LSN (4B) | PrevPageId (4B) | NextPageId (4B) | FreeSpacePtr (4B) | TupleCount (4B) |
 * --------------------------------------------------------------------------------------
 * | Slot 0: Offset (4B), Size (4B) | Slot 1: Offset (4B), Size (4B) | ...                |
 * --------------------------------------------------------------------------------------
 * |                              <--- FREE SPACE --->                                  |
 * --------------------------------------------------------------------------------------
 * |                                            ... | Tuple 1 Data | Tuple 0 Data        |
 * --------------------------------------------------------------------------------------
 *
 * Growth invariant:
 * - Slot directory grows forward from byte 24 toward the end of the page.
 * - Tuple data grows backward from byte PAGE_SIZE toward the slot directory.
 * - Free space pointer points to the start of the lowest allocated tuple.
 */
class TablePage : public Page {
public:
    static constexpr size_t OFFSET_PAGE_ID = 0;
    static constexpr size_t OFFSET_LSN = 4;
    static constexpr size_t OFFSET_PREV_PAGE_ID = 8;
    static constexpr size_t OFFSET_NEXT_PAGE_ID = 12;
    static constexpr size_t OFFSET_FREE_SPACE = 16;
    static constexpr size_t OFFSET_NUM_TUPLES = 20;
    static constexpr size_t SIZE_TABLE_PAGE_HEADER = 24;

    static constexpr size_t OFFSET_SLOT_OFFSET = 0;
    static constexpr size_t OFFSET_SLOT_SIZE = 4;
    static constexpr size_t SIZEOF_TUPLE_SLOT = 8;

    static constexpr uint32_t DELETE_MASK = 1u << 31;

    // Type aliases for iteration
    using Iterator = TablePageIterator;
    using const_iterator = TablePageIterator;
    using iterator = TablePageIterator;

    TablePage() = default;

    /**
     * @brief Initialize a new table page.
     */
    void Init(page_id_t page_id, page_id_t prev_page_id = INVALID_PAGE_ID, page_id_t next_page_id = INVALID_PAGE_ID);

    // Cast helpers from raw Page
    static TablePage *Cast(Page *page) {
        return reinterpret_cast<TablePage *>(page);
    }

    static const TablePage *Cast(const Page *page) {
        return reinterpret_cast<const TablePage *>(page);
    }

    // Header Getters & Setters
    page_id_t GetTablePageId() const;
    void SetTablePageId(page_id_t page_id);

    uint32_t GetLSN() const;
    void SetLSN(uint32_t lsn);

    page_id_t GetPrevPageId() const;
    void SetPrevPageId(page_id_t prev_page_id);

    page_id_t GetNextPageId() const;
    void SetNextPageId(page_id_t next_page_id);

    uint32_t GetFreeSpacePointer() const;
    void SetFreeSpacePointer(uint32_t free_space_pointer);

    uint32_t GetNumTuples() const;
    void SetNumTuples(uint32_t num_tuples);

    uint32_t GetFreeSpaceRemaining() const;
    uint32_t GetTotalFreeSpace() const;
    uint32_t GetFragmentedSpace() const;

    // Slot Directory Access
    uint32_t GetTupleOffset(uint32_t slot_id) const;
    void SetTupleOffset(uint32_t slot_id, uint32_t offset);

    uint32_t GetTupleSize(uint32_t slot_id) const;
    void SetTupleSize(uint32_t slot_id, uint32_t size);

    bool IsTupleDeleted(uint32_t slot_id) const;
    TupleSlot GetSlot(uint32_t slot_id) const;
    void SetSlot(uint32_t slot_id, TupleSlot slot);

    // Tuple CRUD Operations
    bool InsertTuple(const Tuple &tuple, RID *rid);
    bool InsertTuple(std::span<const char> tuple_bytes, RID *rid);

    bool GetTuple(const RID &rid, Tuple *tuple) const;
    bool GetTuple(uint32_t slot_id, Tuple *tuple) const;
    std::optional<Tuple> GetTuple(const RID &rid) const;
    std::optional<Tuple> GetTuple(uint32_t slot_id) const;

    bool UpdateTuple(const Tuple &new_tuple, Tuple *old_tuple, const RID &rid);
    bool UpdateTuple(const Tuple &new_tuple, const RID &rid);

    bool MarkDelete(const RID &rid);
    bool ApplyDelete(const RID &rid);
    bool RollbackDelete(const RID &rid);

    // Iteration helpers
    bool GetFirstTupleRid(RID *first_rid) const;
    bool GetNextTupleRid(const RID &cur_rid, RID *next_rid) const;

    // Page-level Tuple Iterator methods
    Iterator begin();
    Iterator end();
    Iterator begin() const;
    Iterator end() const;
    Iterator cbegin() const;
    Iterator cend() const;

    Iterator Begin();
    Iterator End();
    Iterator Begin() const;
    Iterator End() const;
    Iterator Begin(uint32_t start_slot) const;
    Iterator Begin(const RID &start_rid) const;
    Iterator GetIterator() const;

    // Compaction / Defragmentation
    void Defragment();
    static void Defragment(char *data);

    // Raw static helper functions on arbitrary char* buffers
    static void Init(char *data, page_id_t page_id, page_id_t prev_page_id = INVALID_PAGE_ID, page_id_t next_page_id = INVALID_PAGE_ID);
    static page_id_t GetTablePageId(const char *data);
    static void SetTablePageId(char *data, page_id_t page_id);
    static uint32_t GetFreeSpacePointer(const char *data);
    static void SetFreeSpacePointer(char *data, uint32_t free_space_pointer);
    static uint32_t GetNumTuples(const char *data);
    static void SetNumTuples(char *data, uint32_t num_tuples);
    static uint32_t GetFreeSpaceRemaining(const char *data);
    static uint32_t GetTotalFreeSpace(const char *data);
    static uint32_t GetFragmentedSpace(const char *data);

    static uint32_t GetTupleOffset(const char *data, uint32_t slot_id);
    static void SetTupleOffset(char *data, uint32_t slot_id, uint32_t offset);
    static uint32_t GetTupleSize(const char *data, uint32_t slot_id);
    static void SetTupleSize(char *data, uint32_t slot_id, uint32_t size);
    static bool IsTupleDeleted(const char *data, uint32_t slot_id);

    static bool GetTuple(const char *data, page_id_t page_id, uint32_t slot_id, Tuple *tuple);
    static bool GetTuple(const char *data, const RID &rid, Tuple *tuple);
};

// Ensure TablePage adds no non-static members so reinterpret_cast from Page* is standard-layout compatible
static_assert(sizeof(TablePage) == sizeof(Page), "TablePage must have identical size to Page");
