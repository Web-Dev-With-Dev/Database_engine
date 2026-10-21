<p align="center">
  <img src="assets/dittodb_icon.png" alt="DittoDB Logo" width="180" />
</p>

<h1 align="center">DittoDB</h1>

<p align="center">
  <strong>The malleable, high-performance C++20 relational database storage engine.</strong><br>
  <em>Adaptive page layouts • Scan-resistant LRU-K caching • Multi-page table heaps • Zero shortcuts.</em>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Language-C%2B%2B20-blue.svg" alt="C++20" />
  <img src="https://img.shields.io/badge/Status-In%20Creation%20Mode%20%F0%9F%9A%A7-yellow.svg" alt="Creation Mode" />
  <img src="https://img.shields.io/badge/Tests-185%2F185%20Passed-brightgreen.svg" alt="Tests" />
  <img src="https://img.shields.io/badge/Architecture-5--Tier-purple.svg" alt="Architecture" />
  <img src="https://img.shields.io/badge/Build-CMake%20%7C%20Ninja-orange.svg" alt="Build" />
</p>

> [!NOTE]
> 🚧 **Status: In Creation Mode**  
> DittoDB is actively under development. Core storage layers (Disk Manager, Buffer Pool, LRU-K Replacer, Slotted `TablePage`, Multi-Page `TableHeap`, buffer-pool-backed `ExtendibleHashTable`, and full end-to-end multi-tier eviction stress tests) are fully implemented and verified with 185/185 passing tests. Subsequent tiers (B+Tree indexing, WAL redo/recovery, and query execution) are currently being built.

---

## Architecture Overview

The engine follows a strict multi-tier modular architecture where each layer abstracts the complexity of the layers beneath it:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ Layer 5: Table Heap & Cross-Page Management                                 │
│  - TableHeap: Doubly-linked chain of TablePages                             │
│  - Multi-Page Spanning Inserts                                              │
│  - Cross-Page Migration on Update (Delete Old + Insert New)                 │
│  - TableIterator: Multi-page scan skipping deleted/tombstoned slots         │
├─────────────────────────────────────────────────────────────────────────────┤
│ Layer 4: Records, Identifiers, & Slotted Pages                              │
│  - TablePage: Slotted-page layout on top of 4096-byte Page                  │
│  - Dual-End Growth: Slot array (forward), Tuple data (backward)             │
│  - Indirection Vector: RID (page_id, slot_num) with offset stability        │
│  - Compaction / Defragmentation: Hole consolidation & space reclamation     │
│  - TablePageIterator: Page-level forward iterator (std::forward_iterator)   │
│  - Tuple: Variable-length payload abstraction                               │
├─────────────────────────────────────────────────────────────────────────────┤
│ Layer 3: Buffer Pool Management & Caching                                   │
│  - BufferPoolManager: Manages fixed pool of in-memory frames                │
│  - Page Table: Hash map mapping page_id → frame_id                          │
│  - Pin / Unpin concurrency protocol with dirty-page writeback tracking      │
├──────────────────────────────────────┬──────────────────────────────────────┤
│ Layer 2A: Replacement Policy         │ Layer 2B: In-Memory Frame            │
│  - LRUKReplacer: LRU-K algorithm     │  - Page: 4096-byte data frame        │
│  - Cold queue (FIFO) for k < K       │  - std::shared_mutex R/W latch       │
│  - Hot queue (k-th access timestamp) │  - Atomic pin counter & dirty flag   │
│  - Scan resistance                   │  - Zero-copy cast to TablePage       │
├──────────────────────────────────────┴──────────────────────────────────────┤
│ Layer 1: Persistent Disk Storage                                            │
│  - DiskManager: Page-addressed file I/O (PAGE_SIZE = 4096 bytes)            │
│  - Monotonic page allocation, zero-fill sparse writes, thread-safe access   │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Core Components

### 1. `TableHeap` & `TableIterator`
**Headers:** [`include/table_heap.h`](include/table_heap.h) · **Source:** [`src/table_heap.cpp`](src/table_heap.cpp)

Represents a physical relational table stored as a doubly-linked list of `TablePage`s inside the `BufferPoolManager`.

```
Page 0 (TablePage)               Page 1 (TablePage)               Page 2 (TablePage)
┌──────────────────────┐         ┌──────────────────────┐         ┌──────────────────────┐
│ Prev: -1  | Next: 1  │ ◄─────► │ Prev: 0   | Next: 2  │ ◄─────► │ Prev: 1   | Next: -1 │
│ [Slot 0] [Slot 1]... │         │ [Slot 0] [Slot 1]... │         │ [Slot 0] [Slot 1]... │
└──────────────────────┘         └──────────────────────┘         └──────────────────────┘
```

