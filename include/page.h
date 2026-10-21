#pragma once

#include <array>
#include <atomic>

#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#else
#include <shared_mutex>
#endif

#include "config.h"  // PAGE_SIZE, page_id_t, INVALID_PAGE_ID

class Page {
private:
    std::atomic<page_id_t> page_id_;
    std::atomic<int> pin_count_;
    std::atomic<bool> is_dirty_;
#if defined(__unix__) || defined(__APPLE__)
    mutable pthread_rwlock_t latch_;
#else
    mutable std::shared_mutex latch_;
#endif
    std::array<char, PAGE_SIZE> data_;

public:
    Page();
    virtual ~Page();

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