# LRU-K Replacer — Design Deep Dive

## What Problem Does LRU-K Solve?

Plain LRU (k=1) evicts the page that was **least recently accessed**. This works
well for workloads with temporal locality — recently accessed pages are likely to
be accessed again soon.

It breaks down for **sequential scans**: scanning a large table accesses every
page exactly once. These single-use pages displace hot pages from the buffer pool.
This is called the **sequential scan problem** or **cache pollution**.

**LRU-K** (proposed by O'Neil, O'Neil, Weikum 1993) uses the **k-th most recent
access time** as the eviction key. A page needs `k` accesses before its backward
distance is even considered for the hot pool. Single-access pages are always
evicted first.

---

## The Two-Queue Design

This implementation uses two queues to naturally separate "cold" from "hot" pages:

```
cold_queue_  (frames with access_count < k)
─────────────────────────────────────────
  HEAD ──► [Frame A] ──► [Frame B] ──► [Frame C] ──► TAIL
            oldest                       newest
            ↑
         evicted first (FIFO)

hot_queue_  (frames with access_count >= k)
─────────────────────────────────────────
  HEAD ──► [Frame X] ──► [Frame Y] ──► [Frame Z] ──► TAIL
            smallest                    largest
            k-th access time            k-th access time
            ↑
         evicted first (oldest k-th access)
```

### Cold Queue — FIFO

Frames that have been accessed fewer than `k` times have **infinite backward
distance** under LRU-K theory. The correct policy for these frames is FIFO:
they're evicted in the order they first entered the pool.

Implementation: `std::list` with `push_back` (newest at tail) and `pop_front`
(oldest at head).

### Hot Queue — Sorted by k-th Access Time

Frames with `access_count >= k` have a well-defined backward distance: the time
elapsed since their k-th most recent access. The queue is kept sorted so the
frame with the **oldest k-th access** is always at the front.

Implementation: `std::list` with O(n) sorted insertion (`AddToQueue` scans
forward to find the right position). `Evict()` always pops from the front in O(1).

**Eviction priority**: cold queue is always drained first. Only if the cold queue
is empty does eviction fall back to the hot queue.

---

## Data Structures

```
frame_info_  (std::vector<FrameInfo>)
  ─────────────────────────────────────────────────────────────────
  Index:     0          1          2          3     ...  N-1
             ┌──────────┬──────────┬──────────┬──────────
             │FrameInfo │FrameInfo │FrameInfo │FrameInfo ...
             └──────────┴──────────┴──────────┴──────────
  
  FrameInfo fields:
    - frame_id       : size_t  (redundant with index; kept for clarity)
    - access_history : deque<size_t>  (sliding window of last k timestamps)
    - access_count   : size_t  (total accesses; determines cold vs hot)
    - is_pinned      : bool
    - is_evictable   : bool

cold_queue_ / hot_queue_  (std::list<int>)
  ─────────────────────────────────────────────────────────────────
  Doubly-linked list of frame IDs. std::list is chosen because:
  - O(1) insertion at any position (given an iterator)
  - O(1) removal at any position (given an iterator)
  - Iterators remain valid after insertions/removals elsewhere in the list

position_in_cold_ / position_in_hot_  (std::unordered_map<int, list<int>::iterator>)
  ─────────────────────────────────────────────────────────────────
  Maps frame_id → iterator into the corresponding queue.
  Enables O(1) removal from either queue by frame_id.
  Must be kept in sync with the lists at all times.
```

---

## Frame Lifecycle

```
                     RecordAccess()
 ┌──────────────┐   access_count < k     ┌──────────────────┐
 │  NEW / RESET │ ─────────────────────► │  COLD            │
 │              │                        │  (in cold_queue_)│
 └──────────────┘                        └──────┬───────────┘
       ▲                                        │
       │ Evict()                                │ RecordAccess()
       │ resets state                           │ access_count reaches k
       │                                        ▼
       │                                ┌──────────────────┐
       │    Evict() resets state        │  HOT             │
       └──────────────────────────────  │  (in hot_queue_) │
                                        └──────┬───────────┘
                                               │
                               Pin()           │ SetEvictable(false)
                               ┌───────────────┘
                               ▼
                        ┌──────────────────┐
                        │  PINNED          │
                        │  (not in any     │
                        │   queue)         │
                        └──────┬───────────┘
                               │
                  Unpin() or SetEvictable(true)
                               │
                               ▼
                    Back to COLD or HOT (re-enqueued)
```

---

## Critical Implementation Details

### The Iterator Correctness Bug (Fixed)

When inserting into the **middle** of `hot_queue_`, `std::list::insert()` returns
an iterator pointing to the newly inserted element. An early version of the code
did:

```cpp
// BUG: end()-1 points to the tail, not the newly inserted node
hot_queue_.insert(insert_pos, frame_id);
auto it = hot_queue_.end();
--it;
position_in_hot_[frame_id] = it;  // WRONG when insert_pos != end()
```

