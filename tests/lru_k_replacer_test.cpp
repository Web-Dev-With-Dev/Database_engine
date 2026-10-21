#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "lru_k_replacer.h"
#include <random>
#include <chrono>
#include <set>

using ::testing::Contains;
using ::testing::Not;

class LRUKReplacerTest : public ::testing::Test {
protected:
    void SetUp() override {
        replacer_ = std::make_unique<LRUKReplacer>(10, 2);
    }

    void TearDown() override {
        replacer_.reset();
    }

    std::unique_ptr<LRUKReplacer> replacer_;
};

// ==================== Basic Functionality Tests ====================

TEST_F(LRUKReplacerTest, Constructor) {
    EXPECT_EQ(replacer_->GetTotalFrames(), 10);
    EXPECT_EQ(replacer_->GetEvictableCount(), 0);
    
    EXPECT_THROW(LRUKReplacer(0, 2), std::invalid_argument);
    EXPECT_THROW(LRUKReplacer(10, 0), std::invalid_argument);
}

TEST_F(LRUKReplacerTest, RecordAccessAndEvict) {
    EXPECT_EQ(replacer_->GetEvictableCount(), 0);
    
    replacer_->RecordAccess(0);
    replacer_->SetEvictable(0, true);
    EXPECT_EQ(replacer_->GetEvictableCount(), 1);
    EXPECT_EQ(replacer_->GetAccessCount(0), 1);
    EXPECT_TRUE(replacer_->IsEvictable(0));
    EXPECT_FALSE(replacer_->IsPinned(0));
    
    int evicted = replacer_->Evict();
    EXPECT_EQ(evicted, 0);
    EXPECT_EQ(replacer_->GetEvictableCount(), 0);
    EXPECT_EQ(replacer_->GetAccessCount(0), 0);
    EXPECT_FALSE(replacer_->IsEvictable(0));
}

TEST_F(LRUKReplacerTest, MultipleRecords) {
    for (int i = 0; i < 5; ++i) {
        replacer_->RecordAccess(0);
    }
    replacer_->SetEvictable(0, true);
    
    EXPECT_EQ(replacer_->GetAccessCount(0), 5);
    EXPECT_EQ(replacer_->GetEvictableCount(), 1);
    
    int evicted = replacer_->Evict();
    EXPECT_EQ(evicted, 0);
}

TEST_F(LRUKReplacerTest, Reset) {
    for (int i = 0; i < 5; ++i) {
        replacer_->RecordAccess(i);
        replacer_->SetEvictable(i, true);
    }
    
    EXPECT_EQ(replacer_->GetEvictableCount(), 5);
    replacer_->Reset();
    EXPECT_EQ(replacer_->GetEvictableCount(), 0);
    EXPECT_EQ(replacer_->GetAccessCount(0), 0);
}

// ==================== LRU-K Specific Tests ====================

TEST_F(LRUKReplacerTest, LRUK2EvictionOrder) {
    for (int i = 0; i < 3; ++i) {
        replacer_->RecordAccess(i);
        replacer_->SetEvictable(i, true);
    }
    EXPECT_EQ(replacer_->GetEvictableCount(), 3);
    
    EXPECT_EQ(replacer_->Evict(), 0);
    EXPECT_EQ(replacer_->GetEvictableCount(), 2);
    
    replacer_->RecordAccess(0);
    replacer_->SetEvictable(0, true);
    EXPECT_EQ(replacer_->GetEvictableCount(), 3);
    
    EXPECT_EQ(replacer_->Evict(), 1);
    EXPECT_EQ(replacer_->Evict(), 2);
    EXPECT_EQ(replacer_->GetEvictableCount(), 1);
    EXPECT_EQ(replacer_->Evict(), 0);
}

TEST_F(LRUKReplacerTest, LRUK3EvictionOrder) {
    std::unique_ptr<LRUKReplacer> replacer3(new LRUKReplacer(10, 3));
    
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 3; ++j) {
            replacer3->RecordAccess(j);
        }
    }
    
    for (int i = 0; i < 3; ++i) {
        replacer3->SetEvictable(i, true);
    }
    
    EXPECT_EQ(replacer3->Evict(), 0);
    EXPECT_EQ(replacer3->Evict(), 1);
    EXPECT_EQ(replacer3->Evict(), 2);
}

// ==================== Pin/Unpin Tests ====================

