cmake_minimum_required(VERSION 3.14)
project(GleamTests)

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Google Test
add_subdirectory(../googletest googletest EXCLUDE_FROM_ALL)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/../Gleam
    ${CMAKE_SOURCE_DIR}/../GleeBug
)

# Test executable
add_executable(GleamTests
    test_main.cpp
    test_caches.cpp
    test_constants.cpp
)

# Link Google Test
target_link_libraries(GleamTests
    gtest
    gtest_main
)

# Enable testing
enable_testing()
add_test(NAME GleamTests COMMAND GleamTests)
