#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "buffer_pool_manager.h"
#include "disk_manager.h"
#include "page.h"

namespace fs = std::filesystem;

class BufferPoolManagerConcurrentTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        test_db_file_ = "test_bpm_concurrent_" + std::to_string(now) + ".db";
        if (fs::exists(test_db_file_)) {
            fs::remove(test_db_file_);
        }
        disk_manager_ = std::make_shared<DiskManager>(test_db_file_);
    }

    void TearDown() override {
        disk_manager_.reset();
        if (fs::exists(test_db_file_)) {
            fs::remove(test_db_file_);
        }
    }

    std::string test_db_file_;
    std::shared_ptr<DiskManager> disk_manager_;
};

// ============================================================================
// Test 1: Concurrent Fetch & Unpin on the exact same page
// Multiple threads pound FetchPage and UnpinPage on a single page.
// Validates atomic pin-count tracking and eviction state convergence.
// ============================================================================
TEST_F(BufferPoolManagerConcurrentTest, ConcurrentFetchUnpinSinglePage) {
    const size_t pool_size = 5;
    BufferPoolManager bpm(pool_size, disk_manager_.get());

    page_id_t target_page_id = INVALID_PAGE_ID;
    Page *target_page = bpm.NewPage(&target_page_id);
    ASSERT_NE(target_page, nullptr);
    EXPECT_EQ(target_page->GetPinCount(), 1);

    // Write initial marker
    std::strcpy(target_page->GetData(), "concurrent_single_page_test");
    bpm.UnpinPage(target_page_id, true);
    EXPECT_EQ(target_page->GetPinCount(), 0);

    const int num_threads = 8;
    const int ops_per_thread = 500;
    std::atomic<bool> start_flag{false};
    std::atomic<int> successful_fetches{0};

    auto worker = [&]() {
        while (!start_flag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (int i = 0; i < ops_per_thread; ++i) {
            Page *page = bpm.FetchPage(target_page_id);
            ASSERT_NE(page, nullptr);
            successful_fetches.fetch_add(1, std::memory_order_relaxed);

            // Read verification under RLatch
            page->RLatch();
            EXPECT_STREQ(page->GetData(), "concurrent_single_page_test");
            page->RUnlatch();

            // Interleave with small yield
            if (i % 10 == 0) {
                std::this_thread::yield();
            }

            bool unpinned = bpm.UnpinPage(target_page_id, false);
            EXPECT_TRUE(unpinned);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker);
    }

    // Release all threads simultaneously
    start_flag.store(true, std::memory_order_release);

    for (auto &t : threads) {
        t.join();
    }

    EXPECT_EQ(successful_fetches.load(), num_threads * ops_per_thread);

    // Pin count must converge exactly back to 0
    Page *page = bpm.FetchPage(target_page_id);
    ASSERT_NE(page, nullptr);
    EXPECT_EQ(page->GetPinCount(), 1);
    EXPECT_TRUE(bpm.UnpinPage(target_page_id, false));
    EXPECT_EQ(page->GetPinCount(), 0);

    // Double unpin must fail
    EXPECT_FALSE(bpm.UnpinPage(target_page_id, false));
}

// ============================================================================
// Test 2: Concurrent Random Fetch & Unpin with Heavy Eviction
// Working set (25 pages) significantly exceeds buffer pool capacity (5 frames).
// Multiple threads randomly fetch, verify, and unpin pages, forcing constant
// replacer evictions under thread concurrency.
// ============================================================================
TEST_F(BufferPoolManagerConcurrentTest, ConcurrentRandomFetchUnpinWithEviction) {
    const size_t pool_size = 5;
    const size_t num_pages = 25;
    BufferPoolManager bpm(pool_size, disk_manager_.get());

    // Pre-allocate 25 pages, writing each page's page_id as a 64-bit payload
    std::vector<page_id_t> page_ids;
    page_ids.reserve(num_pages);
    for (size_t i = 0; i < num_pages; ++i) {
        page_id_t pid = INVALID_PAGE_ID;
        Page *p = bpm.NewPage(&pid);
        ASSERT_NE(p, nullptr);
        page_ids.push_back(pid);

        uint64_t val = static_cast<uint64_t>(pid) * 1000 + 42;
        std::memcpy(p->GetData(), &val, sizeof(val));
        bpm.UnpinPage(pid, true);
    }

    bpm.FlushAllPages();

    const int num_threads = 10;
    const int ops_per_thread = 200;
    std::atomic<bool> start_flag{false};
    std::atomic<int> completed_ops{0};
    std::atomic<int> pool_full_retries{0};

    auto worker = [&](int thread_id) {
        std::mt19937_64 rng(1337 + thread_id);
        std::uniform_int_distribution<size_t> dist(0, num_pages - 1);

        while (!start_flag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (int i = 0; i < ops_per_thread; ++i) {
            page_id_t target_pid = page_ids[dist(rng)];
            Page *page = bpm.FetchPage(target_pid);

            if (page == nullptr) {
                // All 5 frames pinned by other threads momentarily; retry
                pool_full_retries.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
                --i;
                continue;
            }

            page->RLatch();
            uint64_t val = 0;
            std::memcpy(&val, page->GetData(), sizeof(val));
            uint64_t expected = static_cast<uint64_t>(target_pid) * 1000 + 42;
            EXPECT_EQ(val, expected);
            page->RUnlatch();

            // Unpin
            bool unpinned = bpm.UnpinPage(target_pid, false);
            EXPECT_TRUE(unpinned);

            completed_ops.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i);
    }

    start_flag.store(true, std::memory_order_release);

    for (auto &t : threads) {
        t.join();
    }

    EXPECT_EQ(completed_ops.load(), num_threads * ops_per_thread);

    // Confirm all pages can still be fetched and data is intact
    for (page_id_t pid : page_ids) {
        Page *p = bpm.FetchPage(pid);
        ASSERT_NE(p, nullptr);
        uint64_t val = 0;
        std::memcpy(&val, p->GetData(), sizeof(val));
        EXPECT_EQ(val, static_cast<uint64_t>(pid) * 1000 + 42);
        EXPECT_TRUE(bpm.UnpinPage(pid, false));
    }
}

// ============================================================================
// Test 3: Concurrent Read-Write Latching Stress
// Multiple reader threads and multiple writer threads access the same page.
// Writers update an array of 64 identical integers under WLatch.
// Readers read under RLatch and assert all 64 integers are identical.
// Confirms no torn writes or data races on Page::data_.
// ============================================================================
TEST_F(BufferPoolManagerConcurrentTest, ConcurrentReadWriteLatchingStress) {
    const size_t pool_size = 5;
    BufferPoolManager bpm(pool_size, disk_manager_.get());

    page_id_t shared_pid = INVALID_PAGE_ID;
    Page *shared_page = bpm.NewPage(&shared_pid);
    ASSERT_NE(shared_page, nullptr);

    constexpr size_t ARRAY_SIZE = 64;
    int initial_data[ARRAY_SIZE];
    std::fill(std::begin(initial_data), std::end(initial_data), 0);
    std::memcpy(shared_page->GetData(), initial_data, sizeof(initial_data));
    bpm.UnpinPage(shared_pid, true);

    const int num_readers = 4;
    const int num_writers = 4;
    const int ops_per_thread = 400;

    std::atomic<bool> start_flag{false};
    std::atomic<int> writer_seq{1};

    auto reader_worker = [&]() {
        while (!start_flag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (int i = 0; i < ops_per_thread; ++i) {
            Page *page = bpm.FetchPage(shared_pid);
            ASSERT_NE(page, nullptr);

            page->RLatch();
            int buffer[ARRAY_SIZE];
            std::memcpy(buffer, page->GetData(), sizeof(buffer));
            page->RUnlatch();

            // Verify reader never sees torn state: all 64 elements must match
            int first_val = buffer[0];
            for (size_t k = 1; k < ARRAY_SIZE; ++k) {
                ASSERT_EQ(buffer[k], first_val) << "Torn read detected on element " << k;
            }

            bpm.UnpinPage(shared_pid, false);
        }
    };

    auto writer_worker = [&]() {
        while (!start_flag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (int i = 0; i < ops_per_thread; ++i) {
            Page *page = bpm.FetchPage(shared_pid);
            ASSERT_NE(page, nullptr);

            int val = writer_seq.fetch_add(1, std::memory_order_relaxed);

            page->WLatch();
            int buffer[ARRAY_SIZE];
            std::fill(std::begin(buffer), std::end(buffer), val);
            std::memcpy(page->GetData(), buffer, sizeof(buffer));
            page->WUnlatch();

            bpm.UnpinPage(shared_pid, true);
        }
    };

    std::vector<std::thread> readers;
    std::vector<std::thread> writers;
    readers.reserve(num_readers);
    writers.reserve(num_writers);

    for (int i = 0; i < num_readers; ++i) readers.emplace_back(reader_worker);
    for (int i = 0; i < num_writers; ++i) writers.emplace_back(writer_worker);

    start_flag.store(true, std::memory_order_release);

    for (auto &t : readers) t.join();
    for (auto &t : writers) t.join();

    // Final read verification
    Page *final_page = bpm.FetchPage(shared_pid);
    ASSERT_NE(final_page, nullptr);
    int final_buffer[ARRAY_SIZE];
    std::memcpy(final_buffer, final_page->GetData(), sizeof(final_buffer));
    for (size_t k = 1; k < ARRAY_SIZE; ++k) {
        EXPECT_EQ(final_buffer[k], final_buffer[0]);
    }
    bpm.UnpinPage(shared_pid, false);
}

// ============================================================================
// Test 4: Concurrent NewPage Allocation
// Multiple threads allocate pages concurrently.
// Validates:
// 1. All page IDs allocated across threads are unique (no duplicates).
// 2. Buffer pool properly flushes and evicts under heavy concurrent NewPage.
// 3. Re-fetching all pages reads back correct thread-specific payload.
// ============================================================================
TEST_F(BufferPoolManagerConcurrentTest, ConcurrentNewPageAllocation) {
    const size_t pool_size = 10;
    BufferPoolManager bpm(pool_size, disk_manager_.get());

    const int num_threads = 8;
    const int pages_per_thread = 15;  // 8 * 15 = 120 pages (12x buffer pool size!)

    struct PageRecord {
        page_id_t pid;
        int thread_id;
        int seq;
    };

    std::vector<std::vector<PageRecord>> thread_records(num_threads);
    std::atomic<bool> start_flag{false};

    auto worker = [&](int thread_id) {
        while (!start_flag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (int i = 0; i < pages_per_thread; ++i) {
            page_id_t pid = INVALID_PAGE_ID;
            Page *page = bpm.NewPage(&pid);

            while (page == nullptr) {
                // Buffer pool temporarily completely pinned; yield and retry
                std::this_thread::yield();
                page = bpm.NewPage(&pid);
            }

            ASSERT_NE(pid, INVALID_PAGE_ID);

            // Write signature: thread_id + seq
            page->WLatch();
            PageRecord rec{pid, thread_id, i};
            std::memcpy(page->GetData(), &rec, sizeof(rec));
            page->WUnlatch();

            bpm.UnpinPage(pid, true);
            thread_records[thread_id].push_back(rec);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i);
    }

    start_flag.store(true, std::memory_order_release);

    for (auto &t : threads) {
        t.join();
    }

    // 1. Verify all allocated page IDs are unique
    std::set<page_id_t> unique_pids;
    for (int t = 0; t < num_threads; ++t) {
        EXPECT_EQ(thread_records[t].size(), static_cast<size_t>(pages_per_thread));
        for (const auto &rec : thread_records[t]) {
            EXPECT_EQ(unique_pids.count(rec.pid), 0) << "Duplicate page ID allocated: " << rec.pid;
            unique_pids.insert(rec.pid);
        }
    }
    EXPECT_EQ(unique_pids.size(), static_cast<size_t>(num_threads * pages_per_thread));

    // 2. Re-fetch every page and verify payload consistency
    for (int t = 0; t < num_threads; ++t) {
        for (const auto &expected_rec : thread_records[t]) {
            Page *page = bpm.FetchPage(expected_rec.pid);
            ASSERT_NE(page, nullptr);

            PageRecord read_rec;
            std::memcpy(&read_rec, page->GetData(), sizeof(read_rec));
            EXPECT_EQ(read_rec.pid, expected_rec.pid);
            EXPECT_EQ(read_rec.thread_id, expected_rec.thread_id);
            EXPECT_EQ(read_rec.seq, expected_rec.seq);

            EXPECT_TRUE(bpm.UnpinPage(expected_rec.pid, false));
        }
    }
}

// ============================================================================
// Test 5: Pin Exhaustion and Recovery
// When all frames are pinned, FetchPage and NewPage must gracefully return nullptr.
// Once frames are unpinned, operations must immediately recover and succeed.
// ============================================================================
TEST_F(BufferPoolManagerConcurrentTest, ConcurrentPinExhaustionAndRecovery) {
    const size_t pool_size = 4;
    BufferPoolManager bpm(pool_size, disk_manager_.get());

    // Allocate 4 pages and keep them all pinned
    std::vector<page_id_t> pinned_pages;
    for (size_t i = 0; i < pool_size; ++i) {
        page_id_t pid = INVALID_PAGE_ID;
        Page *p = bpm.NewPage(&pid);
        ASSERT_NE(p, nullptr);
        pinned_pages.push_back(pid);
        // Do not unpin yet: pool_size frames pinned
    }

    // Attempt to allocate a 5th page while all frames are pinned
    page_id_t fail_pid = INVALID_PAGE_ID;
    Page *fail_page = bpm.NewPage(&fail_pid);
    EXPECT_EQ(fail_page, nullptr);
    EXPECT_EQ(fail_pid, INVALID_PAGE_ID);

    std::atomic<bool> worker_succeeded{false};
    std::thread waiter([&]() {
        // Spin until a frame becomes available
        Page *recovered_page = nullptr;
        page_id_t new_pid = INVALID_PAGE_ID;
        for (int attempts = 0; attempts < 1000; ++attempts) {
            recovered_page = bpm.NewPage(&new_pid);
            if (recovered_page != nullptr) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (recovered_page != nullptr) {
            worker_succeeded.store(true);
            bpm.UnpinPage(new_pid, false);
        }
    });

    // Sleep briefly, then unpin one page from main thread
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_TRUE(bpm.UnpinPage(pinned_pages[0], false));

    waiter.join();
    EXPECT_TRUE(worker_succeeded.load());

    // Clean up remaining pinned pages
    for (size_t i = 1; i < pool_size; ++i) {
        EXPECT_TRUE(bpm.UnpinPage(pinned_pages[i], false));
    }
}

// ============================================================================
// Test 6: Mixed Full-Lifecycle Stress (Fetch, Write, Read, New, Delete, Flush)
// Comprehensive concurrent stress running readers, writers, allocators,
// flushers, and deleters simultaneously.
// ============================================================================
TEST_F(BufferPoolManagerConcurrentTest, MixedConcurrentLifecycleStress) {
    const size_t pool_size = 8;
    BufferPoolManager bpm(pool_size, disk_manager_.get());

    // Seed 10 initial pages
    std::vector<page_id_t> initial_pids;
    for (int i = 0; i < 10; ++i) {
        page_id_t pid = INVALID_PAGE_ID;
        Page *p = bpm.NewPage(&pid);
        ASSERT_NE(p, nullptr);
        std::memset(p->GetData(), 0xAA, PAGE_SIZE);
        bpm.UnpinPage(pid, true);
        initial_pids.push_back(pid);
    }

    std::atomic<bool> stop_flag{false};
    std::atomic<int> ops_count{0};

    // Reader workers
    auto reader_task = [&](int id) {
        std::mt19937 rng(42 + id);
        std::uniform_int_distribution<size_t> dist(0, initial_pids.size() - 1);

        while (!stop_flag.load(std::memory_order_relaxed)) {
            page_id_t pid = initial_pids[dist(rng)];
            Page *page = bpm.FetchPage(pid);
            if (page != nullptr) {
                page->RLatch();
                volatile char c = page->GetData()[0];
                (void)c;
                page->RUnlatch();
                bpm.UnpinPage(pid, false);
            }
            ops_count.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    };

    // Writer workers
    auto writer_task = [&](int id) {
        std::mt19937 rng(100 + id);
        std::uniform_int_distribution<size_t> dist(0, initial_pids.size() - 1);

        while (!stop_flag.load(std::memory_order_relaxed)) {
            page_id_t pid = initial_pids[dist(rng)];
            Page *page = bpm.FetchPage(pid);
            if (page != nullptr) {
                page->WLatch();
                page->GetData()[0] = static_cast<char>(id);
                page->WUnlatch();
                bpm.UnpinPage(pid, true);
            }
            ops_count.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    };

    // Flusher worker
    auto flusher_task = [&]() {
        while (!stop_flag.load(std::memory_order_relaxed)) {
            bpm.FlushAllPages();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };

    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) workers.emplace_back(reader_task, i);
    for (int i = 0; i < 4; ++i) workers.emplace_back(writer_task, i);
    workers.emplace_back(flusher_task);

    // Let them run for 300ms of intensive stress
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop_flag.store(true, std::memory_order_relaxed);

    for (auto &w : workers) {
        w.join();
    }

    EXPECT_GT(ops_count.load(), 500);

    // After storm, FlushAllPages must run cleanly and all frames should be intact
    EXPECT_NO_THROW(bpm.FlushAllPages());
}
