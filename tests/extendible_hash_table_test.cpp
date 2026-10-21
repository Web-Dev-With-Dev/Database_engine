#include <gtest/gtest.h>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "buffer_pool_manager.h"
#include "disk_manager.h"
#include "extendible_hash_table.h"
#include "extendible_htable_bucket_page.h"
#include "extendible_htable_directory_page.h"

namespace fs = std::filesystem;

class ExtendibleHashTableTest : public ::testing::Test {
protected:
    const std::string db_file_ = "test_extendible_htable.db";
    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPoolManager> bpm_;

    void SetUp() override {
        if (fs::exists(db_file_)) {
            fs::remove(db_file_);
        }
        disk_manager_ = std::make_unique<DiskManager>(db_file_);
        bpm_ = std::make_unique<BufferPoolManager>(50, disk_manager_.get());
    }

    void TearDown() override {
        bpm_.reset();
        disk_manager_.reset();
        if (fs::exists(db_file_)) {
            fs::remove(db_file_);
        }
    }
};

// ==================== Directory Page Unit Tests ====================

TEST_F(ExtendibleHashTableTest, DirectoryInit) {
    page_id_t dir_page_id = INVALID_PAGE_ID;
    Page *page = bpm_->NewPage(&dir_page_id);
    ASSERT_NE(page, nullptr);

    auto *dir = reinterpret_cast<ExtendibleHTableDirectoryPage *>(page->GetData());
    dir->Init(dir_page_id, 100);

    EXPECT_EQ(dir->GetPageId(), dir_page_id);
    EXPECT_EQ(dir->GetGlobalDepth(), 0u);
    EXPECT_EQ(dir->Size(), 1u);
    EXPECT_EQ(dir->GetBucketPageId(0), 100);
    EXPECT_EQ(dir->GetLocalDepth(0), 0u);
    EXPECT_TRUE(dir->VerifyIntegrity());

    bpm_->UnpinPage(dir_page_id, true);
}

TEST_F(ExtendibleHashTableTest, DirectoryHashToBucketIndex) {
    page_id_t dir_page_id = INVALID_PAGE_ID;
    Page *page = bpm_->NewPage(&dir_page_id);
    auto *dir = reinterpret_cast<ExtendibleHTableDirectoryPage *>(page->GetData());
    dir->Init(dir_page_id, 0);

    // Global depth 0 -> all hashes map to index 0
    EXPECT_EQ(dir->HashToBucketIndex(0b0), 0u);
    EXPECT_EQ(dir->HashToBucketIndex(0b1011), 0u);

    // Global depth 1 -> lowest bit
    dir->IncrGlobalDepth();
    EXPECT_EQ(dir->GetGlobalDepth(), 1u);
    EXPECT_EQ(dir->HashToBucketIndex(0b10), 0u);
    EXPECT_EQ(dir->HashToBucketIndex(0b11), 1u);

    // Global depth 2 -> lowest 2 bits
    dir->IncrGlobalDepth();
    EXPECT_EQ(dir->GetGlobalDepth(), 2u);
    EXPECT_EQ(dir->HashToBucketIndex(0b100), 0u);
    EXPECT_EQ(dir->HashToBucketIndex(0b101), 1u);
    EXPECT_EQ(dir->HashToBucketIndex(0b110), 2u);
    EXPECT_EQ(dir->HashToBucketIndex(0b111), 3u);

    // Global depth 3 -> lowest 3 bits
    dir->IncrGlobalDepth();
    EXPECT_EQ(dir->GetGlobalDepth(), 3u);
    EXPECT_EQ(dir->HashToBucketIndex(0b10101), 0b101u); // 5

    bpm_->UnpinPage(dir_page_id, true);
}

TEST_F(ExtendibleHashTableTest, DirectorySplitImageIndex) {
    page_id_t dir_page_id = INVALID_PAGE_ID;
    Page *page = bpm_->NewPage(&dir_page_id);
    auto *dir = reinterpret_cast<ExtendibleHTableDirectoryPage *>(page->GetData());
    dir->Init(dir_page_id, 0);

    dir->IncrGlobalDepth();
    dir->IncrGlobalDepth(); // GD = 2, size = 4
    dir->SetLocalDepth(0, 2);
    dir->SetLocalDepth(1, 2);
    dir->SetLocalDepth(2, 2);
    dir->SetLocalDepth(3, 2);

    // For LD = 2, bit mask is (1 << 1) = 2
    // Split image of 0 (00b) is 2 (10b)
    EXPECT_EQ(dir->GetSplitImageIndex(0), 2u);
    EXPECT_EQ(dir->GetSplitImageIndex(2), 0u);
    // Split image of 1 (01b) is 3 (11b)
    EXPECT_EQ(dir->GetSplitImageIndex(1), 3u);
    EXPECT_EQ(dir->GetSplitImageIndex(3), 1u);

    bpm_->UnpinPage(dir_page_id, true);
}

