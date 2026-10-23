// etch benchmark: what one log call costs the calling thread, step by step.
//
// Build with a Release preset and run etch_bench. Each row is one kind of call made many times
// on one machine: the mean, the median, the 99th and 99.9th percentiles and the worst call, all
// on the calling thread, plus how many heap allocations the call made there. The tail is what a
// game loop feels; the mean hides it. Two kinds of async row: "burst" uses a ring far larger than
// the run, so the queue never fills and the row measures the enqueue alone; "sustained" uses the
// default ring, so once the worker falls behind the caller blocks, and the row measures what an
// application that logs this hard pays. See the wiki for the figures this produced.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <new>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <etch/etch.hpp>

#if defined(_M_X64) || defined(__x86_64__)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#define ETCH_BENCH_HAS_TSC 1
#else
#define ETCH_BENCH_HAS_TSC 0
#endif

// Every allocation on the calling thread is counted, so a row can say how many a call made.
// The global operator new is replaced for this program only; the worker thread's allocations
// land in its own counter and are not reported.
namespace {
constinit thread_local std::size_t g_allocations = 0;
}
void* operator new(std::size_t size) {
  ++g_allocations;
  if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using Clock = std::chrono::steady_clock;

// Keeps the optimiser from deleting a measured computation: the value is stored where the
// compiler cannot prove nobody reads it.
volatile std::size_t g_keep = 0;
template <class T>
void keep(const T& value) {
  g_keep = g_keep + static_cast<std::size_t>(value);
}

// The cycle counter where there is one: steady_clock ticks at 100 ns on Windows, too coarse to
// time one 150 ns call. Ticks are converted to nanoseconds with the steady_clock time of the
// whole run, so the mean is exact and only the percentiles rely on the counter's rate.
std::uint64_t ticks() noexcept {
#if ETCH_BENCH_HAS_TSC
  return __rdtsc();
#else
  return static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
#endif
}

struct Stats {
  double mean_ns = 0;
  double p50_ns = 0;
  double p99_ns = 0;
  double p999_ns = 0;
  double max_ns = 0;
  double allocations = 0;  // per call
};

// The timing itself costs two counter reads per call; measured once on an empty call and taken
// off every row, so the numbers are the call's own.
double g_timing_overhead_ns = 0;

// Runs f n times, timing every call, and reduces the samples. The per-call tick samples are
// kept in the caller's vector so a multi-thread row can pool several threads' samples.
template <class F>
Stats measure_into(std::vector<std::uint64_t>& samples, int n, F f) {
  samples.resize(static_cast<std::size_t>(n));
  const std::size_t allocations_before = g_allocations;
  const auto start = Clock::now();
  const std::uint64_t first = ticks();
  for (int i = 0; i < n; ++i) {
    const std::uint64_t before = ticks();
    f();
    samples[static_cast<std::size_t>(i)] = ticks() - before;
  }
  const std::uint64_t last = ticks();
  const double elapsed_ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
  const double ns_per_tick = last > first ? elapsed_ns / static_cast<double>(last - first) : 0;
  const std::size_t allocations = g_allocations - allocations_before;

  std::sort(samples.begin(), samples.end());
  const auto at = [&](double quantile) {
    const auto index = static_cast<std::size_t>(quantile * static_cast<double>(samples.size() - 1));
    return static_cast<double>(samples[index]) * ns_per_tick;
  };
  const auto net = [&](double ns) { return std::max(0.0, ns - g_timing_overhead_ns); };
  return Stats{.mean_ns = net(elapsed_ns / n),
               .p50_ns = net(at(0.5)),
               .p99_ns = net(at(0.99)),
               .p999_ns = net(at(0.999)),
               .max_ns = net(at(1.0)),
               .allocations = static_cast<double>(allocations) / n};
}

template <class F>
Stats measure(int n, F f) {
  std::vector<std::uint64_t> samples;
  samples.reserve(static_cast<std::size_t>(n));  // before measuring: not one of the call's own
  return measure_into(samples, n, std::move(f));
}

// The cheapest possible sink, to measure everything in front of it.
struct NullSink final : etch::Sink {
  void write(const etch::Record&) override {}
};

void header() {
  std::println("  {:<40} {:>7} {:>7} {:>7} {:>7} {:>8} {:>6}", "", "mean", "p50", "p99", "p99.9",
               "max", "allocs");
}

void row(std::string_view what, const Stats& s, std::string_view note = "") {
  std::println("  {:<40} {:>7.0f} {:>7.0f} {:>7.0f} {:>7.0f} {:>8.0f} {:>6.2f}  {}", what,
               s.mean_ns, s.p50_ns, s.p99_ns, s.p999_ns, s.max_ns, s.allocations, note);
}

std::filesystem::path temp_log(std::string_view name) {
  return std::filesystem::temp_directory_path() / name;
}

// Logs `calls` lines through an async sink in front of `inner` and reports the caller's cost and
// the end-to-end time until everything is written.
void async_rows(std::string_view label, std::shared_ptr<etch::Sink> inner, std::size_t capacity,
                int calls) {
  auto async = std::make_shared<etch::AsyncSink>(std::move(inner), capacity);
  etch::Logger log{"bench", {async}};
  const auto start = Clock::now();
  const Stats caller = measure(calls, [&] { log.info("thread {} message {}", 3, 12345); });
  async->flush();
  const auto total = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  row(label, caller,
      std::format("{} lines on disk after {:.1f} ms ({:.0f} ns per line)", calls, total,
                  total * 1e6 / calls));
}

}  // namespace

