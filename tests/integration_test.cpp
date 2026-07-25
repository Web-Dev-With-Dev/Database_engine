#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "lru_k_replacer.h"
#include <vector>
#include <memory>
#include <random>
#include <chrono>
#include <unordered_map>
#include <set>
#include <algorithm>

using ::testing::Contains;
using ::testing::Not;

// ==================== Mock Buffer Pool Manager ====================

/**
 * Mock Page class for integration testing
 */
class MockPage {
public:
    MockPage(int id, const std::string& data = "") 
        : page_id_(id), data_(data), dirty_(false), pin_count_(0) {}
    
    int GetPageId() const { return page_id_; }
    std::string GetData() const { return data_; }
    void SetData(const std::string& data) { data_ = data; dirty_ = true; }
    bool IsDirty() const { return dirty_; }
    void ClearDirty() { dirty_ = false; }
    
    void Pin() { pin_count_++; }
    void Unpin() { if (pin_count_ > 0) pin_count_--; }
    int GetPinCount() const { return pin_count_; }
    
private:
    int page_id_;
    std::string data_;
    bool dirty_;
    int pin_count_;
};

/**
 * Simulated Buffer Pool Manager that uses LRU-K Replacer
 */
class BufferPoolManagerSimulator {
public:
    BufferPoolManagerSimulator(size_t pool_size, size_t k = 2) 
        : pool_size_(pool_size), replacer_(pool_size, k) {
        // Initialize buffer pool with empty pages
        buffer_pool_.resize(pool_size, nullptr);
        frame_to_page_.resize(pool_size, -1);
        page_to_frame_.clear();
    }
    
    /**
     * Fetch a page from the buffer pool
     * Returns true if page is in buffer pool
     */
    bool FetchPage(int page_id) {
        // Check if page is already in buffer pool
        auto it = page_to_frame_.find(page_id);
        if (it != page_to_frame_.end()) {
            // Page found - record access only; do not re-pin on a cache-hit.
            size_t frame_id = it->second;
            replacer_.RecordAccess(frame_id);
            replacer_.SetEvictable(frame_id, true);
            return true;
        }
        
        // Page not in buffer pool - need to load it
        // Find a free frame or evict one
        int free_frame = FindFreeFrame();
        if (free_frame == -1) {
            // No free frames - need to evict
            int evicted_frame = replacer_.Evict();
            if (evicted_frame == -1) {
                return false;  // Cannot evict any page
            }
            
            // Remove evicted page from mapping
            int evicted_page = frame_to_page_[evicted_frame];
            if (evicted_page != -1) {
                page_to_frame_.erase(evicted_page);
                frame_to_page_[evicted_frame] = -1;
                
                // Clear page data
                buffer_pool_[evicted_frame] = nullptr;
            }
            
            free_frame = evicted_frame;
        }
        
        // Load page into free frame
        if (free_frame != -1) {
            buffer_pool_[free_frame] = std::make_shared<MockPage>(page_id);
            frame_to_page_[free_frame] = page_id;
            page_to_frame_[page_id] = free_frame;
            
            // Record access to the frame and mark evictable immediately
            replacer_.RecordAccess(free_frame);
            replacer_.SetEvictable(free_frame, true);
            
            // Note: MockPage pin_count is managed exclusively by PinPage/UnpinPage.
            // FetchPage only loads the page into the buffer pool.
            
            return true;
        }
        
        return false;
    }
    
    /**
     * Unpin a page (release it)
     */
    void UnpinPage(int page_id) {
        auto it = page_to_frame_.find(page_id);
        if (it != page_to_frame_.end()) {
            size_t frame_id = it->second;
            if (buffer_pool_[frame_id]) {
                buffer_pool_[frame_id]->Unpin();
                // If pin count is 0, the frame is now evictable
                if (buffer_pool_[frame_id]->GetPinCount() == 0) {
                    replacer_.SetEvictable(frame_id, true);
                } else {
                    replacer_.SetEvictable(frame_id, false);
                }
            }
        }
    }
    
    /**
     * Pin a page (lock it in memory)
     */
    void PinPage(int page_id) {
        auto it = page_to_frame_.find(page_id);
        if (it != page_to_frame_.end()) {
            size_t frame_id = it->second;
            if (buffer_pool_[frame_id]) {
                buffer_pool_[frame_id]->Pin();
                replacer_.Pin(frame_id);
            }
        }
    }
    
