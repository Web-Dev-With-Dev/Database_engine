#include "disk_manager.h"
#include <cstring>
#include <iostream>
#include <stdexcept>

int main(int argc, char* argv[]) {
    // Optional: pass a database file path as first argument.
    // Defaults to the Data/ directory relative to the project root.
    const char* db_path = (argc > 1) ? argv[1] : "../Data/database.db";

    std::cout << "Database Engine Demo\n";
    std::cout << "Opening: " << db_path << "\n\n";

    try {
        DiskManager disk(db_path);

        // Allocate and write a page
        page_id_t id = disk.AllocatePage();
        std::cout << "Allocated page id: " << id << "\n";

        char page[PAGE_SIZE];
        memset(page, 0, PAGE_SIZE);
        strcpy(page, "Hello Database!");

        disk.WritePage(id, page);
        std::cout << "Written to page " << id << ": \"" << page << "\"\n";

        // Read it back
        char buf[PAGE_SIZE];
        disk.ReadPage(id, buf);
        std::cout << "Read from page  " << id << ": \"" << buf << "\"\n";

        std::cout << "\nNext free page id: " << disk.GetNextPageId() << "\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}