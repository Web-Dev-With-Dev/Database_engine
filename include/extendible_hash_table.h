#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <vector>

#include "buffer_pool_manager.h"
#include "config.h"
#include "extendible_htable_bucket_page.h"
#include "extendible_htable_directory_page.h"

/**
 * @brief Extendible Hash Table implementation backed by BufferPoolManager.
 *
 * Demonstrates:
 * - Dynamic directory page management in buffer frames
 * - Bucket page allocation, splitting, and key redistribution
 * - Directory doubling (IncrGlobalDepth) and shrinking (DecrGlobalDepth)
 * - Safe page pinning and dirty flag tracking in BufferPoolManager
 */
template <typename KeyType, typename ValueType, typename HashFunc = std::hash<KeyType>>
class ExtendibleHashTable {
public:
    using BucketPage = ExtendibleHTableBucketPage<KeyType, ValueType>;

    /**
     * @brief Construct a new ExtendibleHashTable.
     * Allocates an initial DirectoryPage and initial BucketPage in the BufferPoolManager.
     * @param name Diagnostic identifier name.
     * @param bpm Pointer to BufferPoolManager.
     * @param bucket_max_size Optional custom capacity per bucket (default: max fit in 4096B).
     * @param hash_fn Hash function to use.
     */
    ExtendibleHashTable(const std::string &name, BufferPoolManager *bpm,
                        uint32_t bucket_max_size = 0, HashFunc hash_fn = HashFunc{})
        : name_(name), bpm_(bpm), bucket_max_size_(bucket_max_size), hash_fn_(hash_fn) {
        
        // 1. Allocate directory page
        Page *dir_page = bpm_->NewPage(&directory_page_id_);
        assert(dir_page != nullptr && "Failed to allocate directory page for ExtendibleHashTable");

        // 2. Allocate initial bucket page
        page_id_t initial_bucket_id = INVALID_PAGE_ID;
        Page *bucket_page = bpm_->NewPage(&initial_bucket_id);
        assert(bucket_page != nullptr && "Failed to allocate initial bucket page");

        // 3. Initialize bucket page
        auto *bucket = reinterpret_cast<BucketPage *>(bucket_page->GetData());
        bucket->Init(initial_bucket_id, bucket_max_size_);

        // 4. Initialize directory page with initial bucket
        auto *dir = reinterpret_cast<ExtendibleHTableDirectoryPage *>(dir_page->GetData());
        dir->Init(directory_page_id_, initial_bucket_id);

        // 5. Unpin both pages as dirty
        bpm_->UnpinPage(initial_bucket_id, true);
        bpm_->UnpinPage(directory_page_id_, true);
    }

    /**
     * @brief Open an existing ExtendibleHashTable from an existing directory page on disk.
     */
    static ExtendibleHashTable Open(const std::string &name, BufferPoolManager *bpm,
                                    page_id_t directory_page_id, uint32_t bucket_max_size = 0,
                                    HashFunc hash_fn = HashFunc{}) {
        ExtendibleHashTable table;
        table.name_ = name;
        table.bpm_ = bpm;
        table.directory_page_id_ = directory_page_id;
        table.bucket_max_size_ = bucket_max_size;
        table.hash_fn_ = hash_fn;
        return table;
    }

private:
    ExtendibleHashTable() = default;

public:
    ExtendibleHashTable(ExtendibleHashTable &&) noexcept = default;
    ExtendibleHashTable &operator=(ExtendibleHashTable &&) noexcept = default;
    ExtendibleHashTable(const ExtendibleHashTable &) = delete;
    ExtendibleHashTable &operator=(const ExtendibleHashTable &) = delete;

    ~ExtendibleHashTable() = default;

