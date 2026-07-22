#pragma once

#include <fstream>
#include <string>
#include <mutex>
#include <cstddef>

#include "config.h"

class DiskManager {
private:
    std::fstream db_file_;
    std::string file_name_;
    page_id_t next_page_id_;

public:
    explicit DiskManager(const std::string &file_name);

    page_id_t AllocatePage();

    void WritePage(page_id_t page_id, const char *data);

    void ReadPage(page_id_t page_id, char *buffer);

    static std::mutex file_mutex_;

    ~DiskManager();
};