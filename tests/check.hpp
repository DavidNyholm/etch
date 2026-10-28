// A tiny test harness so the suite needs no third-party framework.
//
//   TEST(name) { CHECK(cond); CHECK_EQ(a, b); }
//   int main(int argc, char** argv) { return check::run(argc, argv); }
//
// Run with a substring argument to select tests: etch_tests async
#pragma once

#include <chrono>
#include <exception>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace check {

struct Test {
  const char* name;
  void (*fn)();
};

inline std::vector<Test>& registry() {
  static std::vector<Test> tests;
  return tests;
}

inline int& current_failures() {
  static int failures = 0;
  return failures;
}

inline void fail(std::string_view what, const char* file, int line) {
  ++current_failures();
  std::println("    FAILED {}:{}: {}", file, line, what);
  std::fflush(stdout);  // reaches a pipe even if a later test hangs the binary
}

inline void require(bool ok, const char* expr, const char* file, int line) {
  if (!ok) fail(expr, file, line);
}

template <class A, class B>
void require_eq(const A& a, const B& b, const char* ea, const char* eb, const char* file,
                int line) {
  if (!(a == b)) {
    if constexpr (std::formattable<A, char> && std::formattable<B, char>) {
      fail(std::format("{} == {}  (got [{}] vs [{}])", ea, eb, a, b), file, line);
    } else {
      fail(std::format("{} == {}", ea, eb), file, line);
    }
  }
}

inline int run(int argc, char** argv) {
  const std::string_view filter = argc > 1 ? argv[1] : "";
  int passed = 0;
  int failed = 0;
  const auto start = std::chrono::steady_clock::now();
  for (const auto& test : registry()) {
    if (!filter.empty() && std::string_view(test.name).find(filter) == std::string_view::npos) {
      continue;
    }
    const int before = current_failures();
    try {
      test.fn();
    } catch (const std::exception& e) {
      fail(std::format("unexpected exception: {}", e.what()), "?", 0);
    } catch (...) {
      fail("unexpected non-std exception", "?", 0);
    }
    const bool ok = current_failures() == before;
    std::println("{} {}", ok ? "  ok  " : "  FAIL", test.name);
    std::fflush(stdout);  // the mutation script reads this through a pipe, hang or not
    (ok ? passed : failed)++;
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  std::println("\n{} passed, {} failed, {} total, {} ms", passed, failed, passed + failed,
               std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
  return failed == 0 ? 0 : 1;
}

}  // namespace check

#define TEST(name)                                                                           \
  static void name();                                                                        \
  static const bool name##_registered = (check::registry().push_back({#name, &name}), true); \
  static void name()

#define CHECK(cond) check::require(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b) check::require_eq((a), (b), #a, #b, __FILE__, __LINE__)
