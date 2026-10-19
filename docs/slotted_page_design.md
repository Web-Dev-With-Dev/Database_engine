# Slotted Page Architecture — Design Deep Dive

## 1. What Problem Does the Slotted Page Solve?

Database storage engines operate on fixed-size disk blocks (e.g. 4096 bytes in this engine, 8192 bytes in PostgreSQL, 4096 bytes in modern SQLite). However, database records (tuples) are almost never fixed-size:
- Columns like `VARCHAR`, `TEXT`, `BLOB`, and `JSON` have dynamic lengths.
- NULL bitmaps mean tuples with NULL fields consume less space.
- Integer compression / variable-length encodings (e.g. SQLite varints) vary per value.
- Tuples change size during `UPDATE` operations.

### Naive Approaches and Why They Fail

| Approach | How it Works | Why it Fails |
|---|---|---|
| **Fixed-Length Slots** | Divide 4 KB page into $N$ equal chunks of max possible tuple size. | Massive internal fragmentation. If max string length is 1000 bytes, a page holds at most 4 tuples, even if strings are 5 bytes. |
| **Contiguous Append** | Append tuples sequentially `[T1][T2][T3]...` | Deleting $T_1$ leaves a hole. Shifting $T_2, T_3$ left invalidates their byte offsets, breaking all secondary indexes pointing to them. |
| **Pointers to Offsets** | Secondary index stores `(page_id, byte_offset)`. | Any page reorganization (defrag, row resize) requires updating every secondary index in the entire database. |

### The Core Invariant: The Indirection Vector

To solve this, relational engines introduce a level of **indirection within the page**:
1. Tuples are identified externally by a **Record ID (RID)**:
   $$\text{RID} = (\text{page\_id}, \text{slot\_id})$$
   (In Postgres this is `ctid` / `ItemPointerData`; in SQLite tables this is keyed by `rowid` pointing to B-tree cells).
2. The `slot_id` is an index into an array of **pointers/slots** located at the front of the page.
3. The slot holds the physical **byte offset** and **length** of the tuple data.
4. **Result**: Tuples can move freely anywhere within the 4 KB page (during compaction, defragmentation, or updates) without changing their external `slot_id`! Secondary indexes remain 100% valid.

---

## 2. Anatomy of a Slotted Page: The Dual-End Growth Model

Both Postgres and SQLite adopt a symmetric, two-ended growth layout:
- **Header & Slot Array**: Grow **forward** from byte 0 toward the center.
- **Tuple Storage**: Grows **backward** from byte `PAGE_SIZE` toward the center.
- **Free Space**: A single, contiguous pool in the middle.

```
Byte 0                                                                   Byte 4096
┌──────────────┬───────────────┬──────────────────────────┬──────────────┬──────────────┐
│ Page Header  │ Slot Array    │   Contiguous Free Space  │   Tuple 2    │   Tuple 1    │
│              │ [S0][S1][S2]  │                          │              │              │
└──────────────┴───────────────┴──────────────────────────┴──────────────┴──────────────┘
               ▲               ▲                          ▲              ▲
               │               │                          │              │
          Header End       pd_lower                    pd_upper      PAGE_SIZE
                        (grows forward ──►)      (◄── grows backward)
```

### Why Dual-End Growth?
1. **Zero Partitioning Overhead**: You do not have to guess in advance how much memory to allocate for slots vs. tuple data.
2. **Unified Free Space**: Free space is kept as a single continuous block between `pd_lower` and `pd_upper`. Allocation succeeds until the two boundaries collide:
   $$\text{Contiguous Free Space} = \text{pd\_upper} - \text{pd\_lower}$$
3. **No Premature Out-of-Memory**: A page is full only when `sizeof(Slot) + tuple_length > (pd_upper - pd_lower)`.

---

## 3. PostgreSQL Implementation: `PageHeaderData` & `ItemIdData`

Postgres defines its page format in `src/include/storage/bufpage.h` and `itemid.h`.

### 3.1 Page Header (`PageHeaderData` — 24 bytes)

