// etch tests: logging from inside a sink's write (deferred delivery, lineage, the rules an
// async worker follows so that sinks feeding each other never wait on or feed each other
// forever).
#include <barrier>

#include "support.hpp"

namespace {

using namespace std::chrono_literals;

// A sink that logs through the logger it belongs to from inside its write, the way a sink
// reports its own trouble. It holds a mutex like every real sink, so reaching itself again
// would deadlock; the logger skips it instead.
struct NestingSink final : etch::Sink {
  etch::Logger* log = nullptr;
  std::mutex mutex;
  std::vector<std::string> seen;
  void write(const etch::Record& r) override {
    std::lock_guard lock(mutex);
    seen.emplace_back(r.user_message());
    log->warn("nested from {}", r.user_message());
  }
};

TEST(a_sink_that_logs_from_its_write_reaches_the_other_sinks_and_skips_itself) {
  auto memory = std::make_shared<etch::MemorySink>();
  auto nesting = std::make_shared<NestingSink>();
  etch::Logger log{"nested", {memory, nesting}};
  nesting->log = &log;
  log.warn("outer");  // the inner call runs while the outer one still uses the same list
  CHECK_EQ(memory->messages(), (std::vector<std::string>{"outer", "nested from outer"}));
  CHECK_EQ(nesting->seen, (std::vector<std::string>{"outer"}));
  CHECK_EQ(nesting->reentries(), std::size_t{1});
  CHECK_EQ(memory->reentries(), std::size_t{0});
}

TEST(a_sink_behind_an_async_sink_that_logs_through_it_is_skipped_not_requeued) {
  auto memory = std::make_shared<etch::MemorySink>();
  auto nesting = std::make_shared<NestingSink>();
  auto async = std::make_shared<etch::AsyncSink>(nesting);
  etch::Logger log{"nested", {memory, async}};
  nesting->log = &log;
  log.warn("outer");
  async->flush();
  // The nested record reached the memory sink (from the worker thread) and nothing else: the
  // async sink in front of the nesting sink was writing, so it was skipped and counted.
  CHECK_EQ(memory->messages(), (std::vector<std::string>{"outer", "nested from outer"}));
  CHECK_EQ(nesting->seen, (std::vector<std::string>{"outer"}));
  CHECK_EQ(async->reentries(), std::size_t{1});
}

TEST(nested_records_follow_the_outer_record_whatever_the_sink_order) {
  auto memory = std::make_shared<etch::MemorySink>();
  auto nesting = std::make_shared<NestingSink>();
  etch::Logger log{"nested", {nesting, memory}};  // the nesting sink writes first this time
  nesting->log = &log;
  log.warn("outer");
  CHECK_EQ(memory->messages(), (std::vector<std::string>{"outer", "nested from outer"}));
  CHECK_EQ(nesting->seen, (std::vector<std::string>{"outer"}));  // it never feeds itself
  CHECK_EQ(nesting->reentries(), std::size_t{1});
}

TEST(a_sink_behind_an_async_sink_that_logs_on_every_write_does_not_feed_the_ring) {
  auto nesting = std::make_shared<NestingSink>();
  auto async = std::make_shared<etch::AsyncSink>(nesting, 2);  // a tiny ring
  etch::Logger log{"loop", {async}};
  nesting->log = &log;
  constexpr int kLines = 100;
  for (int i = 0; i < kLines; ++i) log.warn("outer {}", i);
  async->flush();
  CHECK_EQ(nesting->seen.size(), static_cast<std::size_t>(kLines));
  CHECK_EQ(async->reentries(), static_cast<std::size_t>(kLines));
}

// Two sinks that log on every write and feed each other: the exclusions a nested record
// carries are inherited by whatever its delivery logs, so the pair stops after one round
// instead of alternating forever.
TEST(two_sinks_that_log_on_every_write_into_each_other_stop_after_one_round) {
  auto sink_a = std::make_shared<NestingSink>();
  auto sink_b = std::make_shared<NestingSink>();
  etch::Logger log_a{"a", {sink_a}};
  etch::Logger log_b{"b", {sink_b}};
  sink_a->log = &log_b;  // a's write logs into b
  sink_b->log = &log_a;  // b's write logs into a
  log_a.warn("outer");
  CHECK_EQ(sink_a->seen, (std::vector<std::string>{"outer"}));
  CHECK_EQ(sink_b->seen, (std::vector<std::string>{"nested from outer"}));
  CHECK_EQ(sink_a->reentries(), std::size_t{1});  // the third-level record was for a: excluded
}

// Blocks every write until released, counting the writes that entered.
struct HeldSink final : etch::Sink {
  std::atomic<int> entered{0};
  std::atomic<bool> released{false};
  void write(const etch::Record&) override {
    entered.fetch_add(1);
    while (!released.load()) std::this_thread::sleep_for(1ms);
  }
};

// A record logged on one async sink's worker into another sink's full ring goes straight
// through instead of waiting for a slot: two workers waiting on each other's rings would
// never free one.
TEST(an_async_worker_never_waits_for_another_sinks_full_ring) {
  auto held = std::make_shared<HeldSink>();
  auto b = std::make_shared<etch::AsyncSink>(held, 2);  // blocking, two slots
  etch::Logger log_b{"b", {b}};
  log_b.warn("parks b's worker in the held sink");
  for (int i = 0; i < 5000 && held->entered.load() < 1; ++i) std::this_thread::sleep_for(1ms);
  CHECK_EQ(held->entered.load(), 1);
  log_b.warn("fills slot one");
  log_b.warn("fills slot two");  // b's ring is now full and its worker is parked
  auto relay = std::make_shared<etch::CallbackSink>([&](const etch::Record& r) {
    if (r.logger_name == "a") log_b.warn("from a's worker");
  });
  auto a = std::make_shared<etch::AsyncSink>(relay);
  etch::Logger log_a{"a", {a}};
  log_a.warn("trigger");
  // a's worker runs the relay, sets the nested record aside, then delivers it into b: the
  // ring is full, so it goes straight into the held sink, which counts it on entry.
  for (int i = 0; i < 5000 && held->entered.load() < 2; ++i) std::this_thread::sleep_for(1ms);
  CHECK_EQ(held->entered.load(), 2);
  held->released = true;
  a->flush();
  b->flush();
  CHECK_EQ(held->entered.load(), 4);  // the parked one, the two slots, the one written through
}

// Child probe: after etch::shutdown() a new async sink has no worker and writes through.
TEST(zz_child_probe_async_sink_made_after_shutdown_is_synchronous) {
  if (!in_child()) return;
  etch::shutdown();
  const auto caller = std::this_thread::get_id();
  std::atomic<int> on_caller{0};
  std::atomic<int> written{0};
  auto probe = std::make_shared<etch::CallbackSink>([&](const etch::Record&) {
    if (std::this_thread::get_id() == caller) on_caller.fetch_add(1);
    written.fetch_add(1);
  });
  auto async = std::make_shared<etch::AsyncSink>(probe);
  etch::Logger log{"late", {async}};
  for (int i = 0; i < 10; ++i) log.warn("late {}", i);
  CHECK_EQ(written.load(), 10);    // before the calls returned
  CHECK_EQ(on_caller.load(), 10);  // and on the calling thread: there is no worker
}

// Two callbacks that log on every write, each behind an async sink, each into the other's
// logger: the exclusions travel with the record through the queues, so the pair stops.
TEST(two_sinks_behind_async_sinks_logging_into_each_other_stop) {
  std::shared_ptr<etch::Logger> log_a;
  std::shared_ptr<etch::Logger> log_b;
  std::atomic<int> seen_a{0};
  std::atomic<int> seen_b{0};
  auto callback_a = std::make_shared<etch::CallbackSink>([&](const etch::Record& r) {
    seen_a.fetch_add(1);
    log_b->warn("a saw: {}", r.user_message());
  });
  auto callback_b = std::make_shared<etch::CallbackSink>([&](const etch::Record& r) {
    seen_b.fetch_add(1);
    log_a->warn("b saw: {}", r.user_message());
  });
  auto async_a = std::make_shared<etch::AsyncSink>(callback_a);
  auto async_b = std::make_shared<etch::AsyncSink>(callback_b);
  log_a = std::make_shared<etch::Logger>("a", etch::SinkList{async_a});
  log_b = std::make_shared<etch::Logger>("b", etch::SinkList{async_b});
  log_a->warn("outer");
  std::this_thread::sleep_for(50ms);  // long enough for a runaway pair to show
  async_a->flush();
  async_b->flush();
  CHECK_EQ(seen_a.load(), 1);  // "outer"
  CHECK_EQ(seen_b.load(), 1);  // "a saw: outer"; "b saw: ..." was for a, which it came from
  CHECK_EQ(async_a->reentries(), std::size_t{1});
  log_a.reset();
  log_b.reset();
}

// Two independent fatals, one per worker, each of whose callbacks logs a fatal into the other
// async sink while the other is doing the same (a barrier makes sure of that). The delivery
// of a fatal flushes the sink it went to, and on a worker that must not wait for the other
// worker, which is waiting the same way. A hang here would be the failure.
TEST(fatals_from_two_async_workers_into_each_other_do_not_wait_on_each_other) {
  std::shared_ptr<etch::Logger> log_a;
  std::shared_ptr<etch::Logger> log_b;
  std::barrier both_inside(2);
  auto callback_a = std::make_shared<etch::CallbackSink>([&](const etch::Record& r) {
    if (r.user_message() != "start") return;
    both_inside.arrive_and_wait();
    log_b->fatal("from a");
  });
  auto callback_b = std::make_shared<etch::CallbackSink>([&](const etch::Record& r) {
    if (r.user_message() != "start") return;
    both_inside.arrive_and_wait();
    log_a->fatal("from b");
  });
  auto async_a = std::make_shared<etch::AsyncSink>(callback_a);
  auto async_b = std::make_shared<etch::AsyncSink>(callback_b);
  const etch::Logger::Options no_trace{.stacktrace_on_fatal = false};
  log_a = std::make_shared<etch::Logger>("a", etch::SinkList{async_a}, no_trace);
  log_b = std::make_shared<etch::Logger>("b", etch::SinkList{async_b}, no_trace);
  {
    // Two roots from two threads (a root fatal's own flush waits for its worker, which is
    // waiting at the barrier), so neither returning fatal is excluded by lineage.
    std::jthread root_a([&] { log_a->fatal("start"); });
    std::jthread root_b([&] { log_b->fatal("start"); });
  }
  async_a->flush();
  async_b->flush();
  CHECK(true);  // reached: no worker waited for the other
  log_a.reset();
  log_b.reset();
}

// A sink behind an async sink behind another async sink, logging on every write into the
// outer one: the outer sink forwards into the inner async sink, which is in the record's
// lineage, and that forwarding must check it, since the delivery that chose the outer sink
// could not.
TEST(a_sink_behind_two_async_sinks_logging_into_the_outer_one_stops) {
  auto nesting = std::make_shared<NestingSink>();
  auto inner = std::make_shared<etch::AsyncSink>(nesting);
  auto outer = std::make_shared<etch::AsyncSink>(inner);
  etch::Logger log{"stacked", {outer}};
  nesting->log = &log;
  log.warn("outer");
  // The return record is queued into the outer sink by the inner sink's worker, so a flush of
  // the outer sink can run before it is there: wait for the skip itself, with a bound.
  for (int i = 0; i < 5000 && inner->reentries() < 1; ++i) std::this_thread::sleep_for(1ms);
  std::this_thread::sleep_for(50ms);  // long enough for a runaway chain to show
  outer->flush();
  inner->flush();
  CHECK_EQ(nesting->seen, (std::vector<std::string>{"outer"}));
  CHECK_EQ(inner->reentries(), std::size_t{1});  // stopped at the forwarding into inner
}

// Two callbacks behind two dropping two-slot queues, each bursting three records into the
// other on every write, gap markers included. The nested records' lineage stops the direct
// feedback; the markers a worker makes for the dropped ones carry that lineage too, so they
// cannot restart it. Both callbacks see a bounded number of records.
TEST(gap_markers_do_not_restart_a_chain_between_dropping_queues) {
  std::shared_ptr<etch::Logger> log_a;
  std::shared_ptr<etch::Logger> log_b;
  std::atomic<int> seen_a{0};
  std::atomic<int> seen_b{0};
  std::atomic<bool> release_b{false};
  // More than a two-slot ring plus the two records its worker can hold in a batch.
  constexpr int kBurst = 6;
  auto callback_a = std::make_shared<etch::CallbackSink>([&](const etch::Record&) {
    seen_a.fetch_add(1);
    for (int i = 0; i < kBurst; ++i) log_b->warn("burst from a {}", i);
  });
  auto callback_b = std::make_shared<etch::CallbackSink>([&](const etch::Record&) {
    while (!release_b.load()) std::this_thread::sleep_for(1ms);  // parked: the burst overflows
    seen_b.fetch_add(1);
    for (int i = 0; i < kBurst; ++i) log_a->warn("burst from b {}", i);
  });
  // Warnings, so the min-level build keeps them, and droppable (keep_from fatal) so they can
  // be dropped.
  auto async_a = std::make_shared<etch::AsyncSink>(callback_a, 2, etch::AsyncSink::Overflow::kDrop,
                                                   etch::Level::kFatal);
  auto async_b = std::make_shared<etch::AsyncSink>(callback_b, 2, etch::AsyncSink::Overflow::kDrop,
                                                   etch::Level::kFatal);
  log_a = std::make_shared<etch::Logger>("a", etch::SinkList{async_a});
  log_b = std::make_shared<etch::Logger>("b", etch::SinkList{async_b});
  log_a->warn("start");
  for (int i = 0; i < 5000 && async_b->dropped() < 1; ++i) std::this_thread::sleep_for(1ms);
  CHECK(async_b->dropped() >= 1);  // so a marker will be written
  release_b = true;
  std::this_thread::sleep_for(100ms);  // a runaway pair would still be going
  async_a->flush();
  async_b->flush();
  const auto dropped = static_cast<int>(async_b->dropped());
  CHECK_EQ(seen_a.load(), 1);  // "start" only: every burst back was for a, which it came from
  // The burst minus the drops, plus the markers: at least one, at most one per drop.
  CHECK(seen_b.load() >= kBurst - dropped + 1 && seen_b.load() <= kBurst);
  log_a.reset();
  log_b.reset();
}

// A sink behind two async sinks logging on every write into the outer one, with the inner
// async sink dropping: the nested records the outer sink forwards into the full inner ring
// are dropped there, and the inner sink's gap marker must still reach the terminal sink (it
// is a marker, not a nested record) while what the terminal sink logs in response is kept
// from the outer sink. The marker is seen; the chain ends.
TEST(a_gap_marker_reaches_the_terminal_sink_behind_a_second_async_sink) {
  std::shared_ptr<etch::Logger> log;
  std::atomic<int> markers{0};
  std::atomic<int> records{0};
  std::atomic<bool> release{false};
  auto terminal = std::make_shared<etch::CallbackSink>([&](const etch::Record& r) {
    while (!release.load()) std::this_thread::sleep_for(1ms);  // parked: the inner ring fills
    if (r.logger_name == "etch") {
      markers.fetch_add(1);
    } else {
      records.fetch_add(1);
    }
    log->warn("in response to {}", r.user_message());  // on every write, markers included
  });
  auto inner = std::make_shared<etch::AsyncSink>(terminal, 2, etch::AsyncSink::Overflow::kDrop,
                                                 etch::Level::kFatal);
  auto outer = std::make_shared<etch::AsyncSink>(inner);
  log = std::make_shared<etch::Logger>("stacked", etch::SinkList{outer});
  constexpr int kRoots = 8;  // past the inner ring and batch while the terminal sink is parked
  for (int i = 0; i < kRoots; ++i) log->warn("root {}", i);
  for (int i = 0; i < 5000 && inner->dropped() < 1; ++i) std::this_thread::sleep_for(1ms);
  CHECK(inner->dropped() >= 1);
  release = true;
  std::this_thread::sleep_for(100ms);  // a runaway chain would still be going
  outer->flush();
  inner->flush();
  CHECK(markers.load() >= 1);
  CHECK_EQ(records.load(), kRoots - static_cast<int>(inner->dropped()));  // no response came back
  CHECK(inner->reentries() >=
        static_cast<std::size_t>(records.load()));  // every response stopped at inner
  log.reset();
}

// A sink that takes the record by value, destroys it, and only then logs in response: what it
// logs still inherits the record's ancestry (a gap marker's, say), so the set must outlive
// the record inside the sink's write.
struct ConsumingSink final : etch::Sink {
  etch::Logger* respond_through = nullptr;
  void write(const etch::Record& r) override { write(etch::Record(r)); }
  void write(etch::Record&& r) override {
    {
      const etch::Record consumed = std::move(r);
      (void)consumed;
    }
    respond_through->warn("in response");
  }
};

TEST(a_markers_ancestry_outlives_the_marker_inside_the_sink_that_consumes_it) {
  auto kept_from = std::make_shared<etch::MemorySink>();
  etch::Logger kept_from_log{"victim", {kept_from}};
  auto consuming = std::make_shared<ConsumingSink>();
  consuming->respond_through = &kept_from_log;
  auto async = std::make_shared<etch::AsyncSink>(consuming);
  etch::Record marker = fixed_record();
  marker.ancestry = std::make_shared<std::vector<const etch::Sink*>>(
      std::vector<const etch::Sink*>{kept_from.get()});
  async->write(std::move(marker));  // through the queue, consumed on the worker
  async->flush();
  CHECK_EQ(kept_from->size(), 0u);  // the response inherited the ancestry
  CHECK_EQ(kept_from->reentries(), std::size_t{1});
}

TEST(an_async_sink_made_after_shutdown_writes_straight_through) {
  CHECK_EQ(run_test_in_child("zz_child_probe_async_sink_made_after_shutdown_is_synchronous"), 0);
}

// Two callback sinks that each relay the other logger's records, driven from two threads at
// once. Delivered at once, each thread would hold its sink's mutex while waiting for the
// other's; deferred, neither waits while holding one.
TEST(two_sinks_relaying_into_each_other_from_two_threads_do_not_deadlock) {
  std::shared_ptr<etch::Logger> log_a;
  std::shared_ptr<etch::Logger> log_b;
  auto memory = std::make_shared<etch::MemorySink>(100000);
  std::atomic<int> relayed{0};
  const auto relay = [&](std::shared_ptr<etch::Logger>& other, const etch::Record& r) {
    if (r.user_message().starts_with("relayed")) return;  // relay once, not forever
    other->warn("relayed {}", r.user_message());
    relayed.fetch_add(1);
  };
  auto sink_a =
      std::make_shared<etch::CallbackSink>([&](const etch::Record& r) { relay(log_b, r); });
  auto sink_b =
      std::make_shared<etch::CallbackSink>([&](const etch::Record& r) { relay(log_a, r); });
  log_a = std::make_shared<etch::Logger>("a", etch::SinkList{sink_a, memory});
  log_b = std::make_shared<etch::Logger>("b", etch::SinkList{sink_b, memory});
  constexpr int kPerThread = 2000;
  {
    std::jthread ta([&] {
      for (int i = 0; i < kPerThread; ++i) log_a->warn("a {}", i);
    });
    std::jthread tb([&] {
      for (int i = 0; i < kPerThread; ++i) log_b->warn("b {}", i);
    });
  }
  CHECK_EQ(relayed.load(), 2 * kPerThread);
  CHECK_EQ(memory->total(), static_cast<std::size_t>(4 * kPerThread));
}

}  // namespace
