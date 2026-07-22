#include "disk_manager.h"
#include <cstring>
#include <iostream>

int main() {

    std::cout << "Program started\n";

    DiskManager disk("../Data/database.db");

    page_id_t id = disk.AllocatePage();

    char page[PAGE_SIZE];

    memset(page, 0, PAGE_SIZE);

    strcpy(page, "Hello Database!");

    disk.WritePage(id, page);

    char read[PAGE_SIZE];

    disk.ReadPage(id, read);

    std::cout << "Read: " << read << std::endl;

    return 0;
}