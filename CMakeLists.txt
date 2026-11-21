cmake_minimum_required(VERSION 3.20)
project(fastlob VERSION 0.5.0 LANGUAGES CXX)

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

find_package(ZLIB QUIET)
if(ZLIB_FOUND)
  target_link_libraries(fastlob INTERFACE ZLIB::ZLIB)
  target_compile_definitions(fastlob INTERFACE FASTLOB_HAS_ZLIB=1)
endif()

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

add_executable(page_param_sweep bench/page_param_sweep.cpp)
target_link_libraries(page_param_sweep PRIVATE fastlob)
fastlob_apply_warnings(page_param_sweep)

# ---- Sanitizer helpers (use -DFASTLOB_SANITIZER=asan|ubsan|asan+ubsan|tsan) ----
set(FASTLOB_SANITIZER "" CACHE STRING "asan|ubsan|asan+ubsan|tsan")
if(FASTLOB_SANITIZER STREQUAL "asan")
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
elseif(FASTLOB_SANITIZER STREQUAL "ubsan")
  add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=undefined)
elseif(FASTLOB_SANITIZER STREQUAL "asan+ubsan")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
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
    tests/price_index_diff_test.cpp
    tests/page_param_test.cpp
    tests/match_test.cpp
    tests/hot_cold_test.cpp
    tests/itch50_test.cpp
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

  add_executable(price_index_bench bench/price_index_bench.cpp)
  target_link_libraries(price_index_bench PRIVATE fastlob)
  fastlob_apply_warnings(price_index_bench)

  add_executable(itch_replay_bench bench/itch_replay_bench.cpp)
  target_link_libraries(itch_replay_bench PRIVATE fastlob)
  fastlob_apply_warnings(itch_replay_bench)

  # Bench-only third-party books. The core fastlob target does not see them.
  FetchContent_Declare(
    liquibook
    GIT_REPOSITORY https://github.com/enewhuis/liquibook.git
    GIT_TAG 2427613b32f1667abae68a01df6af9ba8270f8e7
  )
  FetchContent_Declare(
    tzadiko_orderbook
    GIT_REPOSITORY https://github.com/Tzadiko/Orderbook.git
    GIT_TAG dd136dd219ead95796f0e396e9e1395542bf673f
  )
  FetchContent_Declare(
    br_orderbook
    GIT_REPOSITORY https://github.com/brprojects/Limit-Order-Book.git
    GIT_TAG af6e5349874649fe196bd6c26653d357f5a751f2
  )
  FetchContent_GetProperties(liquibook)
  if(NOT liquibook_POPULATED)
    FetchContent_Populate(liquibook)
  endif()
  FetchContent_GetProperties(tzadiko_orderbook)
  if(NOT tzadiko_orderbook_POPULATED)
    FetchContent_Populate(tzadiko_orderbook)
  endif()
  # GCC treats '_' as a real name, so two structured bindings in one block fail.
  file(READ "${tzadiko_orderbook_SOURCE_DIR}/Orderbook.cpp" FASTLOB_TZ_SRC)
  string(REPLACE "const auto& [order, _] = entry;"
                 "const auto& [order, location] = entry;"
                 FASTLOB_TZ_SRC "${FASTLOB_TZ_SRC}")
  file(WRITE "${tzadiko_orderbook_SOURCE_DIR}/Orderbook.cpp" "${FASTLOB_TZ_SRC}")
  FetchContent_GetProperties(br_orderbook)
  if(NOT br_orderbook_POPULATED)
    FetchContent_Populate(br_orderbook)
  endif()

  set(FASTLOB_VENDOR_SOURCES
    ${liquibook_SOURCE_DIR}/src/simple/simple_order.cpp
    ${tzadiko_orderbook_SOURCE_DIR}/Orderbook.cpp
    ${br_orderbook_SOURCE_DIR}/Limit_Order_Book/Book.cpp
    ${br_orderbook_SOURCE_DIR}/Limit_Order_Book/Limit.cpp
    ${br_orderbook_SOURCE_DIR}/Limit_Order_Book/Order.cpp
  )
  set(FASTLOB_EXTERNAL_SOURCES
    ${FASTLOB_VENDOR_SOURCES}
    bench/external/run_fastlob.cpp
    bench/external/run_liquibook.cpp
    bench/external/run_tzadiko.cpp
    bench/external/run_br.cpp
  )
  set_source_files_properties(
    ${liquibook_SOURCE_DIR}/src/simple/simple_order.cpp
    ${br_orderbook_SOURCE_DIR}/Limit_Order_Book/Book.cpp
    ${br_orderbook_SOURCE_DIR}/Limit_Order_Book/Limit.cpp
    ${br_orderbook_SOURCE_DIR}/Limit_Order_Book/Order.cpp
    PROPERTIES COMPILE_OPTIONS "-w")
  set_source_files_properties(${tzadiko_orderbook_SOURCE_DIR}/Orderbook.cpp PROPERTIES
    COMPILE_OPTIONS "-w;-include;${CMAKE_SOURCE_DIR}/bench/external/tzadiko_posix.h")

  add_executable(external_compare_bench
    bench/external_compare_bench.cpp
    ${FASTLOB_EXTERNAL_SOURCES})
  target_link_libraries(external_compare_bench PRIVATE fastlob)
  target_include_directories(external_compare_bench PRIVATE
    ${CMAKE_SOURCE_DIR}/bench
    ${CMAKE_SOURCE_DIR}/bench/external
    ${liquibook_SOURCE_DIR}/src
    ${tzadiko_orderbook_SOURCE_DIR}
    ${br_orderbook_SOURCE_DIR}/Limit_Order_Book)
  fastlob_apply_warnings(external_compare_bench)
  if(NOT WIN32)
    target_link_libraries(external_compare_bench PRIVATE pthread)
  endif()

  if(FASTLOB_BUILD_TESTS)
    add_executable(external_book_tests
      tests/external_liquibook_test.cpp
      tests/external_tzadiko_test.cpp
      tests/external_br_test.cpp
      ${FASTLOB_VENDOR_SOURCES})
    target_link_libraries(external_book_tests PRIVATE fastlob GTest::gtest_main pthread)
    target_include_directories(external_book_tests PRIVATE
      ${CMAKE_SOURCE_DIR}
      ${CMAKE_SOURCE_DIR}/tests
      ${CMAKE_SOURCE_DIR}/bench/external
      ${liquibook_SOURCE_DIR}/src
      ${tzadiko_orderbook_SOURCE_DIR}
      ${br_orderbook_SOURCE_DIR}/Limit_Order_Book)
    fastlob_apply_warnings(external_book_tests)
    gtest_discover_tests(external_book_tests)
  endif()
endif()

if(FASTLOB_BUILD_PYTHON)
  FetchContent_MakeAvailable(pybind11)
  pybind11_add_module(pyfastlob python/pyfastlob.cpp)
  target_link_libraries(pyfastlob PRIVATE fastlob)
  fastlob_apply_warnings(pyfastlob)
endif()

message(STATUS "PagedLOB: tests=${FASTLOB_BUILD_TESTS} bench=${FASTLOB_BUILD_BENCH} python=${FASTLOB_BUILD_PYTHON} sanitizer=${FASTLOB_SANITIZER}")
