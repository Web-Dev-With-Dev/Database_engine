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