- **Multi-Page Inserts**: Sequentially checks pages for free space. If all pages in the chain are full, allocates and initializes a new page via `BufferPoolManager::NewPage` and links it into the chain.
- **Cross-Page Migration on Update (Delete Old + Insert New)**:
  - If a tuple update fits on the current page (in-place or via defragmentation), updates it locally with **zero RID change**.
  - If the updated tuple cannot fit on the current page even after compaction, executes **Strategy 3**:
    1. Soft-deletes the old tuple on the current page (`table_page->MarkDelete(old_rid)`).
    2. Inserts the updated tuple on an existing or newly allocated page in the heap.
    3. Returns the `new_rid` to update any secondary indexes.
    4. If the insertion fails, automatically rolls back the delete on the old page to preserve atomicity.
- **`TableIterator`**: Implements `std::forward_iterator` over the entire table heap. Seamlessly advances across page boundaries, fetches/unpins buffer pool frames, and skips all deleted/empty slots.

---

### 2. `TablePage` & `TablePageIterator`
**Headers:** [`include/table_page.h`](include/table_page.h) · **Source:** [`src/table_page.cpp`](src/table_page.cpp)

Implements the **Slotted-Page Architecture** on top of the 4096-byte raw `Page`.

```
Offset 0                                                              Offset 4096
┌─────────────────┬──────────────────┬──────────────┬──────────────┬─────────────┐
│ Page Header     │ Slot Directory   │ Free Space   │ Tuple 1 Data │ Tuple 0 Data│
│ (24 bytes)      │ [S0][S1][S2]...  │ (Contiguous) │ (Variable)   │ (Variable)  │
└─────────────────┴──────────────────┴──────────────┴──────────────┴─────────────┘
▲                 ▲                  ▲              ▲                            ▲
0                 24                 Lower (pd)     Upper (free_ptr)             4096
                                  (grows ──►)       (◄── grows)
```

- **Dual-End Growth Model**:
  - **Slot Directory**: Grows **forward** from byte 24 toward 4096. Each slot is 8 bytes (`uint32_t offset` + `uint32_t size`).
  - **Tuple Data**: Grows **backward** from byte 4096 toward 24.
  - **Unified Free Space**: A single dynamic pool in the middle (`free_ptr - slot_end`).
- **Tombstones & Deletion**:
  - `MarkDelete(rid)`: Fast soft delete setting the high-order tombstone bit (`1u << 31`) on slot size. Reversible via `RollbackDelete(rid)`.
  - `ApplyDelete(rid)`: Physically zeroes slot offset and size, enabling immediate slot index reuse without growing the slot directory.
- **Compaction & Defragmentation (`Defragment()`)**:
  - Eliminates dead space holes created by deletions and tuple updates.
  - Shifts all live tuples tightly toward byte 4096 and reclaims trailing dead slots.
  - **Guarantees RID Stability**: Shifts physical memory offsets while preserving identical slot indices.
- **`TablePageIterator`**:
  - Single-page forward iterator satisfying C++20 `std::forward_iterator`.
  - Automatically skips tombstoned, zeroed, and deleted slots.
  - Supports range-based for loops: `for (const auto &tuple : page)`.

---

### 3. Record Identifiers (`RID`) & `Tuple`
**Headers:** [`include/rid.h`](include/rid.h) · [`include/tuple.h`](include/tuple.h)

- **`RID` (Record Identifier)**: The logical address of any record in the database:
  $$\text{RID} = (\text{page\_id}, \text{slot\_num})$$
  - Packed into a 64-bit integer (`uint64_t`) for fast comparisons and hashing.
  - Provides physical indirection: external indexes store RIDs, never physical byte offsets.
- **`Tuple`**:
  - Represents variable-length record data backed by `std::vector<char>`.
  - Provides zero-copy `std::string_view` and `std::span<const char>` inspection methods.

---

### 4. `BufferPoolManager`
**Headers:** [`include/buffer_pool_manager.h`](include/buffer_pool_manager.h) · **Source:** [`src/buffer_pool_manager.cpp`](src/buffer_pool_manager.cpp)

Maintains an in-memory cache of disk pages to minimize slow storage I/O.

- **Page Table**: `std::unordered_map<page_id_t, frame_id_t>` for $O(1)$ page lookups.
- **`FetchPage(page_id)`**: Hits in-memory cache or transparently fetches from disk into a free frame (or evicts an unpinned frame).
- **Pin / Unpin Protocol**: Pages with `pin_count > 0` are pinned and cannot be evicted by other concurrent threads.
- **Dirty Tracking**: Dirty pages modified in memory are automatically flushed back to disk upon eviction or explicit flush.
- **`NewPage(&page_id)`**: Allocates a new page ID from `DiskManager` and pins a frame for immediate writing.