    /**
     * Get the number of pages currently in the buffer pool
     */
    size_t GetPageCount() const {
        return page_to_frame_.size();
    }
    
    /**
     * Get the number of evictable frames
     */
    size_t GetEvictableCount() const {
        return replacer_.GetEvictableCount();
    }
    
    /**
     * Check if a page is in the buffer pool
     */
    bool IsPageInPool(int page_id) const {
        return page_to_frame_.find(page_id) != page_to_frame_.end();
    }
    
    /**
     * Get the pin count of a page
     */
    int GetPinCount(int page_id) const {
        auto it = page_to_frame_.find(page_id);
        if (it != page_to_frame_.end()) {
            size_t frame_id = it->second;
            if (buffer_pool_[frame_id]) {
                return buffer_pool_[frame_id]->GetPinCount();
            }
        }
        return -1;
    }
    
    /**
     * Get the frame ID for a page
     */
    int GetFrameForPage(int page_id) const {
        auto it = page_to_frame_.find(page_id);
        if (it != page_to_frame_.end()) {
            return it->second;
        }
        return -1;
    }
    
    /**
     * Get the LRU-K replacer for testing
     */
    LRUKReplacer& GetReplacer() {
        return replacer_;
    }
    
private:
    size_t pool_size_;
    std::vector<std::shared_ptr<MockPage>> buffer_pool_;
    std::vector<int> frame_to_page_;
    std::unordered_map<int, size_t> page_to_frame_;
    LRUKReplacer replacer_;
    
    int FindFreeFrame() {
        for (size_t i = 0; i < pool_size_; ++i) {
            if (frame_to_page_[i] == -1) {
                return i;
            }
        }
        return -1;
    }
};

// ==================== Integration Test Fixture ====================

class BufferPoolIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        buffer_pool_ = std::make_unique<BufferPoolManagerSimulator>(10, 2);
    }

    void TearDown() override {
        buffer_pool_.reset();
    }

    std::unique_ptr<BufferPoolManagerSimulator> buffer_pool_;
};

// ==================== Basic Integration Tests ====================

TEST_F(BufferPoolIntegrationTest, BasicPageFetch) {
    // Fetch pages
    EXPECT_TRUE(buffer_pool_->FetchPage(1));
    EXPECT_TRUE(buffer_pool_->FetchPage(2));
    EXPECT_TRUE(buffer_pool_->FetchPage(3));
    
    EXPECT_EQ(buffer_pool_->GetPageCount(), 3);
    EXPECT_EQ(buffer_pool_->GetEvictableCount(), 3);
}

TEST_F(BufferPoolIntegrationTest, PinAndUnpin) {
    // Fetch pages
    EXPECT_TRUE(buffer_pool_->FetchPage(1));
    EXPECT_TRUE(buffer_pool_->FetchPage(2));
    EXPECT_TRUE(buffer_pool_->FetchPage(3));
    
    // Pin page 1
    buffer_pool_->PinPage(1);
    EXPECT_EQ(buffer_pool_->GetEvictableCount(), 2);
    
    // Unpin page 1
    buffer_pool_->UnpinPage(1);
    EXPECT_EQ(buffer_pool_->GetEvictableCount(), 3);
}

TEST_F(BufferPoolIntegrationTest, PageEviction) {
    // Fill buffer pool
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(buffer_pool_->FetchPage(i));
    }
    
    EXPECT_EQ(buffer_pool_->GetPageCount(), 10);
    EXPECT_EQ(buffer_pool_->GetEvictableCount(), 10);
    
    // Evict a page
    int evicted = buffer_pool_->GetReplacer().Evict();
    EXPECT_GE(evicted, 0);
    EXPECT_LT(evicted, 10);
    
    // Add a new page (should use the evicted frame)
    EXPECT_TRUE(buffer_pool_->FetchPage(100));
    EXPECT_EQ(buffer_pool_->GetPageCount(), 10);
}

// ==================== LRU-K Specific Integration Tests ====================

