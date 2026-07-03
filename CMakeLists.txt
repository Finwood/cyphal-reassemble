cmake_minimum_required(VERSION 3.16)
project(cyphal_reassemble LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Arrow REQUIRED)

# Vendored libcanard (C)
add_library(canard STATIC third_party/libcanard/libcanard/canard.c)
target_include_directories(canard PUBLIC
    third_party/libcanard/libcanard
    third_party/libcanard/lib/cavl2)

# Core library
add_library(reassemble_core STATIC
    src/can_id.c
    src/reassembler.cpp
    src/arrow_io.cpp)
target_include_directories(reassemble_core PUBLIC src)
target_link_libraries(reassemble_core PUBLIC canard Arrow::arrow_shared)

add_executable(cyphal-reassemble src/main.cpp src/usage.cpp)
target_link_libraries(cyphal-reassemble PRIVATE reassemble_core)

# Tests
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(unit_tests
    test/test_smoke.cpp
    test/test_can_id.cpp
    test/test_reassembler.cpp
    test/test_arrow_io.cpp)
target_link_libraries(unit_tests PRIVATE reassemble_core GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(unit_tests)

add_executable(golden_tests test/test_golden.cpp)
target_link_libraries(golden_tests PRIVATE reassemble_core GTest::gtest_main)
target_compile_definitions(golden_tests PRIVATE
    FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
gtest_discover_tests(golden_tests)