---

### 5. `LRUKReplacer`
**Headers:** [`include/lru_k_replacer.h`](include/lru_k_replacer.h) · **Source:** [`src/lru_k_replacer.cpp`](src/lru_k_replacer.cpp)

Implements the scan-resistant **LRU-K cache eviction policy** (default $K=2$).

- **Cold Queue (History Queue)**: Frames accessed fewer than $K$ times (evicted in FIFO order).
- **Hot Queue (Cache Queue)**: Frames accessed $\ge K$ times (evicted by earliest $K$-th backward access timestamp).
- **Scan Resistance**: Prevents single-pass sequential table scans from flushing hot index/data pages out of the buffer pool.

---

### 6. `Page` & `DiskManager`
**Headers:** [`include/page.h`](include/page.h) · [`include/disk_manager.h`](include/disk_manager.h) · **Source:** [`src/page.cpp`](src/page.cpp) · [`src/disk_manager.cpp`](src/disk_manager.cpp)

- **`Page`**:
  - A fixed 4096-byte memory frame.
  - Contains atomic pin counters, atomic dirty flags, and a `std::shared_mutex` for concurrent reader/writer latching.
  - Standard layout: `sizeof(TablePage) == sizeof(Page) == 4096` enables zero-copy `TablePage::Cast()`.
- **`DiskManager`**:
  - Performs direct 4096-byte block file I/O on the underlying database file.
  - Zero-fills sparse page allocations and validates file boundaries (`file_size % 4096 == 0`).

---

## Project Structure

```
database-engine/
├── include/
│   ├── config.h                 # Global constants: PAGE_SIZE (4096), page_id_t, frame_id_t
│   ├── disk_manager.h           # DiskManager interface
│   ├── page.h                   # Page (4 KB frame, latches, pin count)
│   ├── lru_k_replacer.h         # LRU-K cache eviction replacer
│   ├── buffer_pool_manager.h    # BufferPoolManager interface
│   ├── rid.h                    # Record ID (page_id + slot_num)
│   ├── tuple.h                  # Tuple record container
│   ├── table_page.h             # Slotted-page layout and TablePageIterator
│   └── table_heap.h             # Multi-page TableHeap and TableIterator
├── src/
│   ├── main.cpp                 # Demo executable
│   ├── disk_manager.cpp         # DiskManager implementation
│   ├── page.cpp                 # Page implementation
│   ├── lru_k_replacer.cpp       # LRU-K Replacer implementation
│   ├── buffer_pool_manager.cpp  # BufferPoolManager implementation
│   ├── table_page.cpp           # TablePage & TablePageIterator implementation
│   └── table_heap.cpp           # TableHeap & TableIterator implementation
├── scripts/
│   ├── run_tsan.sh              # Build and run under ThreadSanitizer
│   └── run_helgrind.sh          # Build and run under Valgrind Helgrind
├── tests/
│   ├── disk_manager_test.cpp        # 42 DiskManager tests + benchmarks
│   ├── page_test.cpp                # 28 Page latching & pin tests
│   ├── lru_k_replacer_test.cpp      # 14 LRU-K unit tests + benchmark
│   ├── buffer_pool_manager_test.cpp # 13 BufferPoolManager tests
│   ├── integration_test.cpp         # 15 BufferPool integration workload tests
│   ├── rid_test.cpp                 # 9 RID packing, hashing, & comparison tests
│   ├── table_page_test.cpp          # 41 Slotted page, iterator, defrag, & stability tests
│   ├── table_heap_test.cpp          # 9 Multi-page TableHeap & cross-page migration tests
│   ├── extendible_hash_table_test.cpp # 10 Directory & bucket split tests
│   ├── e2e_storage_test.cpp         # 4 Multi-tier storage integration tests
│   └── buffer_pool_manager_concurrent_test.cpp # 6 Multithreaded stress & latching tests
├── docs/
│   ├── disk_manager_design.md   # Disk storage architecture deep-dive
│   ├── lru_k_design.md          # LRU-K cache algorithm deep-dive
│   └── slotted_page_design.md   # Slotted page architecture & Postgres/SQLite comparison
├── CMakeLists.txt
└── README.md
```

---

## Build & Test

### Requirements
- CMake ≥ 3.20
- C++20 compiler (GCC 10+ / Clang 12+ / MSVC 2022)
- Ninja or Make
- Google Test

### Build Steps

```bash
# Configure with Ninja and C++20
cmake -B build -G Ninja

# Compile library, demo, and all test suites
cmake --build build
```

