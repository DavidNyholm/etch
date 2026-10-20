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
#include <filesystem>
#include <fstream>
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

#include "check.hpp"

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
