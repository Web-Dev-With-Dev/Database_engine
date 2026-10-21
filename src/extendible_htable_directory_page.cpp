#include "extendible_htable_directory_page.h"
#include <algorithm>
#include <cassert>

void ExtendibleHTableDirectoryPage::Init(page_id_t page_id, page_id_t initial_bucket_page_id, uint32_t max_depth) {
    page_id_ = page_id;
    lsn_ = 0;
    global_depth_ = 0;
    max_depth_ = std::min<uint32_t>(max_depth, HTABLE_DIRECTORY_MAX_DEPTH);

    for (size_t i = 0; i < HTABLE_DIRECTORY_ARRAY_SIZE; ++i) {
        local_depths_[i] = 0;
        bucket_page_ids_[i] = initial_bucket_page_id;
    }
}

uint32_t ExtendibleHTableDirectoryPage::HashToBucketIndex(size_t hash) const {
    if (global_depth_ == 0) {
        return 0;
    }
    return static_cast<uint32_t>(hash & ((1u << global_depth_) - 1u));
}

uint32_t ExtendibleHTableDirectoryPage::GetSplitImageIndex(uint32_t bucket_idx) const {
    uint8_t ld = GetLocalDepth(bucket_idx);
    if (ld == 0) {
        return 0;
    }
    return bucket_idx ^ (1u << (ld - 1u));
}

uint32_t ExtendibleHTableDirectoryPage::GetGlobalDepthMask() const {
    if (global_depth_ == 0) {
        return 0;
    }
    return (1u << global_depth_) - 1u;
}

uint32_t ExtendibleHTableDirectoryPage::GetLocalDepthMask(uint32_t bucket_idx) const {
    uint8_t ld = GetLocalDepth(bucket_idx);
    if (ld == 0) {
        return 0;
    }
    return (1u << ld) - 1u;
}

void ExtendibleHTableDirectoryPage::SetGlobalDepth(uint32_t global_depth) {
    global_depth_ = std::min(global_depth, max_depth_);
}

void ExtendibleHTableDirectoryPage::IncrGlobalDepth() {
    if (global_depth_ >= max_depth_) {
        return;
    }
    uint32_t old_size = 1u << global_depth_;
    for (uint32_t i = 0; i < old_size; ++i) {
        bucket_page_ids_[i + old_size] = bucket_page_ids_[i];
        local_depths_[i + old_size] = local_depths_[i];
    }
    ++global_depth_;
}

void ExtendibleHTableDirectoryPage::DecrGlobalDepth() {
    if (CanShrink()) {
        --global_depth_;
    }
}

bool ExtendibleHTableDirectoryPage::CanShrink() const {
    if (global_depth_ == 0) {
        return false;
    }
    uint32_t current_size = 1u << global_depth_;
    for (uint32_t i = 0; i < current_size; ++i) {
        if (local_depths_[i] >= global_depth_) {
            return false;
        }
    }
    return true;
}

page_id_t ExtendibleHTableDirectoryPage::GetBucketPageId(uint32_t bucket_idx) const {
    if (bucket_idx >= HTABLE_DIRECTORY_ARRAY_SIZE) {
        return INVALID_PAGE_ID;
    }
    return bucket_page_ids_[bucket_idx];
}

void ExtendibleHTableDirectoryPage::SetBucketPageId(uint32_t bucket_idx, page_id_t bucket_page_id) {
    if (bucket_idx < HTABLE_DIRECTORY_ARRAY_SIZE) {
        bucket_page_ids_[bucket_idx] = bucket_page_id;
    }
}

uint8_t ExtendibleHTableDirectoryPage::GetLocalDepth(uint32_t bucket_idx) const {
    if (bucket_idx >= HTABLE_DIRECTORY_ARRAY_SIZE) {
        return 0;
    }
    return local_depths_[bucket_idx];
}

void ExtendibleHTableDirectoryPage::SetLocalDepth(uint32_t bucket_idx, uint8_t local_depth) {
    if (bucket_idx < HTABLE_DIRECTORY_ARRAY_SIZE) {
        local_depths_[bucket_idx] = local_depth;
    }
}

void ExtendibleHTableDirectoryPage::IncrLocalDepth(uint32_t bucket_idx) {
    if (bucket_idx < HTABLE_DIRECTORY_ARRAY_SIZE) {
        ++local_depths_[bucket_idx];
    }
}

bool ExtendibleHTableDirectoryPage::VerifyIntegrity() const {
    if (global_depth_ > max_depth_) {
        return false;
    }
    uint32_t current_size = 1u << global_depth_;
    for (uint32_t i = 0; i < current_size; ++i) {
        if (local_depths_[i] > global_depth_) {
            return false;
        }
        if (bucket_page_ids_[i] == INVALID_PAGE_ID) {
            return false;
        }
    }
    return true;
}