TEST_F(ExtendibleHashTableTest, DirectoryDoublingAndShrinking) {
    page_id_t dir_page_id = INVALID_PAGE_ID;
    Page *page = bpm_->NewPage(&dir_page_id);
    auto *dir = reinterpret_cast<ExtendibleHTableDirectoryPage *>(page->GetData());
    dir->Init(dir_page_id, 42);

    EXPECT_EQ(dir->GetGlobalDepth(), 0u);
    EXPECT_EQ(dir->Size(), 1u);

    dir->IncrGlobalDepth(); // GD = 1
    EXPECT_EQ(dir->GetGlobalDepth(), 1u);
    EXPECT_EQ(dir->Size(), 2u);
    EXPECT_EQ(dir->GetBucketPageId(0), 42);
    EXPECT_EQ(dir->GetBucketPageId(1), 42);

    dir->IncrGlobalDepth(); // GD = 2
    EXPECT_EQ(dir->GetGlobalDepth(), 2u);
    EXPECT_EQ(dir->Size(), 4u);
    for (uint32_t i = 0; i < 4; ++i) {
        EXPECT_EQ(dir->GetBucketPageId(i), 42);
    }

    // Since all local depths are 0 < 2, directory can shrink
    EXPECT_TRUE(dir->CanShrink());
    dir->DecrGlobalDepth();
    EXPECT_EQ(dir->GetGlobalDepth(), 1u);
    EXPECT_EQ(dir->Size(), 2u);

    // If local depth == global depth, cannot shrink
    dir->SetLocalDepth(0, 1);
    EXPECT_FALSE(dir->CanShrink());

    bpm_->UnpinPage(dir_page_id, true);
}

// ==================== Bucket Page Unit Tests ====================

TEST_F(ExtendibleHashTableTest, BucketPageInsertLookupRemove) {
    page_id_t bucket_page_id = INVALID_PAGE_ID;
    Page *page = bpm_->NewPage(&bucket_page_id);
    ASSERT_NE(page, nullptr);

    using Bucket = ExtendibleHTableBucketPage<int, std::string>;
    auto *bucket = reinterpret_cast<Bucket *>(page->GetData());
    bucket->Init(bucket_page_id, 3); // max capacity 3

    EXPECT_TRUE(bucket->IsEmpty());
    EXPECT_FALSE(bucket->IsFull());
    EXPECT_EQ(bucket->Size(), 0u);

    // Insert 1
    EXPECT_TRUE(bucket->Insert(1, "one"));
    EXPECT_EQ(bucket->Size(), 1u);
    std::string val;
    EXPECT_TRUE(bucket->Lookup(1, &val));
    EXPECT_EQ(val, "one");

    // Insert duplicate key -> updates value
    EXPECT_TRUE(bucket->Insert(1, "ONE_UPDATED"));
    EXPECT_EQ(bucket->Size(), 1u);
    EXPECT_TRUE(bucket->Lookup(1, &val));
    EXPECT_EQ(val, "ONE_UPDATED");

    // Insert 2 and 3
    EXPECT_TRUE(bucket->Insert(2, "two"));
    EXPECT_TRUE(bucket->Insert(3, "three"));
    EXPECT_TRUE(bucket->IsFull());
    EXPECT_EQ(bucket->Size(), 3u);

    // Insert 4 when full fails
    EXPECT_FALSE(bucket->Insert(4, "four"));

    // Remove 2
    EXPECT_TRUE(bucket->Remove(2));
    EXPECT_EQ(bucket->Size(), 2u);
    EXPECT_FALSE(bucket->Lookup(2, &val));
    EXPECT_TRUE(bucket->Lookup(1, &val));
    EXPECT_TRUE(bucket->Lookup(3, &val));

    // Remove non-existent key
    EXPECT_FALSE(bucket->Remove(999));

    bpm_->UnpinPage(bucket_page_id, true);
}

// ==================== ExtendibleHashTable Integration Tests ====================

TEST_F(ExtendibleHashTableTest, TableBasicInsertAndLookup) {
    ExtendibleHashTable<int, int> ht("test_table", bpm_.get(), 4);

    EXPECT_EQ(ht.GetGlobalDepth(), 0u);
    EXPECT_EQ(ht.GetNumUniqueBuckets(), 1u);

    // Insert up to bucket capacity (4 elements)
    EXPECT_TRUE(ht.Insert(1, 10));
    EXPECT_TRUE(ht.Insert(2, 20));
    EXPECT_TRUE(ht.Insert(3, 30));
    EXPECT_TRUE(ht.Insert(4, 40));

    int val = 0;
    EXPECT_TRUE(ht.GetValue(1, &val));
    EXPECT_EQ(val, 10);
    EXPECT_TRUE(ht.GetValue(2, &val));
    EXPECT_EQ(val, 20);
    EXPECT_TRUE(ht.GetValue(3, &val));
    EXPECT_EQ(val, 30);
    EXPECT_TRUE(ht.GetValue(4, &val));
    EXPECT_EQ(val, 40);

    // Non-existent key
    EXPECT_FALSE(ht.GetValue(999, &val));
}

