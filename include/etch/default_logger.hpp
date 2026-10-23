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

// The named loggers, and the levels ETCH_LEVEL or apply_level_spec() asked for by name, kept
// so a logger made later under that name starts at its level. Never destroyed: loggers are
// used from static destructors.
struct LoggerRegistry {
  std::mutex mutex;
  std::map<std::string, std::unique_ptr<Logger>, std::less<>> loggers;
  std::map<std::string, Level, std::less<>> levels;
};
inline LoggerRegistry& logger_registry() {
  static LoggerRegistry* const registry = new LoggerRegistry;
  return *registry;
}

// Applies a spec given the root logger, so it can run while default_logger() is still being
// initialised (the public apply_level_spec() calls default_logger(), which would re-enter that
// initialisation). Takes the registry lock, so the caller must not hold it.
inline void apply_level_spec_to(Logger& root, const LevelSpec& spec) {
  if (spec.root) root.set_level(*spec.root);
  LoggerRegistry& registry = logger_registry();
  std::lock_guard lock(registry.mutex);
  for (const auto& [name, level] : spec.loggers) {
    registry.levels.insert_or_assign(name, level);
    if (const auto it = registry.loggers.find(name); it != registry.loggers.end()) {
      it->second->set_level(level);
    }
  }
}

// Parses and applies text; a bad value is reported on the root logger and nothing is applied.
inline bool apply_level_text_to(Logger& root, std::string_view text) {
  const std::expected<LevelSpec, std::string> spec = parse_level_spec(text);
  if (!spec) {
    root.warn("etch: level spec '{}' ignored: {}", text, spec.error());
    return false;
  }
  apply_level_spec_to(root, *spec);
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
    if (!text.empty()) detail::apply_level_text_to(*logger, text);
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
    logger->set_level(it->second);
  }
  Logger& ref = *logger;
  registry.loggers.emplace(std::string(name), std::move(logger));
  return ref;
}

// Sets levels from a spec: the root level on the default logger, each named level on that
// etch::get() logger, now if it exists and when it is first made otherwise. A level set this
// way is a plain set_level(): a later set_level() call replaces it.
inline void apply_level_spec(const LevelSpec& spec) {
  detail::apply_level_spec_to(default_logger(), spec);
}

// Parses and applies "warn,net=debug" text, such as a --log-level argument. An entry that is
// not a level is reported on the default logger and the whole text is ignored, so a typo turns
// one subsystem's silence into a warning rather than a surprise.
inline bool apply_level_spec(std::string_view text) {
  return detail::apply_level_text_to(default_logger(), text);
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
