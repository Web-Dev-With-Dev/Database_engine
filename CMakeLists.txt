cmake_minimum_required(VERSION 3.20)

project(DatabaseEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

add_executable(database
    src/main.cpp
    src/page.cpp
)

enable_testing()

find_package(GTest REQUIRED)

add_executable(page_test
    tests/page_test.cpp
    src/page.cpp
)

target_include_directories(page_test PRIVATE include)

target_link_libraries(page_test PRIVATE
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(page_test)