TEST_F(LRUKReplacerTest, PinAndUnpin) {
    replacer_->RecordAccess(0);
    replacer_->RecordAccess(1);
    replacer_->SetEvictable(0, true);
    replacer_->SetEvictable(1, true);
    EXPECT_EQ(replacer_->GetEvictableCount(), 2);
    
    replacer_->Pin(0);
    EXPECT_EQ(replacer_->GetEvictableCount(), 1);
    EXPECT_FALSE(replacer_->IsEvictable(0));
    EXPECT_TRUE(replacer_->IsPinned(0));
    
    EXPECT_EQ(replacer_->Evict(), 1);
    EXPECT_EQ(replacer_->GetEvictableCount(), 0);
    
    replacer_->Unpin(0);
    EXPECT_EQ(replacer_->GetEvictableCount(), 1);
    EXPECT_TRUE(replacer_->IsEvictable(0));
    EXPECT_FALSE(replacer_->IsPinned(0));
    EXPECT_EQ(replacer_->Evict(), 0);
}

TEST_F(LRUKReplacerTest, PinEvictableFrame) {
    replacer_->RecordAccess(0);
    replacer_->SetEvictable(0, true);
    EXPECT_TRUE(replacer_->IsEvictable(0));
    EXPECT_EQ(replacer_->GetEvictableCount(), 1);
    
    replacer_->Pin(0);
    EXPECT_FALSE(replacer_->IsEvictable(0));
    EXPECT_EQ(replacer_->GetEvictableCount(), 0);
    EXPECT_EQ(replacer_->Evict(), -1);
    
    replacer_->Unpin(0);
    EXPECT_TRUE(replacer_->IsEvictable(0));
    EXPECT_EQ(replacer_->GetEvictableCount(), 1);
}

// ==================== Edge Cases Tests ====================

TEST_F(LRUKReplacerTest, NoEvictableFrames) {
    replacer_->RecordAccess(0);
    replacer_->SetEvictable(0, false);
    EXPECT_EQ(replacer_->GetEvictableCount(), 0);
    EXPECT_EQ(replacer_->Evict(), -1);
}

TEST_F(LRUKReplacerTest, AllPinned) {
    for (int i = 0; i < 5; ++i) {
        replacer_->RecordAccess(i);
        replacer_->SetEvictable(i, true);
    }
    EXPECT_EQ(replacer_->GetEvictableCount(), 5);
    
    for (int i = 0; i < 5; ++i) {
        replacer_->Pin(i);
    }
    EXPECT_EQ(replacer_->GetEvictableCount(), 0);
    EXPECT_EQ(replacer_->Evict(), -1);
}

TEST_F(LRUKReplacerTest, EvictAllFrames) {
    const int num_frames = 10;
    for (int i = 0; i < num_frames; ++i) {
        replacer_->RecordAccess(i);
        replacer_->SetEvictable(i, true);
    }
    
    std::set<int> evicted_frames;
    for (int i = 0; i < num_frames; ++i) {
        int evicted = replacer_->Evict();
        EXPECT_GE(evicted, 0);
        EXPECT_LT(evicted, num_frames);
        EXPECT_TRUE(evicted_frames.find(evicted) == evicted_frames.end());
        evicted_frames.insert(evicted);
    }
    
    EXPECT_EQ(evicted_frames.size(), num_frames);
    EXPECT_EQ(replacer_->GetEvictableCount(), 0);
}

TEST_F(LRUKReplacerTest, OutOfRangeAccess) {
    EXPECT_THROW(replacer_->RecordAccess(10), std::out_of_range);
    EXPECT_THROW(replacer_->Pin(10), std::out_of_range);
    EXPECT_THROW(replacer_->Unpin(10), std::out_of_range);
    EXPECT_THROW(replacer_->SetEvictable(10, true), std::out_of_range);
}

// ==================== Stress Tests ====================

TEST_F(LRUKReplacerTest, StressTestRandom) {
    const int num_frames = 20;
    std::unique_ptr<LRUKReplacer> replacer3(new LRUKReplacer(num_frames, 3));
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> frame_dist(0, num_frames - 1);
    std::uniform_int_distribution<> op_dist(0, 3);
    
    for (int iter = 0; iter < 1000; ++iter) {
        int frame = frame_dist(gen);
        int operation = op_dist(gen);
        
        switch (operation) {
            case 0: {
                replacer3->RecordAccess(frame);
                break;
            }
            case 1: {
                if (replacer3->GetEvictableCount() > 0) {
                    int evicted = replacer3->Evict();
                    EXPECT_GE(evicted, 0);
                    EXPECT_LT(evicted, num_frames);
                }
                break;
            }
            case 2: {
                replacer3->Pin(frame);
                break;
            }
            case 3: {
                replacer3->Unpin(frame);
                break;
            }
        }
        
        size_t evictable = replacer3->GetEvictableCount();
        EXPECT_LE(evictable, num_frames);
    }
}

// ==================== CMU 15-445 Project #1 Edge Case Tests ====================

