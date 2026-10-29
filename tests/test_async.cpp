// etch tests: the async sink (ordering, policies, flush barrier, failure handling).
#include "support.hpp"

#if defined(_WIN32) && !defined(_WINDOWS_)
extern "C" {
__declspec(dllimport) void* __stdcall GetCurrentThread();
__declspec(dllimport) int __stdcall GetThreadPriority(void*);  // int WINAPI (HANDLE)
}
#elif defined(__linux__)
#include <unistd.h>

#include <sys/resource.h>
#endif

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

TEST(replacing_the_sinks_releases_them_on_every_thread) {
  // Each thread caches the sink list it last used; a replacement must clear those caches,
  // not wait for the thread to log again or end. The other thread stays alive and idle while
  // the check runs.
  auto sink = std::make_shared<etch::MemorySink>();
  std::weak_ptr<etch::Sink> weak = sink;
  etch::Logger log{"release", {sink}};
  std::atomic<int> phase{0};
  std::jthread other([&] {
    log.warn("from the other thread");
    phase = 1;
    while (phase.load() < 2) std::this_thread::yield();
  });
  while (phase.load() < 1) std::this_thread::yield();
  log.warn("from this thread");
  log.set_sinks({});
  sink.reset();
  CHECK(weak.expired());
  phase = 2;
}

TEST(a_thread_that_ended_holds_no_sink) {
  auto sink = std::make_shared<etch::MemorySink>();
  std::weak_ptr<etch::Sink> weak = sink;
  etch::Logger log{"ended", {sink}};
  std::jthread([&] { log.warn("cached on a thread that then ends"); }).join();
  log.set_sinks({});
  sink.reset();
  CHECK(weak.expired());
}

TEST(destroying_a_logger_releases_its_sinks) {
  auto sink = std::make_shared<etch::MemorySink>();
  std::weak_ptr<etch::Sink> weak = sink;
  {
    etch::Logger log{"gone", {sink}};
    log.warn("cached on this thread");
  }
  sink.reset();
  CHECK(weak.expired());
}

