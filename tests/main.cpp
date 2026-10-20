// etch tests: entry point. Each test_*.cpp registers its tests at static-init time.
#include <filesystem>
#include <print>
#include <system_error>

#include "check.hpp"
#include "support.hpp"

int main(int argc, char** argv) {
  std::println("etch tests (ETCH_MIN_LEVEL={}), scratch files in {}", ETCH_MIN_LEVEL,
               scratch_dir().string());
  const int result = check::run(argc, argv);
  std::error_code ec;
  std::filesystem::remove_all(scratch_dir(), ec);
  return result;
}
