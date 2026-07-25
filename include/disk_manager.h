#pragma once

#include <fstream>
#include <string>
#include <mutex>
#include <memory>
#include <cstddef>

#include "config.h"

class DiskManager {
private:
    std::fstream db_file_;
    std::string file_name_;
    page_id_t next_page_id_;
    // Shared mutex keyed by file path — instances on the same file serialize I/O
    std::shared_ptr<std::mutex> file_mutex_;

public:
    explicit DiskManager(const std::string &file_name);

    page_id_t AllocatePage();
    page_id_t GetNextPageId() const;  // Non-mutating peek at the allocation cursor

    void WritePage(page_id_t page_id, const char *data);

    void ReadPage(page_id_t page_id, char *buffer);

    ~DiskManager();
};