```c
typedef struct PageHeaderData {
    PageXLogRecPtr  pd_lsn;           // 8 bytes: LSN of last WAL write modifying this page
    uint16          pd_checksum;      // 2 bytes: CRC-16 page checksum (if checksums enabled)
    uint16          pd_flags;         // 2 bytes: flag bits (e.g., PD_PAGE_FULL, PD_ALL_VISIBLE)
    LocationIndex   pd_lower;         // 2 bytes: byte offset to start of free space
    LocationIndex   pd_upper;         // 2 bytes: byte offset to end of free space
    LocationIndex   pd_special;       // 2 bytes: byte offset to start of special space (page end)
    uint16          pd_pagesize_version; // 2 bytes: page size (8192) & layout version number
    TransactionId   pd_prune_xid;     // 4 bytes: oldest unpruned XMAX, hint for VACUUM pruning
} PageHeaderData;                     // Total = 24 bytes
```

### 3.2 The Slot / Line Pointer (`ItemIdData` — 4 bytes)

In Postgres, slots are called **Line Pointers** (`ItemIdData`). Each slot is a bit-packed 32-bit unsigned integer:

```c
typedef struct ItemIdData {
    unsigned lp_off:15,     // Offset to tuple from start of page (0 .. 32767)
             lp_flags:2,    // State flags: UNUSED, NORMAL, REDIRECT, DEAD
             lp_len:15;     // Length of tuple in bytes (0 .. 32767)
} ItemIdData;               // Total = 4 bytes (sizeof = 4)
```

The 2-bit `lp_flags` field is the foundation of MVCC and HOT (Heap-Only Tuples):
- `LP_UNUSED (0)`: Slot is unused/free. Can be allocated to a new tuple.
- `LP_NORMAL (1)`: Slot points to a valid live/dead heap tuple at `lp_off` of length `lp_len`.
- `LP_REDIRECT (2)`: HOT optimization! The tuple was updated without modifying indexed columns. This slot forwards to another slot on the *same page* (`lp_off` contains target `slot_id`). Outer indexes continue pointing to this original slot.
- `LP_DEAD (3)`: Tuple was vacuumed/pruned, but slot remains to preserve index references until index cleanup.

### 3.3 PostgreSQL Page Layout Diagram

```
Offset 0
┌────────────────────────────────────────────────────────┐
│ PageHeaderData (24 bytes)                              │
│   pd_lsn (8B) | pd_checksum (2B) | pd_flags (2B)       │
│   pd_lower (2B) | pd_upper (2B) | pd_special (2B)      │
│   pd_pagesize_version (2B) | pd_prune_xid (4B)         │
├────────────────────────────────────────────────────────┤
│ ItemIdData[0] (4 bytes: lp_off, lp_flags, lp_len)      │ ──┐ points to Tuple 0
│ ItemIdData[1] (4 bytes: lp_off, lp_flags, lp_len)      │ ──┼──┐ points to Tuple 1
│ ItemIdData[2] (4 bytes: lp_off, lp_flags, lp_len)      │ ──┼──┼──┐ points to Tuple 2
│ ...                                                    │   │  │  │
├────────────────────────────────────────────────────────┤ ◄─┘  │  │
│ pd_lower ──► (Free space start)                        │      │  │
│                                                        │      │  │
│                  UNALLOCATED FREE SPACE                │      │  │
│                                                        │      │  │
│ pd_upper ──► (Free space end)                          │      │  │
├────────────────────────────────────────────────────────┤ ◄────┼──┘
│ Tuple 2: HeapTupleHeader (23B) + null_bitmap + data    │      │
├────────────────────────────────────────────────────────┤ ◄────┘
│ Tuple 1: HeapTupleHeader (23B) + null_bitmap + data    │
├────────────────────────────────────────────────────────┤ ◄─────
│ Tuple 0: HeapTupleHeader (23B) + null_bitmap + data    │
├────────────────────────────────────────────────────────┤
│ pd_special ──► Special Space (e.g., B-tree opaque data)│ (0 bytes for Heap)
└────────────────────────────────────────────────────────┘
Offset 8192 (or 4096)
```

