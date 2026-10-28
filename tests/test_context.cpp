// Tests: context fields (per-thread, scoped) and levels from a spec or the environment.
#include "support.hpp"

namespace {

using Pair = std::pair<std::string, etch::FieldValue>;
std::vector<Pair> pairs(const etch::Fields& fields) {
  std::vector<Pair> out;
  for (const auto& f : fields) out.emplace_back(f.key, f.value);
  return out;
}
etch::FieldValue number(std::int64_t n) { return etch::FieldValue{n}; }
etch::FieldValue text(std::string_view s) { return etch::FieldValue{std::string(s)}; }

TEST(context_fields_reach_every_record_while_the_context_lives) {
  Captured c;
  {
    auto ctx = etch::context({{"request", "r-17"}, {"frame", 1442}});
    c.log.warn("inside");
    auto timer = c.log.scope("scoped", etch::Level::kWarn);
  }
  c.log.warn("outside");
  const auto records = c.sink->records();
  CHECK_EQ(records.size(), 4u);
  if (records.size() < 4) return;
  const std::vector<Pair> expected{{"request", text("r-17")}, {"frame", number(1442)}};
  CHECK(pairs(records[0].fields) == expected);  // the log call
  CHECK(pairs(records[1].fields) == expected);  // the scope's begin line
  CHECK(pairs(records[2].fields) == expected);  // the scope's end line, delivered on its own path
  CHECK(records[3].fields.empty());
  CHECK(etch::Context::current().empty());
}

TEST(inner_context_and_call_fields_shadow_outer_keys) {
  Captured c;
  auto outer = etch::context({{"a", 1}, {"b", 2}});
  {
    auto inner = etch::context({{"b", 3}});
    CHECK_EQ(etch::Context::current().size(), 3u);
    c.log.warn("nested");
    c.log.with({{"a", 9}, {"c", 4}}).warn("with");
  }
  CHECK_EQ(etch::Context::current().size(), 2u);
  const auto records = c.sink->records();
  CHECK_EQ(records.size(), 2u);
  if (records.size() < 2) return;
  CHECK(pairs(records[0].fields) == (std::vector<Pair>{{"a", number(1)}, {"b", number(3)}}));
  CHECK(pairs(records[1].fields) ==
        (std::vector<Pair>{{"b", number(3)}, {"a", number(9)}, {"c", number(4)}}));
}

TEST(context_belongs_to_the_thread_that_made_it) {
  Captured c;
  auto ctx = etch::context({{"thread", "main"}});
  std::jthread other([&] {
    CHECK(etch::Context::current().empty());
    c.log.warn("from the other thread");
    auto own = etch::context({{"thread", "other"}});
    c.log.warn("with its own");
  });
  other.join();
  c.log.warn("from the main thread");
  const auto records = c.sink->records();
  CHECK_EQ(records.size(), 3u);
  if (records.size() < 3) return;
  CHECK(records[0].fields.empty());
  CHECK(pairs(records[1].fields) == (std::vector<Pair>{{"thread", text("other")}}));
  CHECK(pairs(records[2].fields) == (std::vector<Pair>{{"thread", text("main")}}));
}

TEST(level_spec_parses_root_and_named_levels) {
  const auto empty = etch::parse_level_spec("");
  CHECK(empty && !empty->root && empty->loggers.empty());
  const auto root = etch::parse_level_spec("warn");
  CHECK(root && root->root == etch::Level::kWarn && root->loggers.empty());
  const auto mixed = etch::parse_level_spec(" Error , net = debug ; render=TRACE,, ");
  CHECK(mixed && mixed->root == etch::Level::kError);
  if (mixed && mixed->loggers.size() == 2) {
    CHECK_EQ(mixed->loggers[0].first, std::string("net"));
    CHECK(mixed->loggers[0].second == etch::Level::kDebug);
    CHECK_EQ(mixed->loggers[1].first, std::string("render"));
    CHECK(mixed->loggers[1].second == etch::Level::kTrace);
  } else {
    CHECK(false && "two named levels expected");
  }
  const auto bogus = etch::parse_level_spec("warn,net=loud");
  CHECK(!bogus && bogus.error().find("loud") != std::string::npos);
  const auto nameless = etch::parse_level_spec("=debug");
  CHECK(!nameless && nameless.error().find("names no logger") != std::string::npos);
  CHECK(!etch::parse_level_spec("net="));
}

TEST(level_spec_applies_to_existing_and_future_named_loggers) {
  const etch::Level previous = etch::default_logger().get_level();
  etch::Logger& existing = etch::get("spec-existing");
  CHECK(etch::apply_level_spec("error,spec-existing=trace,spec-later=debug"));
  CHECK(etch::default_logger().get_level() == etch::Level::kError);
  CHECK(existing.get_level() == etch::Level::kTrace);
  CHECK(etch::get("spec-later").get_level() == etch::Level::kDebug);
  CHECK(etch::get("spec-untouched").get_level() == etch::Level::kError);  // inherits the root
  etch::default_logger().set_level(previous);
}

TEST(a_bad_level_spec_is_reported_and_ignored) {
  auto sink = std::make_shared<etch::MemorySink>();
  const auto previous_sinks = etch::default_logger().sinks();
  const etch::Level previous = etch::default_logger().get_level();
  etch::default_logger().set_sinks({sink});
  CHECK(!etch::apply_level_spec("fatal,spec-bad=verbose"));
  etch::default_logger().set_sinks(previous_sinks);
  CHECK(etch::default_logger().get_level() == previous);  // nothing from the text was applied
  const auto messages = sink->messages();
  CHECK_EQ(messages.size(), 1u);
  CHECK(!messages.empty() && messages[0].find("verbose") != std::string::npos);
  CHECK(!messages.empty() && messages[0].find("ignored") != std::string::npos);
}

// Runs one test of this binary in a fresh process with ETCH_LEVEL set, so the first use of
// the default logger (or of get()) happens with the variable in force. Returns the exit code.
int run_fresh_with_level(const char* level, const char* test_name) {
  return run_test_in_child(test_name, {{etch::kLevelEnvironmentVariable, level}});
}

// The probes below do their checks in the child only, where nothing has touched the default
// logger before them; in a normal run they pass vacuously.
TEST(zz_child_probe_default_logger_first) {
  if (!in_child()) return;
  CHECK(etch::default_logger().get_level() == etch::Level::kWarn);
  CHECK(etch::get("net").get_level() == etch::Level::kDebug);
}
TEST(zz_child_probe_get_first) {
  if (!in_child()) return;
  CHECK(etch::get("net").get_level() == etch::Level::kDebug);
  CHECK(etch::default_logger().get_level() == etch::Level::kWarn);
}
TEST(zz_child_probe_bad_value) {
  if (!in_child()) return;
  CHECK(etch::default_logger().get_level() == etch::Level::kTrace);  // untouched
}

TEST(environment_levels_apply_on_the_very_first_use) {
  CHECK_EQ(run_fresh_with_level("warn,net=debug", "zz_child_probe_default_logger_first"), 0);
  CHECK_EQ(run_fresh_with_level("warn,net=debug", "zz_child_probe_get_first"), 0);
  CHECK_EQ(run_fresh_with_level("warn,net=loud", "zz_child_probe_bad_value"), 0);
}

TEST(levels_come_from_the_environment_variable) {
  const etch::Level previous = etch::default_logger().get_level();
  set_environment(etch::kLevelEnvironmentVariable, "spec-env=fatal");
  CHECK(etch::apply_levels_from_environment());
  CHECK(etch::get("spec-env").get_level() == etch::Level::kFatal);
  set_environment(etch::kLevelEnvironmentVariable, "");
  CHECK(!etch::apply_levels_from_environment());
  etch::default_logger().set_level(previous);
}

// ---- what a level spec reaches ------------------------------------------------------------

TEST(a_bare_level_reaches_existing_loggers_without_a_level_of_their_own) {
  const etch::Level previous = etch::default_logger().get_level();
  etch::Logger& plain = etch::get("spec-plain");
  etch::Logger& named = etch::get("spec-named");
  CHECK(etch::apply_level_spec("spec-named=debug"));
  CHECK(etch::apply_level_spec("warn"));
  CHECK(etch::default_logger().get_level() == etch::Level::kWarn);
  CHECK(plain.get_level() == etch::Level::kWarn);   // followed the bare level
  CHECK(named.get_level() == etch::Level::kDebug);  // kept its own
  CHECK_EQ(etch::to_string(etch::get("spec-made-after").get_level()), std::string_view("WARN"));
  // "*=level" sets every logger and forgets the named levels.
  CHECK(etch::apply_level_spec("*=error"));
  CHECK(named.get_level() == etch::Level::kError);
  CHECK(plain.get_level() == etch::Level::kError);
  CHECK(etch::default_logger().get_level() == etch::Level::kError);
  CHECK(etch::apply_level_spec("info"));
  CHECK(named.get_level() == etch::Level::kInfo);  // no longer its own
  etch::LevelSpec restore;
  restore.all = previous;
  etch::apply_level_spec(restore);
}

TEST(a_level_set_by_hand_is_the_loggers_own_and_a_bare_spec_leaves_it_alone) {
  const etch::Level previous = etch::default_logger().get_level();
  etch::Logger& own = etch::get("spec-own");
  CHECK(!own.has_own_level());  // inherited from the default logger
  own.set_level(etch::Level::kDebug);
  CHECK(own.has_own_level());
  CHECK(etch::apply_level_spec("error"));
  CHECK(own.get_level() == etch::Level::kDebug);
  CHECK(etch::apply_level_spec("*=info"));  // takes the own levels away
  CHECK(own.get_level() == etch::Level::kInfo && !own.has_own_level());
  CHECK(etch::apply_level_spec("warn"));
  CHECK(own.get_level() == etch::Level::kWarn);
  etch::LevelSpec restore;
  restore.all = previous;
  etch::apply_level_spec(restore);
}

TEST(loggers_lists_the_names_made_so_far_and_a_bad_name_is_rejected) {
  etch::get("spec-listed");
  const auto names = etch::loggers();
  CHECK(std::find(names.begin(), names.end(), "spec-listed") != names.end());
  CHECK(!etch::parse_level_spec("my net=debug"));
  CHECK(!etch::parse_level_spec("my\vnet=debug"));
  CHECK(!etch::parse_level_spec("net*=debug"));
  // The 0.4 aggregate form, {root, {names}}, must still compile and mean the same: the new
  // member comes last. GCC's -Wextra warns about the member left out; that is the point here.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
  const etch::LevelSpec aggregate{etch::Level::kWarn, {{"net", etch::Level::kDebug}}};
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
  CHECK(aggregate.root == etch::Level::kWarn && aggregate.loggers.size() == 1);
  const auto star = etch::parse_level_spec("*=debug");
  CHECK(star && star->all == etch::Level::kDebug && !star->root);
  const etch::Level previous = etch::get("spec-listed").get_level();
  CHECK(!etch::apply_level_spec("net*=debug,spec-listed=fatal"));  // reported, nothing applied
  CHECK(etch::get("spec-listed").get_level() == previous);
}

TEST(a_spec_naming_a_logger_that_does_not_exist_yet_is_reported_and_remembered) {
  const auto previous_sinks = etch::default_logger().sinks();
  const etch::Level previous = etch::default_logger().get_level();
  // Muted: etch's word about a spec must still come out, since the spec may be what muted it.
  etch::default_logger().set_level(etch::Level::kOff);
  Captured c;
  etch::default_logger().set_sinks({c.sink});
  etch::get("spec-exists");
  CHECK(etch::apply_level_spec("spec-missing=error"));
  etch::default_logger().set_sinks(previous_sinks);
  etch::default_logger().set_level(previous);
  const auto messages = c.messages();
  CHECK_EQ(messages.size(), 1u);
  CHECK(!messages.empty() && messages[0].find("'spec-missing'") != std::string::npos);
  CHECK(etch::get("spec-missing").get_level() == etch::Level::kError);  // remembered
}

}  // namespace
