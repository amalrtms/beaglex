cmake_minimum_required(VERSION 3.24)
project(beaglex VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BGX_BUILD_TESTS "Build unit and fuzz tests" ON)
option(BGX_BUILD_BENCH "Build the order book benchmark" ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_library(beaglex
    src/order_book.cpp
    src/baseline_order_book.cpp
    src/types.cpp
    src/sim/simulation.cpp
    src/sim/agents.cpp
    src/sim/population.cpp
    src/analysis/stats.cpp
    src/analysis/stylized_facts.cpp
    src/evo/genome.cpp
    src/evo/evolved_trader.cpp
    src/evo/evolution.cpp
)
target_include_directories(beaglex PUBLIC include)

add_executable(beaglex_demo apps/demo.cpp)
target_link_libraries(beaglex_demo PRIVATE beaglex)

add_executable(beaglex_sim apps/sim.cpp)
target_link_libraries(beaglex_sim PRIVATE beaglex)

add_executable(beaglex_validate apps/validate.cpp)
target_link_libraries(beaglex_validate PRIVATE beaglex)

add_executable(beaglex_evolve apps/evolve.cpp)
target_link_libraries(beaglex_evolve PRIVATE beaglex)

if(BGX_BUILD_BENCH)
    add_executable(beaglex_bench bench/bench.cpp)
    target_link_libraries(beaglex_bench PRIVATE beaglex)
endif()

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
        tests/order_index_test.cpp
        tests/sim_test.cpp
        tests/stats_test.cpp
        tests/evo_test.cpp
    )
    target_link_libraries(beaglex_tests PRIVATE beaglex GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(beaglex_tests)
endif()
