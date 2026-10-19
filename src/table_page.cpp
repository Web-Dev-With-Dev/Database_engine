#include "table_page.h"

void TablePage::Init(page_id_t page_id, page_id_t prev_page_id, page_id_t next_page_id) {
    SetPageId(page_id);
    Init(GetData(), page_id, prev_page_id, next_page_id);
    SetDirty(true);
}

void TablePage::Init(char *data, page_id_t page_id, page_id_t prev_page_id, page_id_t next_page_id) {
    std::memset(data, 0, PAGE_SIZE);
    SetTablePageId(data, page_id);
    *reinterpret_cast<uint32_t *>(data + OFFSET_LSN) = 0;
    *reinterpret_cast<page_id_t *>(data + OFFSET_PREV_PAGE_ID) = prev_page_id;
    *reinterpret_cast<page_id_t *>(data + OFFSET_NEXT_PAGE_ID) = next_page_id;
    SetFreeSpacePointer(data, static_cast<uint32_t>(PAGE_SIZE));
    SetNumTuples(data, 0);
}

page_id_t TablePage::GetTablePageId() const {
    return GetTablePageId(const_cast<TablePage *>(this)->GetData());
}

page_id_t TablePage::GetTablePageId(const char *data) {
    return *reinterpret_cast<const page_id_t *>(data + OFFSET_PAGE_ID);
}

void TablePage::SetTablePageId(page_id_t page_id) {
    SetTablePageId(GetData(), page_id);
    SetPageId(page_id);
    SetDirty(true);
}

void TablePage::SetTablePageId(char *data, page_id_t page_id) {
    *reinterpret_cast<page_id_t *>(data + OFFSET_PAGE_ID) = page_id;
}

uint32_t TablePage::GetLSN() const {
    return *reinterpret_cast<const uint32_t *>(const_cast<TablePage *>(this)->GetData() + OFFSET_LSN);
}

void TablePage::SetLSN(uint32_t lsn) {
    *reinterpret_cast<uint32_t *>(GetData() + OFFSET_LSN) = lsn;
    SetDirty(true);
}

page_id_t TablePage::GetPrevPageId() const {
    return *reinterpret_cast<const page_id_t *>(const_cast<TablePage *>(this)->GetData() + OFFSET_PREV_PAGE_ID);
}

void TablePage::SetPrevPageId(page_id_t prev_page_id) {
    *reinterpret_cast<page_id_t *>(GetData() + OFFSET_PREV_PAGE_ID) = prev_page_id;
    SetDirty(true);
}

page_id_t TablePage::GetNextPageId() const {
    return *reinterpret_cast<const page_id_t *>(const_cast<TablePage *>(this)->GetData() + OFFSET_NEXT_PAGE_ID);
}

void TablePage::SetNextPageId(page_id_t next_page_id) {
    *reinterpret_cast<page_id_t *>(GetData() + OFFSET_NEXT_PAGE_ID) = next_page_id;
    SetDirty(true);
}

uint32_t TablePage::GetFreeSpacePointer() const {
    return GetFreeSpacePointer(const_cast<TablePage *>(this)->GetData());
}

uint32_t TablePage::GetFreeSpacePointer(const char *data) {
    return *reinterpret_cast<const uint32_t *>(data + OFFSET_FREE_SPACE);
}

void TablePage::SetFreeSpacePointer(uint32_t free_space_pointer) {
    SetFreeSpacePointer(GetData(), free_space_pointer);
    SetDirty(true);
}

void TablePage::SetFreeSpacePointer(char *data, uint32_t free_space_pointer) {
    *reinterpret_cast<uint32_t *>(data + OFFSET_FREE_SPACE) = free_space_pointer;
}

uint32_t TablePage::GetNumTuples() const {
    return GetNumTuples(const_cast<TablePage *>(this)->GetData());
}

