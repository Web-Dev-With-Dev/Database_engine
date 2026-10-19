#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "rid.h"

/**
 * @brief Represents a record / tuple stored in a TablePage.
 */
class Tuple {
private:
    RID rid_{};
    std::vector<char> data_;

public:
    Tuple() = default;

    explicit Tuple(RID rid) : rid_(rid) {}

    Tuple(const char *data, uint32_t size, RID rid = {})
        : rid_(rid), data_(data, data + size) {}

    Tuple(std::string_view sv, RID rid = {})
        : rid_(rid), data_(sv.begin(), sv.end()) {}

    Tuple(const std::string &str, RID rid = {})
        : rid_(rid), data_(str.begin(), str.end()) {}

    Tuple(const char *str, RID rid = {})
        : rid_(rid), data_(str, str + std::strlen(str)) {}

    Tuple(std::vector<char> data, RID rid = {})
        : rid_(rid), data_(std::move(data)) {}

    RID GetRid() const { return rid_; }
    void SetRid(RID rid) { rid_ = rid; }

    const char *GetData() const { return data_.data(); }
    char *GetData() { return data_.data(); }

    uint32_t GetLength() const { return static_cast<uint32_t>(data_.size()); }
    uint32_t GetSize() const { return static_cast<uint32_t>(data_.size()); }

    std::string_view AsStringView() const {
        return std::string_view(data_.data(), data_.size());
    }

    std::span<const char> AsSpan() const {
        return std::span<const char>(data_.data(), data_.size());
    }

    bool IsEmpty() const { return data_.empty(); }

    bool operator==(const Tuple &other) const {
        return rid_ == other.rid_ && data_ == other.data_;
    }

    bool operator!=(const Tuple &other) const {
        return !(*this == other);
    }
};
