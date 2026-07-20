#pragma once

#include <array>
#include <shared_mutex>
#include <cstdint>
#include <cstddef>

constexpr size_t PAGE_SIZE = 4096;
using page_id_t = int32_t;

class Page {
    
    private :

    page_id_t page_id_;
    int pin_count_;
    bool is_dirty_;
    std::shared_mutex latch_;
    std::array<char, PAGE_SIZE> data_;

    public:

    Page()
    {
        page_id_ = -1;
        pin_count_ = 0;
        is_dirty_ = false;

        data_.fill(0);
    }

    page_id_t GetPageId() const
    {
        return page_id_;
    }

    int GetPinCount() const
    {
        return pin_count_;
    }

    bool IsDirty() const
    {
        return is_dirty_;
    }

    char* GetData()
    {
        return data_.data();
    }

    void SetPageId(page_id_t id)
    {
        page_id_ = id;
    }

    void SetDirty(bool dirty)
    {
        is_dirty_ = dirty;
    }

    void Pin()
    {
        pin_count_++;
    }

    void Unpin()
    {
        if(pin_count_ > 0)
            pin_count_--;
    }

    void WLatch()
    {
        latch_.lock();
    }

    void WUnlatch()
    {
        latch_.unlock();
    }

    void RLatch()
    {
        latch_.lock_shared();
    }

    void RUnlatch()
    {
        latch_.unlock_shared();
    }
};