### 3.4 Alignment Rules (MAXALIGN)
Tuples are aligned on CPU word boundaries (`MAXALIGN = 8` bytes on 64-bit systems).
When Postgres allocates a tuple of size $L$ from `pd_upper`:
$$\text{new\_upper} = (\text{pd\_upper} - L) \ \& \sim 7$$
This ensures direct memory access without unaligned fault penalties.

---

## 4. SQLite Implementation: B-Tree Page Header & Cell Pointers

Unlike Postgres which separates the heap file from B-tree index files, **SQLite is an Index-Organized B-Tree engine**. Every table is a B-Tree where leaf nodes hold row data, and interior nodes hold routing keys.

### 4.1 SQLite B-Tree Header (8 bytes for leaf, 12 bytes for interior)

```
Byte 0      : Page Type Flag
              0x0a = Leaf Index Page
              0x0d = Leaf Table Page (Data / B+Tree leaf)
              0x02 = Interior Index Page
              0x05 = Interior Table Page
Byte 1..2   : Offset of first Freeblock (0 if none)
Byte 3..4   : Number of cells on this page (nCell)
Byte 5..6   : Start of Cell Content Area (cell_content_offset, 0 means 65536)
Byte 7      : Number of fragmented free bytes (nFrag)
Byte 8..11  : Right-most child page pointer (Interior pages only)
```
*(Note: Page 1 of an SQLite file has a 100-byte database file header, so its B-tree header starts at byte 100).*

### 4.2 SQLite Cell Pointer Array
- Begins immediately following the B-tree page header (byte 8 or 12).
- An array of **2-byte unsigned big-endian integers** (`u16`).
- Each entry contains the byte offset where that cell begins in the page.
- Cells in the pointer array are kept **sorted by key** (rowid). To binary search a rowid, SQLite inspects the 2-byte offsets in the pointer array and accesses the cell keys.

### 4.3 SQLite Cell Format (Table B-Tree Leaf)
In a leaf table page (flag `0x0d`), each cell stores:
1. `payload_size`: Variable-length integer (varint, 1-9 bytes).
2. `rowid`: Variable-length integer (varint, 1-9 bytes).
3. `payload`: The actual row data (Record format: column types followed by column data).
4. `overflow_page_id`: 4-byte page number (present only if payload exceeds page limit).

### 4.4 Freeblocks & Fragmented Free Bytes (`nFrag`)
When a cell in SQLite is deleted:
- If the freed space is **$\ge 4$ bytes**, it is formatted as a **Freeblock**:
  - Byte 0..1: Offset to the next freeblock in the page (sorted ascending by address).
  - Byte 2..3: Size of this freeblock in bytes.
- If the freed space is **$< 4$ bytes**, it cannot store the 4-byte freeblock header. SQLite adds this small hole to `nFrag` (byte 7 in header).
- **Compaction Trigger**: If SQLite needs to allocate a cell and `contiguous_free < size`, but `contiguous_free + freeblock_bytes + nFrag >= size`, SQLite runs `defragmentPage()`.

---

## 5. Architectural Comparison: PostgreSQL vs. SQLite

| Dimension | PostgreSQL | SQLite |
|---|---|---|
| **Storage Model** | Heap Page (Unordered heap files + separate B-tree indexes) | B+Tree Page (Index-Organized: tables are B+Trees keyed by `rowid`) |
| **Page Header Size** | 24 bytes (`PageHeaderData`) | 8 bytes (leaf) or 12 bytes (interior) |
| **Slot Entry Size** | 4 bytes (`ItemIdData`: 15-bit off, 2-bit flags, 15-bit len) | 2 bytes (raw offset `u16`) |
| **Slot Sorting** | Unsorted (slots represent permanent physical slot numbers) | Sorted by search key (cell pointers ordered for binary search) |
| **Slot Reuse** | Reuses dead slot indices to avoid slot array expansion | Shifts pointer array when cells are inserted/deleted to keep sorted |
| **Record Header** | Heavy 23-byte `HeapTupleHeaderData` (MVCC xmin/xmax, infomasks) | Lightweight per-cell varints (payload size + rowid) |
| **Deleted Space Tracking** | Unpruned tuples stay until `VACUUM`; slots can mark `LP_DEAD` | In-page singly linked list of **Freeblocks** + `nFrag` counter |
| **Compaction Strategy** | Compacts during `VACUUM` or HOT-prune; repacks live tuples | `defragmentPage()` when fragmented free space is needed |
| **Oversized Records** | **TOAST**: Compress or split out-of-line into separate table | **Overflow Pages**: Linked list of overflow pages pointed by cell |

