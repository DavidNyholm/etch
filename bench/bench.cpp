// etch benchmark: what one log call costs the calling thread, step by step.
//
// Build with the Release preset and run etch_bench. Every number is an average over many calls
// on one machine; see README.md for the figures this produced and what they mean.
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <print>
#include <string>
#include <thread>
#include <vector>

#include <etch/etch.hpp>

namespace {

using Clock = std::chrono::steady_clock;

template <class F>
double ns_per_call(int n, F f) {
  const auto start = Clock::now();
  for (int i = 0; i < n; ++i) f();
  return std::chrono::duration<double, std::nano>(Clock::now() - start).count() / n;
}

// The cheapest possible sink, to measure everything in front of it.
struct NullSink final : etch::Sink {
  void write(const etch::Record&) override {}
};

void row(std::string_view what, double ns, std::string_view note = "") {
  std::println("  {:<34} {:>8.0f} ns  {}", what, ns, note);
}

}  // namespace

int main() {
  constexpr int kCalls = 200'000;
  constexpr std::size_t kBigQueue = 1u << 20;  // large enough that the queue never fills
  constexpr int kThreads = 4;
  std::string s;

  std::println("etch benchmark: {} calls per row, Release build expected", kCalls);
  std::println("-- primitives --");
  row("system_clock::now", ns_per_call(kCalls, [&] {
        auto t = std::chrono::system_clock::now();
        (void)t;
      }));
  row("this_thread::get_id", ns_per_call(kCalls, [&] {
        auto t = std::this_thread::get_id();
        (void)t;
      }));
  row("std::format, 22-char message",
      ns_per_call(kCalls, [&] { s = std::format("thread {} message {}", 3, 12345); }));
  row("std::string allocate 22 chars", ns_per_call(kCalls, [&] {
        std::string x("thread 3 message 12345");
        (void)x;
      }));

  std::println("-- one log call, measured on the calling thread --");
  {
    auto null = std::make_shared<NullSink>();
    etch::Logger log{"bench", {null}};
    row("logger -> null sink",
        ns_per_call(kCalls, [&] { log.info("thread {} message {}", 3, 12345); }));
    log.set_level(etch::Level::kWarn);
    row("logger -> null sink, below level",
        ns_per_call(kCalls, [&] { log.info("thread {} message {}", 3, 12345); }));
  }
  {
    auto memory = std::make_shared<etch::MemorySink>(16);
    etch::Logger log{"bench", {memory}};
    row("logger -> memory sink",
        ns_per_call(kCalls, [&] { log.info("thread {} message {}", 3, 12345); }));
  }
  {
    auto null = std::make_shared<NullSink>();
    auto async = std::make_shared<etch::AsyncSink>(null, kBigQueue);
    etch::Logger log{"bench", {async}};
    row("logger -> async -> null sink",
        ns_per_call(kCalls, [&] { log.info("thread {} message {}", 3, 12345); }),
        "worker is never busy, so the queue hands off every record");
    async->flush();
  }
  {
    const auto path = std::filesystem::temp_directory_path() / "etch-bench.log";
    auto file = etch::FileSink::open(path, {.truncate = true});
    if (!file) {
      std::println("cannot open {}: {}", path.string(), file.error().message());
      return 1;
    }
    auto async = std::make_shared<etch::AsyncSink>(*file, kBigQueue);
    etch::Logger log{"bench", {async}};
    const auto start = Clock::now();
    const double caller = ns_per_call(kCalls, [&] { log.info("thread {} message {}", 3, 12345); });
    async->flush();
    const auto total = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    row("logger -> async -> file sink", caller,
        std::format("all {} lines on disk after {:.1f} ms ({:.0f} ns/line on the worker)", kCalls,
                    total, total * 1e6 / kCalls));
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }

  std::println("-- {} threads logging at once -> async -> null sink --", kThreads);
  {
    auto null = std::make_shared<NullSink>();
    auto async = std::make_shared<etch::AsyncSink>(null, kBigQueue);
    etch::Logger log{"bench", {async}};
    const auto start = Clock::now();
    {
      std::vector<std::jthread> threads;
      for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&log, t] {
          for (int i = 0; i < kCalls / kThreads; ++i) log.info("thread {} message {}", t, i);
        });
      }
    }
    const auto wall = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    async->flush();
    row("per call, wall time per thread", wall / (kCalls / kThreads),
        std::format("{} calls each; dropped {}", kCalls / kThreads, async->dropped()));
  }
  return 0;
}
