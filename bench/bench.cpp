// etch benchmark: what one log call costs the calling thread, step by step.
//
// Build with a Release preset and run etch_bench. Every number is an average over many calls on
// one machine; see README.md for the figures this produced and what they mean. Two kinds of
// async row: "burst" uses a ring far larger than the run, so the queue never fills and the row
// measures the enqueue alone; "sustained" uses the default ring, so once the worker falls
// behind the caller blocks, and the row measures what an application that logs this hard pays.
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <print>
#include <string>
#include <thread>
#include <vector>

#include <etch/etch.hpp>

namespace {

using Clock = std::chrono::steady_clock;

// Keeps the optimiser from deleting a measured computation: the value is stored where the
// compiler cannot prove nobody reads it.
volatile std::size_t g_keep = 0;
template <class T>
void keep(const T& value) {
  g_keep = g_keep + static_cast<std::size_t>(value);
}

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
  std::println("  {:<44} {:>8.0f} ns  {}", what, ns, note);
}

std::filesystem::path temp_log(std::string_view name) {
  return std::filesystem::temp_directory_path() / name;
}

// Logs kCalls lines through an async sink in front of `inner` and reports the caller's cost and
// the end-to-end time until everything is written.
void async_rows(std::string_view label, std::shared_ptr<etch::Sink> inner, std::size_t capacity,
                int calls) {
  auto async = std::make_shared<etch::AsyncSink>(std::move(inner), capacity);
  etch::Logger log{"bench", {async}};
  const auto start = Clock::now();
  const double caller = ns_per_call(calls, [&] { log.info("thread {} message {}", 3, 12345); });
  async->flush();
  const auto total = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  row(label, caller,
      std::format("{} lines written after {:.1f} ms end to end ({:.0f} ns per line)", calls, total,
                  total * 1e6 / calls));
}

}  // namespace

int main() {
  constexpr int kCalls = 200'000;
  constexpr std::size_t kBurstQueue = 1u << 20;  // larger than the run: the queue never fills
  constexpr int kThreads = 4;
  std::string s;

  std::println("etch benchmark: {} calls per row, Release build expected", kCalls);
  std::println("-- primitives --");
  row("system_clock::now", ns_per_call(kCalls, [&] {
        keep(std::chrono::system_clock::now().time_since_epoch().count());
      }));
  row("this_thread::get_id",
      ns_per_call(kCalls, [&] { keep(std::hash<std::thread::id>{}(std::this_thread::get_id())); }));
  row("std::format, 22-char message", ns_per_call(kCalls, [&] {
        s = std::format("thread {} message {}", 3, 12345);
        keep(s.size());
      }));
  row("std::string allocate 22 chars", ns_per_call(kCalls, [&] {
        const std::string x("thread 3 message 12345");
        keep(x.size());
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
  async_rows("logger -> async -> null sink, burst", std::make_shared<NullSink>(), kBurstQueue,
             kCalls);
  {
    const auto path = temp_log("etch-bench.log");
    auto file = etch::FileSink::open(path, {.truncate = true});
    if (!file) {
      std::println("cannot open {}: {}", path.string(), file.error().message());
      return 1;
    }
    async_rows("logger -> async -> file sink, burst", *file, kBurstQueue, kCalls);
    async_rows("logger -> async -> file sink, sustained (default ring)", *file,
               etch::AsyncSink::kDefaultCapacity, kCalls);
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  {
    const auto path = temp_log("etch-bench.jsonl");
    auto file = etch::FileSink::open(path, {.truncate = true});
    if (!file) {
      std::println("cannot open {}: {}", path.string(), file.error().message());
      return 1;
    }
    (*file)->set_style(etch::LineStyle{.format = etch::LineFormat::kJson});
    async_rows("logger -> async -> JSON file sink, sustained", *file,
               etch::AsyncSink::kDefaultCapacity, kCalls);
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }

  std::println("-- {} threads logging at once -> async -> null sink, burst --", kThreads);
  {
    auto null = std::make_shared<NullSink>();
    auto async = std::make_shared<etch::AsyncSink>(null, kBurstQueue);
    etch::Logger log{"bench", {async}};
    std::vector<double> per_thread_ns(kThreads);
    const auto start = Clock::now();
    {
      std::vector<std::jthread> threads;
      for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&log, &per_thread_ns, t] {
          per_thread_ns[static_cast<std::size_t>(t)] =
              ns_per_call(kCalls / kThreads, [&] { log.info("thread {} message {}", t, 12345); });
        });
      }
    }
    const auto wall = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    async->flush();
    double sum = 0;
    for (const double ns : per_thread_ns) sum += ns;
    row("per call, measured inside each thread", sum / kThreads,
        std::format("average of {} threads; {} calls each; dropped {}", kThreads, kCalls / kThreads,
                    async->dropped()));
    row("per call, wall time of the whole run / calls per thread", wall / (kCalls / kThreads),
        "includes thread start and the slowest thread");
  }
  return 0;
}