---

## 6. Page Operations & Invariants

Let's trace how the fundamental page operations maintain invariants.

### 6.1 Tuple Insertion

1. **Calculate Required Space**:
   $$\text{needed} = \text{aligned}(T_{\text{len}}) + (\text{reusing\_slot} \ ? \ 0 : \text{sizeof(Slot)})$$
2. **Space Check**:
   $$\text{contiguous\_free} = \text{pd\_upper} - \text{pd\_lower}$$
   - If $\text{contiguous\_free} \ge \text{needed}$: proceed.
   - Else if $\text{total\_free} \ge \text{needed}$: **Defragment / Compact** the page, making all free space contiguous, then proceed.
   - Else: Page is full! Allocate or find another page.
3. **Allocate Slot**:
   - Check if there is an unused tombstone slot (`lp_flags == LP_UNUSED`).
   - If yes: reuse it (`slot_id = found_idx`).
   - If no: append to slot array at `pd_lower`, advance `pd_lower += sizeof(Slot)`.
4. **Allocate Tuple Data**:
   - `pd_upper -= aligned(T_len)`
   - Copy tuple bytes into `data[pd_upper]`.
5. **Update Slot**:
   - Set `slot.offset = pd_upper`, `slot.length = T_len`, `slot.flags = LP_NORMAL`.
6. **Return RID**:
   $$\text{RID} = \{\text{page\_id}, \text{slot\_id}\}$$

### 6.2 Tuple Read (O(1))

1. Validate `slot_id < num_slots`.
2. Inspect `slot = slots[slot_id]`.
3. Check `slot.flags == LP_NORMAL` (if `LP_UNUSED` or `LP_DEAD`, record is deleted).
4. Direct pointer:
   $$\text{tuple\_ptr} = \text{page\_data} + \text{slot.offset}$$
   $$\text{tuple\_len} = \text{slot.length}$$

### 6.3 Tuple Deletion

1. Locate slot `slots[slot_id]`.
2. Mark `slot.flags = LP_DEAD` (or `LP_UNUSED`).
3. Note: The tuple data at `slot.offset` is **not immediately shifted or zeroed out**.
   - Shifting all tuples on every delete would be $O(M)$ where $M$ is total tuple bytes in the page.
   - The dead tuple space becomes a "hole" (fragmented free space).
   - Reclaiming this space is deferred until defragmentation/compaction is required.

### 6.4 Defragmentation / Compaction (In-Place Re-packing)

When multiple insertions and deletions create fragmentation, `pd_upper - pd_lower` may be smaller than needed even though total free space is sufficient. Compaction restores the single contiguous free space block:

```
BEFORE COMPACTION (Fragmented):
┌──────┬────────────┬──────┬─────────┬──────────┬─────────┬──────────┐
│ Hdr  │ Slots      │ Free │ Tuple 3 │   DEAD   │ Tuple 1 │   DEAD   │
│      │ [S0][S1].. │      │ (Live)  │  (Hole)  │ (Live)  │  (Hole)  │
└──────┴────────────┴──────┴─────────┴──────────┴─────────┴──────────┘
                    ▲      ▲
                 pd_lower  pd_upper

AFTER COMPACTION (Repacked to end of page):
┌──────┬────────────┬───────────────────────────┬─────────┬──────────┐
│ Hdr  │ Slots      │   Unified Contiguous      │ Tuple 3 │ Tuple 1  │
│      │ [S0][S1].. │       Free Space          │ (Live)  │ (Live)   │
└──────┴────────────┴───────────────────────────┴─────────┴──────────┘
                    ▲                           ▲
                 pd_lower                    pd_upper (reset!)
```