    /**
     * @brief Insert or update a key-value pair.
     * Automatically handles bucket splitting and directory expansion when a bucket overflows.
     * @param key Key to insert.
     * @param value Value to associate with key.
     * @return true on success, false if table has reached maximum allowable capacity.
     */
    bool Insert(const KeyType &key, const ValueType &value) {
        std::unique_lock<std::shared_mutex> lock(*rwlock_);

        while (true) {
            Page *dir_page = bpm_->FetchPage(directory_page_id_);
            if (!dir_page) return false;
            auto *dir = reinterpret_cast<ExtendibleHTableDirectoryPage *>(dir_page->GetData());

            size_t hash = hash_fn_(key);
            uint32_t bucket_idx = dir->HashToBucketIndex(hash);
            page_id_t bucket_page_id = dir->GetBucketPageId(bucket_idx);

            Page *b_page = bpm_->FetchPage(bucket_page_id);
            if (!b_page) {
                bpm_->UnpinPage(directory_page_id_, false);
                return false;
            }
            auto *bucket = reinterpret_cast<BucketPage *>(b_page->GetData());

            // Case 1: Key already exists or bucket has free space
            if (bucket->Insert(key, value)) {
                bpm_->UnpinPage(bucket_page_id, true);
                bpm_->UnpinPage(directory_page_id_, false);
                return true;
            }

            // Case 2: Bucket is full and key does not exist -> Bucket Split needed!
            uint32_t local_depth = dir->GetLocalDepth(bucket_idx);
            uint32_t global_depth = dir->GetGlobalDepth();

            if (local_depth >= dir->GetMaxDepth()) {
                // Table reached maximum supported depth
                bpm_->UnpinPage(bucket_page_id, false);
                bpm_->UnpinPage(directory_page_id_, false);
                return false;
            }

            // If local depth equals global depth, double the directory
            if (local_depth == global_depth) {
                dir->IncrGlobalDepth();
                global_depth = dir->GetGlobalDepth();
            }

            // Allocate a new bucket page
            page_id_t new_bucket_page_id = INVALID_PAGE_ID;
            Page *new_b_page = bpm_->NewPage(&new_bucket_page_id);
            if (!new_b_page) {
                bpm_->UnpinPage(bucket_page_id, false);
                bpm_->UnpinPage(directory_page_id_, true);
                return false;
            }
            auto *new_bucket = reinterpret_cast<BucketPage *>(new_b_page->GetData());
            new_bucket->Init(new_bucket_page_id, bucket_max_size_);

            // Update directory entries: indices matching (old_local_depth)-th bit point to new bucket
            uint32_t new_ld = local_depth + 1;
            uint32_t split_mask = 1u << local_depth;
            uint32_t dir_size = dir->Size();

            for (uint32_t i = 0; i < dir_size; ++i) {
                if (dir->GetBucketPageId(i) == bucket_page_id) {
                    dir->SetLocalDepth(i, static_cast<uint8_t>(new_ld));
                    if ((i & split_mask) != 0) {
                        dir->SetBucketPageId(i, new_bucket_page_id);
                    }
                }
            }

            // Redistribute existing keys from old bucket into old or new bucket
            std::vector<std::pair<KeyType, ValueType>> old_entries;
            old_entries.reserve(bucket->Size());
            for (uint32_t i = 0; i < bucket->Size(); ++i) {
                old_entries.push_back(bucket->EntryAt(i));
            }
            bucket->Clear();

            for (const auto &entry : old_entries) {
                size_t h = hash_fn_(entry.first);
                uint32_t target_idx = dir->HashToBucketIndex(h);
                if (dir->GetBucketPageId(target_idx) == new_bucket_page_id) {
                    new_bucket->Insert(entry.first, entry.second);
                } else {
                    bucket->Insert(entry.first, entry.second);
                }
            }

            // Unpin pages
            bpm_->UnpinPage(new_bucket_page_id, true);
            bpm_->UnpinPage(bucket_page_id, true);
            bpm_->UnpinPage(directory_page_id_, true);

            // Loop continues and retries insertion
        }
    }

    /**
     * @brief Lookup the value associated with a key.
     * @param key Key to find.
     * @param result Pointer to store the value.
     * @return true if key found, false otherwise.
     */
    bool GetValue(const KeyType &key, ValueType *result) const {
        std::shared_lock<std::shared_mutex> lock(*rwlock_);

        Page *dir_page = bpm_->FetchPage(directory_page_id_);
        if (!dir_page) return false;
        auto *dir = reinterpret_cast<ExtendibleHTableDirectoryPage *>(dir_page->GetData());

        size_t hash = hash_fn_(key);
        uint32_t bucket_idx = dir->HashToBucketIndex(hash);
        page_id_t bucket_page_id = dir->GetBucketPageId(bucket_idx);

        bpm_->UnpinPage(directory_page_id_, false);

        Page *b_page = bpm_->FetchPage(bucket_page_id);
        if (!b_page) return false;
        auto *bucket = reinterpret_cast<BucketPage *>(b_page->GetData());

        bool found = bucket->Lookup(key, result);
        bpm_->UnpinPage(bucket_page_id, false);
        return found;
    }

    /**
     * @brief Remove a key from the hash table.
     * @param key Key to remove.
     * @return true if removed, false if key did not exist.
     */
    bool Remove(const KeyType &key) {
        std::unique_lock<std::shared_mutex> lock(*rwlock_);

        Page *dir_page = bpm_->FetchPage(directory_page_id_);
        if (!dir_page) return false;
        auto *dir = reinterpret_cast<ExtendibleHTableDirectoryPage *>(dir_page->GetData());

        size_t hash = hash_fn_(key);
        uint32_t bucket_idx = dir->HashToBucketIndex(hash);
        page_id_t bucket_page_id = dir->GetBucketPageId(bucket_idx);

        Page *b_page = bpm_->FetchPage(bucket_page_id);
        if (!b_page) {
            bpm_->UnpinPage(directory_page_id_, false);
            return false;
        }
        auto *bucket = reinterpret_cast<BucketPage *>(b_page->GetData());

        bool removed = bucket->Remove(key);
        bpm_->UnpinPage(bucket_page_id, removed);

        // Check if directory can shrink
        if (dir->CanShrink()) {
            dir->DecrGlobalDepth();
            bpm_->UnpinPage(directory_page_id_, true);
        } else {
            bpm_->UnpinPage(directory_page_id_, false);
        }

        return removed;
    }

