// etch - the process-wide default logger, the registry of named loggers behind etch::get(),
// levels from ETCH_LEVEL or a level spec, and the free functions (etch::info(...)) that log
// through the default logger.
#pragma once

#include <chrono>
#include <cstdlib>
#include <expected>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "level.hpp"
#include "logger.hpp"
#include "record.hpp"
#include "sinks.hpp"

namespace etch {

// A process-wide logger named "main" that writes to the console. Swap its sinks with
// default_logger().set_sinks(...) to send it elsewhere; etch::get("name") makes named children
// that share them.
//
// It is never destroyed, on purpose: objects with static storage may log from their
// destructors, which can run after a function-local static logger would already be gone. An
// atexit handler registered on first use flushes its sinks and marks the process as exiting,
// after which file sinks flush every line, so even a destructor that runs later (one whose
// object was constructed before the logger was first used) gets its lines out. Async sinks are
// drained and switched to synchronous writes by their own exit handler (see etch::shutdown).
// The environment variable read once, when the default logger is first used; see
// apply_level_spec below.
inline constexpr const char* kLevelEnvironmentVariable = "ETCH_LEVEL";

namespace detail {

// The named loggers, and the levels ETCH_LEVEL or apply_level_spec() asked for by name before
// the logger existed, so one made later under that name starts at that level as its own.
// Never destroyed: loggers are used from static destructors.
struct LoggerRegistry {
  std::mutex mutex;
  std::map<std::string, std::unique_ptr<Logger>, std::less<>> loggers;
  std::map<std::string, Level, std::less<>> levels;  // for loggers not made yet
};
inline LoggerRegistry& logger_registry() {
  static LoggerRegistry* const registry = new LoggerRegistry;
  return *registry;
}

// Applies a spec given the root logger, so it can run while default_logger() is still being
// initialised (the public apply_level_spec() calls default_logger(), which would re-enter that
// initialisation). Takes the registry lock, so the caller must not hold it. A bare level
// reaches the loggers that have no level of their own (see Logger::set_level); "*" reaches all
// of them and takes their own levels away. A name that matches no logger made so far is stored
// for when one is made, and reported on the root logger when `warn_unknown` is set and other
// loggers already exist: at start-up, before any subsystem has logged, every name is unknown
// and silence is right; from a console command in a running program, a typo should say so.
// The report goes out before the spec is applied, so a spec cannot silence its own diagnosis.
inline void apply_level_spec_to(Logger& root, const LevelSpec& spec, bool warn_unknown) {
  LoggerRegistry& registry = logger_registry();
  if (warn_unknown) {
    std::vector<std::pair<std::string, Level>> unknown;
    {
      std::lock_guard lock(registry.mutex);
      if (!registry.loggers.empty()) {
        for (const auto& [name, level] : spec.loggers) {
          if (!registry.loggers.contains(name)) unknown.emplace_back(name, level);
        }
      }
    }
    for (const auto& [name, level] : unknown) {
      root.report(
          Level::kWarn,
          "etch: level spec names logger '{}', which does not exist yet (see etch::loggers()); "
          "it will start at {} if it is made",
          name, level_name(level));
    }
  }
  std::lock_guard lock(registry.mutex);
  if (spec.all) {
    registry.levels.clear();
    root.set_level(*spec.all, /*own=*/false);
    for (const auto& [name, logger] : registry.loggers) {
      logger->set_level(*spec.all, /*own=*/false);
    }
  } else if (spec.root) {
    root.set_level(*spec.root, /*own=*/false);
    for (const auto& [name, logger] : registry.loggers) logger->inherit_level(*spec.root);
  }
  for (const auto& [name, level] : spec.loggers) {
    if (const auto it = registry.loggers.find(name); it != registry.loggers.end()) {
      it->second->set_level(level);
    } else {
      registry.levels.insert_or_assign(name, level);
    }
  }
}

// Parses and applies text; a bad value is reported on the root logger and nothing is applied.
inline bool apply_level_text_to(Logger& root, std::string_view text, bool warn_unknown) {
  const std::expected<LevelSpec, std::string> spec = parse_level_spec(text);
  if (!spec) {
    root.report(Level::kWarn, "etch: level spec '{}' ignored: {}", text, spec.error());
    return false;
  }
  apply_level_spec_to(root, *spec, warn_unknown);
  return true;
}

}  // namespace detail

inline Logger& default_logger() {
  // One guarded static: everything the first use needs, ETCH_LEVEL included, happens inside
  // this initialiser, through helpers that take the logger rather than calling back here.
  static Logger* const instance = [] {
    auto* logger = new Logger{"main", {std::make_shared<ConsoleSink>()}};
    std::atexit([] {
      detail::process_exiting().store(true, std::memory_order_relaxed);
      try {
        default_logger().flush();
      } catch (...) {
        // A sink that throws on flush at exit has nowhere left to report to.
      }
    });
    const std::string_view text = detail::env(kLevelEnvironmentVariable);
    if (!text.empty()) detail::apply_level_text_to(*logger, text, /*warn_unknown=*/false);
    return logger;
  }();
  return *instance;
}

// A named logger that shares the default logger's sinks: etch::get("render").info(...). The
// same name always gives the same logger, created on first use and never destroyed, so it can
// be used from static destructors like the default logger.
inline Logger& get(std::string_view name) {
  Logger& root = default_logger();  // before the lock: its first use may itself need the lock
  detail::LoggerRegistry& registry = detail::logger_registry();
  std::lock_guard lock(registry.mutex);
  if (const auto it = registry.loggers.find(name); it != registry.loggers.end()) {
    return *it->second;
  }
  auto logger = std::make_unique<Logger>(root.child(std::string(name)));
  if (const auto it = registry.levels.find(name); it != registry.levels.end()) {
    logger->set_level(it->second);  // asked for by name before it existed: its own level
    registry.levels.erase(it);
  }
  Logger& ref = *logger;
  registry.loggers.emplace(std::string(name), std::move(logger));
  return ref;
}

// The names of the etch::get() loggers made so far, in order. For a console command that
// lists what "/log name=level" can name.
inline std::vector<std::string> loggers() {
  detail::LoggerRegistry& registry = detail::logger_registry();
  std::lock_guard lock(registry.mutex);
  std::vector<std::string> names;
  names.reserve(registry.loggers.size());
  for (const auto& [name, logger] : registry.loggers) names.push_back(name);
  return names;
}

// Sets levels from a spec (see LevelSpec): a bare level on the default logger and on every
// etch::get() logger that has no level of its own (one never given a level by name, in a spec
// or by set_level); each named level on that logger, as its own, now if it exists and when it
// is first made otherwise; "*=level" on every logger, taking their own levels away. A name that
// matches no logger yet, while others exist, is reported on the default logger.
inline void apply_level_spec(const LevelSpec& spec) {
  detail::apply_level_spec_to(default_logger(), spec, /*warn_unknown=*/true);
}

// Parses and applies "warn,net=debug" text, such as a --log-level argument. An entry that is
// not a level, or not a logger name, is reported on the default logger and the whole text is
// ignored, so a typo turns one subsystem's silence into a warning rather than a surprise.
inline bool apply_level_spec(std::string_view text) {
  return detail::apply_level_text_to(default_logger(), text, /*warn_unknown=*/true);
}

// Reads ETCH_LEVEL and applies it; false when the variable is empty or unset. The first use of
// default_logger() does this itself; call it after changing the variable from inside the
// program.
inline bool apply_levels_from_environment() {
  const std::string_view text = detail::env(kLevelEnvironmentVariable);
  if (text.empty()) return false;
  return apply_level_spec(text);
}

// Free functions on the default logger: etch::info("hello {}", name).
template <class... Args>
void trace(FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) {
  default_logger().log_at<Level::kTrace>(fmt, std::forward<Args>(args)...);
}
template <class... Args>
void debug(FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) {
  default_logger().log_at<Level::kDebug>(fmt, std::forward<Args>(args)...);
}
template <class... Args>
void info(FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) {
  default_logger().log_at<Level::kInfo>(fmt, std::forward<Args>(args)...);
}
template <class... Args>
void warn(FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) {
  default_logger().log_at<Level::kWarn>(fmt, std::forward<Args>(args)...);
}
template <class... Args>
void error(FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) {
  default_logger().log_at<Level::kError>(fmt, std::forward<Args>(args)...);
}
template <class... Args>
void fatal(FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) {
  default_logger().log_at<Level::kFatal>(fmt, std::forward<Args>(args)...);
}
template <class... Args>
void log(Level level, FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) {
  default_logger().log(level, fmt, std::forward<Args>(args)...);
}
inline Logger::Entry every(std::chrono::nanoseconds interval) {
  return default_logger().every(interval);
}
inline Logger::Entry once() { return default_logger().once(); }
inline Logger::Entry with(std::initializer_list<Field> fields) {
  return default_logger().with(fields);
}
inline Logger::Entry with(Level gate, std::initializer_list<Field> fields) {
  return default_logger().with(gate, fields);
}
inline Logger::Entry with(Fields fields) { return default_logger().with(std::move(fields)); }
inline void hexdump(Level level, std::string_view label, const void* data, std::size_t size,
                    std::size_t limit = kDefaultHexdumpLimit,
                    std::source_location where = std::source_location::current()) {
  default_logger().hexdump(level, label, data, size, limit, where);
}
template <std::ranges::contiguous_range R>
  requires std::ranges::sized_range<R>
void hexdump(Level level, std::string_view label, const R& range,
             std::size_t limit = kDefaultHexdumpLimit,
             std::source_location where = std::source_location::current()) {
  default_logger().hexdump(level, label, range, limit, where);
}
[[nodiscard]] inline ScopeTimer scope(
    std::string_view what, Level level = Level::kDebug,
    std::source_location where = std::source_location::current()) {
  return default_logger().scope(what, level, where);
}
[[nodiscard]] inline ScopeTimer scope(
    std::string_view what, ScopeOptions options,
    std::source_location where = std::source_location::current()) {
  return default_logger().scope(what, options, where);
}

}  // namespace etch