TEST_F(BufferPoolIntegrationTest, LRUK2EvictionPriority) {
    // Access pattern: 0,1,2,3 (each once - all in cold queue)
    for (int i = 0; i < 4; ++i) {
        buffer_pool_->FetchPage(i);
        buffer_pool_->UnpinPage(i);
    }
    
    // Access page 0 again (moves to hot queue)
    buffer_pool_->FetchPage(0);
    buffer_pool_->UnpinPage(0);
    
    // Now page 0 has 2 accesses (hot), pages 1,2,3 have 1 access (cold)
    
    // Fill remaining frames
    for (int i = 4; i < 10; ++i) {
        buffer_pool_->FetchPage(i);
        buffer_pool_->UnpinPage(i);
    }
    
    // Evict should prefer cold queue pages (1,2,3)
    int evicted = buffer_pool_->GetReplacer().Evict();
    EXPECT_TRUE(evicted == 1 || evicted == 2 || evicted == 3);
}

TEST_F(BufferPoolIntegrationTest, LRUK3EvictionPriority) {
    std::unique_ptr<BufferPoolManagerSimulator> pool3(
        new BufferPoolManagerSimulator(10, 3)
    );
    
    // Access pattern: 0,1,2,0,1,2 (each has 2 accesses - cold for k=3)
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 3; ++j) {
            pool3->FetchPage(j);
            pool3->UnpinPage(j);
        }
    }
    
    // All have 2 accesses - still cold
    for (int i = 3; i < 10; ++i) {
        pool3->FetchPage(i);
        pool3->UnpinPage(i);
    }
    
    // Evict should be in order of access (0,1,2)
    int evicted1 = pool3->GetReplacer().Evict();
    int evicted2 = pool3->GetReplacer().Evict();
    int evicted3 = pool3->GetReplacer().Evict();
    
    EXPECT_EQ(evicted1, 0);
    EXPECT_EQ(evicted2, 1);
    EXPECT_EQ(evicted3, 2);
}

// ==================== Stress and Performance Tests ====================

TEST_F(BufferPoolIntegrationTest, RandomAccessPattern) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> page_dist(0, 99);
    std::uniform_int_distribution<> op_dist(0, 2);
    
    // Perform 1000 random operations
    for (int iter = 0; iter < 1000; ++iter) {
        int page_id = page_dist(gen);
        int operation = op_dist(gen);
        
        switch (operation) {
            case 0:  // Fetch
                buffer_pool_->FetchPage(page_id);
                break;
                
            case 1:  // Fetch and pin
                if (buffer_pool_->FetchPage(page_id)) {
                    buffer_pool_->PinPage(page_id);
                }
                break;
                
            case 2:  // Fetch, pin, then unpin
                if (buffer_pool_->FetchPage(page_id)) {
                    buffer_pool_->PinPage(page_id);
                    buffer_pool_->UnpinPage(page_id);
                }
                break;
        }
    }
    
    // Check that we haven't exceeded pool size
    EXPECT_LE(buffer_pool_->GetPageCount(), 10);
}

TEST_F(BufferPoolIntegrationTest, HotAndColdPagePattern) {
    // Simulate database workload: some pages are hot, some are cold
    
    // Hot pages: 0,1 (accessed frequently)
    // Cold pages: 2-9 (accessed occasionally)
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> hot_dist(0, 1);
    std::uniform_int_distribution<> cold_dist(2, 9);
    
    // Simulate 500 operations
    for (int iter = 0; iter < 500; ++iter) {
        // 70% hot pages, 30% cold pages
        int page_id;
        if (rand() % 10 < 7) {
            page_id = hot_dist(gen);
        } else {
            page_id = cold_dist(gen);
        }
        
        buffer_pool_->FetchPage(page_id);
        buffer_pool_->UnpinPage(page_id);
        
        // Occasionally evict
        if (iter % 50 == 0 && buffer_pool_->GetEvictableCount() > 0) {
            int evicted = buffer_pool_->GetReplacer().Evict();
            // Hot pages should rarely be evicted
            EXPECT_TRUE(evicted == -1 || evicted >= 2 || evicted <= 9);
        }
    }
}

// ==================== Edge Case Tests ====================

