#pragma once

#include <array>
#include <atomic>
#include <shared_mutex>

#include "config.h"  // PAGE_SIZE, page_id_t, INVALID_PAGE_ID

class Page {
private:
    std::atomic<page_id_t> page_id_;
    std::atomic<int> pin_count_;
    std::atomic<bool> is_dirty_;
    std::shared_mutex latch_;
    std::array<char, PAGE_SIZE> data_;

public:
    Page();

    page_id_t GetPageId() const;
    int GetPinCount() const;
    bool IsDirty() const;
    char *GetData();
    const char *GetData() const;

    void SetPageId(page_id_t id);
    void SetDirty(bool dirty);

    void Pin();
    void Unpin();

    void WLatch();
    void WUnlatch();

    void RLatch();
    void RUnlatch();
};