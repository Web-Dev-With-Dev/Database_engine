#pragma once

#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "disk_manager.h"
#include "lru_k_replacer.h"
#include "page.h"

class BufferPoolManager {
public:
    /**
     * @brief Construct a new BufferPoolManager instance.
     * @param pool_size Maximum number of pages that can be held in memory.
     * @param disk_manager Pointer to the underlying DiskManager.
     * @param k The k parameter for the LRU-K replacer policy (default: 2).
     */
    BufferPoolManager(size_t pool_size, DiskManager *disk_manager, size_t k = 2);

    BufferPoolManager(size_t pool_size, std::shared_ptr<DiskManager> disk_manager, size_t k = 2);

    ~BufferPoolManager();

    // Disable copy and move semantics
    BufferPoolManager(const BufferPoolManager &) = delete;
    BufferPoolManager &operator=(const BufferPoolManager &) = delete;
    BufferPoolManager(BufferPoolManager &&) = delete;
    BufferPoolManager &operator=(BufferPoolManager &&) = delete;

    Page *FetchPage(page_id_t page_id);

    bool UnpinPage(page_id_t page_id, bool is_dirty);

    Page *NewPage(page_id_t *page_id);

    bool DeletePage(page_id_t page_id);

    bool FlushPage(page_id_t page_id);

    void FlushAllPages();

    // Diagnostics & Inspection
    size_t GetPoolSize() const { return pool_size_; }
    size_t GetFreeListSize();
    bool IsPageInPool(page_id_t page_id);
    frame_id_t GetFrameId(page_id_t page_id);
    Page *GetPages() { return pages_.get(); }
    Page *GetPage(frame_id_t frame_id);

private:

    bool FindAvailableFrame(frame_id_t *frame_id);

    size_t pool_size_;
    DiskManager *disk_manager_{nullptr};
    std::shared_ptr<DiskManager> disk_manager_sp_{nullptr};
    std::unique_ptr<LRUKReplacer> replacer_;

    // In-memory contiguous frame array
    std::unique_ptr<Page[]> pages_;

    // Lookup: page_id -> frame_id
    std::unordered_map<page_id_t, frame_id_t> page_table_;

    // Unassigned frames
    std::list<frame_id_t> free_list_;

    // Concurrency control for BufferPoolManager metadata
    std::mutex latch_;
};
