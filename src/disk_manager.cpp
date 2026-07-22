#include "disk_manager.h"

#include <cstring>
#include <stdexcept>

DiskManager::DiskManager(const std::string &file_name)
    : file_name_(file_name), next_page_id_(0)
{
    db_file_.open(file_name,
                  std::ios::binary |
                  std::ios::in |
                  std::ios::out);

    if (!db_file_.is_open())
    {
        std::ofstream create(file_name, std::ios::binary);

        if (!create.is_open())
        {
            throw std::runtime_error("Failed to create database file: " + file_name);
        }

        create.close();

        db_file_.open(file_name,
                      std::ios::binary |
                      std::ios::in |
                      std::ios::out);

        if (!db_file_.is_open())
        {
            throw std::runtime_error("Failed to open database file: " + file_name);
        }
    }

    db_file_.seekg(0, std::ios::end);

    std::streamoff size = db_file_.tellg();

    if (size == -1)
    {
        throw std::runtime_error("Failed to determine database file size.");
    }

    if (size % PAGE_SIZE != 0)
    {
        throw std::runtime_error("Database file is corrupted.");
    }

    next_page_id_ = static_cast<page_id_t>(size / PAGE_SIZE);
}

page_id_t DiskManager::AllocatePage()
{
    return next_page_id_++;
}

void DiskManager::WritePage(page_id_t page_id, const char *data)
{
    const size_t offset = static_cast<size_t>(page_id) * PAGE_SIZE;

    db_file_.seekg(0, std::ios::end);

    if (!db_file_)
    {
        throw std::runtime_error("Failed to seek to end of file.");
    }

    std::streamoff file_size = db_file_.tellg();

    if (file_size == -1)
    {
        throw std::runtime_error("Failed to determine file size.");
    }

    if (file_size % PAGE_SIZE != 0)
    {
        throw std::runtime_error("Corrupted database file.");
    }

    db_file_.seekp(0, std::ios::end);

    if (!db_file_)
    {
        throw std::runtime_error("Failed to seek write pointer.");
    }

    char zero_page[PAGE_SIZE] = {};

    while (static_cast<size_t>(file_size) < offset)
    {
        db_file_.write(zero_page, PAGE_SIZE);

        if (!db_file_)
        {
            throw std::runtime_error("Failed while zero-filling pages.");
        }

        file_size += PAGE_SIZE;
    }

    db_file_.seekp(offset);

    if (!db_file_)
    {
        throw std::runtime_error("Failed to seek page.");
    }

    db_file_.write(data, PAGE_SIZE);

    if (!db_file_)
    {
        throw std::runtime_error("Failed to write page.");
    }

    db_file_.flush();

    if (!db_file_)
    {
        throw std::runtime_error("Failed to flush file.");
    }
}

void DiskManager::ReadPage(page_id_t page_id, char *buffer)
{
    std::memset(buffer, 0, PAGE_SIZE);

    size_t offset = static_cast<size_t>(page_id) * PAGE_SIZE;

    db_file_.seekg(offset);

    if (!db_file_)
    {
        throw std::runtime_error("Failed to seek.");
    }

    db_file_.read(buffer, PAGE_SIZE);

    std::streamsize bytes_read = db_file_.gcount();

    if (bytes_read < PAGE_SIZE)
    {
        std::memset(buffer + bytes_read, 0, PAGE_SIZE - bytes_read);

        db_file_.clear();
    }
}

DiskManager::~DiskManager()
{
    if (db_file_.is_open())
    {
        db_file_.close();
    }
}