/**
 * @brief Canonical CMU 15-445 BusTub Project #1 LRU-K Replacer Sample Test.
 * Tests frames [1, 2, 3, 4, 5] with k=2.
 * Frame 4 has +inf backward distance and is evicted first.
 * Then frames 2, 3, 1 are evicted according to their 2nd-to-last access times.
 */
TEST_F(LRUKReplacerTest, CMU15445_SampleTest) {
    LRUKReplacer lru_replacer(7, 2);

    // Scenario: Add six frames to the replacer: [1, 2, 3, 4, 5]. Frame 6 not touched.
    lru_replacer.RecordAccess(1);
    lru_replacer.RecordAccess(2);
    lru_replacer.RecordAccess(3);
    lru_replacer.RecordAccess(4);
    lru_replacer.RecordAccess(1);
    lru_replacer.RecordAccess(2);
    lru_replacer.RecordAccess(3);
    lru_replacer.RecordAccess(1);

    // Enable evicting on frames 1, 2, 3, 4. Frame 5 is unevictable.
    lru_replacer.SetEvictable(1, true);
    lru_replacer.SetEvictable(2, true);
    lru_replacer.SetEvictable(3, true);
    lru_replacer.SetEvictable(4, true);
    lru_replacer.SetEvictable(5, false);

    // Size (evictable count) should be 4.
    EXPECT_EQ(4, lru_replacer.Size());

    // Frame 5 receives accesses while unevictable
    lru_replacer.RecordAccess(5);
    lru_replacer.RecordAccess(5);
    lru_replacer.SetEvictable(5, false);
    EXPECT_EQ(4, lru_replacer.Size());

    // Evict frame: Frame 4 has infinite backward k-distance (count = 1 < 2), so it must be evicted first!
    int victim = lru_replacer.Evict();
    EXPECT_EQ(4, victim);
    EXPECT_EQ(3, lru_replacer.Size());

    // Next evictions: frames 1, 2, 3 all have >= 2 accesses.
    // 2nd-to-last access timestamps:
    // Frame 2: timestamp 2
    // Frame 3: timestamp 3
    // Frame 1: timestamp 5 (accesses were at 1, 5, 8)
    // Smallest timestamp (largest backward distance) is Frame 2, then Frame 3, then Frame 1.
    EXPECT_EQ(2, lru_replacer.Evict());
    EXPECT_EQ(2, lru_replacer.Size());

    EXPECT_EQ(3, lru_replacer.Evict());
    EXPECT_EQ(1, lru_replacer.Size());

    EXPECT_EQ(1, lru_replacer.Evict());
    EXPECT_EQ(0, lru_replacer.Size());

    // No evictable frames remain
    EXPECT_EQ(-1, lru_replacer.Evict());

    // Now set frame 5 evictable
    lru_replacer.SetEvictable(5, true);
    EXPECT_EQ(1, lru_replacer.Size());

    EXPECT_EQ(5, lru_replacer.Evict());
    EXPECT_EQ(0, lru_replacer.Size());
    EXPECT_EQ(-1, lru_replacer.Evict());
}

/**
 * @brief CMU 15-445 Edge Case: Infinite Backward Distance Ties Broken by FIFO.
 * When multiple frames have fewer than k accesses, all have +inf backward distance.
 * The policy requires evicting them in strict FIFO order based on earliest timestamp.
 */
TEST_F(LRUKReplacerTest, CMU15445_EvictionTiesEarliestTimestampFIFO) {
    LRUKReplacer replacer(5, 3);  // k = 3

    // Access frames 0, 1, 2 once each (all have < 3 accesses, so +inf distance)
    replacer.RecordAccess(0);
    replacer.RecordAccess(1);
    replacer.RecordAccess(2);

    replacer.SetEvictable(0, true);
    replacer.SetEvictable(1, true);
    replacer.SetEvictable(2, true);
    EXPECT_EQ(3, replacer.Size());

    // Strict FIFO order: 0 was first, 1 second, 2 third
    EXPECT_EQ(0, replacer.Evict());
    EXPECT_EQ(1, replacer.Evict());
    EXPECT_EQ(2, replacer.Evict());
    EXPECT_EQ(0, replacer.Size());
    EXPECT_EQ(-1, replacer.Evict());
}

/**
 * @brief CMU 15-445 Edge Case: Evicted Frame Has History Completely Reset.
 * If a frame is evicted and later accessed again, it must start with access count 1
 * and enter the cold queue with +inf distance.
 */
