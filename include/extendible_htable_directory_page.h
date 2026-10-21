#pragma once

#include <cstdint>
#include <cstddef>
#include "config.h"

/**
 * @brief Directory page for Extendible Hash Table.
 *
 * Stored inside a 4096-byte Page in BufferPoolManager.
 * Manages global depth, local depths, and page pointers to bucket pages.
 *
 * Page layout:
 * -----------------------------------------------------------------------------
 * | PageId (4B) | LSN (4B) | GlobalDepth (4B) | MaxDepth (4B)                |
 * -----------------------------------------------------------------------------
 * | LocalDepths (512B) [1 byte per directory entry]                           |
 * -----------------------------------------------------------------------------
 * | BucketPageIds (2048B) [4 bytes per directory entry (page_id_t)]           |
 * -----------------------------------------------------------------------------
 * | Padding / Unused space (up to PAGE_SIZE = 4096B)                          |
 * -----------------------------------------------------------------------------
 */
class ExtendibleHTableDirectoryPage {
public:
    static constexpr size_t HTABLE_DIRECTORY_MAX_DEPTH = 9;
    static constexpr size_t HTABLE_DIRECTORY_ARRAY_SIZE = 1u << HTABLE_DIRECTORY_MAX_DEPTH; // 512 entries

    ExtendibleHTableDirectoryPage() = default;

    /**
     * @brief Initialize directory page with initial depth 0 and initial bucket page.
     * @param page_id The physical page_id assigned to this directory.
     * @param initial_bucket_page_id The initial bucket page ID.
     * @param max_depth Maximum global depth supported (default 9).
     */
    void Init(page_id_t page_id, page_id_t initial_bucket_page_id, uint32_t max_depth = HTABLE_DIRECTORY_MAX_DEPTH);

    // ==================== Depth & Index Operations ====================

    /**
     * @brief Compute directory index from hash value.
     */
    uint32_t HashToBucketIndex(size_t hash) const;

    /**
     * @brief Get the split image index for a given bucket index.
     * Toggles the bit at position (local_depth - 1).
     */
    uint32_t GetSplitImageIndex(uint32_t bucket_idx) const;

    /**
     * @brief Returns bitmask for the current global depth (lowest global_depth bits set to 1).
     */
    uint32_t GetGlobalDepthMask() const;

    /**
     * @brief Returns bitmask for the local depth of a given bucket.
     */
    uint32_t GetLocalDepthMask(uint32_t bucket_idx) const;

    uint32_t GetGlobalDepth() const { return global_depth_; }
    void SetGlobalDepth(uint32_t global_depth);
    void IncrGlobalDepth();
    void DecrGlobalDepth();
    bool CanShrink() const;

    uint32_t GetMaxDepth() const { return max_depth_; }
    uint32_t Size() const { return 1u << global_depth_; }

    // ==================== Local Depths & Bucket IDs ====================

    page_id_t GetBucketPageId(uint32_t bucket_idx) const;
    void SetBucketPageId(uint32_t bucket_idx, page_id_t bucket_page_id);

    uint8_t GetLocalDepth(uint32_t bucket_idx) const;
    void SetLocalDepth(uint32_t bucket_idx, uint8_t local_depth);
    void IncrLocalDepth(uint32_t bucket_idx);

    page_id_t GetPageId() const { return page_id_; }
    void SetPageId(page_id_t page_id) { page_id_ = page_id; }

    uint32_t GetLsn() const { return lsn_; }
    void SetLsn(uint32_t lsn) { lsn_ = lsn; }

    /**
     * @brief Verifies integrity of directory entries and local depths.
     */
    bool VerifyIntegrity() const;

private:
    page_id_t page_id_{INVALID_PAGE_ID};
    uint32_t lsn_{0};
    uint32_t global_depth_{0};
    uint32_t max_depth_{HTABLE_DIRECTORY_MAX_DEPTH};

    uint8_t local_depths_[HTABLE_DIRECTORY_ARRAY_SIZE];
    page_id_t bucket_page_ids_[HTABLE_DIRECTORY_ARRAY_SIZE];
};

static_assert(sizeof(ExtendibleHTableDirectoryPage) <= PAGE_SIZE,
              "ExtendibleHTableDirectoryPage must fit within PAGE_SIZE (4096 bytes)");
