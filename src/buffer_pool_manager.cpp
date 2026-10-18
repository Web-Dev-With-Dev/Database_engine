#include "buffer_pool_manager.h"

#include <cstring>
#include <stdexcept>

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager *disk_manager, size_t k)
    : pool_size_(pool_size), disk_manager_(disk_manager) {
    if (pool_size_ == 0) {
        throw std::invalid_argument("Buffer pool size must be greater than 0");
    }
    if (disk_manager_ == nullptr) {
        throw std::invalid_argument("DiskManager pointer cannot be null");
    }

    pages_ = std::make_unique<Page[]>(pool_size_);
    replacer_ = std::make_unique<LRUKReplacer>(pool_size_, k);

    for (size_t i = 0; i < pool_size_; ++i) {
        free_list_.push_back(i);
    }
}

BufferPoolManager::BufferPoolManager(size_t pool_size, std::shared_ptr<DiskManager> disk_manager, size_t k)
    : BufferPoolManager(pool_size, disk_manager.get(), k) {
    disk_manager_sp_ = disk_manager;
}

BufferPoolManager::~BufferPoolManager() {
    FlushAllPages();
}

bool BufferPoolManager::FindAvailableFrame(frame_id_t *frame_id) {
    // 1. Try to get a frame from the free list first
    if (!free_list_.empty()) {
        *frame_id = free_list_.front();
        free_list_.pop_front();
        return true;
    }

    // 2. No free frames available: consult the replacer for an evictable victim
    int victim = replacer_->Evict();
    if (victim == -1) {
        // All frames in buffer pool are currently pinned
        return false;
    }

    frame_id_t victim_fid = static_cast<frame_id_t>(victim);
    Page &victim_page = pages_[victim_fid];

    // 3. If the victim page is dirty, write it back to disk
    if (victim_page.IsDirty()) {
        disk_manager_->WritePage(victim_page.GetPageId(), victim_page.GetData());
        victim_page.SetDirty(false);
    }

    // 4. Remove victim page from page_table_ mapping
    page_table_.erase(victim_page.GetPageId());

    *frame_id = victim_fid;
    return true;
}

Page *BufferPoolManager::FetchPage(page_id_t page_id) {
    std::lock_guard<std::mutex> lock(latch_);

    if (page_id == INVALID_PAGE_ID || page_id < 0) {
        return nullptr;
    }

    // Check if the page is already in the buffer pool
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        frame_id_t frame_id = it->second;
        Page &page = pages_[frame_id];
        page.Pin();
        replacer_->RecordAccess(frame_id);
        replacer_->SetEvictable(frame_id, false);
        return &page;
    }

    // Page miss: find a frame to load the page into
    frame_id_t frame_id;
    if (!FindAvailableFrame(&frame_id)) {
        return nullptr;
    }

    Page &page = pages_[frame_id];
    page.SetPageId(page_id);
    page.SetDirty(false);

    // Read page data from disk
    disk_manager_->ReadPage(page_id, page.GetData());

    page.Pin();
    page_table_[page_id] = frame_id;
    replacer_->RecordAccess(frame_id);
    replacer_->SetEvictable(frame_id, false);

    return &page;
}

bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
    std::lock_guard<std::mutex> lock(latch_);

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }

    frame_id_t frame_id = it->second;
    Page &page = pages_[frame_id];

    if (page.GetPinCount() <= 0) {
        return false;
    }

    if (is_dirty) {
        page.SetDirty(true);
    }

    page.Unpin();

    if (page.GetPinCount() == 0) {
        replacer_->SetEvictable(frame_id, true);
    }

    return true;
}

Page *BufferPoolManager::NewPage(page_id_t *page_id) {
    std::lock_guard<std::mutex> lock(latch_);

    frame_id_t frame_id;
    if (!FindAvailableFrame(&frame_id)) {
        if (page_id != nullptr) {
            *page_id = INVALID_PAGE_ID;
        }
        return nullptr;
    }

    page_id_t new_page_id = disk_manager_->AllocatePage();
    if (page_id != nullptr) {
        *page_id = new_page_id;
    }

    Page &page = pages_[frame_id];
    page.SetPageId(new_page_id);
    page.SetDirty(false);
    std::memset(page.GetData(), 0, PAGE_SIZE);

    page.Pin();
    page_table_[new_page_id] = frame_id;
    replacer_->RecordAccess(frame_id);
    replacer_->SetEvictable(frame_id, false);

    return &page;
}

bool BufferPoolManager::DeletePage(page_id_t page_id) {
    std::lock_guard<std::mutex> lock(latch_);

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return true;
    }

    frame_id_t frame_id = it->second;
    Page &page = pages_[frame_id];

    if (page.GetPinCount() > 0) {
        return false;
    }

    // Stop tracking in page_table_ and replacer
    page_table_.erase(it);
    replacer_->SetEvictable(frame_id, false);

    // Reset frame state
    page.SetPageId(INVALID_PAGE_ID);
    page.SetDirty(false);
    std::memset(page.GetData(), 0, PAGE_SIZE);

    free_list_.push_back(frame_id);
    return true;
}

bool BufferPoolManager::FlushPage(page_id_t page_id) {
    std::lock_guard<std::mutex> lock(latch_);

    if (page_id == INVALID_PAGE_ID) {
        return false;
    }

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }

    frame_id_t frame_id = it->second;
    Page &page = pages_[frame_id];

    if (page.IsDirty()) {
        disk_manager_->WritePage(page_id, page.GetData());
        page.SetDirty(false);
    }

    return true;
}

void BufferPoolManager::FlushAllPages() {
    std::lock_guard<std::mutex> lock(latch_);

    for (const auto &[page_id, frame_id] : page_table_) {
        Page &page = pages_[frame_id];
        if (page.IsDirty()) {
            disk_manager_->WritePage(page_id, page.GetData());
            page.SetDirty(false);
        }
    }
}

size_t BufferPoolManager::GetFreeListSize() {
    std::lock_guard<std::mutex> lock(latch_);
    return free_list_.size();
}

bool BufferPoolManager::IsPageInPool(page_id_t page_id) {
    std::lock_guard<std::mutex> lock(latch_);
    return page_table_.find(page_id) != page_table_.end();
}

frame_id_t BufferPoolManager::GetFrameId(page_id_t page_id) {
    std::lock_guard<std::mutex> lock(latch_);
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        return it->second;
    }
    return INVALID_FRAME_ID;
}

Page *BufferPoolManager::GetPage(frame_id_t frame_id) {
    if (frame_id < pool_size_) {
        return &pages_[frame_id];
    }
    return nullptr;
}
