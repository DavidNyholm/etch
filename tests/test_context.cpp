// Tests: context fields (per-thread, scoped) and levels from a spec or the environment.
#include "support.hpp"

#if defined(_WIN32)
#include <cstdlib>
#else
#include <stdlib.h>

#include <sys/wait.h>  // WIFEXITED/WEXITSTATUS, to decode what std::system returns
#endif

namespace {

void set_environment(const char* name, const char* value) {
#if defined(_WIN32)
  _putenv_s(name, value);
#else
  if (std::string_view(value).empty()) {
    unsetenv(name);
  } else {
    setenv(name, value, 1);
  }
#endif
}

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
constexpr const char* kChildMarker = "ETCH_TEST_CHILD";
int run_fresh_with_level(const char* level, const char* test_name) {
  set_environment(etch::kLevelEnvironmentVariable, level);
  set_environment(kChildMarker, "1");
  // The path has a space in it on this project, so it is quoted. cmd.exe strips one outer pair
  // of quotes from the whole line, so it gets a second pair; sh would merge that pair with the
  // path into one program name, so it gets the plain form.
  const std::string quoted = "\"" + test_executable() + "\" " + test_name;
#if defined(_WIN32)
  const std::string command = "\"" + quoted + "\"";
  const int code = std::system(command.c_str());
#else
  const int status = std::system(quoted.c_str());
  const int code = WIFEXITED(status) ? WEXITSTATUS(status) : status;
#endif
  set_environment(kChildMarker, "");
  set_environment(etch::kLevelEnvironmentVariable, "");
  return code;
}
bool in_child() { return !etch::detail::env(kChildMarker).empty(); }

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
  if (test_executable().empty()) {
    CHECK(false && "test executable path unknown");
    return;
  }
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

}  // namespace
