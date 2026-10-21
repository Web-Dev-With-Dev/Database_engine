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

    /**
     * @brief Construct an end-of-iteration sentinel iterator.
     */
    TablePageIterator() = default;

    /**
     * @brief Construct an iterator pointing to a specific slot within a TablePage.
     * Advances automatically to the first live (non-deleted) slot at or after slot_id.
     * @param page Pointer to TablePage.
     * @param slot_id Initial slot index (default: 0).
     */
    explicit TablePageIterator(const TablePage *page, uint32_t slot_id = 0);

    /**
     * @brief Construct an iterator starting at the slot identified by an RID.
     * @param page Pointer to TablePage.
     * @param rid Record identifier.
     */
    TablePageIterator(const TablePage *page, const RID &rid);

    reference operator*() const { return current_tuple_; }
    pointer operator->() const { return &current_tuple_; }

    TablePageIterator &operator++();
    TablePageIterator operator++(int);

    bool operator==(const TablePageIterator &other) const;
    bool operator!=(const TablePageIterator &other) const { return !(*this == other); }

    /**
     * @brief Get a copy of the current tuple.
     * @return Tuple value.
     */
    Tuple GetTuple() const { return current_tuple_; }

    /**
     * @brief Load current tuple into an output parameter.
     * @param[out] tuple Destination tuple pointer.
     * @return true if iterator points to a valid tuple, false otherwise.
     */
    bool GetTuple(Tuple *tuple) const;

    /**
     * @brief Get a const reference to the current tuple.
     * @return Const reference to current tuple.
     */
    const Tuple &GetTupleRef() const { return current_tuple_; }

    /**
     * @brief Get the RID of the current live slot.
     * @return Current RID.
     */
    RID GetRid() const;

    /**
     * @brief Get the current slot index.
     * @return Slot index.
     */
    uint32_t GetSlotNum() const { return slot_id_; }

    /**
     * @brief Get the page ID of the page being iterated.
     * @return Page ID.
     */
    page_id_t GetPageId() const;

    /**
     * @brief Get pointer to the TablePage being iterated.
     * @return Const TablePage pointer.
     */
    const TablePage *GetPage() const { return page_; }

    /**
     * @brief Check whether iterator has reached the end of the page.
     * @return true if finished, false otherwise.
     */
    bool IsEnd() const;

    /**
     * @brief Check whether iterator currently points to a valid live tuple.
     * @return true if live tuple is available.
     */
    bool HasNext() const { return !IsEnd(); }

    /**
     * @brief Check whether iterator is valid.
     * @return true if valid.
     */
    bool IsValid() const { return !IsEnd(); }

    /**
     * @brief Explicit boolean conversion checking if iterator has more elements.
     */
    explicit operator bool() const { return !IsEnd(); }

    /**
     * @brief Retrieve current item and advance iterator forward.
     * @param[out] tuple Optional output pointer for tuple.
     * @param[out] rid Optional output pointer for RID.
     * @return true if successful, false if already at end.
     */
    bool Next(Tuple *tuple = nullptr, RID *rid = nullptr);

    /**
     * @brief Re-read the current slot from page memory.
     */
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
     * @param page_id The physical page ID assigned to this page.
     * @param prev_page_id Optional pointer to preceding page in the table heap (default: INVALID_PAGE_ID).
     * @param next_page_id Optional pointer to subsequent page in the table heap (default: INVALID_PAGE_ID).
     */
    void Init(page_id_t page_id, page_id_t prev_page_id = INVALID_PAGE_ID, page_id_t next_page_id = INVALID_PAGE_ID);

    /**
     * @brief Reinterpret cast a raw Page pointer to TablePage pointer.
     * Safe because TablePage adds no non-static data members.
     * @param page Raw Page pointer.
     * @return TablePage pointer.
     */
    static TablePage *Cast(Page *page) {
        return reinterpret_cast<TablePage *>(page);
    }

    /**
     * @brief Reinterpret cast a const raw Page pointer to const TablePage pointer.
     * @param page Const raw Page pointer.
     * @return Const TablePage pointer.
     */
    static const TablePage *Cast(const Page *page) {
        return reinterpret_cast<const TablePage *>(page);
    }

    // ==================== Header Getters & Setters ====================

    /**
     * @brief Get logical page ID from page header.
     * @return page_id_t Page ID.
     */
    page_id_t GetTablePageId() const;

    /**
     * @brief Set logical page ID in page header and mark page dirty.
     * @param page_id The new page ID.
     */
    void SetTablePageId(page_id_t page_id);

    /**
     * @brief Get Log Sequence Number (LSN) from header.
     * @return uint32_t LSN.
     */
    uint32_t GetLSN() const;

    /**
     * @brief Set Log Sequence Number (LSN) in header.
     * @param lsn New LSN.
     */
    void SetLSN(uint32_t lsn);

    /**
     * @brief Get page ID of preceding page in the heap list.
     * @return page_id_t Previous page ID.
     */
    page_id_t GetPrevPageId() const;

    /**
     * @brief Set page ID of preceding page in the heap list.
     * @param prev_page_id Preceding page ID.
     */
    void SetPrevPageId(page_id_t prev_page_id);

    /**
     * @brief Get page ID of subsequent page in the heap list.
     * @return page_id_t Next page ID.
     */
    page_id_t GetNextPageId() const;

    /**
     * @brief Set page ID of subsequent page in the heap list.
     * @param next_page_id Subsequent page ID.
     */
    void SetNextPageId(page_id_t next_page_id);

    /**
     * @brief Get byte offset of the free space pointer.
     * Represents the boundary where the lowest active tuple begins.
     * @return uint32_t Byte offset.
     */
    uint32_t GetFreeSpacePointer() const;

    /**
     * @brief Set byte offset of the free space pointer.
     * @param free_space_pointer New offset.
     */
    void SetFreeSpacePointer(uint32_t free_space_pointer);

    /**
     * @brief Get total number of slots allocated in the slot directory.
     * @return uint32_t Slot count.
     */
    uint32_t GetNumTuples() const;

    /**
     * @brief Set total number of slots in the slot directory.
     * @param num_tuples New slot count.
     */
    void SetNumTuples(uint32_t num_tuples);

    /**
     * @brief Get remaining contiguous free bytes between slot directory and tuple data.
     * @return uint32_t Contiguous free bytes.
     */
    uint32_t GetFreeSpaceRemaining() const;

    /**
     * @brief Get total usable free space including fragmented space from deleted/updated tuples.
     * @return uint32_t Total free bytes.
     */
    uint32_t GetTotalFreeSpace() const;

    /**
     * @brief Get amount of reclaimable fragmented space from deleted or reduced tuples.
     * @return uint32_t Fragmented bytes.
     */
    uint32_t GetFragmentedSpace() const;

    // ==================== Slot Directory Access ====================

    /**
     * @brief Get the byte offset for a specific slot.
     * @param slot_id Slot index.
     * @return uint32_t Byte offset in page.
     */
    uint32_t GetTupleOffset(uint32_t slot_id) const;

    /**
     * @brief Set the byte offset for a specific slot.
     * @param slot_id Slot index.
     * @param offset Byte offset in page.
     */
    void SetTupleOffset(uint32_t slot_id, uint32_t offset);

    /**
     * @brief Get payload size in bytes for a specific slot (excluding delete mask).
     * @param slot_id Slot index.
     * @return uint32_t Tuple size in bytes.
     */
    uint32_t GetTupleSize(uint32_t slot_id) const;

    /**
     * @brief Set payload size for a specific slot.
     * @param slot_id Slot index.
     * @param size Size in bytes.
     */
    void SetTupleSize(uint32_t slot_id, uint32_t size);

    /**
     * @brief Check whether a specific slot is marked deleted (tombstoned).
     * @param slot_id Slot index.
     * @return true if deleted, false if active.
     */
    bool IsTupleDeleted(uint32_t slot_id) const;

    /**
     * @brief Get TupleSlot metadata (offset and size) for a slot.
     * @param slot_id Slot index.
     * @return TupleSlot struct.
     */
    TupleSlot GetSlot(uint32_t slot_id) const;

    /**
     * @brief Set TupleSlot metadata for a slot.
     * @param slot_id Slot index.
     * @param slot TupleSlot struct.
     */
    void SetSlot(uint32_t slot_id, TupleSlot slot);

    // ==================== Tuple CRUD Operations ====================

    /**
     * @brief Insert a new tuple into this page.
     * Reuses an existing empty slot if available, or appends a new slot to directory.
     * Triggers defragmentation if contiguous space is insufficient but total space suffices.
     * @param tuple Tuple to insert.
     * @param[out] rid Output pointer to receive assigned RID.
     * @return true on success, false if page does not have sufficient space.
     */
    bool InsertTuple(const Tuple &tuple, RID *rid);

    /**
     * @brief Insert raw tuple bytes into this page.
     * @param tuple_bytes Span of bytes.
     * @param[out] rid Output pointer to receive assigned RID.
     * @return true on success, false if page does not have sufficient space.
     */
    bool InsertTuple(std::span<const char> tuple_bytes, RID *rid);

    /**
     * @brief Retrieve tuple by RID.
     * @param rid Record identifier.
     * @param[out] tuple Output tuple pointer.
     * @return true if found and not deleted, false otherwise.
     */
    bool GetTuple(const RID &rid, Tuple *tuple) const;

    /**
     * @brief Retrieve tuple by slot index.
     * @param slot_id Slot index.
     * @param[out] tuple Output tuple pointer.
     * @return true if found and not deleted, false otherwise.
     */
    bool GetTuple(uint32_t slot_id, Tuple *tuple) const;

    /**
     * @brief Retrieve tuple as std::optional by RID.
     * @param rid Record identifier.
     * @return Optional containing tuple if active, std::nullopt otherwise.
     */
    std::optional<Tuple> GetTuple(const RID &rid) const;

    /**
     * @brief Retrieve tuple as std::optional by slot index.
     * @param slot_id Slot index.
     * @return Optional containing tuple if active, std::nullopt otherwise.
     */
    std::optional<Tuple> GetTuple(uint32_t slot_id) const;

    /**
     * @brief Update a tuple within this page.
     * If new tuple is smaller or same size, updates in-place.
     * If larger, checks if space allows (with defragmentation if necessary).
     * @param new_tuple New tuple data.
     * @param[out] old_tuple Optional output pointer to receive prior tuple content.
     * @param rid RID of tuple to update.
     * @return true on success, false if tuple does not fit on this page.
     */
    bool UpdateTuple(const Tuple &new_tuple, Tuple *old_tuple, const RID &rid);
    bool UpdateTuple(const Tuple &new_tuple, const RID &rid);

    /**
     * @brief Soft-delete a tuple by setting the tombstone flag in its slot.
     * @param rid RID of tuple to mark deleted.
     * @return true on success, false if slot invalid or already deleted.
     */
    bool MarkDelete(const RID &rid);

    /**
     * @brief Physically clear slot metadata for slot reuse.
     * @param rid RID of tuple to clear.
     * @return true on success, false if slot invalid.
     */
    bool ApplyDelete(const RID &rid);

    /**
     * @brief Rollback a soft-deleted tuple (clears tombstone flag).
     * @param rid RID of tuple to restore.
     * @return true on success, false if slot invalid or not marked deleted.
     */
    bool RollbackDelete(const RID &rid);

    // ==================== Iteration Helpers ====================

    /**
     * @brief Get the RID of the first active tuple on this page.
     * @param[out] first_rid Output pointer.
     * @return true if active tuple exists, false if page has no live tuples.
     */
    bool GetFirstTupleRid(RID *first_rid) const;

    /**
     * @brief Get the RID of the next active tuple following cur_rid on this page.
     * @param cur_rid Current RID.
     * @param[out] next_rid Output pointer.
     * @return true if subsequent live tuple found, false if reached end of page.
     */
    bool GetNextTupleRid(const RID &cur_rid, RID *next_rid) const;

    // ==================== Page-Level Iterators ====================

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

    // ==================== Compaction / Defragmentation ====================

    /**
     * @brief Compact page memory: shifts all active tuples toward the high end of the page,
     * removing fragmented gaps left by deleted or shrunk tuples, and restores contiguous free space.
     * Slot indices and RIDs are strictly preserved.
     */
    void Defragment();

    /**
     * @brief Defragment raw page data buffer.
     * @param data Pointer to page buffer.
     */
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