int main() {
  constexpr int kCalls = 200'000;
  constexpr std::size_t kBurstQueue = 1u << 20;  // larger than the run: the queue never fills
  constexpr int kThreads = 4;
  std::string s;

  g_timing_overhead_ns = measure(kCalls, [] {}).mean_ns;
  std::println("etch benchmark: {} calls per row, Release build expected; nanoseconds on the",
               kCalls);
  std::println("calling thread, minus the {:.0f} ns the timing itself costs per call; allocs is",
               g_timing_overhead_ns);
  std::println("heap allocations per call on that thread");
  std::println("-- primitives --");
  header();
  row("system_clock::now",
      measure(kCalls, [&] { keep(std::chrono::system_clock::now().time_since_epoch().count()); }));
  row("this_thread::get_id",
      measure(kCalls, [&] { keep(std::hash<std::thread::id>{}(std::this_thread::get_id())); }));
  row("std::format, 22-char message", measure(kCalls, [&] {
        s = std::format("thread {} message {}", 3, 12345);
        keep(s.size());
      }));
  row("std::string allocate 22 chars", measure(kCalls, [&] {
        const std::string x("thread 3 message 12345");
        keep(x.size());
      }));

  std::println("-- one log call, measured on the calling thread --");
  header();
  {
    auto null = std::make_shared<NullSink>();
    etch::Logger log{"bench", {null}};
    row("logger -> null sink",
        measure(kCalls, [&] { log.info("thread {} message {}", 3, 12345); }));
    log.set_level(etch::Level::kWarn);
    row("logger -> null sink, below level",
        measure(kCalls, [&] { log.info("thread {} message {}", 3, 12345); }));
  }
  {
    auto memory = std::make_shared<etch::MemorySink>(16);
    etch::Logger log{"bench", {memory}};
    row("logger -> memory sink",
        measure(kCalls, [&] { log.info("thread {} message {}", 3, 12345); }));
  }
  async_rows("async -> null sink, burst", std::make_shared<NullSink>(), kBurstQueue, kCalls);
  {
    const auto path = temp_log("etch-bench.log");
    auto file = etch::FileSink::open(path, {.truncate = true});
    if (!file) {
      std::println("cannot open {}: {}", path.string(), file.error().message());
      return 1;
    }
    async_rows("async -> file sink, burst", *file, kBurstQueue, kCalls);
    async_rows("async -> file sink, sustained", *file, etch::AsyncSink::kDefaultCapacity, kCalls);
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
    async_rows("async -> JSON file sink, sustained", *file, etch::AsyncSink::kDefaultCapacity,
               kCalls);
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }

  std::println("-- {} threads logging at once -> async -> null sink, burst --", kThreads);
  header();
  {
    auto null = std::make_shared<NullSink>();
    auto async = std::make_shared<etch::AsyncSink>(null, kBurstQueue);
    etch::Logger log{"bench", {async}};
    constexpr int kCallsPerThread = kCalls / kThreads;
    std::vector<Stats> per_thread(kThreads);
    std::vector<std::vector<std::uint64_t>> samples(kThreads);
    for (auto& v : samples) v.reserve(kCallsPerThread);
    const auto start = Clock::now();
    {
      std::vector<std::jthread> threads;
      for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
          const auto index = static_cast<std::size_t>(t);
          per_thread[index] = measure_into(samples[index], kCallsPerThread,
                                           [&] { log.info("thread {} message {}", t, 12345); });
        });
      }
    }
    const auto wall = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    async->flush();
    // Mean and allocations averaged over the threads; percentiles of the slowest thread, since
    // the slowest is what the frame waits for.
    Stats pooled;
    for (const Stats& t : per_thread) {
      pooled.mean_ns += t.mean_ns / kThreads;
      pooled.allocations += t.allocations / kThreads;
      pooled.p50_ns = std::max(pooled.p50_ns, t.p50_ns);
      pooled.p99_ns = std::max(pooled.p99_ns, t.p99_ns);
      pooled.p999_ns = std::max(pooled.p999_ns, t.p999_ns);
      pooled.max_ns = std::max(pooled.max_ns, t.max_ns);
    }
    row("per call, inside each thread", pooled,
        std::format("mean of {} threads, percentiles of the slowest; dropped {}", kThreads,
                    async->dropped()));
    std::println("  {:<40} {:>7.0f}  wall time of the run / calls per thread, with thread start",
                 "per call, from outside", wall / kCallsPerThread);
  }
  return 0;
}
