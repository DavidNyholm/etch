cmake_minimum_required(VERSION 3.28)

project(etch
  VERSION 0.2.0
  DESCRIPTION "A small header-only C++23 logger"
  LANGUAGES CXX)

# The library: header-only, so an INTERFACE target. Consumers do
#   target_link_libraries(app PRIVATE etch::etch)
add_library(etch INTERFACE)
add_library(etch::etch ALIAS etch)
target_include_directories(etch INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(etch INTERFACE cxx_std_23)

# The async sink starts a std::jthread; on some platforms that needs an explicit link.
find_package(Threads REQUIRED)
target_link_libraries(etch INTERFACE Threads::Threads)

# libstdc++ keeps std::stacktrace in a separate library (libstdc++exp). Detect by trying,
# so Clang on libstdc++ gets it too and Clang on libc++ (no <stacktrace>) is left alone.
if(NOT MSVC)
  include(CheckCXXSourceCompiles)
  set(_etch_stacktrace_probe "
    #include <stacktrace>
    int main() { return static_cast<int>(std::stacktrace::current().size()) >= 0 ? 0 : 1; }")
  set(CMAKE_REQUIRED_FLAGS "-std=c++23")
  check_cxx_source_compiles("${_etch_stacktrace_probe}" ETCH_STACKTRACE_LINKS_ALONE)
  if(NOT ETCH_STACKTRACE_LINKS_ALONE)
    set(CMAKE_REQUIRED_LIBRARIES stdc++exp)
    check_cxx_source_compiles("${_etch_stacktrace_probe}" ETCH_STACKTRACE_NEEDS_STDCXXEXP)
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(ETCH_STACKTRACE_NEEDS_STDCXXEXP)
      target_link_libraries(etch INTERFACE stdc++exp)
    endif()
  endif()
  unset(CMAKE_REQUIRED_FLAGS)
endif()

option(ETCH_BUILD_EXAMPLES "Build the demo program" ${PROJECT_IS_TOP_LEVEL})
option(ETCH_BUILD_TESTS "Build the tests and register them with CTest" ${PROJECT_IS_TOP_LEVEL})
option(ETCH_BUILD_BENCH "Build the calling-thread cost benchmark" ${PROJECT_IS_TOP_LEVEL})

# Strict warnings for our own executables only; never forced on consumers of the library.
function(etch_strict_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

if(ETCH_BUILD_EXAMPLES)
  add_executable(etch_demo examples/demo.cpp)
  target_link_libraries(etch_demo PRIVATE etch::etch)
  etch_strict_warnings(etch_demo)
endif()

if(ETCH_BUILD_TESTS)
  enable_testing()

  add_executable(etch_tests tests/tests.cpp)
  target_link_libraries(etch_tests PRIVATE etch::etch)
  etch_strict_warnings(etch_tests)
  add_test(NAME etch_tests COMMAND etch_tests)

  # Same suite with trace, debug and info compiled out, to prove ETCH_MIN_LEVEL works.
  add_executable(etch_tests_minlevel tests/tests.cpp)
  target_link_libraries(etch_tests_minlevel PRIVATE etch::etch)
  target_compile_definitions(etch_tests_minlevel PRIVATE ETCH_MIN_LEVEL=3)
  etch_strict_warnings(etch_tests_minlevel)
  add_test(NAME etch_tests_minlevel COMMAND etch_tests_minlevel)
endif()

if(ETCH_BUILD_BENCH)
  add_executable(etch_bench bench/bench.cpp)
  target_link_libraries(etch_bench PRIVATE etch::etch)
  etch_strict_warnings(etch_bench)
endif()
