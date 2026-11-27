cmake_minimum_required(VERSION 3.16)
project(canedge_mf4 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Arrow REQUIRED)

add_library(canedge_mf4_core STATIC
    cpp/crc32.cpp
    cpp/mfc.cpp
    cpp/third_party/heatshrink/heatshrink_decoder.c
)
target_include_directories(canedge_mf4_core PUBLIC cpp cpp/third_party/heatshrink)
target_link_libraries(canedge_mf4_core PUBLIC Arrow::arrow_shared)

include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(canedge_mf4_tests
    tests/cpp/test_crc.cpp
    tests/cpp/test_mfc.cpp
    cpp/third_party/heatshrink/heatshrink_encoder.c
)
target_link_libraries(canedge_mf4_tests PRIVATE canedge_mf4_core GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(canedge_mf4_tests)

add_executable(mfc-wrap
    cpp/mfc_wrap.cpp
    cpp/third_party/heatshrink/heatshrink_encoder.c
)
target_link_libraries(mfc-wrap PRIVATE canedge_mf4_core)