**Compaction Algorithm**:
1. Allocate a temporary buffer of size `PAGE_SIZE` (or use scratch memory).
2. Set `new_upper = PAGE_SIZE`.
3. For each active slot in the slot array (`slot.flags == LP_NORMAL`):
   - Calculate aligned size: `aligned_len = align(slot.length)`.
   - `new_upper -= aligned_len`.
   - Copy original tuple data from `page_data + slot.offset` into `temp_buf + new_upper`.
   - Update `slot.offset = new_upper`.
4. Copy the packed tuples from `temp_buf` back into the page from `new_upper` to `PAGE_SIZE`.
5. Set `page_header.pd_upper = new_upper`.
6. Now all holes are eliminated, and free space between `pd_lower` and `pd_upper` is maximized!

---

## 7. Concrete C++20 Blueprint for this Database Engine

To integrate this directly into `d:\database-engine`, we build `SlottedPage` directly on top of the 4096-byte `Page::GetData()` buffer.

### 7.1 Header and Slot Structs

```cpp
#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include "config.h" // PAGE_SIZE = 4096, page_id_t

// Identifier for any record in the database
struct RID {
    page_id_t page_id{INVALID_PAGE_ID};
    uint16_t slot_num{0};

    bool operator==(const RID &other) const = default;
};

// 4-byte slot structure (inspired by Postgres ItemIdData)
struct Slot {
    uint16_t offset : 15;     // Byte offset of tuple from start of page
    uint16_t is_used : 1;     // 1 if active, 0 if deleted / tombstone
    uint16_t length;          // Byte length of tuple data
};
static_assert(sizeof(Slot) == 4, "Slot must be exactly 4 bytes");

// Slotted Page Header (16 bytes, clean power-of-two alignment)
struct SlottedPageHeader {
    page_id_t page_id{INVALID_PAGE_ID}; // 4 bytes
    uint16_t num_slots{0};              // 2 bytes: total slots currently allocated
    uint16_t num_active_tuples{0};      // 2 bytes: active (non-deleted) tuples
    uint16_t lower{0};                  // 2 bytes: byte offset to start of free space
    uint16_t upper{PAGE_SIZE};          // 2 bytes: byte offset to end of free space
    uint16_t fragmented_bytes{0};       // 2 bytes: dead space in deleted/shrunk tuples
    uint16_t flags{0};                  // 2 bytes: reserved/status flags
};
static_assert(sizeof(SlottedPageHeader) == 16, "Header must be 16 bytes");
```

### 7.2 Zero-Copy Member Layout Mapping

```cpp
class SlottedPage {
private:
    char *data_; // Points to Page::GetData() (4096 bytes)

    SlottedPageHeader *Header() {
        return reinterpret_cast<SlottedPageHeader *>(data_);
    }

    const SlottedPageHeader *Header() const {
        return reinterpret_cast<const SlottedPageHeader *>(data_);
    }

    Slot *SlotArray() {
        return reinterpret_cast<Slot *>(data_ + sizeof(SlottedPageHeader));
    }

    const Slot *SlotArray() const {
        return reinterpret_cast<const Slot *>(data_ + sizeof(SlottedPageHeader));
    }

public:
    explicit SlottedPage(char *data) : data_(data) {}

    void Init(page_id_t page_id) {
        auto *hdr = Header();
        hdr->page_id = page_id;
        hdr->num_slots = 0;
        hdr->num_active_tuples = 0;
        hdr->lower = sizeof(SlottedPageHeader);
        hdr->upper = PAGE_SIZE;
        hdr->fragmented_bytes = 0;
        hdr->flags = 0;
    }

    // Helper: 8-byte alignment
    static constexpr uint16_t Align(uint16_t n) {
        return (n + 7) & ~7;
    }

    uint16_t GetContiguousFreeSpace() const {
        auto *hdr = Header();
        return (hdr->upper >= hdr->lower) ? (hdr->upper - hdr->lower) : 0;
    }

    uint16_t GetTotalFreeSpace() const {
        return GetContiguousFreeSpace() + Header()->fragmented_bytes;
    }
};
```

