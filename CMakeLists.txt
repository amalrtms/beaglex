cmake_minimum_required(VERSION 3.24)
project(beaglex VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BGX_BUILD_TESTS "Build unit and fuzz tests" ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_library(beaglex
    src/order_book.cpp
    src/types.cpp
)
target_include_directories(beaglex PUBLIC include)

add_executable(beaglex_demo apps/demo.cpp)
target_link_libraries(beaglex_demo PRIVATE beaglex)

if(BGX_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/releases/download/v1.15.2/googletest-1.15.2.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_executable(beaglex_tests
        tests/order_book_test.cpp
        tests/fuzz_test.cpp
    )
    target_link_libraries(beaglex_tests PRIVATE beaglex GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(beaglex_tests)
endif()
