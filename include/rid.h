#pragma once

#include <cstdint>
#include <string>
#include "config.h"

/**
 * @brief Record Identifier (RID) represents the physical location of a record/tuple.
 * Format: (page_id, slot_num)
 */
class RID {
private:
    page_id_t page_id_{INVALID_PAGE_ID};
    uint32_t slot_num_{0};

public:
    RID() = default;

    RID(page_id_t page_id, uint32_t slot_num)
        : page_id_(page_id), slot_num_(slot_num) {}

    page_id_t GetPageId() const { return page_id_; }
    uint32_t GetSlotNum() const { return slot_num_; }

    void Set(page_id_t page_id, uint32_t slot_num) {
        page_id_ = page_id;
        slot_num_ = slot_num;
    }

    bool operator==(const RID &other) const {
        return page_id_ == other.page_id_ && slot_num_ == other.slot_num_;
    }

    bool operator!=(const RID &other) const {
        return !(*this == other);
    }

    std::string ToString() const {
        return "[" + std::to_string(page_id_) + ", " + std::to_string(slot_num_) + "]";
    }
};