### 7.3 Tuple Insertion Implementation

```cpp
std::optional<RID> SlottedPage::InsertTuple(std::span<const char> tuple_bytes) {
    auto *hdr = Header();
    const uint16_t tuple_len = static_cast<uint16_t>(tuple_bytes.size());
    const uint16_t aligned_len = Align(tuple_len);

    // 1. Check for reusable tombstone slot
    int reusable_slot = -1;
    auto *slots = SlotArray();
    for (uint16_t i = 0; i < hdr->num_slots; ++i) {
        if (!slots[i].is_used && slots[i].length == 0) {
            reusable_slot = i;
            break;
        }
    }

    const uint16_t slot_space_needed = (reusable_slot != -1) ? 0 : sizeof(Slot);
    const uint16_t total_needed = aligned_len + slot_space_needed;

    // 2. Check if page has enough room
    if (GetContiguousFreeSpace() < total_needed) {
        if (GetTotalFreeSpace() >= total_needed) {
            Defragment(); // Reclaim fragmented holes
        } else {
            return std::nullopt; // Page full
        }
    }

    // 3. Determine slot index
    uint16_t slot_idx = 0;
    if (reusable_slot != -1) {
        slot_idx = static_cast<uint16_t>(reusable_slot);
    } else {
        slot_idx = hdr->num_slots++;
        hdr->lower += sizeof(Slot);
    }

    // 4. Allocate tuple storage from upper end
    hdr->upper -= aligned_len;
    std::memcpy(data_ + hdr->upper, tuple_bytes.data(), tuple_len);

    // 5. Update slot
    slots[slot_idx].offset = hdr->upper;
    slots[slot_idx].length = tuple_len;
    slots[slot_idx].is_used = 1;

    hdr->num_active_tuples++;
    return RID{hdr->page_id, slot_idx};
}
```

### 7.4 Defragmentation / Compaction Implementation

```cpp
void SlottedPage::Defragment() {
    auto *hdr = Header();
    auto *slots = SlotArray();

    // Temporary buffer to stage active tuples
    alignas(8) char temp_buffer[PAGE_SIZE];
    uint16_t new_upper = PAGE_SIZE;

    for (uint16_t i = 0; i < hdr->num_slots; ++i) {
        if (slots[i].is_used) {
            const uint16_t aligned_len = Align(slots[i].length);
            new_upper -= aligned_len;

            // Copy active tuple to bottom of temp buffer
            std::memcpy(temp_buffer + new_upper, data_ + slots[i].offset, slots[i].length);

            // Update slot offset to point to new location
            slots[i].offset = new_upper;
        }
    }

    // Copy packed tuples back into original page
    const uint16_t bytes_to_copy = PAGE_SIZE - new_upper;
    if (bytes_to_copy > 0) {
        std::memcpy(data_ + new_upper, temp_buffer + new_upper, bytes_to_copy);
    }

    hdr->upper = new_upper;
    hdr->fragmented_bytes = 0;
}
```

---

## 8. Summary of Key Design Principles

1. **Physical Indirection is King**: The slot array decouples external addressing (RID) from internal storage location. This enables garbage collection, defragmentation, and updates without touching external indexes.
2. **Dual-End Growth Maximizes Density**: Placing the slot array at the front and tuple data at the rear ensures dynamic records share a unified contiguous free space buffer.
3. **Lazy Compaction Minimizes I/O Overhead**: Do not repack tuples on every deletion. Mark slots as unused/dead, accumulate fragmented bytes, and compact only when a new allocation strictly demands contiguous space.
4. **Hardware Alignment Matters**: Aligning tuples to 8 bytes (`Align()`) prevents unaligned memory faults and leverages native 64-bit load/store instructions.