uint32_t TablePage::GetNumTuples(const char *data) {
    return *reinterpret_cast<const uint32_t *>(data + OFFSET_NUM_TUPLES);
}

void TablePage::SetNumTuples(uint32_t num_tuples) {
    SetNumTuples(GetData(), num_tuples);
    SetDirty(true);
}

void TablePage::SetNumTuples(char *data, uint32_t num_tuples) {
    *reinterpret_cast<uint32_t *>(data + OFFSET_NUM_TUPLES) = num_tuples;
}

uint32_t TablePage::GetFreeSpaceRemaining() const {
    return GetFreeSpaceRemaining(const_cast<TablePage *>(this)->GetData());
}

uint32_t TablePage::GetFreeSpaceRemaining(const char *data) {
    uint32_t free_ptr = GetFreeSpacePointer(data);
    uint32_t num_tuples = GetNumTuples(data);
    uint32_t slot_end = static_cast<uint32_t>(SIZE_TABLE_PAGE_HEADER + num_tuples * SIZEOF_TUPLE_SLOT);
    if (free_ptr < slot_end) {
        return 0;
    }
    return free_ptr - slot_end;
}

uint32_t TablePage::GetTupleOffset(uint32_t slot_id) const {
    const char *data = const_cast<TablePage *>(this)->GetData();
    size_t slot_offset = SIZE_TABLE_PAGE_HEADER + slot_id * SIZEOF_TUPLE_SLOT + OFFSET_SLOT_OFFSET;
    return *reinterpret_cast<const uint32_t *>(data + slot_offset);
}

void TablePage::SetTupleOffset(uint32_t slot_id, uint32_t offset) {
    char *data = GetData();
    size_t slot_offset = SIZE_TABLE_PAGE_HEADER + slot_id * SIZEOF_TUPLE_SLOT + OFFSET_SLOT_OFFSET;
    *reinterpret_cast<uint32_t *>(data + slot_offset) = offset;
    SetDirty(true);
}

uint32_t TablePage::GetTupleSize(uint32_t slot_id) const {
    const char *data = const_cast<TablePage *>(this)->GetData();
    size_t slot_offset = SIZE_TABLE_PAGE_HEADER + slot_id * SIZEOF_TUPLE_SLOT + OFFSET_SLOT_SIZE;
    uint32_t raw_size = *reinterpret_cast<const uint32_t *>(data + slot_offset);
    return raw_size & ~DELETE_MASK;
}

void TablePage::SetTupleSize(uint32_t slot_id, uint32_t size) {
    char *data = GetData();
    size_t slot_offset = SIZE_TABLE_PAGE_HEADER + slot_id * SIZEOF_TUPLE_SLOT + OFFSET_SLOT_SIZE;
    *reinterpret_cast<uint32_t *>(data + slot_offset) = size;
    SetDirty(true);
}

bool TablePage::IsTupleDeleted(uint32_t slot_id) const {
    if (slot_id >= GetNumTuples()) {
        return true;
    }
    const char *data = GetData();
    size_t slot_offset = SIZE_TABLE_PAGE_HEADER + slot_id * SIZEOF_TUPLE_SLOT + OFFSET_SLOT_SIZE;
    uint32_t raw_size = *reinterpret_cast<const uint32_t *>(data + slot_offset);
    return (raw_size & DELETE_MASK) != 0 || (GetTupleOffset(slot_id) == 0 && (raw_size & ~DELETE_MASK) == 0);
}

TupleSlot TablePage::GetSlot(uint32_t slot_id) const {
    return TupleSlot{GetTupleOffset(slot_id), GetTupleSize(slot_id)};
}

void TablePage::SetSlot(uint32_t slot_id, TupleSlot slot) {
    SetTupleOffset(slot_id, slot.offset);
    SetTupleSize(slot_id, slot.size);
}

bool TablePage::InsertTuple(const Tuple &tuple, RID *rid) {
    return InsertTuple(tuple.AsSpan(), rid);
}

