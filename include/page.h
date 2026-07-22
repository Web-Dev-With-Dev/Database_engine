#pragma once

#include <array>
#include <shared_mutex>
#include <cstdint>
#include <cstddef>

constexpr size_t PAGE_SIZE = 4096;
using page_id_t = int32_t;

class Page {
private:
    page_id_t page_id_;
    int pin_count_;
    bool is_dirty_;
    std::shared_mutex latch_;
    std::array<char, PAGE_SIZE> data_;

public:
    Page();

    page_id_t GetPageId() const;
    int GetPinCount() const;
    bool IsDirty() const;
    char *GetData();

    void SetPageId(page_id_t id);
    void SetDirty(bool dirty);

    void Pin();
    void Unpin();

    void WLatch();
    void WUnlatch();

    void RLatch();
    void RUnlatch();
};