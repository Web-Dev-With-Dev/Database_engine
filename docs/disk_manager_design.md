# Disk Manager — Design Deep Dive

## Overview

The `DiskManager` is the lowest layer of the storage engine. Its entire job is to
translate logical **page IDs** into byte offsets in a binary file and perform
fixed-size reads and writes at those offsets.

Everything above the DiskManager (Page, Buffer Pool, Query Engine) thinks in terms
of page IDs and never touches file offsets or stream pointers directly.

---

## File Format

```
database.db
═══════════

 Byte 0                                      Byte 4096
 ┌─────────────────────────────────────────┐
 │                  Page 0                 │  4096 bytes
 └─────────────────────────────────────────┘
 Byte 4096                                    Byte 8192
 ┌─────────────────────────────────────────┐
 │                  Page 1                 │  4096 bytes
 └─────────────────────────────────────────┘
 ...
 Byte N * 4096                                Byte (N+1) * 4096
 ┌─────────────────────────────────────────┐
 │                  Page N                 │  4096 bytes
 └─────────────────────────────────────────┘
```

**Invariant**: file size is always a multiple of `PAGE_SIZE`. The constructor
validates this and throws `std::runtime_error` if violated — detecting truncated
or externally modified files before any I/O takes place.

There is **no header page**. Page 0 is a regular data page. A higher-level
catalog / header page structure will be managed by a future layer (e.g. the
catalog manager).

---

## Page ID ↔ Offset Mapping

```
offset = static_cast<size_t>(page_id) * PAGE_SIZE
```

Simple direct mapping — no indirection. This means:
- O(1) seek to any page
- No fragmentation within the file (pages are always `PAGE_SIZE` bytes)
- Page IDs are stable for the lifetime of the database file

---

## AllocatePage

```cpp
page_id_t DiskManager::AllocatePage() {
    return next_page_id_++;
}
```

Returns a monotonically increasing ID. Critically, `AllocatePage` does **not**
physically write anything. The page is not on disk until `WritePage` is called.
This mirrors how real DBMSes work: page allocation is a logical operation; the
physical write happens at first use.

`next_page_id_` is seeded from the current file size in the constructor:

```cpp
next_page_id_ = static_cast<page_id_t>(size / PAGE_SIZE);
```

This means after a crash + restart, the engine resumes allocating from where it
left off — no gaps or duplicate IDs.

---

## WritePage — Sparse Write Handling

Writing to page N when the file only has M < N pages requires filling the gap.
We zero-fill each intermediate page:

```cpp
while (static_cast<size_t>(file_size) < offset) {
    db_file_.write(zero_page, PAGE_SIZE);
    file_size += PAGE_SIZE;
}
db_file_.seekp(offset);
db_file_.write(data, PAGE_SIZE);
db_file_.flush();
```

**Why zero-fill instead of using OS sparse files?**
- Portability: sparse files (POSIX `fallocate`, Windows `SetFileValidData`) are
  OS-specific; zero-fill works everywhere
- Correctness: a ReadPage on an intermediate page returns deterministic zeros
- Alignment: the `file_size % PAGE_SIZE == 0` invariant is maintained

**Why flush after every write?**
At this layer, durability is unconditional. The Buffer Pool Manager (future) can
choose to batch writes if performance demands it — but the DiskManager itself
must be correct in isolation. This is the same design choice PostgreSQL's
`smgrwrite` makes for WAL pages.

---

## ReadPage — Partial Read Handling

```cpp
db_file_.read(buffer, PAGE_SIZE);
std::streamsize bytes_read = db_file_.gcount();
if (bytes_read < static_cast<std::streamsize>(PAGE_SIZE)) {
    std::memset(buffer + bytes_read, 0, PAGE_SIZE - bytes_read);
    db_file_.clear();  // Reset eofbit so further operations work
}
```

Reading a page ID beyond the current end-of-file is valid — it returns a zeroed
page. This handles the case where `AllocatePage()` was called but `WritePage`
hasn't been issued yet. The `db_file_.clear()` call is important: without it, the
`eofbit` would persist and make subsequent reads fail.

---

## Thread Safety

`file_mutex_` is a **per-instance** `std::mutex`. It serialises concurrent calls
to `WritePage` / `ReadPage` on the **same** `DiskManager` instance — protecting
the `std::fstream` seek+read/write sequence from interleaving.

It does **not** synchronise between different `DiskManager` instances pointing to
the same file. That is intentional: the `ConcurrentWriteRead` test uses separate
instances, each writing to its own dedicated page — page-level isolation makes
cross-instance coordination unnecessary.

For production use, file-level locking (e.g. `flock` / `LockFileEx`) would be
needed when multiple processes access the same database file. That belongs in a
future `StorageBackend` abstraction.

---

## Error Handling Strategy

Every I/O operation that can fail throws `std::runtime_error`. Callers are
expected to propagate these exceptions upward. The design choice is:
- **No error codes** — avoids checked-return anti-patterns at every call site
- **No silent failure** — a disk error is always fatal at this layer
- **Fail-fast in constructor** — corrupt files are rejected at open time, not
  discovered mid-query

---

## What the DiskManager Deliberately Does NOT Do

| Feature | Where it belongs |
|---|---|
| Free page tracking / reuse | Free-list manager (future) |
| Page checksums | Page header format (future) |
| Write-ahead logging | WAL manager (future) |
| Buffer caching | Buffer Pool Manager (Day 8) |
| Index structures | B-Tree / index layer (future) |