bool TablePage::InsertTuple(std::span<const char> tuple_bytes, RID *rid) {
    const uint32_t tuple_len = static_cast<uint32_t>(tuple_bytes.size());
    if (tuple_len == 0 || tuple_len > PAGE_SIZE - SIZE_TABLE_PAGE_HEADER - SIZEOF_TUPLE_SLOT) {
        return false;
    }

    uint32_t num_tuples = GetNumTuples();

    // Check if we can reuse an empty slot (offset == 0 and size == 0)
    uint32_t slot_id = num_tuples;
    bool reuse_slot = false;
    for (uint32_t i = 0; i < num_tuples; ++i) {
        if (GetTupleOffset(i) == 0 && GetTupleSize(i) == 0) {
            slot_id = i;
            reuse_slot = true;
            break;
        }
    }

    uint32_t needed_space = tuple_len + (reuse_slot ? 0 : SIZEOF_TUPLE_SLOT);
    if (GetFreeSpaceRemaining() < needed_space) {
        return false;
    }

    // Allocate tuple at bottom of free space
    uint32_t free_ptr = GetFreeSpacePointer();
    free_ptr -= tuple_len;
    SetFreeSpacePointer(free_ptr);

    // Copy data into page
    std::memcpy(GetData() + free_ptr, tuple_bytes.data(), tuple_len);

    // Update slot
    SetTupleOffset(slot_id, free_ptr);
    SetTupleSize(slot_id, tuple_len);

    if (!reuse_slot) {
        SetNumTuples(num_tuples + 1);
    }

    if (rid != nullptr) {
        rid->Set(GetTablePageId(), slot_id);
    }

    SetDirty(true);
    return true;
}

bool TablePage::GetTuple(const RID &rid, Tuple *tuple) const {
    uint32_t slot_id = rid.GetSlotNum();
    if (slot_id >= GetNumTuples()) {
        return false;
    }
    if (IsTupleDeleted(slot_id)) {
        return false;
    }

    uint32_t offset = GetTupleOffset(slot_id);
    uint32_t size = GetTupleSize(slot_id);

    if (offset < SIZE_TABLE_PAGE_HEADER || offset + size > PAGE_SIZE) {
        return false;
    }

    if (tuple != nullptr) {
        *tuple = Tuple(GetData() + offset, size, rid);
    }
    return true;
}

bool TablePage::UpdateTuple(const Tuple &new_tuple, Tuple *old_tuple, const RID &rid) {
    uint32_t slot_id = rid.GetSlotNum();
    if (slot_id >= GetNumTuples() || IsTupleDeleted(slot_id)) {
        return false;
    }

    uint32_t old_offset = GetTupleOffset(slot_id);
    uint32_t old_size = GetTupleSize(slot_id);

    if (old_tuple != nullptr) {
        *old_tuple = Tuple(GetData() + old_offset, old_size, rid);
    }

    uint32_t new_len = new_tuple.GetLength();

    if (new_len <= old_size) {
        // Fits in-place
        std::memcpy(GetData() + old_offset, new_tuple.GetData(), new_len);
        SetTupleSize(slot_id, new_len);
        SetDirty(true);
        return true;
    }

    // Needs more space
    if (GetFreeSpaceRemaining() < new_len) {
        return false;
    }

    uint32_t free_ptr = GetFreeSpacePointer();
    free_ptr -= new_len;
    SetFreeSpacePointer(free_ptr);

    std::memcpy(GetData() + free_ptr, new_tuple.GetData(), new_len);
    SetTupleOffset(slot_id, free_ptr);
    SetTupleSize(slot_id, new_len);

    SetDirty(true);
    return true;
}

bool TablePage::UpdateTuple(const Tuple &new_tuple, const RID &rid) {
    return UpdateTuple(new_tuple, nullptr, rid);
}

