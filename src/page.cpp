#include "page.h"

Page::Page()
    : page_id_(-1),
      pin_count_(0),
      is_dirty_(false)
{
    data_.fill(0);
}

//Getters

page_id_t Page::GetPageId() const
{
    return page_id_.load();
}

int Page::GetPinCount() const
{
    return pin_count_.load();
}

bool Page::IsDirty() const
{
    return is_dirty_.load();
}

char *Page::GetData()
{
    return data_.data();
}

const char *Page::GetData() const
{
    return data_.data();
}

//Setters

void Page::SetPageId(page_id_t id)
{
    page_id_.store(id);
}

void Page::SetDirty(bool dirty)
{
    is_dirty_.store(dirty);
}

//Pin Management

void Page::Pin()
{
    ++pin_count_;
}

void Page::Unpin()
{
    int current = pin_count_.load();
    while (current > 0 && !pin_count_.compare_exchange_weak(current, current - 1))
    {
    }
}

//Latching

void Page::WLatch()
{
    latch_.lock();
}

void Page::WUnlatch()
{
    latch_.unlock();
}

void Page::RLatch()
{
    latch_.lock_shared();
}

void Page::RUnlatch()
{
    latch_.unlock_shared();
}