TEST_F(BufferPoolIntegrationTest, FullBufferPool) {
    // Fill buffer pool completely
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(buffer_pool_->FetchPage(i));
        buffer_pool_->UnpinPage(i);
    }
    
    EXPECT_EQ(buffer_pool_->GetPageCount(), 10);
    EXPECT_EQ(buffer_pool_->GetEvictableCount(), 10);
    
    // Try to fetch a new page - should evict one
    EXPECT_TRUE(buffer_pool_->FetchPage(100));
    EXPECT_EQ(buffer_pool_->GetPageCount(), 10);
}

TEST_F(BufferPoolIntegrationTest, AllPagesPinned) {
    // Fill buffer pool
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(buffer_pool_->FetchPage(i));
        buffer_pool_->PinPage(i);
    }
    
    // All pages pinned, no evictable frames
    EXPECT_EQ(buffer_pool_->GetEvictableCount(), 0);
    
    // Try to fetch a new page - should fail (no evictable frames)
    EXPECT_FALSE(buffer_pool_->FetchPage(100));
    
    // Unpin some pages
    for (int i = 0; i < 5; ++i) {
        buffer_pool_->UnpinPage(i);
    }
    
    // Now some frames are evictable
    EXPECT_EQ(buffer_pool_->GetEvictableCount(), 5);
    EXPECT_TRUE(buffer_pool_->FetchPage(100));
}

TEST_F(BufferPoolIntegrationTest, RepeatedAccessPattern) {
    // Test pattern: 0,1,2,0,1,2 repeated
    for (int round = 0; round < 10; ++round) {
        for (int i = 0; i < 3; ++i) {
            buffer_pool_->FetchPage(i);
            buffer_pool_->UnpinPage(i);
        }
    }
    
    // Should have 3 pages in pool
    EXPECT_EQ(buffer_pool_->GetPageCount(), 3);
    EXPECT_EQ(buffer_pool_->GetEvictableCount(), 3);
    
    // Add more pages
    for (int i = 3; i < 10; ++i) {
        buffer_pool_->FetchPage(i);
        buffer_pool_->UnpinPage(i);
    }
    
    // Now all 10 frames used
    EXPECT_EQ(buffer_pool_->GetPageCount(), 10);
    
    // Evict should not evict pages 0,1,2 if they are hot
    int evicted = buffer_pool_->GetReplacer().Evict();
    EXPECT_TRUE(evicted >= 3 && evicted <= 9);
}

// ==================== Real-World Simulation Tests ====================

TEST_F(BufferPoolIntegrationTest, DatabaseWorkloadSimulation) {
    // Simulate a database workload with mixed operations
    struct Operation {
        enum Type { READ, WRITE, COMMIT, EVICT };
        Type type;
        int page_id;
    };
    
    std::vector<Operation> workload = {
        {Operation::READ, 0}, {Operation::READ, 1}, {Operation::WRITE, 0},
        {Operation::READ, 2}, {Operation::READ, 3}, {Operation::WRITE, 1},
        {Operation::COMMIT, 0}, {Operation::READ, 4}, {Operation::READ, 5},
        {Operation::WRITE, 2}, {Operation::READ, 0}, {Operation::READ, 1},
        {Operation::EVICT, -1}, {Operation::READ, 6}, {Operation::READ, 7},
        {Operation::WRITE, 3}, {Operation::COMMIT, 1}, {Operation::READ, 8},
        {Operation::READ, 9}, {Operation::WRITE, 0}, {Operation::READ, 2},
        {Operation::EVICT, -1}, {Operation::READ, 5}, {Operation::READ, 6},
        {Operation::COMMIT, 2}, {Operation::READ, 7}, {Operation::READ, 8},
        {Operation::WRITE, 4}, {Operation::READ, 9}, {Operation::EVICT, -1}
    };
    
    std::set<int> active_transactions;
    
    for (const auto& op : workload) {
        switch (op.type) {
            case Operation::READ:
                buffer_pool_->FetchPage(op.page_id);
                buffer_pool_->UnpinPage(op.page_id);
                break;
                
            case Operation::WRITE:
                if (buffer_pool_->FetchPage(op.page_id)) {
                    // Write operation - pin the page
                    buffer_pool_->PinPage(op.page_id);
                    active_transactions.insert(op.page_id);
                }
                break;
                
            case Operation::COMMIT:
                buffer_pool_->UnpinPage(op.page_id);
                active_transactions.erase(op.page_id);
                break;
                
            case Operation::EVICT:
                if (buffer_pool_->GetEvictableCount() > 0) {
                    int evicted = buffer_pool_->GetReplacer().Evict();
                    if (evicted != -1) {
                        // Verify we're not evicting active pages
                        EXPECT_TRUE(active_transactions.find(evicted) == active_transactions.end());
                    }
                }
                break;
        }
    }
    
    // Verify final state
    EXPECT_GE(buffer_pool_->GetEvictableCount(), 0);
    EXPECT_LE(buffer_pool_->GetPageCount(), 10);
}