TEST(a_sink_may_replace_the_sinks_during_its_own_write) {
  etch::Logger log{"self", {}};
  auto after = std::make_shared<etch::MemorySink>();
  int calls = 0;
  auto hook = std::make_shared<etch::CallbackSink>([&](const etch::Record&) {
    ++calls;
    log.set_sinks({after});  // replaces the list this very call is iterating
  });
  log.set_sinks({hook});
  std::weak_ptr<etch::Sink> weak = hook;
  log.warn("first");  // reaches the hook only; the call then drops the list it was using
  hook.reset();
  CHECK(weak.expired());
  log.warn("second");
  CHECK_EQ(calls, 1);
  CHECK_EQ(after->messages(), (std::vector<std::string>{"second"}));
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
  // Generous: on a loaded machine the worker thread can take a while to be scheduled at all.
  for (int i = 0; i < 2000 && counter->flushes.load() == 0; ++i) std::this_thread::sleep_for(1ms);
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

TEST(flush_during_shutdown_waits_for_the_worker_to_finish) {
  std::atomic<int> delivered{0};
  auto async = std::make_shared<etch::AsyncSink>(slow_sink(delivered, 500us));
  etch::Logger log{"flush-shut", {async}};
  for (int i = 0; i < 40; ++i) log.warn("{}", i);  // about 20 ms of work for the worker
  std::jthread closer([&] { async->shutdown(); });
  while (async->queued() > 30) std::this_thread::yield();  // shutdown is under way
  async->flush();
  CHECK_EQ(delivered.load(), 40);  // flush returned only once everything was written
}

#if !defined(_WIN32)
// The daemon start: logging set up, then fork(). In the child the inherited sink has no worker
// and writes through; a sink the child makes itself has a worker of its own; destroying the
// inherited sink and exiting normally (destructors, exit handlers) touch none of the parent's
// thread or lock state, so neither hangs.
TEST(a_forked_child_logs_through_inherited_and_new_async_sinks_and_exits_normally) {
  const auto path = scratch_dir() / "fork.log";
  auto file = etch::FileSink::open(path, {.truncate = true});
  CHECK(file.has_value());
  if (!file) return;
  auto async = std::make_shared<etch::AsyncSink>(*file);  // kBlock: a full ring would wait forever
  auto log = std::make_unique<etch::Logger>("fork", etch::SinkList{async});
  log->warn("parent before fork");
  async->flush();
  constexpr int kInheritedLines = 5000;  // more than the ring holds
  constexpr int kOwnLines = 100;
  etch::prepare_fork();  // the worker is asleep holding nothing when the fork copies it
  const int failures_before = check::current_failures();  // other tests' failures are not ours
  const pid_t pid = fork();
  if (pid == 0) {
    for (int i = 0; i < kInheritedLines; ++i) log->warn("child {}", i);
    async->flush();
    log.reset();
    async.reset();  // the inherited sink: let go, not joined
    {
      // A sink made in the child has a worker of its own: its inner sink runs on a thread
      // that is not the one logging.
      const auto main_thread = std::this_thread::get_id();
      std::atomic<int> on_another_thread{0};
      auto probe = std::make_shared<etch::CallbackSink>([&](const etch::Record&) {
        if (std::this_thread::get_id() != main_thread) on_another_thread.fetch_add(1);
      });
      auto own = std::make_shared<etch::AsyncSink>(probe);
      etch::Logger own_log{"fork-child", {own, *file}};
      for (int i = 0; i < kOwnLines; ++i) own_log.warn("own {}", i);
      own->flush();
      CHECK_EQ(on_another_thread.load(), kOwnLines);  // every record, on the worker
    }  // joined here
    file->reset();
    // Through the exit handlers the parent registered; a failed check above fails the test.
    std::exit(check::current_failures() == failures_before ? 0 : 7);
  }
  CHECK(pid > 0);
  int status = 0;
  waitpid(pid, &status, 0);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  async->flush();
  CHECK_EQ(read_lines(path).size(), static_cast<std::size_t>(kInheritedLines + kOwnLines) + 1);
}
#endif

TEST(flush_all_does_not_deadlock_when_a_callback_makes_another_async_sink) {
  std::atomic<bool> made_one{false};
  auto nested = std::make_shared<etch::CallbackSink>([&](const etch::Record&) {
    if (made_one.exchange(true)) return;
    auto inner = std::make_shared<etch::AsyncSink>(std::make_shared<etch::MemorySink>(4));
    etch::Logger other{"nested", {inner}};
    other.warn("from inside a worker callback");
  });
  auto async = std::make_shared<etch::AsyncSink>(nested);
  etch::Logger log{"outer", {async}};
  log.warn("trigger");
  etch::flush_all();  // held a global lock across the worker before: this would deadlock
  CHECK(made_one.load());
}

// ---- sweeps and reserved slots (0.5.0) ---------------------------------------------------

// Inside a sweep (begin_batch .. end_batch) a line at flush_on is held in the stream buffer
// and flushed once when the sweep ends; outside a sweep it is flushed at once; a sweep with
// nothing at flush_on in it flushes nothing.
TEST(a_file_sink_holds_its_level_flush_until_the_sweep_ends) {
  const auto path = scratch_dir() / "sweep.log";
  // error/warn rather than warn/info: the min-level build compiles info out.
  auto file = etch::FileSink::open(path, {.flush_on = etch::Level::kError, .truncate = true});
  CHECK(file.has_value());
  if (!file) return;
  etch::Logger log{"sweep", {*file}};
  (*file)->begin_batch();
  log.error("inside the sweep");
  CHECK(read_bytes(path).empty());  // still in the stream buffer
  (*file)->end_batch();
  CHECK_EQ(read_lines(path).size(), 1u);
  log.error("outside any sweep");
  CHECK_EQ(read_lines(path).size(), 2u);
  (*file)->begin_batch();
  log.warn("below flush_on");
  (*file)->end_batch();
  CHECK_EQ(read_lines(path).size(), 2u);  // nothing asked for a flush
  (*file)->flush();
  CHECK_EQ(read_lines(path).size(), 3u);
  CHECK_EQ((*file)->write_failures(), 0u);
}

struct SweepCounter final : etch::Sink {
  std::atomic<int> open{0};
  std::atomic<int> begins{0};
  std::atomic<int> ends{0};
  std::atomic<int> writes{0};
  std::atomic<int> writes_outside{0};
  void begin_batch() override {
    ++begins;
    ++open;
  }
  void end_batch() override {
    --open;
    ++ends;
  }
  void write(const etch::Record&) override {
    ++writes;
    if (open.load() == 0) ++writes_outside;
  }
};

TEST(the_worker_writes_every_sweep_inside_a_bracket) {
  auto counter = std::make_shared<SweepCounter>();
  {
    auto async = std::make_shared<etch::AsyncSink>(counter, 8);
    etch::Logger log{"bracket", {async}};
    for (int i = 0; i < 100; ++i) log.warn("{}", i);
    async->flush();
  }
  CHECK_EQ(counter->writes.load(), 100);
  CHECK_EQ(counter->writes_outside.load(), 0);
  CHECK(counter->begins.load() > 0);
  CHECK_EQ(counter->begins.load(), counter->ends.load());
}

// Records what it gets, in order, letting the n-th write through only once `allowed` has
// reached n: the worker can be parked inside any chosen write, so the ring can be filled and
// a kept record made to wait while the test decides when slots are freed.
struct GatedRecorder final : etch::Sink {
  std::atomic<int> allowed{0};
  std::atomic<int> entered{0};
  std::mutex mutex;
  std::vector<std::string> messages;
  void write(const etch::Record& r) override {
    const int mine = entered.fetch_add(1) + 1;
    while (allowed.load() < mine) std::this_thread::sleep_for(1ms);
    std::lock_guard lock(mutex);
    messages.push_back(r.message);
  }
};

// Finding 2 of the server testbed: a warning waiting for a slot lost the race to the flood of
// droppable records that kept taking the freed slots, for up to seconds. Now the flood is
// dropped while a kept record waits, so the kept record is in the ring before any flood
// record logged after it began waiting. The worker is parked again right after the sweep
// that frees the slots, so without the rule the flood (already in its loop) takes both slots
// while the kept producer is still waking, and the order shows it.
TEST(a_waiting_kept_record_gets_the_freed_slot_before_the_flood) {
  constexpr int kFlood = 2000;        // at least this many, and until the kept record is in
  constexpr int kFloodCap = 1 << 20;  // bounded, so a broken reservation fails, not hangs
  auto held = std::make_shared<GatedRecorder>();
  auto async = std::make_shared<etch::AsyncSink>(held, 2, etch::AsyncSink::Overflow::kDrop,
                                                 etch::Level::kError);
  etch::Logger log{"reserve", {async}};
  log.warn("parks the worker");
  for (int i = 0; i < 5000 && held->entered.load() < 1; ++i) std::this_thread::sleep_for(1ms);
  CHECK_EQ(held->entered.load(), 1);
  log.warn("slot one");
  log.warn("slot two");  // the ring is full and the worker is parked in the first write
  std::jthread kept([&] { log.error("kept"); });  // at keep_from: waits for a slot
  for (int i = 0; i < 5000 && async->waiting() < 1; ++i) std::this_thread::sleep_for(1ms);
  CHECK_EQ(async->waiting(), 1u);
  // Two more writes: the worker finishes the first, reports the flood's first drops as a gap
  // marker (a write of its own) if the flood has begun, sweeps both slots (freeing them) and
  // parks again by its third write at the latest. The flood runs while the kept producer is
  // waking up.
  held->allowed = 2;
  // The ring is still full when the flood starts (the worker wakes on a millisecond sleep),
  // so the flood must outlast the freeing sweep: it runs until the kept record is in.
  int sent = 0;
  for (; sent < kFlood || (async->waiting() > 0 && sent < kFloodCap); ++sent) {
    log.warn("flood {}", sent);
  }
  held->allowed = 1 << 20;  // let everything through
  kept.join();
  async->flush();
  std::lock_guard lock(held->mutex);
  const auto& got = held->messages;
  const auto kept_at = std::ranges::find(got, "kept");
  const auto first_flood =
      std::ranges::find_if(got, [](const std::string& m) { return m.starts_with("flood "); });
  CHECK(kept_at != got.end());
  CHECK(kept_at < first_flood);  // also holds when no flood record was delivered at all
  const auto flood_delivered = static_cast<std::size_t>(
      std::ranges::count_if(got, [](const std::string& m) { return m.starts_with("flood "); }));
  CHECK_EQ(flood_delivered + async->dropped(), static_cast<std::size_t>(sent));
  CHECK_EQ(async->waiting(), 0u);
}

// ---- worker priority (0.5.0) -------------------------------------------------------------

// The calling thread's scheduling priority as the OS reports it: Windows thread priority (0
// normal, 1 above normal), Linux nice value (0 normal, negative is higher priority).
int this_thread_priority() {
#if defined(_WIN32)
  return GetThreadPriority(GetCurrentThread());
#elif defined(__linux__)
  return getpriority(PRIO_PROCESS, static_cast<id_t>(gettid()));
#else
  return 0;
#endif
}

bool is_above(int priority, int normal) {
#if defined(__linux__)
  return priority < normal;  // a lower nice value is a higher priority
#else
  return priority > normal;
#endif
}

// The worker says truthfully whether it runs above normal: on Windows it always can; on
// Linux only with CAP_SYS_NICE, and then the sink says it was refused rather than pretend.
TEST(the_worker_runs_above_normal_priority_when_the_system_allows_it) {
  constexpr int kUnset = 1000;
  const int normal = this_thread_priority();
  std::atomic<int> on_worker{kUnset};
  auto probe = std::make_shared<etch::CallbackSink>(
      [&](const etch::Record&) { on_worker = this_thread_priority(); });
  auto async = std::make_shared<etch::AsyncSink>(probe);  // default: kAboveNormal
  etch::Logger log{"prio", {async}};
  log.warn("on the worker");
  async->flush();
  CHECK(on_worker.load() != kUnset);
  CHECK_EQ(is_above(on_worker.load(), normal), async->worker_priority_raised());
#if defined(_WIN32)
  CHECK(async->worker_priority_raised());
#endif
  std::atomic<int> on_plain_worker{kUnset};
  auto plain_probe = std::make_shared<etch::CallbackSink>(
      [&](const etch::Record&) { on_plain_worker = this_thread_priority(); });
  auto plain = std::make_shared<etch::AsyncSink>(
      plain_probe, etch::AsyncSink::kDefaultCapacity, etch::AsyncSink::Overflow::kBlock,
      etch::Level::kWarn, etch::AsyncSink::WorkerPriority::kInherit);
  etch::Logger plain_log{"prio-plain", {plain}};
  plain_log.warn("on the plain worker");
  plain->flush();
  CHECK(!plain->worker_priority_raised());
  CHECK_EQ(on_plain_worker.load(), normal);
}

}  // namespace
