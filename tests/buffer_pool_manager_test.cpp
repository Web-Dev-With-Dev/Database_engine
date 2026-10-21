#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "buffer_pool_manager.h"
#include "disk_manager.h"
#include "page.h"

namespace fs = std::filesystem;

class BufferPoolManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_db_file_ = "test_bpm_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".db";
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

TEST_F(BufferPoolManagerTest, ConstructorValidation) {
    EXPECT_THROW(BufferPoolManager(0, disk_manager_.get()), std::invalid_argument);
    DiskManager *null_dm = nullptr;
    EXPECT_THROW(BufferPoolManager(5, null_dm), std::invalid_argument);

    BufferPoolManager bpm(5, disk_manager_.get());
    EXPECT_EQ(bpm.GetPoolSize(), 5);
    EXPECT_EQ(bpm.GetFreeListSize(), 5);
}

TEST_F(BufferPoolManagerTest, NewPageAllocation) {
    const size_t pool_size = 3;
    BufferPoolManager bpm(pool_size, disk_manager_.get());

    page_id_t page_id0 = INVALID_PAGE_ID;
    Page *page0 = bpm.NewPage(&page_id0);
    ASSERT_NE(page0, nullptr);
    EXPECT_EQ(page_id0, 0);
    EXPECT_EQ(page0->GetPageId(), 0);
    EXPECT_EQ(page0->GetPinCount(), 1);
    EXPECT_EQ(bpm.GetFreeListSize(), 2);

    page_id_t page_id1 = INVALID_PAGE_ID;
    Page *page1 = bpm.NewPage(&page_id1);
    ASSERT_NE(page1, nullptr);
    EXPECT_EQ(page_id1, 1);
    EXPECT_EQ(bpm.GetFreeListSize(), 1);

    page_id_t page_id2 = INVALID_PAGE_ID;
    Page *page2 = bpm.NewPage(&page_id2);
    ASSERT_NE(page2, nullptr);
    EXPECT_EQ(page_id2, 2);
    EXPECT_EQ(bpm.GetFreeListSize(), 0);

    // Buffer pool is full and all pages are pinned: NewPage must fail
    page_id_t page_id3 = INVALID_PAGE_ID;
    Page *page3 = bpm.NewPage(&page_id3);
    EXPECT_EQ(page3, nullptr);
    EXPECT_EQ(page_id3, INVALID_PAGE_ID);
}

TEST_F(BufferPoolManagerTest, UnpinAndEviction) {
    const size_t pool_size = 2;
    BufferPoolManager bpm(pool_size, disk_manager_.get());

    page_id_t pid0;
    Page *p0 = bpm.NewPage(&pid0);
    ASSERT_NE(p0, nullptr);
    std::snprintf(p0->GetData(), PAGE_SIZE, "Hello Page 0");

    page_id_t pid1;
    Page *p1 = bpm.NewPage(&pid1);
    ASSERT_NE(p1, nullptr);
    std::snprintf(p1->GetData(), PAGE_SIZE, "Hello Page 1");

    // Unpin page 0 as dirty
    EXPECT_TRUE(bpm.UnpinPage(pid0, true));
    EXPECT_EQ(p0->GetPinCount(), 0);

    // Now page 0 is evictable. Creating page 2 should evict page 0
    page_id_t pid2;
    Page *p2 = bpm.NewPage(&pid2);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(pid2, 2);
    std::snprintf(p2->GetData(), PAGE_SIZE, "Hello Page 2");

    // Page 0 should no longer be in pool
    EXPECT_FALSE(bpm.IsPageInPool(pid0));
    EXPECT_TRUE(bpm.IsPageInPool(pid1));
    EXPECT_TRUE(bpm.IsPageInPool(pid2));

    // Unpin page 1 and page 2
    EXPECT_TRUE(bpm.UnpinPage(pid1, false));
    EXPECT_TRUE(bpm.UnpinPage(pid2, false));

    // Fetch page 0 back into pool: data should have been written to disk before eviction!
    Page *fetched_p0 = bpm.FetchPage(pid0);
    ASSERT_NE(fetched_p0, nullptr);
    EXPECT_STREQ(fetched_p0->GetData(), "Hello Page 0");
    EXPECT_EQ(fetched_p0->GetPinCount(), 1);

    EXPECT_TRUE(bpm.UnpinPage(pid0, false));
}