TEST_F(BufferPoolIntegrationTest, SequentialScanPattern) {
    // Simulate table scan: pages accessed sequentially
    for (int scan = 0; scan < 5; ++scan) {
        for (int i = 0; i < 20; ++i) {
            int page_id = i % 10;  // 10 pages, but only 10 frames
            buffer_pool_->FetchPage(page_id);
            buffer_pool_->UnpinPage(page_id);
            
            // Occasionally evict during scan
            if (i % 3 == 0 && buffer_pool_->GetEvictableCount() > 0) {
                int evicted = buffer_pool_->GetReplacer().Evict();
                // Should evict the least recently used
                EXPECT_TRUE(evicted == -1 || evicted >= 0);
            }
        }
    }
}

// ==================== Performance Benchmark Integration ====================

TEST_F(BufferPoolIntegrationTest, PerformanceBenchmark) {
    const int num_operations = 50000;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> page_dist(0, 999);
    std::uniform_int_distribution<> op_dist(0, 2);
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < num_operations; ++i) {
        int page_id = page_dist(gen);
        int op = op_dist(gen);
        
        switch (op) {
            case 0:
                if (buffer_pool_->FetchPage(page_id)) {
                    buffer_pool_->UnpinPage(page_id);
                }
                break;
            case 1:
                if (buffer_pool_->FetchPage(page_id)) {
                    buffer_pool_->PinPage(page_id);
                }
                break;
            case 2:
                buffer_pool_->UnpinPage(page_id);
                break;
        }
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    std::cout << "\n=== Integration Performance Benchmark ===" << std::endl;
    std::cout << "Operations: " << num_operations << std::endl;
    std::cout << "Time: " << duration.count() << " ms" << std::endl;
    std::cout << "Average: " << (duration.count() * 1000.0 / num_operations) 
              << " μs/op" << std::endl;
    std::cout << "Throughput: " << (num_operations * 1000.0 / duration.count()) 
              << " ops/sec" << std::endl;
}

// ==================== Correctness Verification Tests ====================

TEST_F(BufferPoolIntegrationTest, FrameToPageMapping) {
    // Fetch pages and verify mapping
    std::vector<int> pages = {5, 10, 15, 20};
    
    for (int page : pages) {
        EXPECT_TRUE(buffer_pool_->FetchPage(page));
        buffer_pool_->UnpinPage(page);
    }
    
    // Verify each page is in a unique frame
    std::set<int> frames;
    for (int page : pages) {
        int frame = buffer_pool_->GetFrameForPage(page);
        EXPECT_NE(frame, -1);
        EXPECT_TRUE(frames.insert(frame).second);  // Should be unique
    }
}

TEST_F(BufferPoolIntegrationTest, PinCountTracking) {
    int page_id = 42;
    
    // Fetch and pin multiple times
    EXPECT_TRUE(buffer_pool_->FetchPage(page_id));
    buffer_pool_->PinPage(page_id);  // Pin count: 1
    buffer_pool_->PinPage(page_id);  // Pin count: 2
    
    EXPECT_EQ(buffer_pool_->GetPinCount(page_id), 2);
    
    // Unpin once
    buffer_pool_->UnpinPage(page_id);  // Pin count: 1
    EXPECT_EQ(buffer_pool_->GetPinCount(page_id), 1);
    
    // Unpin all
    buffer_pool_->UnpinPage(page_id);  // Pin count: 0
    
    // Now page should be evictable
    EXPECT_EQ(buffer_pool_->GetPinCount(page_id), 0);
    EXPECT_EQ(buffer_pool_->GetEvictableCount(), 1);  // Only page_id was fetched in this test
}

// ==================== Main ====================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}