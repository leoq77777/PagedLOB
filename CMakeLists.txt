cmake_minimum_required(VERSION 3.20)
project(fastlob VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FASTLOB_BUILD_TESTS "Build unit / differential tests" ON)
option(FASTLOB_BUILD_BENCH "Build Google Benchmark targets" ON)
option(FASTLOB_BUILD_PYTHON "Build pybind11 module" ON)
option(FASTLOB_ENABLE_ALLOC_PROBE "Override global new for ScopedAllocProbe" ON)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_Declare(
  benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.9.1
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)

if(FASTLOB_BUILD_PYTHON)
  FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG v2.13.6
  )
endif()

add_library(fastlob INTERFACE)
target_include_directories(fastlob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(fastlob_alloc_probe STATIC src/alloc_probe.cpp)
target_include_directories(fastlob_alloc_probe PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(FASTLOB_ENABLE_ALLOC_PROBE)
  target_compile_definitions(fastlob_alloc_probe PUBLIC FASTLOB_ENABLE_ALLOC_PROBE=1)
endif()

function(fastlob_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

# ---- Sanitizer helpers (use -DFASTLOB_SANITIZER=asan|ubsan|tsan) ----
set(FASTLOB_SANITIZER "" CACHE STRING "asan|ubsan|tsan")
if(FASTLOB_SANITIZER STREQUAL "asan")
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
elseif(FASTLOB_SANITIZER STREQUAL "ubsan")
  add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=undefined)
elseif(FASTLOB_SANITIZER STREQUAL "tsan")
  add_compile_options(-fsanitize=thread)
  add_link_options(-fsanitize=thread)
endif()

if(FASTLOB_BUILD_TESTS)
  FetchContent_MakeAvailable(googletest)
  enable_testing()

  add_executable(fastlob_tests
    tests/order_layout_test.cpp
    tests/reference_lob_test.cpp
    tests/fast_lob_test.cpp
    tests/differential_lob_test.cpp
    tests/zero_alloc_hotpath_test.cpp
    tests/execution_sim_test.cpp
    tests/replay_test.cpp
  )
  target_link_libraries(fastlob_tests PRIVATE fastlob fastlob_alloc_probe GTest::gtest_main)
  fastlob_apply_warnings(fastlob_tests)
  include(GoogleTest)
  gtest_discover_tests(fastlob_tests)
endif()

if(FASTLOB_BUILD_BENCH)
  FetchContent_MakeAvailable(benchmark)
  add_executable(fastlob_bench bench/lob_bench.cpp)
  target_link_libraries(fastlob_bench PRIVATE fastlob benchmark::benchmark benchmark::benchmark_main)
  fastlob_apply_warnings(fastlob_bench)

  add_executable(fastlob_latency_hist bench/latency_hist.cpp)
  target_link_libraries(fastlob_latency_hist PRIVATE fastlob)
  fastlob_apply_warnings(fastlob_latency_hist)
endif()

if(FASTLOB_BUILD_PYTHON)
  FetchContent_MakeAvailable(pybind11)
  pybind11_add_module(pyfastlob python/pyfastlob.cpp)
  target_link_libraries(pyfastlob PRIVATE fastlob)
  fastlob_apply_warnings(pyfastlob)
endif()

message(STATUS "FastLOB: tests=${FASTLOB_BUILD_TESTS} bench=${FASTLOB_BUILD_BENCH} python=${FASTLOB_BUILD_PYTHON} sanitizer=${FASTLOB_SANITIZER}")