TEST_F(ExtendibleHashTableTest, TableSplitAndDirectoryExpansion) {
    // Bucket capacity = 2 to force splits early
    ExtendibleHashTable<int, int> ht("split_table", bpm_.get(), 2);

    EXPECT_EQ(ht.GetGlobalDepth(), 0u);
    EXPECT_EQ(ht.GetNumUniqueBuckets(), 1u);

    // Insert 2 elements (fills bucket 0)
    EXPECT_TRUE(ht.Insert(4, 400));
    EXPECT_TRUE(ht.Insert(8, 800));

    // Insert 3rd element -> forces bucket split and directory expansion to GD = 1 or higher!
    EXPECT_TRUE(ht.Insert(5, 500));

    EXPECT_GE(ht.GetGlobalDepth(), 1u);
    EXPECT_GE(ht.GetNumUniqueBuckets(), 2u);

    // Verify all keys remain accessible
    int val = 0;
    EXPECT_TRUE(ht.GetValue(4, &val));
    EXPECT_EQ(val, 400);
    EXPECT_TRUE(ht.GetValue(8, &val));
    EXPECT_EQ(val, 800);
    EXPECT_TRUE(ht.GetValue(5, &val));
    EXPECT_EQ(val, 500);
}

TEST_F(ExtendibleHashTableTest, TableLargeDatasetManySplits) {
    // Bucket capacity = 4, insert 100 elements
    ExtendibleHashTable<int, int> ht("large_table", bpm_.get(), 4);

    const int num_keys = 100;
    for (int i = 0; i < num_keys; ++i) {
        EXPECT_TRUE(ht.Insert(i, i * 10));
    }

    // Verify all 100 keys
    for (int i = 0; i < num_keys; ++i) {
        int val = -1;
        EXPECT_TRUE(ht.GetValue(i, &val)) << "Failed to find key " << i;
        EXPECT_EQ(val, i * 10);
    }

    EXPECT_GT(ht.GetGlobalDepth(), 2u);
    EXPECT_GT(ht.GetNumUniqueBuckets(), 5u);
    EXPECT_TRUE(ht.VerifyIntegrity());
}

TEST_F(ExtendibleHashTableTest, TableRemove) {
    ExtendibleHashTable<int, std::string> ht("remove_table", bpm_.get(), 2);

    ht.Insert(1, "apple");
    ht.Insert(2, "banana");
    ht.Insert(3, "cherry");
    ht.Insert(4, "date");

    std::string val;
    EXPECT_TRUE(ht.GetValue(2, &val));
    EXPECT_EQ(val, "banana");

    // Remove key 2
    EXPECT_TRUE(ht.Remove(2));
    EXPECT_FALSE(ht.GetValue(2, &val));

    // Other keys still present
    EXPECT_TRUE(ht.GetValue(1, &val));
    EXPECT_EQ(val, "apple");
    EXPECT_TRUE(ht.GetValue(3, &val));
    EXPECT_EQ(val, "cherry");
    EXPECT_TRUE(ht.GetValue(4, &val));
    EXPECT_EQ(val, "date");

    // Removing non-existent key returns false
    EXPECT_FALSE(ht.Remove(2));
    EXPECT_FALSE(ht.Remove(999));
}

TEST_F(ExtendibleHashTableTest, PersistenceAcrossRestart) {
    page_id_t directory_id = INVALID_PAGE_ID;

    {
        // Session 1: Create hash table and insert 50 elements
        ExtendibleHashTable<int, int> ht("persisted_table", bpm_.get(), 4);
        directory_id = ht.GetDirectoryPageId();

        for (int i = 0; i < 50; ++i) {
            EXPECT_TRUE(ht.Insert(i, i * 100));
        }

        // Flush all dirty pages to disk
        bpm_->FlushAllPages();
    }

    // Reset buffer pool manager (simulating cache eviction / restart)
    bpm_.reset();
    bpm_ = std::make_unique<BufferPoolManager>(50, disk_manager_.get());

    {
        // Session 2: Reopen hash table from saved directory page ID
        auto ht = ExtendibleHashTable<int, int>::Open("persisted_table", bpm_.get(), directory_id, 4);

        for (int i = 0; i < 50; ++i) {
            int val = -1;
            EXPECT_TRUE(ht.GetValue(i, &val)) << "Failed to find persisted key " << i;
            EXPECT_EQ(val, i * 100);
        }

        EXPECT_TRUE(ht.VerifyIntegrity());
    }
}