The fix captures the return value of `insert()`:

```cpp
// CORRECT: insert() returns an iterator to the new element
auto inserted_it = hot_queue_.insert(insert_pos, frame_id);
position_in_hot_[frame_id] = inserted_it;
```

The stale iterator caused `RemoveFromQueue` to erase the wrong node, leaving
the map with a dangling pointer. Under the stress test, this manifested as an
infinite loop or memory corruption.

### SetEvictable Is the Authoritative API

The Buffer Pool Manager drives evictability through `SetEvictable()`, not through
`Pin()`/`Unpin()` alone. The replacer's `Pin`/`Unpin` are lower-level primitives.

`SetEvictable(frame_id, true)` therefore clears `is_pinned` — it is an explicit
override that says "this frame is now eligible for eviction regardless of prior
pin state".

`SetEvictable(frame_id, false)` sets `is_pinned = true`, preventing eviction.

### Never-Accessed Frames

`Unpin()` and `SetEvictable(true)` check `access_count > 0` before enqueuing.
A frame that has never been accessed has no meaningful LRU-K history; it should
not be placed in the cold queue because:
- Its backward distance is undefined
- It would be evicted immediately, wasting a fetch that never happened

---

## Access History — The Sliding Window

```cpp
info.access_history.push_back(current_time_);
info.access_count++;

// Trim: keep only the last k timestamps
while (info.access_history.size() > k_) {
    info.access_history.pop_front();
}
```

`access_history` is a `std::deque<size_t>` acting as a fixed-size sliding window
of the last `k` access timestamps. `access_count` is a monotonically increasing
counter (never trimmed) used to determine cold vs. hot status.

The k-th access time used for hot queue ordering is `access_history.front()` — the
oldest timestamp still in the window, which is by definition the k-th most recent.

---

## Complexity Analysis

| Operation | Time | Notes |
|---|---|---|
| `RecordAccess` | O(1) amortized | O(n) in the worst case when repositioning in hot queue |
| `AddToQueue` — cold | O(1) | `push_back` + map insert |
| `AddToQueue` — hot | O(n/2) avg, O(n) worst | Linear scan to find sorted position |
| `RemoveFromQueue` | O(1) | Map lookup + `list::erase(iterator)` |
| `Evict` | O(1) | `pop_front` on the appropriate queue |
| `Pin` | O(1) | State update + `RemoveFromQueue` |
| `Unpin` | O(1) or O(n/2) | O(n/2) if re-inserting into hot queue |
| `SetEvictable` | O(1) or O(n/2) | Same as Unpin |
| `GetEvictableCount` | O(1) | `cold_queue_.size() + hot_queue_.size()` |
| `GetEvictionCount` | O(1) | Reads `eviction_count_` |

### Memory Layout

```
Per frame (approximate):
  frame_id          :  8 bytes
  access_history    :  sizeof(deque) + k * sizeof(size_t)
                      ≈ 40 + k * 8 bytes  (for k=2: ~56 bytes)
  access_count      :  8 bytes
  is_pinned         :  1 byte
  is_evictable      :  1 byte
  padding           :  ~6 bytes
  ─────────────────────────────
  Total per frame   :  ~70-80 bytes (for k=2)

  frame_info_ vector:  N * ~80 bytes
  cold/hot queue nodes: 32 bytes each (doubly-linked list node overhead)
  position maps:        ~48 bytes per entry (unordered_map overhead)

  For N=256 frames, k=2:
  ≈ 256 * 80  +  256 * 2 * 32  +  256 * 2 * 48
  ≈ 20 KB  +  16 KB  +  24 KB
  ≈ 60 KB total  (negligible vs. the 256 * 4KB = 1 MB buffer pool)
```

---

## Alternative Designs Considered

### Why not `std::priority_queue` for the hot queue?

A heap would give O(log n) sorted insertions but O(n) removal by arbitrary key.
Since we need O(1) removal by `frame_id` (on `Pin()`, `SetEvictable(false)`), the
sorted `std::list` + iterator map is a better tradeoff for buffer pool sizes.

For very large pools (> 10 000 frames), a **skip list** or an **ordered set** with
a custom comparator could achieve O(log n) insertion AND O(log n) deletion.

### Why not a single queue?

Merging cold and hot into a single queue sorted by backward distance requires
assigning cold frames (infinite backward distance) a sentinel large value. This
complicates insertion logic and makes the cold-first eviction priority implicit.
Two queues make the algorithm's intent explicit and auditable.

---

## References

- O'Neil, E. J., O'Neil, P. E., & Weikum, G. (1993). *The LRU-K page replacement algorithm for database disk buffering.* ACM SIGMOD Record.
- CMU 15-445 Lecture 6: Buffer Pools
- PostgreSQL clock-sweep buffer replacement: `backend/storage/buffer/freelist.c`