TEST_F(LRUKReplacerTest, CMU15445_EvictedFrameHistoryWiped) {
    LRUKReplacer replacer(4, 2);

    // Frame 1 accessed 5 times
    for (int i = 0; i < 5; ++i) {
        replacer.RecordAccess(1);
    }
    replacer.SetEvictable(1, true);
    EXPECT_EQ(5, replacer.GetAccessCount(1));

    // Evict frame 1
    EXPECT_EQ(1, replacer.Evict());
    EXPECT_EQ(0, replacer.GetAccessCount(1));
    EXPECT_FALSE(replacer.IsEvictable(1));

    // Re-access frame 1: must start with access count 1 (cold queue, +inf distance)
    replacer.RecordAccess(1);
    replacer.SetEvictable(1, true);
    EXPECT_EQ(1, replacer.GetAccessCount(1));

    // Frame 2 accessed twice (k=2, hot queue)
    replacer.RecordAccess(2);
    replacer.RecordAccess(2);
    replacer.SetEvictable(2, true);

    // Frame 1 (+inf distance) must be evicted BEFORE Frame 2 (finite distance)
    EXPECT_EQ(1, replacer.Evict());
    EXPECT_EQ(2, replacer.Evict());
    EXPECT_EQ(-1, replacer.Evict());
}

/**
 * @brief CMU 15-445 Edge Case: Idempotent SetEvictable and Unpin calls.
 * Repeated calls to SetEvictable(fid, true) or Unpin(fid) on an already evictable
 * frame must not duplicate queue entries or corrupt the evictable size.
 */
TEST_F(LRUKReplacerTest, CMU15445_IdempotentSetEvictableAndUnpin) {
    LRUKReplacer replacer(5, 2);

    replacer.RecordAccess(0);
    replacer.SetEvictable(0, true);
    EXPECT_EQ(1, replacer.Size());

    // Repeated SetEvictable(true)
    replacer.SetEvictable(0, true);
    replacer.SetEvictable(0, true);
    EXPECT_EQ(1, replacer.Size());

    // Repeated Unpin()
    replacer.Unpin(0);
    replacer.Unpin(0);
    EXPECT_EQ(1, replacer.Size());

    // Single eviction should exhaust it
    EXPECT_EQ(0, replacer.Evict());
    EXPECT_EQ(0, replacer.Size());
    EXPECT_EQ(-1, replacer.Evict());
}

/**
 * @brief CMU 15-445 Edge Case: Dynamic Queue Migration While Evictable.
 * An evictable frame in the cold queue (< k accesses) receives another access,
 * pushing it to >= k accesses. It must dynamically transition to the hot queue.
 */
TEST_F(LRUKReplacerTest, CMU15445_DynamicQueueMigration) {
    LRUKReplacer replacer(4, 2);

    // Frame 0: accessed 1 time, evictable (cold queue)
    replacer.RecordAccess(0);
    replacer.SetEvictable(0, true);

    // Frame 1: accessed 1 time, evictable (cold queue)
    replacer.RecordAccess(1);
    replacer.SetEvictable(1, true);

    EXPECT_EQ(2, replacer.Size());

    // Now give Frame 0 a 2nd access while still evictable -> transitions to hot queue!
    replacer.RecordAccess(0);
    EXPECT_EQ(2, replacer.Size());

    // Frame 1 has 1 access (+inf distance), Frame 0 has 2 accesses (finite distance).
    // Frame 1 must be evicted first!
    EXPECT_EQ(1, replacer.Evict());
    EXPECT_EQ(0, replacer.Evict());
    EXPECT_EQ(-1, replacer.Evict());
}

// ==================== Performance Benchmark ====================

TEST(LRUKReplacerBenchmark, PerformanceTest) {
    const int num_frames = 1000;
    const int num_operations = 100000;
    LRUKReplacer replacer(num_frames, 2);
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> frame_dist(0, num_frames - 1);
    std::uniform_int_distribution<> op_dist(0, 3);
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < num_operations; ++i) {
        int frame = frame_dist(gen);
        int op = op_dist(gen);
        
        switch (op) {
            case 0:
                replacer.RecordAccess(frame);
                replacer.SetEvictable(frame, true);
                break;
            case 1:
                if (replacer.GetEvictableCount() > 0) {
                    replacer.Evict();
                }
                break;
            case 2:
                replacer.Pin(frame);
                break;
            case 3:
                replacer.Unpin(frame);
                break;
        }
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    std::cout << "\n=== Performance Benchmark ===" << std::endl;
    std::cout << "Operations: " << num_operations << std::endl;
    std::cout << "Time: " << duration.count() << " ms" << std::endl;
    std::cout << "Average: " << (duration.count() * 1000.0 / num_operations) 
              << " μs/op" << std::endl;
    std::cout << "Throughput: " << (num_operations * 1000.0 / duration.count()) 
              << " ops/sec" << std::endl;
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}