bool TablePage::MarkDelete(const RID &rid) {
    uint32_t slot_id = rid.GetSlotNum();
    if (slot_id >= GetNumTuples() || IsTupleDeleted(slot_id)) {
        return false;
    }
    char *data = GetData();
    size_t slot_offset = SIZE_TABLE_PAGE_HEADER + slot_id * SIZEOF_TUPLE_SLOT + OFFSET_SLOT_SIZE;
    uint32_t raw_size = *reinterpret_cast<uint32_t *>(data + slot_offset);
    *reinterpret_cast<uint32_t *>(data + slot_offset) = raw_size | DELETE_MASK;
    SetDirty(true);
    return true;
}

bool TablePage::ApplyDelete(const RID &rid) {
    uint32_t slot_id = rid.GetSlotNum();
    if (slot_id >= GetNumTuples()) {
        return false;
    }
    SetTupleOffset(slot_id, 0);
    SetTupleSize(slot_id, 0);
    SetDirty(true);
    return true;
}

bool TablePage::RollbackDelete(const RID &rid) {
    uint32_t slot_id = rid.GetSlotNum();
    if (slot_id >= GetNumTuples()) {
        return false;
    }
    char *data = GetData();
    size_t slot_offset = SIZE_TABLE_PAGE_HEADER + slot_id * SIZEOF_TUPLE_SLOT + OFFSET_SLOT_SIZE;
    uint32_t raw_size = *reinterpret_cast<uint32_t *>(data + slot_offset);
    if ((raw_size & DELETE_MASK) == 0) {
        return false; // Not marked deleted
    }
    *reinterpret_cast<uint32_t *>(data + slot_offset) = raw_size & ~DELETE_MASK;
    SetDirty(true);
    return true;
}

bool TablePage::GetFirstTupleRid(RID *first_rid) const {
    uint32_t num = GetNumTuples();
    for (uint32_t i = 0; i < num; ++i) {
        if (!IsTupleDeleted(i) && GetTupleSize(i) > 0) {
            if (first_rid != nullptr) {
                first_rid->Set(GetTablePageId(), i);
            }
            return true;
        }
    }
    return false;
}

bool TablePage::GetNextTupleRid(const RID &cur_rid, RID *next_rid) const {
    uint32_t num = GetNumTuples();
    for (uint32_t i = cur_rid.GetSlotNum() + 1; i < num; ++i) {
        if (!IsTupleDeleted(i) && GetTupleSize(i) > 0) {
            if (next_rid != nullptr) {
                next_rid->Set(GetTablePageId(), i);
            }
            return true;
        }
    }
    return false;
}

void TablePage::Defragment() {
    uint32_t num_tuples = GetNumTuples();
    char temp[PAGE_SIZE];
    uint32_t new_free_ptr = static_cast<uint32_t>(PAGE_SIZE);

    for (uint32_t i = 0; i < num_tuples; ++i) {
        if (!IsTupleDeleted(i) && GetTupleSize(i) > 0 && GetTupleOffset(i) > 0) {
            uint32_t size = GetTupleSize(i);
            uint32_t old_offset = GetTupleOffset(i);
            new_free_ptr -= size;

            std::memcpy(temp + new_free_ptr, GetData() + old_offset, size);
            SetTupleOffset(i, new_free_ptr);
        } else {
            SetTupleOffset(i, 0);
            SetTupleSize(i, 0);
        }
    }

    uint32_t packed_bytes = static_cast<uint32_t>(PAGE_SIZE) - new_free_ptr;
    if (packed_bytes > 0) {
        std::memcpy(GetData() + new_free_ptr, temp + new_free_ptr, packed_bytes);
    }

    SetFreeSpacePointer(new_free_ptr);

    // Reclaim trailing deleted slots
    while (num_tuples > 0 && GetTupleOffset(num_tuples - 1) == 0 && GetTupleSize(num_tuples - 1) == 0) {
        num_tuples--;
    }
    SetNumTuples(num_tuples);

    SetDirty(true);
}
