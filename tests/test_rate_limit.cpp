// Tests: rate limiting per call site (every, once) and the debugger sink.
#include "support.hpp"

namespace {

using namespace std::chrono_literals;

std::optional<std::int64_t> suppressed_field(const etch::Record& r) {
  for (const auto& f : r.fields) {
    if (f.key == "suppressed") {
      if (const auto* n = std::get_if<std::int64_t>(&f.value)) return *n;
    }
  }
  return std::nullopt;
}

TEST(a_rate_gated_scope_with_a_threshold_is_decided_by_the_slow_scopes) {
  Captured c;
  // The same call site: one fast scope, then two slow ones. The fast one must not spend the
  // budget, and there is no begin line to spend it on; the first slow one is the one reported.
  constexpr auto kThreshold = 100ms;  // far above what a descheduled fast scope could reach
  constexpr auto kSlow = 150ms;
  for (int i = 0; i < 3; ++i) {
    auto scope = c.log.once().scope(std::format("frame {}", i),
                                    {.level = etch::Level::kWarn, .min_duration = kThreshold});
    if (i > 0) std::this_thread::sleep_for(kSlow);
  }
  const auto records = c.sink->records();
  CHECK_EQ(records.size(), 1u);
  if (records.empty()) return;
  // The first slow scope: frame 1, unless the machine stalled frame 0 past the threshold,
  // which the duration check then still accepts.
  CHECK(records[0].message.starts_with("frame "));
  CHECK(records[0].duration.has_value() && *records[0].duration >= kThreshold);
  CHECK(!suppressed_field(records[0]));
}

TEST(with_then_every_means_the_same_as_every_then_with) {
  Captured c;
  for (int i = 0; i < 3; ++i) c.log.with({{"k", i}}).every(1h).warn("w {}", i);
  for (int i = 0; i < 3; ++i) c.log.with({{"k", i}}).once().warn("o {}", i);
  CHECK_EQ(c.messages(), (std::vector<std::string>{"w 0", "o 0"}));
  for (const auto& r : c.sink->records()) {
    CHECK_EQ(r.fields.size(), 1u);
    CHECK(!r.fields.empty() && r.fields[0].key == "k");
  }
}

TEST(an_entry_takes_a_scope_level_like_the_logger_does) {
  Captured c;
  {
    auto scope = c.log.with({{"k", 1}}).scope("load", etch::Level::kWarn);
  }
  const auto records = c.sink->records();
  CHECK_EQ(records.size(), 2u);
  if (records.size() < 2) return;
  CHECK_EQ(records[0].message, std::string("load: begin"));
  CHECK(records[1].message.starts_with("load: done in"));
  CHECK(records[0].fields.size() == 1 && records[1].fields.size() == 1);
}

TEST(once_logs_the_first_call_only_per_call_site) {
  Captured c;
  for (int i = 0; i < 5; ++i) c.log.once().warn("first site {}", i);
  for (int i = 0; i < 5; ++i) c.log.once().warn("second site {}", i);
  CHECK_EQ(c.messages(), (std::vector<std::string>{"first site 0", "second site 0"}));
  CHECK(!suppressed_field(c.sink->records()[0]));  // nothing was held back before the first
}

TEST(every_passes_one_call_per_interval_and_counts_the_rest) {
  Captured c;
  // A long interval: within it only the first call passes, whatever the machine's speed.
  for (int i = 0; i < 10; ++i) c.log.every(1h).warn("spam {}", i);
  CHECK_EQ(c.messages(), (std::vector<std::string>{"spam 0"}));
  // A short interval, waited out before the last call of the same site: that call passes and
  // says how many were held back.
  for (int i = 0; i < 5; ++i) {
    if (i == 4) std::this_thread::sleep_for(20ms);
    c.log.every(5ms).warn("burst {}", i);
  }
  const auto records = c.sink->records();
  CHECK_EQ(records.size(), 3u);
  if (records.size() < 3) return;
  CHECK_EQ(records[1].message, std::string("burst 0"));
  CHECK(!suppressed_field(records[1]));
  CHECK_EQ(records[2].message, std::string("burst 4"));
  CHECK(suppressed_field(records[2]) == std::optional<std::int64_t>{3});
}

TEST(rate_gated_calls_below_the_level_are_not_counted) {
  Captured c;
  c.log.set_level(etch::Level::kError);
  for (int i = 0; i < 3; ++i) c.log.every(1h).warn("filtered {}", i);
  c.log.set_level(etch::Level::kTrace);
  c.log.every(1h).warn("passes: the filtered calls did not use up the budget");
  CHECK_EQ(c.messages().size(), 1u);
}

// One helper, one call site, so the filtered and the enabled call share a budget.
void once_per_site(etch::Logger& log) { log.once().warn("from the helper"); }
void scope_once_per_site(etch::Logger& log) {
  auto timer = log.once().scope("helper", etch::ScopeOptions{.level = etch::Level::kWarn});
}

TEST(a_filtered_call_at_a_site_does_not_use_its_budget) {
  Captured c;
  c.log.set_level(etch::Level::kError);
  once_per_site(c.log);
  scope_once_per_site(c.log);
  c.log.set_level(etch::Level::kTrace);
  once_per_site(c.log);
  scope_once_per_site(c.log);
  CHECK_EQ(c.messages().size(), 3u);  // the message, the scope's begin and end lines
}

TEST(the_generated_suppressed_field_replaces_a_supplied_one) {
  Captured c;
  for (int i = 0; i < 3; ++i) {
    if (i == 2) std::this_thread::sleep_for(20ms);
    c.log.every(5ms).with({{"suppressed", 99}}).warn("pass {}", i);
  }
  const auto records = c.sink->records();
  CHECK_EQ(records.size(), 2u);
  if (records.size() < 2) return;
  std::size_t count = 0;
  for (const auto& f : records[1].fields) count += f.key == "suppressed";
  CHECK_EQ(count, 1u);
  CHECK(suppressed_field(records[1]) == std::optional<std::int64_t>{1});
}

TEST(rate_gated_entry_carries_fields_and_gates_hexdump_and_scope) {
  Captured c;
  for (int i = 0; i < 3; ++i) {
    c.log.every(1h).with({{"fps", 12}}).warn("slow");
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    c.log.every(1h).hexdump(etch::Level::kWarn, "bytes", bytes);
    auto timer = c.log.once().scope("load", etch::ScopeOptions{.level = etch::Level::kWarn});
  }
  const auto records = c.sink->records();
  CHECK_EQ(records.size(), 4u);  // slow, the hexdump, the scope's begin and end lines
  if (records.size() < 4) return;
  CHECK_EQ(records[0].fields.size(), 1u);
  CHECK(records[0].fields.size() == 1 && records[0].fields[0].key == "fps");
  CHECK_EQ(records[1].bytes.size(), 4u);
  CHECK(records[2].message.starts_with("load"));
  CHECK(records[3].message.starts_with("load"));
}

TEST(free_function_every_and_once_use_the_default_logger) {
  auto sink = std::make_shared<etch::MemorySink>();
  const auto previous = etch::default_logger().sinks();
  etch::default_logger().set_sinks({sink});
  for (int i = 0; i < 3; ++i) etch::once().warn("free once");
  for (int i = 0; i < 3; ++i) etch::every(1h).warn("free every");
  etch::default_logger().set_sinks(previous);
  CHECK_EQ(sink->messages(), (std::vector<std::string>{"free once", "free every"}));
}

TEST(debugger_sink_accepts_records_without_colour) {
  auto sink = std::make_shared<etch::DebuggerSink>(etch::LineStyle{.colour = true});
  etch::Logger log{"dbg", {sink}};
  log.warn("to the debugger, if one is listening");
  log.hexdump(etch::Level::kDebug, "bytes", std::array<std::uint8_t, 3>{1, 2, 3});
  sink->set_style(etch::LineStyle{.colour = true, .format = etch::LineFormat::kJson});
  log.with({{"k", 1}}).error("as JSON");
  CHECK(true);  // what reaches OutputDebugString cannot be read back; the point is no crash
}

// ---- held-back counts at the end (0.5.0) --------------------------------------------------

std::optional<std::int64_t> int_field(const etch::Record& r, std::string_view key) {
  for (const auto& f : r.fields) {
    if (f.key == key) {
      if (const auto* n = std::get_if<std::int64_t>(&f.value)) return *n;
    }
  }
  return std::nullopt;
}

// Game testbed, finding 17: after 1,000 errors through every(1h) the summary said error=1 and
// nothing reported the other 999. The count still pending is now written as one line in the
// shape of the first held call: its level, logger, message and fields, plus `suppressed`.
TEST(held_back_counts_are_reported_in_the_first_held_calls_shape) {
  Captured c("pending");
  for (int i = 0; i < 5; ++i) c.log.every(1h).with({{"frame", i}}).warn("slow frame {}", i);
  CHECK_EQ(c.messages(), (std::vector<std::string>{"slow frame 0"}));
  etch::detail::report_pending_rates();
  const auto records = c.sink->records();
  CHECK_EQ(records.size(), 2u);
  if (records.size() < 2) return;
  const auto& held = records[1];
  CHECK_EQ(held.message, std::string("slow frame 1"));
  CHECK(held.level == etch::Level::kWarn);
  CHECK_EQ(held.logger_name, std::string("pending"));
  CHECK(suppressed_field(held) == std::optional<std::int64_t>{4});
  CHECK(int_field(held, "frame") == std::optional<std::int64_t>{1});  // the held call's own
  CHECK(std::string_view(held.where.file_name()).ends_with("test_rate_limit.cpp"));
  etch::detail::report_pending_rates();  // reported once: the count started again at zero
  CHECK_EQ(c.sink->records().size(), 2u);
}

TEST(a_held_call_keeps_the_context_fields_of_its_thread) {
  Captured c("pending-context");
  {
    const auto request = etch::context({{"request", 7}});
    for (int i = 0; i < 3; ++i) c.log.every(1h).warn("in a request {}", i);
  }
  etch::detail::report_pending_rates();
  const auto records = c.sink->records();
  CHECK_EQ(records.size(), 2u);
  if (records.size() < 2) return;
  CHECK(int_field(records[1], "request") == std::optional<std::int64_t>{7});
}

TEST(once_reports_the_calls_after_the_first_at_the_end) {
  Captured c("once-pending");
  for (int i = 0; i < 4; ++i) c.log.once().error("boot {}", i);
  etch::detail::report_pending_rates();
  const auto records = c.sink->records();
  CHECK_EQ(records.size(), 2u);
  if (records.size() < 2) return;
  CHECK_EQ(records[1].message, std::string("boot 1"));
  CHECK(suppressed_field(records[1]) == std::optional<std::int64_t>{3});
}

TEST(held_hexdumps_and_scopes_are_reported_too) {
  Captured c("shapes");
  const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
  for (int i = 0; i < 3; ++i) c.log.every(1h).hexdump(etch::Level::kWarn, "bad bytes", bytes);
  for (int i = 0; i < 3; ++i) {
    const auto tick = c.log.every(1h).scope("tick", etch::Level::kWarn);
  }
  for (int i = 0; i < 3; ++i) {
    // A threshold scope: gated at the end, by the slow ones; every one of these is slow.
    const auto frame = c.log.every(1h).scope(
        "frame", etch::ScopeOptions{.level = etch::Level::kWarn, .min_duration = 1us});
    const auto until = std::chrono::steady_clock::now() + 200us;
    while (std::chrono::steady_clock::now() < until) {
    }
  }
  etch::detail::report_pending_rates();
  const auto records = c.sink->records();
  std::vector<std::string> held;
  for (const auto& r : records) {
    if (suppressed_field(r)) held.push_back(r.message);
  }
  CHECK_EQ(held.size(), 3u);
  if (held.size() < 3) return;
  CHECK_EQ(held[0], std::string("bad bytes"));
  CHECK_EQ(held[1], std::string("tick"));
  CHECK(held[2].starts_with("frame: done in "));
  for (const auto& r : records) {
    if (suppressed_field(r)) CHECK(suppressed_field(r) == std::optional<std::int64_t>{2});
  }
}

TEST(a_logger_that_is_gone_takes_its_pending_count_with_it) {
  auto sink = std::make_shared<etch::MemorySink>();
  {
    etch::Logger log{"gone-pending", {sink}};
    for (int i = 0; i < 3; ++i) log.every(1h).warn("x {}", i);
  }
  etch::detail::report_pending_rates();
  CHECK_EQ(sink->records().size(), 1u);  // its sinks may be gone too; nothing is guessed at
}

// Child probe: etch::shutdown() writes the pending report before it closes the queues, so
// the lines go through an async sink like any other.
TEST(zz_child_probe_shutdown_reports_the_held_back_counts) {
  if (!in_child()) return;
  auto memory = std::make_shared<etch::MemorySink>();
  auto async = std::make_shared<etch::AsyncSink>(memory);
  etch::Logger log{"shutdown-pending", {async}};
  for (int i = 0; i < 6; ++i) log.every(1h).warn("held {}", i);
  etch::shutdown();
  const auto records = memory->records();
  CHECK_EQ(records.size(), 2u);
  if (records.size() < 2) return;
  CHECK_EQ(records[1].message, std::string("held 1"));
  CHECK(suppressed_field(records[1]) == std::optional<std::int64_t>{5});
}

TEST(shutdown_reports_the_held_back_counts) {
  CHECK_EQ(run_test_in_child("zz_child_probe_shutdown_reports_the_held_back_counts"), 0);
}

// Child probe: counts made while shutdown drains the async sinks (a sink that logs through a
// gate on every record) are reported too, by the second pass, through the then-synchronous
// sinks.
TEST(zz_child_probe_shutdown_reports_counts_made_during_its_drain) {
  if (!in_child()) return;
  auto memory = std::make_shared<etch::MemorySink>();
  etch::Logger nested{"drain-nested", {memory}};
  auto relay = std::make_shared<etch::CallbackSink>([&](const etch::Record& r) {
    // Parked until shutdown has begun, so every nested call happens inside the drain.
    while (!etch::detail::process_exiting().load()) std::this_thread::sleep_for(1ms);
    nested.every(1h).warn("nested {}", r.message);
  });
  auto async = std::make_shared<etch::AsyncSink>(relay);
  etch::Logger log{"drain", {async}};
  for (int i = 0; i < 4; ++i) log.warn("{}", i);
  etch::shutdown();
  const auto records = memory->records();
  CHECK_EQ(records.size(), 2u);
  if (records.size() < 2) return;
  CHECK_EQ(records[0].message, std::string("nested 0"));
  CHECK_EQ(records[1].message, std::string("nested 1"));
  CHECK(suppressed_field(records[1]) == std::optional<std::int64_t>{3});
}

TEST(shutdown_reports_counts_made_during_its_drain) {
  CHECK_EQ(run_test_in_child("zz_child_probe_shutdown_reports_counts_made_during_its_drain"), 0);
}

}  // namespace
