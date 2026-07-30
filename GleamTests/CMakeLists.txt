cmake_minimum_required(VERSION 3.14)
project(GleamTests)

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use dynamic runtime library (/MD) to match Google Test
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

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
    test_ilt_disambiguation.cpp
    test_error.cpp
    test_history.cpp
    test_memory.cpp
    test_raii.cpp
    test_exception.cpp
)

# Don't include GleamIltDisambiguation.cpp - it depends on full GleeBug headers

# Link Google Test
target_link_libraries(GleamTests
    gtest
    gtest_main
)

# Enable testing
enable_testing()
add_test(NAME GleamTests COMMAND GleamTests)
