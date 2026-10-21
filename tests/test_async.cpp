// etch tests: the async sink (ordering, policies, flush barrier, failure handling).
#include "support.hpp"

namespace {

using namespace std::chrono_literals;

// ---- async --------------------------------------------------------------------------------

TEST(file_sink_with_max_files_zero_restarts_the_file) {
  const auto path = scratch_dir() / "restart.log";
  auto file = etch::FileSink::open(path, {.max_bytes = 60, .max_files = 0, .truncate = true});
  CHECK(file.has_value());
  if (!file) return;
  (*file)->set_style(etch::LineStyle{.time = false, .thread = false, .location = false});
  etch::Logger log{"z", {*file}};
  for (int i = 0; i < 40; ++i) log.warn("{:02}", i);
  log.flush();
  CHECK((*file)->rotations() > 0);
  CHECK((*file)->bytes_written() <= 60u);
  CHECK(!std::filesystem::exists(path.string() + ".0"));
  CHECK(!std::filesystem::exists(path.string() + ".1"));
  CHECK_EQ((*file)->write_failures(), 0u);
}

TEST(sinks_can_be_replaced_while_other_threads_log) {
  constexpr int kThreads = 4;
  constexpr int kPerThread = 2000;
  auto always = std::make_shared<etch::MemorySink>(kThreads * kPerThread);
  auto sometimes = std::make_shared<etch::MemorySink>(kThreads * kPerThread);
  etch::Logger log{"swap", {always}};
  std::atomic<bool> go{false};
  {
    std::vector<std::jthread> workers;
    for (int t = 0; t < kThreads; ++t) {
      workers.emplace_back([&] {
        while (!go.load()) {
        }
        for (int i = 0; i < kPerThread; ++i) log.warn("{}", i);
      });
    }
    go = true;
    for (int i = 0; i < 200; ++i) {
      log.set_sinks({always, sometimes});
      log.add_sink(std::make_shared<etch::MemorySink>(1));
      log.set_sinks({always});
    }
  }
  CHECK_EQ(always->total(), static_cast<std::size_t>(kThreads * kPerThread));
  CHECK(sometimes->total() <= always->total());
  CHECK_EQ(log.sinks().size(), 1u);
}

TEST(a_sink_callback_may_change_the_logger_sinks) {
  auto late = std::make_shared<etch::MemorySink>();
  etch::Logger log{"re", {}};
  bool added = false;
  auto hook = std::make_shared<etch::CallbackSink>([&](const etch::Record&) {
    if (!added) {
      added = true;
      log.add_sink(late);  // allowed: the logger holds no lock while calling sinks
    }
  });
  log.add_sink(hook);
  log.warn("first");   // triggers the add
  log.warn("second");  // reaches both
  CHECK_EQ(late->messages(), (std::vector<std::string>{"second"}));
}

TEST(null_sinks_are_ignored_and_scope_needs_an_lvalue_logger) {
  etch::Logger log{"n", {nullptr, std::make_shared<etch::MemorySink>(), nullptr}};
  CHECK_EQ(log.sinks().size(), 1u);
  log.add_sink(nullptr);
  CHECK_EQ(log.sinks().size(), 1u);
  log.warn("does not crash");
  static_assert(!ScopeCallable<etch::Logger&&>, "scope() on a temporary logger would dangle");
  static_assert(ScopeCallable<etch::Logger&>);
}

TEST(emit_applies_the_logger_level) {
  Captured c;
  c.log.set_level(etch::Level::kError);
  c.log.emit(etch::Level::kWarn, std::source_location::current(), "filtered");
  c.log.emit(etch::Level::kError, std::source_location::current(), "kept");
  CHECK_EQ(c.messages(), (std::vector<std::string>{"kept"}));
}

TEST(async_applies_the_inner_sinks_level) {
  auto memory = std::make_shared<etch::MemorySink>();
  memory->set_level(etch::Level::kError);
  auto async = std::make_shared<etch::AsyncSink>(memory);
  etch::Logger log{"lvl", {async}};
  log.warn("w");
  log.error("e");
  async->flush();
  CHECK_EQ(memory->messages(), (std::vector<std::string>{"e"}));
  CHECK_EQ(async->queued(), 0u);
}

TEST(async_survives_an_inner_sink_that_throws) {
  std::vector<std::string> delivered;
  auto fragile = std::make_shared<etch::CallbackSink>([&](const etch::Record& r) {
    if (r.message == "boom") throw std::runtime_error("sink failed");
    delivered.push_back(r.message);
  });
  auto async = std::make_shared<etch::AsyncSink>(fragile);
  etch::Logger log{"throw", {async}};
  log.warn("a");
  log.warn("boom");
  log.warn("b");
  async->flush();
  CHECK_EQ(delivered, (std::vector<std::string>{"a", "b"}));
  CHECK_EQ(async->write_failures(), 1u);
}

TEST(async_delivers_everything_and_keeps_per_thread_order) {
  constexpr int kThreads = 8;
  constexpr int kPerThread = 500;
  auto memory = std::make_shared<etch::MemorySink>(kThreads * kPerThread);
  auto async = std::make_shared<etch::AsyncSink>(memory, 64, etch::AsyncSink::Overflow::kBlock);
  etch::Logger log{"mt", {async}};
  {
    std::vector<std::jthread> workers;
    for (int t = 0; t < kThreads; ++t) {
      workers.emplace_back([&log, t] {
        for (int i = 0; i < kPerThread; ++i) log.warn("{} {}", t, i);
      });
    }
  }
  async->flush();
  CHECK_EQ(async->dropped(), 0u);
  CHECK_EQ(async->queued(), 0u);
  const auto messages = memory->messages();
  CHECK_EQ(messages.size(), static_cast<std::size_t>(kThreads * kPerThread));
  std::array<int, kThreads> next{};
  int out_of_order = 0;
  for (const auto& m : messages) {
    const auto space = m.find(' ');
    const int t = space == std::string::npos ? -1 : std::stoi(m.substr(0, space));
    const int i = space == std::string::npos ? -1 : std::stoi(m.substr(space + 1));
    if (t < 0 || t >= kThreads || i != next[t]++) ++out_of_order;
  }
  CHECK_EQ(out_of_order, 0);
}

// A sink that takes a while per record, to make the queue fill up. Spins instead of sleeping:
// sleep_for on Windows rounds up to the scheduler tick, which would make these tests slow.
std::shared_ptr<etch::CallbackSink> slow_sink(std::atomic<int>& delivered,
                                              std::chrono::microseconds per_record) {
  return std::make_shared<etch::CallbackSink>([&delivered, per_record](const etch::Record& r) {
    const auto until = std::chrono::steady_clock::now() + per_record;
    while (std::chrono::steady_clock::now() < until) {
    }
    if (r.logger_name != "etch") delivered.fetch_add(1);  // gap markers are not the caller's
  });
}

TEST(async_drop_policy_counts_what_it_loses) {
  constexpr int kSent = 500;
  constexpr std::size_t kCapacity = 8;
  std::atomic<int> delivered{0};
  {
    // keep_from = kOff: everything may be dropped, including these warn lines.
    auto async =
        std::make_shared<etch::AsyncSink>(slow_sink(delivered, 500us), kCapacity,
                                          etch::AsyncSink::Overflow::kDrop, etch::Level::kOff);
    etch::Logger log{"drop", {async}};
    for (int i = 0; i < kSent; ++i) log.warn("{}", i);
    async->flush();
    CHECK(async->dropped() > 0);
    CHECK_EQ(static_cast<int>(async->dropped()) + delivered.load(), kSent);
  }
}

TEST(async_flush_waits_for_the_queue_to_drain) {
  std::atomic<int> delivered{0};
  auto async = std::make_shared<etch::AsyncSink>(slow_sink(delivered, 200us), 1000);
  etch::Logger log{"flush", {async}};
  for (int i = 0; i < 100; ++i) log.warn("{}", i);
  log.flush();
  CHECK_EQ(delivered.load(), 100);
  CHECK_EQ(async->queued(), 0u);
}

TEST(async_destructor_drains_before_exiting) {
  std::atomic<int> delivered{0};
  {
    auto async = std::make_shared<etch::AsyncSink>(slow_sink(delivered, 200us), 1000);
    etch::Logger log{"dtor", {async}};
    for (int i = 0; i < 50; ++i) log.warn("{}", i);
  }
  CHECK_EQ(delivered.load(), 50);
}

TEST(async_block_policy_never_drops) {
  std::atomic<int> delivered{0};
  {
    auto async = std::make_shared<etch::AsyncSink>(slow_sink(delivered, 100us), 4,
                                                   etch::AsyncSink::Overflow::kBlock);
    etch::Logger log{"block", {async}};
    for (int i = 0; i < 200; ++i) log.warn("{}", i);
    CHECK_EQ(async->dropped(), 0u);
  }
  CHECK_EQ(delivered.load(), 200);
}

TEST(async_flush_returns_while_another_thread_keeps_logging) {
  std::atomic<int> delivered{0};
  auto async = std::make_shared<etch::AsyncSink>(slow_sink(delivered, 20us), 256);
  etch::Logger log{"busy", {async}};
  std::atomic<bool> stop{false};
  std::atomic<bool> flushed{false};
  std::jthread producer([&] {
    while (!stop.load()) log.warn("noise");
  });
  // Watchdog: if flush waits for the producer to go idle, it would never return. Stop the
  // producer after a while so the test fails on the timing check instead of hanging.
  std::jthread watchdog([&] {
    for (int i = 0; i < 300 && !flushed.load(); ++i) std::this_thread::sleep_for(10ms);
    stop = true;
  });
  // Only flush once the producer is demonstrably ahead of the worker, otherwise a flush that
  // waits for an idle queue would also return at once and prove nothing.
  while (!stop.load() && (delivered.load() < 50 || async->queued() < 50)) {
    std::this_thread::yield();
  }
  log.warn("before flush");
  const auto start = std::chrono::steady_clock::now();
  log.flush();
  const auto took = std::chrono::steady_clock::now() - start;
  flushed = true;
  stop = true;
  CHECK(took < 2s);
}

TEST(async_drop_keeps_records_at_or_above_keep_from_and_marks_the_gap) {
  std::vector<etch::Record> delivered;
  std::mutex m;
  auto slow = std::make_shared<etch::CallbackSink>([&](const etch::Record& r) {
    const auto until = std::chrono::steady_clock::now() + 200us;
    while (std::chrono::steady_clock::now() < until) {
    }
    std::lock_guard lock(m);
    delivered.push_back(r);
  });
  {
    auto async = std::make_shared<etch::AsyncSink>(slow, 8, etch::AsyncSink::Overflow::kDrop,
                                                   etch::Level::kError);
    etch::Logger log{"drop", {async}};
    for (int i = 0; i < 300; ++i) log.warn("noise {}", i);
    log.error("must survive");
    for (int i = 0; i < 300; ++i) log.warn("noise {}", i);
    async->flush();
    CHECK(async->dropped() > 0);
  }
  std::lock_guard lock(m);
  bool error_seen = false;
  std::size_t markers = 0;
  std::size_t marked_total = 0;
  for (const auto& r : delivered) {
    if (r.message == "must survive") error_seen = true;
    if (r.logger_name == "etch") {
      ++markers;
      CHECK(r.level == etch::Level::kWarn);
      CHECK(r.message.find("records dropped") != std::string::npos);
      CHECK_EQ(r.fields.size(), 1u);
      if (!r.fields.empty()) marked_total += std::get<std::int64_t>(r.fields[0].value);
    }
  }
  CHECK(error_seen);
  CHECK(markers > 0);
  CHECK(marked_total > 0);
}

TEST(shutdown_drains_then_writes_straight_through) {
  std::atomic<int> delivered{0};
  auto async = std::make_shared<etch::AsyncSink>(slow_sink(delivered, 100us));
  etch::Logger log{"shut", {async}};
  for (int i = 0; i < 50; ++i) log.warn("{}", i);
  etch::flush_all();
  CHECK_EQ(delivered.load(), 50);
  async->shutdown();
  log.warn("after shutdown");
  CHECK_EQ(delivered.load(), 51);  // synchronous now: already there when the call returns
  async->shutdown();               // a second time is harmless
  CHECK_EQ(async->queued(), 0u);
}

TEST(shutdown_with_a_blocked_producer_delivers_every_record_once) {
  std::vector<std::string> delivered;
  std::mutex m;
  auto slow = std::make_shared<etch::CallbackSink>([&](const etch::Record& r) {
    const auto until = std::chrono::steady_clock::now() + 300us;
    while (std::chrono::steady_clock::now() < until) {
    }
    std::lock_guard lock(m);
    delivered.push_back(r.message);
  });
  auto async = std::make_shared<etch::AsyncSink>(slow, 2, etch::AsyncSink::Overflow::kBlock);
  etch::Logger log{"blocked", {async}};
  constexpr int kSent = 40;
  std::jthread producer([&] {
    for (int i = 0; i < kSent; ++i) log.warn("record {}", i);
  });
  while (async->queued() < 2) std::this_thread::yield();  // the producer is now blocking
  async->shutdown();
  producer.join();
  std::lock_guard lock(m);
  CHECK_EQ(delivered.size(), static_cast<std::size_t>(kSent));
  std::vector<std::string> expected;
  for (int i = 0; i < kSent; ++i) expected.push_back(std::format("record {}", i));
  CHECK_EQ(delivered, expected);  // every record once, in order, none empty
}

TEST(shutdown_from_two_threads_at_once_is_safe) {
  std::atomic<int> delivered{0};
  auto async = std::make_shared<etch::AsyncSink>(slow_sink(delivered, 50us));
  etch::Logger log{"twice", {async}};
  for (int i = 0; i < 20; ++i) log.warn("{}", i);
  {
    std::jthread a([&] { async->shutdown(); });
    std::jthread b([&] { async->shutdown(); });
  }
  CHECK_EQ(delivered.load(), 20);
  log.warn("after");
  CHECK_EQ(delivered.load(), 21);
}

TEST(gap_report_matches_the_drop_count_exactly_after_flush) {
  std::vector<etch::Record> delivered;
  std::mutex m;
  auto slow = std::make_shared<etch::CallbackSink>([&](const etch::Record& r) {
    const auto until = std::chrono::steady_clock::now() + 200us;
    while (std::chrono::steady_clock::now() < until) {
    }
    std::lock_guard lock(m);
    delivered.push_back(r);
  });
  auto async = std::make_shared<etch::AsyncSink>(slow, 2, etch::AsyncSink::Overflow::kDrop,
                                                 etch::Level::kOff);
  etch::Logger log{"gap", {async}};
  for (int i = 0; i < 60; ++i) log.warn("noise {}", i);
  log.flush();
  CHECK(async->dropped() > 0);
  std::lock_guard lock(m);
  std::size_t marked = 0;
  for (const auto& r : delivered) {
    if (r.logger_name == "etch") marked += std::get<std::int64_t>(r.fields.at(0).value);
  }
  CHECK_EQ(marked, async->dropped());  // flush returns with every drop reported
}

struct FlushCounter final : etch::Sink {
  std::atomic<int> writes{0};
  std::atomic<int> flushes{0};
  void write(const etch::Record&) override { ++writes; }
  void flush() override { ++flushes; }
};

TEST(idle_worker_flushes_the_inner_sink) {
  auto counter = std::make_shared<FlushCounter>();
  auto async = std::make_shared<etch::AsyncSink>(counter);
  etch::Logger log{"idle", {async}};
  for (int i = 0; i < 10; ++i) log.warn("{}", i);
  // No flush() call: the worker must do it on its own once the queue has gone idle.
  for (int i = 0; i < 500 && counter->flushes.load() == 0; ++i) std::this_thread::sleep_for(1ms);
  CHECK(counter->flushes.load() > 0);
  CHECK_EQ(counter->writes.load(), 10);
}

TEST(flush_all_while_async_sinks_come_and_go) {
  std::atomic<bool> stop{false};
  std::jthread churn([&] {
    while (!stop.load()) {
      auto sink = std::make_shared<etch::AsyncSink>(std::make_shared<etch::MemorySink>(4));
      etch::Logger log{"churn", {sink}};
      log.warn("x");
    }
  });
  for (int i = 0; i < 200; ++i) etch::flush_all();
  stop = true;
}

}  // namespace
