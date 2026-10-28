// Shared helpers for the etch test suite: a capturing logger, a scratch directory, a fixed
// record, and small utilities. Everything is inline so every test file sees one definition.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <etch/etch.hpp>

#if !defined(_WIN32)
#include <stdlib.h>  // setenv, unsetenv

#include <sys/wait.h>  // WIFEXITED/WEXITSTATUS, to decode what std::system returns
#endif

#include "check.hpp"

// The path of this test binary, so a test can start a fresh copy of it (first-use tests).
inline std::string& test_executable() {
  static std::string path;
  return path;
}

inline void set_environment(const char* name, const char* value) {
#if defined(_WIN32)
  _putenv_s(name, value);
#else
  if (std::string_view(value).empty()) {
    unsetenv(name);
  } else {
    setenv(name, value, 1);
  }
#endif
}

// Runs one test of this binary in a fresh process with the given environment variables set,
// and returns its exit code (-1 when the binary's path is unknown). The child is marked by
// ETCH_TEST_CHILD; a probe test checks its conditions only there (in_child()), where nothing
// has touched the default logger before it, and passes vacuously in a normal run.
constexpr const char* kChildMarker = "ETCH_TEST_CHILD";
inline bool in_child() { return !etch::detail::env(kChildMarker).empty(); }
inline int run_test_in_child(
    const char* test_name,
    std::initializer_list<std::pair<const char*, const char*>> environment = {}) {
  if (test_executable().empty()) return -1;
  for (const auto& [name, value] : environment) set_environment(name, value);
  set_environment(kChildMarker, "1");
  // The path has a space in it on this project, so it is quoted. cmd.exe strips one outer pair
  // of quotes from the whole line, so it gets a second pair; sh would merge that pair with the
  // path into one program name, so it gets the plain form.
  const std::string quoted = "\"" + test_executable() + "\" " + test_name;
#if defined(_WIN32)
  const std::string command = "\"" + quoted + "\"";
  const int code = std::system(command.c_str());
#else
  const int status = std::system(quoted.c_str());
  const int code = WIFEXITED(status) ? WEXITSTATUS(status) : status;
#endif
  set_environment(kChildMarker, "");
  for (const auto& [name, value] : environment) set_environment(name, "");
  return code;
}

struct Captured {
  std::shared_ptr<etch::MemorySink> sink = std::make_shared<etch::MemorySink>();
  etch::Logger log;
  explicit Captured(std::string name = "unit", etch::Logger::Options options = {})
      : log(std::move(name), {sink}, options) {}
  std::vector<std::string> messages() const { return sink->messages(); }
};

// One directory per test process, so the two CTest binaries can run in parallel without
// deleting each other's files. main() removes it at the end.
inline std::filesystem::path scratch_dir() {
  static const auto dir = [] {
    auto d = std::filesystem::temp_directory_path() /
             std::format("etch-tests-{:08x}", std::random_device{}());
    std::filesystem::create_directories(d);
    return d;
  }();
  return dir;
}

// The file's bytes exactly, for checks that "untouched" means untouched.
inline std::string read_bytes(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

inline std::vector<std::string> read_lines(const std::filesystem::path& p) {
  std::ifstream in(p);
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);) lines.push_back(line);
  return lines;
}

template <class L>
concept ScopeCallable = requires(L logger) { std::forward<L>(logger).scope("x"); };

inline constexpr std::string_view kEsc = "\x1b[";

inline etch::Record fixed_record() {
  using namespace std::chrono;
  etch::Record r;
  r.level = etch::Level::kInfo;
  r.time = sys_days{2026y / January / 2} + 3h + 4min + 5s + 678ms;
  r.logger_name = "unit";
  r.message = "hello";
  r.where = std::source_location::current();
  return r;
}