### Run All Unit & Integration Tests

```bash
# Run full test suite via CTest (191 tests)
ctest --test-dir build --output-on-failure
```

Run the interactive demo or individual component test suites:
```bash
./build/dittodb.exe                               # DittoDB demo CLI
./build/disk_manager_test.exe                     # Disk I/O & corruption detection
./build/page_test.exe                             # Page frames & latch concurrency
./build/lru_k_replacer_test.exe                   # LRU-K replacement policy
./build/buffer_pool_manager_test.exe              # Buffer pool caching & evictions
./build/integration_test.exe                      # Complex database access simulations
./build/rid_test.exe                              # RID bit-packing & hashing
./build/table_page_test.exe                       # Slotted pages, compactions, & iterators
./build/table_heap_test.exe                       # Multi-page TableHeap & migration updates
./build/extendible_hash_table_test.exe            # Extendible hash directory & bucket splits
./build/e2e_storage_test.exe                      # Multi-tier integration under forced evictions
./build/buffer_pool_manager_concurrent_test.exe   # Multithreaded stress & latching under eviction
```

---

## ThreadSanitizer & Helgrind Verification

DittoDB is designed and verified for zero data races and deadlocks under heavy concurrent access:

### 1. ThreadSanitizer (TSan)
ThreadSanitizer monitors memory accesses at compile/runtime to catch race conditions and lock order inversions.
```bash
# Using the helper script (Linux / macOS / WSL)
./scripts/run_tsan.sh

# Or configure manually with CMake
cmake -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure
```

### 2. Valgrind Helgrind
Helgrind performs dynamic POSIX pthread synchronization and lock hierarchy analysis:
```bash
# Using the helper script
./scripts/run_helgrind.sh

# Or run manually
valgrind --tool=helgrind --error-exitcode=1 ./build/buffer_pool_manager_concurrent_test
```

---

## Test Coverage Summary

**Current Status: 191 / 191 tests passing across 11 suites (100% pass rate).**

| Test Suite | Tests | Features Covered |
|---|---|---|
| `DiskManagerTest` | 42 | Page-addressed read/write, zero-fill sparse allocation, persistence, corruption validation, concurrent stress |
| `PageTest` | 28 | Frame data integrity, pin/unpin saturating counters, dirty flags, shared/exclusive latches, stress tests |
| `LRUKReplacerTest` | 14 | Cold/hot queue transitions, $K$-distance eviction order ($K=2, 3$), pin overrides, empty queues, 100k-op benchmark |
| `BufferPoolManagerTest` | 13 | Page table hits/misses, frame allocation, dirty-page writeback, eviction correctness, thread safety |
| `BufferPoolIntegrationTest` | 15 | Mixed database workloads, repeated access patterns, sequential scans, frame-to-page mappings |
| `RIDTest` | 9 | 64-bit int packing, equality, ordering, stream formatting, `std::hash` in `std::unordered_map` |
| `TablePageTest` | 41 | Dual-end growth, variable-length inserts, in-place updates, tombstones, defragmentation, `TablePageIterator`, insert-until-full, delete + reinsert, RID stability |
| `TableHeapTest` | 9 | Linked-page creation, multi-page spanning inserts, cross-page migration update (Delete Old + Insert New), `TableIterator` across pages, crash-restart persistence |
| `ExtendibleHashTableTest` | 10 | Directory page doubling/shrinking, split images, hash masking, bucket splits, key redistribution, persistence across buffer pool eviction |
| `EndToEndStorageIntegrationTest` | 4 | Real DiskManager + LRUK + BPM + TablePage + TableHeap cooperating across 50+ pages with only 4-5 frames, forced dirty-evictions, random updates, crash persistence |
| `BufferPoolManagerConcurrentTest` | 6 | Concurrent `FetchPage`/`UnpinPage` contention, 10-thread eviction storm, concurrent `NewPage` allocation uniqueness, reader-writer latching isolation, pin exhaustion recovery, mixed lifecycle stress |

---

## References & Theoretical Foundations

- [CMU 15-445/645 Database Systems](https://15445.courses.cs.cmu.edu/) — Storage engine & buffer pool architecture
- [The LRU-K Page Replacement Algorithm (O'Neil et al., 1993)](https://dl.acm.org/doi/10.1145/170036.170081) — Foundation of the replacer
- [PostgreSQL Storage Page Layout](https://www.postgresql.org/docs/current/storage-page-layout.html) — Slotted page format & line pointers
- [SQLite Database File Format](https://www.sqlite.org/fileformat.html) — Slotted cell pointers, freeblocks, and fragmentation

