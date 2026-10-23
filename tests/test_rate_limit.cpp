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

}  // namespace