TEST_F(BufferPoolManagerTest, FetchPageHitAndMiss) {
    const size_t pool_size = 2;
    BufferPoolManager bpm(pool_size, disk_manager_.get());

    // Invalid page id returns nullptr
    EXPECT_EQ(bpm.FetchPage(INVALID_PAGE_ID), nullptr);

    page_id_t pid0;
    Page *p0 = bpm.NewPage(&pid0);
    ASSERT_NE(p0, nullptr);
    std::snprintf(p0->GetData(), PAGE_SIZE, "Cache Hit Test");
    EXPECT_EQ(p0->GetPinCount(), 1);

    // Fetch same page again (cache hit): pin count should increase to 2
    Page *p0_again = bpm.FetchPage(pid0);
    ASSERT_EQ(p0, p0_again);
    EXPECT_EQ(p0->GetPinCount(), 2);

    // Unpin once
    EXPECT_TRUE(bpm.UnpinPage(pid0, false));
    EXPECT_EQ(p0->GetPinCount(), 1);

    // Unpin second time
    EXPECT_TRUE(bpm.UnpinPage(pid0, false));
    EXPECT_EQ(p0->GetPinCount(), 0);

    // Unpinning beyond 0 returns false
    EXPECT_FALSE(bpm.UnpinPage(pid0, false));

    // When all frames are pinned and pool is full, FetchPage on a non-cached page returns nullptr
    page_id_t pid1;
    Page *p1 = bpm.NewPage(&pid1);
    ASSERT_NE(p1, nullptr);

    // Pin p0 again so both frames in pool are pinned
    Page *p0_ref = bpm.FetchPage(pid0);
    ASSERT_NE(p0_ref, nullptr);
    EXPECT_EQ(p0->GetPinCount(), 1);
    EXPECT_EQ(p1->GetPinCount(), 1);

    // Both frames are pinned: FetchPage on miss should fail
    EXPECT_EQ(bpm.FetchPage(999), nullptr);
}

TEST_F(BufferPoolManagerTest, FlushPageAndFlushAll) {
    const size_t pool_size = 2;
    BufferPoolManager bpm(pool_size, disk_manager_.get());

    page_id_t pid0;
    Page *p0 = bpm.NewPage(&pid0);
    std::snprintf(p0->GetData(), PAGE_SIZE, "Flushed Content");
    bpm.UnpinPage(pid0, true);

    EXPECT_TRUE(p0->IsDirty());
    EXPECT_TRUE(bpm.FlushPage(pid0));
    EXPECT_FALSE(p0->IsDirty());

    // Verify disk content directly
    char disk_buffer[PAGE_SIZE] = {};
    disk_manager_->ReadPage(pid0, disk_buffer);
    EXPECT_STREQ(disk_buffer, "Flushed Content");

    // Flushing invalid page or non-cached page returns false
    EXPECT_FALSE(bpm.FlushPage(INVALID_PAGE_ID));
    EXPECT_FALSE(bpm.FlushPage(9999));

    // Test FlushAllPages
    page_id_t pid1;
    Page *p1 = bpm.NewPage(&pid1);
    std::snprintf(p1->GetData(), PAGE_SIZE, "Page 1 Content");
    p0->SetDirty(true);
    p1->SetDirty(true);

    bpm.FlushAllPages();
    EXPECT_FALSE(p0->IsDirty());
    EXPECT_FALSE(p1->IsDirty());
}

TEST_F(BufferPoolManagerTest, DeletePage) {
    const size_t pool_size = 2;
    BufferPoolManager bpm(pool_size, disk_manager_.get());

    page_id_t pid0;
    Page *p0 = bpm.NewPage(&pid0);
    ASSERT_NE(p0, nullptr);

    // Pinned page cannot be deleted
    EXPECT_FALSE(bpm.DeletePage(pid0));
    EXPECT_TRUE(bpm.IsPageInPool(pid0));

    // Unpin page
    EXPECT_TRUE(bpm.UnpinPage(pid0, false));

    // Now delete should succeed
    EXPECT_TRUE(bpm.DeletePage(pid0));
    EXPECT_FALSE(bpm.IsPageInPool(pid0));
    EXPECT_EQ(bpm.GetFreeListSize(), 2);

    // Deleting a page not in the pool returns true
    EXPECT_TRUE(bpm.DeletePage(pid0));
    EXPECT_TRUE(bpm.DeletePage(1234));
}

