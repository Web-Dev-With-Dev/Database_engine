#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
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