    // ==================== Inspection & Metrics ====================

    /**
     * @brief Get current global depth of the hash table directory.
     * @return uint32_t Global depth.
     */
    uint32_t GetGlobalDepth() const {
        std::shared_lock<std::shared_mutex> lock(*rwlock_);
        Page *dir_page = bpm_->FetchPage(directory_page_id_);
        if (!dir_page) return 0;
        auto *dir = reinterpret_cast<ExtendibleHTableDirectoryPage *>(dir_page->GetData());
        uint32_t gd = dir->GetGlobalDepth();
        bpm_->UnpinPage(directory_page_id_, false);
        return gd;
    }

    /**
     * @brief Get local depth of a specific bucket entry.
     * @param bucket_idx Directory index.
     * @return uint32_t Local depth.
     */
    uint32_t GetLocalDepth(uint32_t bucket_idx) const {
        std::shared_lock<std::shared_mutex> lock(*rwlock_);
        Page *dir_page = bpm_->FetchPage(directory_page_id_);
        if (!dir_page) return 0;
        auto *dir = reinterpret_cast<ExtendibleHTableDirectoryPage *>(dir_page->GetData());
        uint32_t ld = dir->GetLocalDepth(bucket_idx);
        bpm_->UnpinPage(directory_page_id_, false);
        return ld;
    }

    /**
     * @brief Get physical page ID of the bucket assigned to a directory index.
     * @param bucket_idx Directory index.
     * @return page_id_t Bucket page ID, or INVALID_PAGE_ID if fetch fails.
     */
    page_id_t GetBucketPageId(uint32_t bucket_idx) const {
        std::shared_lock<std::shared_mutex> lock(*rwlock_);
        Page *dir_page = bpm_->FetchPage(directory_page_id_);
        if (!dir_page) return INVALID_PAGE_ID;
        auto *dir = reinterpret_cast<ExtendibleHTableDirectoryPage *>(dir_page->GetData());
        page_id_t id = dir->GetBucketPageId(bucket_idx);
        bpm_->UnpinPage(directory_page_id_, false);
        return id;
    }

    /**
     * @brief Count total number of unique physical bucket pages referenced by directory.
     * @return size_t Count of unique bucket pages.
     */
    size_t GetNumUniqueBuckets() const {
        std::shared_lock<std::shared_mutex> lock(*rwlock_);
        Page *dir_page = bpm_->FetchPage(directory_page_id_);
        if (!dir_page) return 0;
        auto *dir = reinterpret_cast<ExtendibleHTableDirectoryPage *>(dir_page->GetData());
        std::set<page_id_t> unique_pages;
        uint32_t sz = dir->Size();
        for (uint32_t i = 0; i < sz; ++i) {
            unique_pages.insert(dir->GetBucketPageId(i));
        }
        bpm_->UnpinPage(directory_page_id_, false);
        return unique_pages.size();
    }

    /**
     * @brief Get physical page ID of the directory page.
     * @return page_id_t Directory page ID.
     */
    page_id_t GetDirectoryPageId() const { return directory_page_id_; }

    /**
     * @brief Get table diagnostic identifier name.
     * @return const std::string& Table name.
     */
    const std::string &GetName() const { return name_; }

    /**
     * @brief Check whether internal directory depth and bucket invariants are valid.
     * @return true if valid, false otherwise.
     */
    bool VerifyIntegrity() const {
        std::shared_lock<std::shared_mutex> lock(*rwlock_);
        Page *dir_page = bpm_->FetchPage(directory_page_id_);
        if (!dir_page) return false;
        auto *dir = reinterpret_cast<ExtendibleHTableDirectoryPage *>(dir_page->GetData());
        bool valid = dir->VerifyIntegrity();
        bpm_->UnpinPage(directory_page_id_, false);
        return valid;
    }

private:
    std::string name_;
    BufferPoolManager *bpm_{nullptr};
    page_id_t directory_page_id_{INVALID_PAGE_ID};
    uint32_t bucket_max_size_{0};
    HashFunc hash_fn_{};
    mutable std::unique_ptr<std::shared_mutex> rwlock_{std::make_unique<std::shared_mutex>()};
};