TEST_F(BufferPoolManagerTest, ConcurrentAccess) {
    const size_t pool_size = 10;
    BufferPoolManager bpm(pool_size, disk_manager_.get());

    // Create 5 pages first
    std::vector<page_id_t> page_ids;
    for (int i = 0; i < 5; ++i) {
        page_id_t pid;
        Page *p = bpm.NewPage(&pid);
        ASSERT_NE(p, nullptr);
        page_ids.push_back(pid);
        bpm.UnpinPage(pid, false);
    }

    auto worker = [&](int thread_id) {
        for (int i = 0; i < 50; ++i) {
            page_id_t target_pid = page_ids[(thread_id + i) % page_ids.size()];
            Page *page = bpm.FetchPage(target_pid);
            if (page != nullptr) {
                // Read / write with latches
                page->WLatch();
                char *data = page->GetData();
                data[0] = static_cast<char>('A' + thread_id);
                page->WUnlatch();
                bpm.UnpinPage(target_pid, true);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back(worker, t);
    }

    for (auto &t : threads) {
        t.join();
    }

    bpm.FlushAllPages();
    for (page_id_t pid : page_ids) {
        Page *p = bpm.FetchPage(pid);
        ASSERT_NE(p, nullptr);
        EXPECT_FALSE(p->IsDirty());
        bpm.UnpinPage(pid, false);
    }
}

TEST_F(BufferPoolManagerTest, PinUnpinOrderingNoDeadlock) {
    const size_t pool_size = 4;
    BufferPoolManager bpm(pool_size, disk_manager_.get());

    page_id_t pid0, pid1, pid2, pid3;
    Page *p0 = bpm.NewPage(&pid0);
    Page *p1 = bpm.NewPage(&pid1);
    Page *p2 = bpm.NewPage(&pid2);
    Page *p3 = bpm.NewPage(&pid3);
    ASSERT_NE(p0, nullptr);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    ASSERT_NE(p3, nullptr);

    bpm.UnpinPage(pid0, false);
    bpm.UnpinPage(pid1, false);
    bpm.UnpinPage(pid2, false);
    bpm.UnpinPage(pid3, false);

    const int iterations = 300;

    // Thread 1: Pin A -> Pin B, Unpin A -> Unpin B
    auto t1_fn = [&]() {
        for (int i = 0; i < iterations; ++i) {
            Page *a = bpm.FetchPage(pid0);
            Page *b = bpm.FetchPage(pid1);
            ASSERT_NE(a, nullptr);
            ASSERT_NE(b, nullptr);
            bpm.UnpinPage(pid0, false);
            bpm.UnpinPage(pid1, false);
        }
    };

    // Thread 2: Pin B -> Pin A (reverse ordering), Unpin B -> Unpin A
    auto t2_fn = [&]() {
        for (int i = 0; i < iterations; ++i) {
            Page *b = bpm.FetchPage(pid1);
            Page *a = bpm.FetchPage(pid0);
            ASSERT_NE(b, nullptr);
            ASSERT_NE(a, nullptr);
            bpm.UnpinPage(pid1, false);
            bpm.UnpinPage(pid0, false);
        }
    };

    // Thread 3: Pin C -> Pin D, Unpin D -> Unpin C (LIFO unpin)
    auto t3_fn = [&]() {
        for (int i = 0; i < iterations; ++i) {
            Page *c = bpm.FetchPage(pid2);
            Page *d = bpm.FetchPage(pid3);
            ASSERT_NE(c, nullptr);
            ASSERT_NE(d, nullptr);
            bpm.UnpinPage(pid3, false);
            bpm.UnpinPage(pid2, false);
        }
    };

    // Thread 4: Pin D -> Pin A, Unpin A -> Unpin D (Cross-pair interleaved)
    auto t4_fn = [&]() {
        for (int i = 0; i < iterations; ++i) {
            Page *d = bpm.FetchPage(pid3);
            Page *a = bpm.FetchPage(pid0);
            ASSERT_NE(d, nullptr);
            ASSERT_NE(a, nullptr);
            bpm.UnpinPage(pid0, false);
            bpm.UnpinPage(pid3, false);
        }
    };

    std::thread th1(t1_fn);
    std::thread th2(t2_fn);
    std::thread th3(t3_fn);
    std::thread th4(t4_fn);

    th1.join();
    th2.join();
    th3.join();
    th4.join();

    EXPECT_EQ(p0->GetPinCount(), 0);
    EXPECT_EQ(p1->GetPinCount(), 0);
    EXPECT_EQ(p2->GetPinCount(), 0);
    EXPECT_EQ(p3->GetPinCount(), 0);
}

TEST_F(BufferPoolManagerTest, ConcurrentPinUnpinReplacerStress) {
    const size_t pool_size = 3;
    BufferPoolManager bpm(pool_size, disk_manager_.get());

    // Create 6 pages on disk (more than pool size, requiring continuous eviction)
    std::vector<page_id_t> page_ids;
    for (int i = 0; i < 6; ++i) {
        page_id_t pid;
        Page *p = bpm.NewPage(&pid);
        ASSERT_NE(p, nullptr);
        page_ids.push_back(pid);
        bpm.UnpinPage(pid, false);
    }

    const int iterations = 150;
    auto stress_worker = [&](int thread_id) {
        for (int i = 0; i < iterations; ++i) {
            page_id_t pid = page_ids[(thread_id + i) % page_ids.size()];
            Page *page = bpm.FetchPage(pid);
            if (page != nullptr) {
                page->WLatch();
                page->GetData()[0] = static_cast<char>('A' + (thread_id % 26));
                page->WUnlatch();
                bpm.UnpinPage(pid, true);
            }
        }
    };

    std::vector<std::thread> workers;
    for (int t = 0; t < 6; ++t) {
        workers.emplace_back(stress_worker, t);
    }

    for (auto &w : workers) {
        w.join();
    }

    // Flush and verify all pages are unpinned and readable
    bpm.FlushAllPages();
    for (page_id_t pid : page_ids) {
        Page *p = bpm.FetchPage(pid);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(p->GetPinCount(), 1);
        bpm.UnpinPage(pid, false);
        EXPECT_EQ(p->GetPinCount(), 0);
    }
}

TEST_F(BufferPoolManagerTest, EvictionCorrectnessLRUK) {
    const size_t pool_size = 3;
    const size_t k = 2;
    BufferPoolManager bpm(pool_size, disk_manager_.get(), k);

    // Create 3 pages to fill the buffer pool
    page_id_t pid0, pid1, pid2;
    bpm.NewPage(&pid0);  // Frame 0: accessed 1 time
    bpm.NewPage(&pid1);  // Frame 1: accessed 1 time
    bpm.NewPage(&pid2);  // Frame 2: accessed 1 time

    // Promote pid0 and pid1 to hot queue with a 2nd access (k=2)
    bpm.UnpinPage(pid0, false);
    bpm.FetchPage(pid0);  // Frame 0: 2 accesses (hot queue)
    bpm.UnpinPage(pid0, false);

    bpm.UnpinPage(pid1, false);
    bpm.FetchPage(pid1);  // Frame 1: 2 accesses (hot queue, more recent than frame 0)
    bpm.UnpinPage(pid1, false);

    // Page 2 has only 1 access (cold queue, backward distance = inf)
    bpm.UnpinPage(pid2, false);

    // Allocating a new page must evict Page 2 (from cold queue)
    page_id_t pid3;
    Page *p3 = bpm.NewPage(&pid3);
    ASSERT_NE(p3, nullptr);
    EXPECT_FALSE(bpm.IsPageInPool(pid2));
    EXPECT_TRUE(bpm.IsPageInPool(pid0));
    EXPECT_TRUE(bpm.IsPageInPool(pid1));
    EXPECT_TRUE(bpm.IsPageInPool(pid3));

    // Page 3 has 1 access (cold queue)
    bpm.UnpinPage(pid3, false);

    // Between page 3 (cold queue) and pages 0, 1 (hot queue):
    // Next allocation must evict Page 3
    page_id_t pid4;
    Page *p4 = bpm.NewPage(&pid4);
    ASSERT_NE(p4, nullptr);
    EXPECT_FALSE(bpm.IsPageInPool(pid3));
    EXPECT_TRUE(bpm.IsPageInPool(pid0));
    EXPECT_TRUE(bpm.IsPageInPool(pid1));
    EXPECT_TRUE(bpm.IsPageInPool(pid4));

    // Promote page 4 to hot queue with a 2nd access
    bpm.UnpinPage(pid4, false);
    bpm.FetchPage(pid4);
    bpm.UnpinPage(pid4, false);

    // All pages in pool (0, 1, 4) are now in hot queue.
    // Order of 2nd accesses: frame 0 (oldest), frame 1 (middle), frame 4 (newest).
    // Page 0 has the oldest k-th access time and must be evicted!
    page_id_t pid5;
    Page *p5 = bpm.NewPage(&pid5);
    ASSERT_NE(p5, nullptr);
    EXPECT_FALSE(bpm.IsPageInPool(pid0));
    EXPECT_TRUE(bpm.IsPageInPool(pid1));
    EXPECT_TRUE(bpm.IsPageInPool(pid4));
    EXPECT_TRUE(bpm.IsPageInPool(pid5));

    bpm.UnpinPage(pid1, false);
    bpm.UnpinPage(pid4, false);
    bpm.UnpinPage(pid5, false);
}

TEST_F(BufferPoolManagerTest, DirtyPageFlushOnEvict) {
    const size_t pool_size = 2;
    BufferPoolManager bpm(pool_size, disk_manager_.get());

    page_id_t pid0, pid1;
    Page *p0 = bpm.NewPage(&pid0);
    Page *p1 = bpm.NewPage(&pid1);
    ASSERT_NE(p0, nullptr);
    ASSERT_NE(p1, nullptr);

    // Write distinct data
    std::snprintf(p0->GetData(), PAGE_SIZE, "Persistent Dirty Data Pid 0");
    std::snprintf(p1->GetData(), PAGE_SIZE, "Clean Data Pid 1");

    // Unpin pid0 as dirty, pid1 as clean
    EXPECT_TRUE(bpm.UnpinPage(pid0, true));
    EXPECT_TRUE(bpm.UnpinPage(pid1, false));

    // Allocate page 2: forces eviction of pid0 (first in cold queue).
    // Because pid0 is dirty, its content MUST be written back to disk during eviction.
    page_id_t pid2;
    Page *p2 = bpm.NewPage(&pid2);
    ASSERT_NE(p2, nullptr);
    EXPECT_FALSE(bpm.IsPageInPool(pid0));

    // Verify disk content for pid0 directly using DiskManager
    char disk_check[PAGE_SIZE] = {};
    disk_manager_->ReadPage(pid0, disk_check);
    EXPECT_STREQ(disk_check, "Persistent Dirty Data Pid 0");

    // Allocate page 3: forces eviction of pid1 (clean page)
    page_id_t pid3;
    Page *p3 = bpm.NewPage(&pid3);
    ASSERT_NE(p3, nullptr);
    EXPECT_FALSE(bpm.IsPageInPool(pid1));

    bpm.UnpinPage(pid2, false);
    bpm.UnpinPage(pid3, false);

    // Fetch pid0 back into the pool from disk and verify its content matches exactly
    Page *fetched_p0 = bpm.FetchPage(pid0);
    ASSERT_NE(fetched_p0, nullptr);
    EXPECT_STREQ(fetched_p0->GetData(), "Persistent Dirty Data Pid 0");
    // Initially loaded page from disk must not be dirty
    EXPECT_FALSE(fetched_p0->IsDirty());

    bpm.UnpinPage(pid0, false);
}

TEST_F(BufferPoolManagerTest, PinCountTrackingComprehensive) {
    const size_t pool_size = 3;
    BufferPoolManager bpm(pool_size, disk_manager_.get());

    page_id_t pid0;
    Page *p0 = bpm.NewPage(&pid0);
    ASSERT_NE(p0, nullptr);
    EXPECT_EQ(p0->GetPinCount(), 1);

    // Multiple fetches on the same page increment pin count
    Page *fetch1 = bpm.FetchPage(pid0);
    EXPECT_EQ(fetch1, p0);
    EXPECT_EQ(p0->GetPinCount(), 2);

    Page *fetch2 = bpm.FetchPage(pid0);
    EXPECT_EQ(fetch2, p0);
    EXPECT_EQ(p0->GetPinCount(), 3);

    // Deleting page while pin_count > 0 must fail
    EXPECT_FALSE(bpm.DeletePage(pid0));
    EXPECT_TRUE(bpm.IsPageInPool(pid0));

    // Successive unpins decrement pin count accurately
    EXPECT_TRUE(bpm.UnpinPage(pid0, false));
    EXPECT_EQ(p0->GetPinCount(), 2);

    // Unpinning with dirty=true sets the dirty flag
    EXPECT_FALSE(p0->IsDirty());
    EXPECT_TRUE(bpm.UnpinPage(pid0, true));
    EXPECT_EQ(p0->GetPinCount(), 1);
    EXPECT_TRUE(p0->IsDirty());

    // Subsequent unpin with dirty=false must NOT clear earlier dirty state
    EXPECT_TRUE(bpm.UnpinPage(pid0, false));
    EXPECT_EQ(p0->GetPinCount(), 0);
    EXPECT_TRUE(p0->IsDirty());

    // Unpinning when pin_count is already 0 must return false and not underflow
    EXPECT_FALSE(bpm.UnpinPage(pid0, false));
    EXPECT_FALSE(bpm.UnpinPage(pid0, true));
    EXPECT_EQ(p0->GetPinCount(), 0);

    // Now that pin_count is 0, DeletePage must succeed
    EXPECT_TRUE(bpm.DeletePage(pid0));
    EXPECT_FALSE(bpm.IsPageInPool(pid0));
}

TEST_F(BufferPoolManagerTest, AllPagesPinnedEdgeCases) {
    const size_t pool_size = 2;
    BufferPoolManager bpm(pool_size, disk_manager_.get());

    page_id_t pid0, pid1;
    Page *p0 = bpm.NewPage(&pid0);
    Page *p1 = bpm.NewPage(&pid1);
    ASSERT_NE(p0, nullptr);
    ASSERT_NE(p1, nullptr);

    EXPECT_EQ(p0->GetPinCount(), 1);
    EXPECT_EQ(p1->GetPinCount(), 1);
    EXPECT_EQ(bpm.GetFreeListSize(), 0);

    // Edge Case 1: NewPage fails and sets out-parameter to INVALID_PAGE_ID
    page_id_t pid_fail = 999;
    Page *p_fail = bpm.NewPage(&pid_fail);
    EXPECT_EQ(p_fail, nullptr);
    EXPECT_EQ(pid_fail, INVALID_PAGE_ID);

    // Edge Case 2: FetchPage on un-cached page fails when all frames are pinned
    EXPECT_EQ(bpm.FetchPage(42), nullptr);

    // Edge Case 3: FetchPage on CACHED page succeeds and increments pin count
    Page *p0_hit = bpm.FetchPage(pid0);
    ASSERT_EQ(p0_hit, p0);
    EXPECT_EQ(p0->GetPinCount(), 2);

    // Edge Case 4: DeletePage fails on any pinned page
    EXPECT_FALSE(bpm.DeletePage(pid0));
    EXPECT_FALSE(bpm.DeletePage(pid1));
    EXPECT_TRUE(bpm.IsPageInPool(pid0));
    EXPECT_TRUE(bpm.IsPageInPool(pid1));

    // Edge Case 5: FlushPage still succeeds while pinned
    std::snprintf(p0->GetData(), PAGE_SIZE, "Flushed While Pinned");
    p0->SetDirty(true);
    EXPECT_TRUE(bpm.FlushPage(pid0));
    EXPECT_FALSE(p0->IsDirty());
    EXPECT_EQ(p0->GetPinCount(), 2);  // Pin count unchanged

    // Edge Case 6: Unpinning only ONE page allows eviction
    EXPECT_TRUE(bpm.UnpinPage(pid0, false));
    EXPECT_EQ(p0->GetPinCount(), 1);
    // Still pinned (count 1): NewPage still fails
    EXPECT_EQ(bpm.NewPage(nullptr), nullptr);

    // Unpin pid0 completely (count -> 0)
    EXPECT_TRUE(bpm.UnpinPage(pid0, false));
    EXPECT_EQ(p0->GetPinCount(), 0);

    // Now exactly pid0 is evictable; pid1 remains pinned!
    page_id_t pid2;
    Page *p2 = bpm.NewPage(&pid2);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(p2->GetPageId(), pid2);

    // pid0 was evicted, pid1 is still safely pinned in pool
    EXPECT_FALSE(bpm.IsPageInPool(pid0));
    EXPECT_TRUE(bpm.IsPageInPool(pid1));
    EXPECT_TRUE(bpm.IsPageInPool(pid2));
    EXPECT_EQ(p1->GetPinCount(), 1);
    EXPECT_EQ(p2->GetPinCount(), 1);

    // Clean up
    bpm.UnpinPage(pid1, false);
    bpm.UnpinPage(pid2, false);
}

// ============================================================================
// CMU 15-445 (BusTub) Project #1 Canonical Test Suite & Edge Cases
// ============================================================================

/**
 * @brief CMU 15-445 Project #1 Canonical Sample Test.
 * Allocates 10 pages, modifies page 0, unpins all.
 * Allocates page 10 (forces eviction of page 0 to disk).
 * Fetches page 0 back into pool and verifies content persisted.
 * Deletes page 0 and verifies free frame reclamation.
 */
TEST_F(BufferPoolManagerTest, CMU15445_SampleTest) {
    const size_t pool_size = 10;
    BufferPoolManager bpm(pool_size, disk_manager_.get());

    page_id_t page_id_temp;
    auto *page0 = bpm.NewPage(&page_id_temp);
    ASSERT_NE(page0, nullptr);
    EXPECT_EQ(0, page_id_temp);

    // Scenario: We should be able to create new pages until we fill up the buffer pool.
    std::vector<page_id_t> pages;
    pages.push_back(page_id_temp);
    std::snprintf(page0->GetData(), PAGE_SIZE, "Hello");

    for (size_t i = 1; i < pool_size; ++i) {
        auto *page = bpm.NewPage(&page_id_temp);
        ASSERT_NE(page, nullptr);
        pages.push_back(page_id_temp);
    }

    // Unpin all pages, marking page 0 dirty
    bpm.UnpinPage(pages[0], true);
    for (size_t i = 1; i < pool_size; ++i) {
        bpm.UnpinPage(pages[i], false);
    }

    // Allocate 10 more pages, forcing eviction of all previous 10 pages
    for (size_t i = 0; i < pool_size; ++i) {
        auto *page = bpm.NewPage(&page_id_temp);
        ASSERT_NE(page, nullptr);
        bpm.UnpinPage(page_id_temp, false);
    }

    // Page 0 should have been evicted to disk
    EXPECT_FALSE(bpm.IsPageInPool(0));

    // Fetch page 0 back from disk and verify its content was written back
    auto *fetched_page0 = bpm.FetchPage(0);
    ASSERT_NE(fetched_page0, nullptr);
    EXPECT_STREQ("Hello", fetched_page0->GetData());

    // Unpin fetched page 0
    EXPECT_TRUE(bpm.UnpinPage(0, false));

    // Delete page 0: must return true
    EXPECT_TRUE(bpm.DeletePage(0));
    EXPECT_FALSE(bpm.IsPageInPool(0));
}

/**
 * @brief CMU 15-445 Project #1 Canonical BinaryDataTest.
 * Tests full 4096-byte pages filled with arbitrary non-ASCII binary patterns
 * (including 0x00, 0xFF, random byte streams) across multiple eviction cycles.
 * Validates that binary data is neither truncated nor corrupted by disk transfers.
 */
TEST_F(BufferPoolManagerTest, CMU15445_BinaryDataFullPageTest) {
    const size_t pool_size = 5;
    BufferPoolManager bpm(pool_size, disk_manager_.get());

    const size_t num_pages = 10;
    std::vector<std::vector<char>> expected_data(num_pages, std::vector<char>(PAGE_SIZE));
    std::vector<page_id_t> page_ids(num_pages);

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> byte_dist(0, 255);

    // 1. Allocate pages and fill with randomized binary data
    for (size_t i = 0; i < num_pages; ++i) {
        Page *page = bpm.NewPage(&page_ids[i]);
        ASSERT_NE(page, nullptr);

        for (size_t b = 0; b < PAGE_SIZE; ++b) {
            expected_data[i][b] = static_cast<char>(byte_dist(rng));
        }
        // Ensure embedded null bytes exist to verify non-string binary safety
        expected_data[i][0] = '\0';
        expected_data[i][PAGE_SIZE / 2] = '\0';
        expected_data[i][PAGE_SIZE - 1] = static_cast<char>(0xFF);

        std::memcpy(page->GetData(), expected_data[i].data(), PAGE_SIZE);
        // Unpin page as dirty so it gets written to disk when evicted
        EXPECT_TRUE(bpm.UnpinPage(page_ids[i], true));
    }

    // 2. Fetch all pages back through multiple eviction rounds and verify byte exactness
    for (size_t i = 0; i < num_pages; ++i) {
        Page *page = bpm.FetchPage(page_ids[i]);
        ASSERT_NE(page, nullptr);
        EXPECT_EQ(0, std::memcmp(page->GetData(), expected_data[i].data(), PAGE_SIZE));
        EXPECT_TRUE(bpm.UnpinPage(page_ids[i], false));
    }
}

/**
 * @brief CMU 15-445 Edge Case: NewPage with nullptr as page_id out-parameter.
 * Validates that passing nullptr does not cause segfault or undefined behavior.
 */
TEST_F(BufferPoolManagerTest, CMU15445_NewPageNullptrSafety) {
    const size_t pool_size = 2;
    BufferPoolManager bpm(pool_size, disk_manager_.get());

    Page *p0 = bpm.NewPage(nullptr);
    ASSERT_NE(p0, nullptr);
    EXPECT_EQ(p0->GetPinCount(), 1);

    page_id_t pid0 = p0->GetPageId();
    EXPECT_GE(pid0, 0);

    EXPECT_TRUE(bpm.UnpinPage(pid0, false));
    EXPECT_TRUE(bpm.DeletePage(pid0));
}

/**
 * @brief CMU 15-445 Edge Case: Dirty Flag Stickiness Across Interleaved Unpins.
 * In CMU 15-445, once a page is marked dirty via UnpinPage(..., true), subsequent
 * unpins with UnpinPage(..., false) must NOT clear the dirty flag!
 */
TEST_F(BufferPoolManagerTest, CMU15445_UnpinDirtyStickyFlag) {
    const size_t pool_size = 2;
    BufferPoolManager bpm(pool_size, disk_manager_.get());

    page_id_t pid;
    Page *p = bpm.NewPage(&pid);
    ASSERT_NE(p, nullptr);

    // Pin count is 1. Increment to 2.
    Page *p_again = bpm.FetchPage(pid);
    ASSERT_EQ(p, p_again);
    EXPECT_EQ(2, p->GetPinCount());
    EXPECT_FALSE(p->IsDirty());

    // Worker 1 modifies data and unpins with is_dirty = true
    std::snprintf(p->GetData(), PAGE_SIZE, "Sticky Dirty Test");
    EXPECT_TRUE(bpm.UnpinPage(pid, true));
    EXPECT_EQ(1, p->GetPinCount());
    EXPECT_TRUE(p->IsDirty());

    // Worker 2 reads data and unpins with is_dirty = false
    EXPECT_TRUE(bpm.UnpinPage(pid, false));
    EXPECT_EQ(0, p->GetPinCount());
    // Dirty flag MUST remain true!
    EXPECT_TRUE(p->IsDirty());

    // Force eviction of pid by filling frame 1 with 2 accesses so pid (frame 0) has the older k-th access
    page_id_t pid1, pid2;
    bpm.NewPage(&pid1);
    bpm.FetchPage(pid1);
    bpm.UnpinPage(pid1, false);
    bpm.UnpinPage(pid1, false);

    // Now both frame 0 and frame 1 have 2 accesses.
    // Frame 0 has earlier backward k-distance timestamp, so pid2 allocation evicts frame 0 (pid).
    bpm.NewPage(&pid2);
    bpm.UnpinPage(pid2, false);

    EXPECT_FALSE(bpm.IsPageInPool(pid));

    // Verify disk content was written due to sticky dirty flag
    char disk_buf[PAGE_SIZE] = {};
    disk_manager_->ReadPage(pid, disk_buf);
    EXPECT_STREQ("Sticky Dirty Test", disk_buf);
}

/**
 * @brief CMU 15-445 Edge Case: DeletePage on dirty unpinned page discards it.
 * Deleting a dirty page while unpinned must immediately free the frame to the free list
 * without throwing, and subsequent FetchPage reads the disk image.
 */
TEST_F(BufferPoolManagerTest, CMU15445_DeletePageDiscardDirty) {
    const size_t pool_size = 2;
    BufferPoolManager bpm(pool_size, disk_manager_.get());

    page_id_t pid;
    Page *p = bpm.NewPage(&pid);
    ASSERT_NE(p, nullptr);
    std::snprintf(p->GetData(), PAGE_SIZE, "Never Written Content");

    bpm.UnpinPage(pid, true);
    EXPECT_TRUE(p->IsDirty());

    // Deleting dirty page should discard it and return the frame
    EXPECT_TRUE(bpm.DeletePage(pid));
    EXPECT_FALSE(bpm.IsPageInPool(pid));
    EXPECT_EQ(2, bpm.GetFreeListSize());
}

/**
 * @brief CMU 15-445 Edge Case: BufferPoolManager Destructor Automatically Flushes Dirty Pages.
 * Ensures that un-flushed dirty pages are safely written to disk when BPM is destroyed.
 */
TEST_F(BufferPoolManagerTest, CMU15445_DestructorFlushPersistence) {
    page_id_t pid0, pid1;

    // 1. First BPM instance writes dirty data and is destroyed without explicit FlushPage
    {
        BufferPoolManager bpm(5, disk_manager_.get());
        Page *p0 = bpm.NewPage(&pid0);
        Page *p1 = bpm.NewPage(&pid1);
        ASSERT_NE(p0, nullptr);
        ASSERT_NE(p1, nullptr);

        std::snprintf(p0->GetData(), PAGE_SIZE, "Auto-Flushed Page 0");
        std::snprintf(p1->GetData(), PAGE_SIZE, "Auto-Flushed Page 1");

        bpm.UnpinPage(pid0, true);
        bpm.UnpinPage(pid1, true);
        // bpm destructor executes here
    }

    // 2. Second BPM instance attaches to the same file and verifies data is intact
    {
        BufferPoolManager bpm2(5, disk_manager_.get());
        Page *p0 = bpm2.FetchPage(pid0);
        Page *p1 = bpm2.FetchPage(pid1);

        ASSERT_NE(p0, nullptr);
        ASSERT_NE(p1, nullptr);

        EXPECT_STREQ("Auto-Flushed Page 0", p0->GetData());
        EXPECT_STREQ("Auto-Flushed Page 1", p1->GetData());

        bpm2.UnpinPage(pid0, false);
        bpm2.UnpinPage(pid1, false);
    }
}

/**
 * @brief CMU 15-445 Edge Case: Multiple Pins Prevent Eviction until completely unpinned.
 * A page pinned 3 times must not be evicted after 1 or 2 unpins.
 */
TEST_F(BufferPoolManagerTest, CMU15445_MultiplePinsPreventEviction) {
    const size_t pool_size = 2;
    BufferPoolManager bpm(pool_size, disk_manager_.get());

    page_id_t pid0, pid1;
    Page *p0 = bpm.NewPage(&pid0);
    Page *p1 = bpm.NewPage(&pid1);
    ASSERT_NE(p0, nullptr);
    ASSERT_NE(p1, nullptr);

    // Pin p0 three times
    bpm.FetchPage(pid0);
    bpm.FetchPage(pid0);
    EXPECT_EQ(3, p0->GetPinCount());

    // Unpin p1 so p1 is evictable
    bpm.UnpinPage(pid1, false);
    EXPECT_EQ(0, p1->GetPinCount());

    // Allocating p2 evicts p1 (p0 cannot be evicted)
    page_id_t pid2;
    Page *p2 = bpm.NewPage(&pid2);
    ASSERT_NE(p2, nullptr);
    EXPECT_TRUE(bpm.IsPageInPool(pid0));
    EXPECT_FALSE(bpm.IsPageInPool(pid1));

    // Unpin p0 twice (pin count: 3 -> 2 -> 1)
    EXPECT_TRUE(bpm.UnpinPage(pid0, false));
    EXPECT_EQ(2, p0->GetPinCount());
    EXPECT_TRUE(bpm.UnpinPage(pid0, false));
    EXPECT_EQ(1, p0->GetPinCount());

    // Still pinned (count 1): NewPage must fail because p0 and p2 are both pinned!
    page_id_t pid3 = 888;
    Page *p3 = bpm.NewPage(&pid3);
    EXPECT_EQ(p3, nullptr);
    EXPECT_EQ(pid3, INVALID_PAGE_ID);

    // Unpin p0 completely (count -> 0)
    EXPECT_TRUE(bpm.UnpinPage(pid0, false));
    EXPECT_EQ(0, p0->GetPinCount());

    // Now p0 can be evicted!
    p3 = bpm.NewPage(&pid3);
    ASSERT_NE(p3, nullptr);
    EXPECT_FALSE(bpm.IsPageInPool(pid0));
    EXPECT_TRUE(bpm.IsPageInPool(pid2));
    EXPECT_TRUE(bpm.IsPageInPool(pid3));

    bpm.UnpinPage(pid2, false);
    bpm.UnpinPage(pid3, false);
}



