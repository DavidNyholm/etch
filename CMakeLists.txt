cmake_minimum_required(VERSION 3.28)

project(etch
  VERSION 0.3.0
  DESCRIPTION "A small header-only C++23 logger"
  LANGUAGES CXX)

# The library: header-only, so an INTERFACE target. Consumers do
#   target_link_libraries(app PRIVATE etch::etch)
include(GNUInstallDirs)
add_library(etch INTERFACE)
add_library(etch::etch ALIAS etch)
target_include_directories(etch INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(etch INTERFACE cxx_std_23)

# MSVC's std::print writes UTF-8 to the console correctly only when the translation unit's
# literal encoding is UTF-8, so consumers inherit /utf-8. ETCH_MSVC_UTF8=OFF opts out.
option(ETCH_MSVC_UTF8 "Compile consumers of etch with /utf-8 on MSVC" ON)
if(ETCH_MSVC_UTF8)
  target_compile_options(etch INTERFACE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
endif()

# The compile-time level floor must be one value for the whole program; setting it here puts
# it on every target that links etch. 0 = trace (nothing compiled out) .. 5 = fatal.
set(ETCH_MIN_LEVEL "" CACHE STRING "Compile out calls below this level (0..5) in every consumer")
if(NOT ETCH_MIN_LEVEL STREQUAL "")
  # etch's own test targets set their floor themselves and opt out via this property.
  target_compile_definitions(etch INTERFACE
    $<$<NOT:$<BOOL:$<TARGET_PROPERTY:ETCH_OWN_MIN_LEVEL>>>:ETCH_MIN_LEVEL=${ETCH_MIN_LEVEL}>)
endif()

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

option(ETCH_INSTALL "Generate install rules and an etchConfig.cmake for find_package" ${PROJECT_IS_TOP_LEVEL})
if(ETCH_INSTALL)
  include(CMakePackageConfigHelpers)
  install(DIRECTORY include/etch DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(TARGETS etch EXPORT etchTargets)
  install(EXPORT etchTargets NAMESPACE etch:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/etch)
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/etchConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/etchConfig.cmake"
    "include(CMakeFindDependencyMacro)
find_dependency(Threads)
include(\"\${CMAKE_CURRENT_LIST_DIR}/etchTargets.cmake\")
")
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/etchConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/etchConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/etch)
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

  set(ETCH_TEST_SOURCES
    tests/main.cpp
    tests/test_levels_logger.cpp
    tests/test_format.cpp
    tests/test_sinks.cpp
    tests/test_async.cpp
    tests/test_json.cpp
    tests/test_agent.cpp)

  add_executable(etch_tests ${ETCH_TEST_SOURCES})
  set_property(TARGET etch_tests PROPERTY ETCH_OWN_MIN_LEVEL ON)  # the suite assumes floor 0
  target_link_libraries(etch_tests PRIVATE etch::etch)
  etch_strict_warnings(etch_tests)
  add_test(NAME etch_tests COMMAND etch_tests)
  set_tests_properties(etch_tests PROPERTIES TIMEOUT 120)

  # Same suite with trace, debug and info compiled out, to prove ETCH_MIN_LEVEL works.
  add_executable(etch_tests_minlevel ${ETCH_TEST_SOURCES})
  set_property(TARGET etch_tests_minlevel PROPERTY ETCH_OWN_MIN_LEVEL ON)
  target_link_libraries(etch_tests_minlevel PRIVATE etch::etch)
  target_compile_definitions(etch_tests_minlevel PRIVATE ETCH_MIN_LEVEL=3)
  etch_strict_warnings(etch_tests_minlevel)
  add_test(NAME etch_tests_minlevel COMMAND etch_tests_minlevel)
  set_tests_properties(etch_tests_minlevel PROPERTIES TIMEOUT 120)
endif()

if(ETCH_BUILD_BENCH)
  add_executable(etch_bench bench/bench.cpp)
  target_link_libraries(etch_bench PRIVATE etch::etch)
  etch_strict_warnings(etch_bench)
endif()
