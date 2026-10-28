// etch - Level: the six severities (plus "off" as a filter setting), their names, parsing, and a
// std::formatter for them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace etch {

// kOff is never the level of a record; it is a setting for a sink or logger that should accept
// nothing, fatal included.
enum class Level : std::uint8_t { kTrace = 0, kDebug, kInfo, kWarn, kError, kFatal, kOff };

// How many levels a record can have (kOff excluded); the size of per-level tables.
inline constexpr std::size_t kLevelCount = 6;
// Width of the level column in text output; names are padded to it there, nowhere else.
inline constexpr int kLevelColumnWidth = 5;

// Upper-case name, unpadded: "INFO". The text line pads it; std::format("{}", level) does not.
constexpr std::string_view to_string(Level level) noexcept {
  switch (level) {
    case Level::kTrace:
      return "TRACE";
    case Level::kDebug:
      return "DEBUG";
    case Level::kInfo:
      return "INFO";
    case Level::kWarn:
      return "WARN";
    case Level::kError:
      return "ERROR";
    case Level::kFatal:
      return "FATAL";
    case Level::kOff:
      return "OFF";
  }
  return "?";
}

// Lower-case name, as JSON and config files use it: "info".
constexpr std::string_view level_name(Level level) noexcept {
  switch (level) {
    case Level::kTrace:
      return "trace";
    case Level::kDebug:
      return "debug";
    case Level::kInfo:
      return "info";
    case Level::kWarn:
      return "warn";
    case Level::kError:
      return "error";
    case Level::kFatal:
      return "fatal";
    case Level::kOff:
      return "off";
  }
  return "?";
}

// Case-insensitive, whitespace-trimmed (spaces, tabs, CR, LF), accepts "info", "INFO",
// "warning", "err", "critical", "off", "none" and so on: any prefix of a known name that is at
// least three characters. Returns nullopt for anything else.
constexpr std::optional<Level> parse_level(std::string_view text) noexcept {
  constexpr std::size_t kShortestAccepted = 3;
  constexpr std::string_view kWhitespace = " \t\r\n\f\v";
  while (!text.empty() && kWhitespace.find(text.front()) != std::string_view::npos) {
    text.remove_prefix(1);
  }
  while (!text.empty() && kWhitespace.find(text.back()) != std::string_view::npos) {
    text.remove_suffix(1);
  }
  if (text.size() < kShortestAccepted) return std::nullopt;

  const auto lower = [](char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
  };
  const auto is_prefix_of = [&](std::string_view full) {
    if (text.size() > full.size()) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (lower(text[i]) != full[i]) return false;
    }
    return true;
  };
  if (is_prefix_of("trace")) return Level::kTrace;
  if (is_prefix_of("debug")) return Level::kDebug;
  if (is_prefix_of("information")) return Level::kInfo;  // covers "info" and "information"
  if (is_prefix_of("warning")) return Level::kWarn;
  if (is_prefix_of("error")) return Level::kError;
  if (is_prefix_of("fatal") || is_prefix_of("critical")) return Level::kFatal;
  if (is_prefix_of("off") || is_prefix_of("none")) return Level::kOff;
  return std::nullopt;
}

// Levels from text such as "warn,net=debug,render=trace": an entry without a name is the level
// for the default logger and for every etch::get() logger that the spec does not name (now and
// when made later); "name=level" is the level for the etch::get() logger of that name, which
// then keeps it through later bare levels; "*=level" sets every logger and forgets the named
// levels. Entries are separated by commas or semicolons; names and levels are trimmed; an
// empty entry is skipped. This is what the ETCH_LEVEL environment variable holds; see
// etch::apply_level_spec in default_logger.hpp.
struct LevelSpec {
  std::optional<Level> root;
  std::vector<std::pair<std::string, Level>> loggers;
  std::optional<Level> all;  // "*=level"; last, so {root, {names}} initialisation still works
};

namespace detail {

// The whitespace trimmed around a spec's entries, names and levels.
inline constexpr std::string_view kSpecWhitespace = " \t\r\n\f\v";

// A logger name in a spec: anything but whitespace, '=', '*' and the separators, so that
// "my net=debug" or "net*=debug" is reported rather than stored under a name no logger has.
constexpr bool is_logger_name(std::string_view name) noexcept {
  constexpr std::string_view kForbidden = " \t\r\n\f\v=*,;";
  return !name.empty() && name.find_first_of(kForbidden) == std::string_view::npos;
}

}  // namespace detail

// The spec, or a message naming the entry that could not be read.
inline std::expected<LevelSpec, std::string> parse_level_spec(std::string_view text) {
  LevelSpec spec;
  while (!text.empty()) {
    const std::size_t end = text.find_first_of(",;");
    std::string_view entry = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    const auto trim = [](std::string_view s) {
      while (!s.empty() && detail::kSpecWhitespace.find(s.front()) != std::string_view::npos) {
        s.remove_prefix(1);
      }
      while (!s.empty() && detail::kSpecWhitespace.find(s.back()) != std::string_view::npos) {
        s.remove_suffix(1);
      }
      return s;
    };
    entry = trim(entry);
    if (entry.empty()) continue;
    const std::size_t equals = entry.find('=');
    const std::string_view name =
        equals == std::string_view::npos ? "" : trim(entry.substr(0, equals));
    const std::string_view level_text =
        equals == std::string_view::npos ? entry : trim(entry.substr(equals + 1));
    const std::optional<Level> level = parse_level(level_text);
    if (!level) {
      return std::unexpected(std::format(
          "'{}' is not a level (trace, debug, info, warn, error, fatal, off)", level_text));
    }
    if (equals != std::string_view::npos && name.empty()) {
      return std::unexpected(std::format("'{}' names no logger", entry));
    }
    if (name.empty()) {
      spec.root = *level;
    } else if (name == "*") {
      spec.all = *level;
    } else if (!detail::is_logger_name(name)) {
      return std::unexpected(std::format("'{}' is not a logger name", name));
    } else {
      spec.loggers.emplace_back(std::string(name), *level);
    }
  }
  return spec;
}

// Must be the same value in every translation unit of a program (set it on the target that
// links etch, or globally), because the inline constant and the templates below are defined
// in terms of it; two values in one program violate the one-definition rule.
#ifndef ETCH_MIN_LEVEL
#define ETCH_MIN_LEVEL 0  // trace: nothing is compiled out by default
#endif
static_assert(ETCH_MIN_LEVEL >= 0 && ETCH_MIN_LEVEL <= 5,
              "ETCH_MIN_LEVEL must be 0 (trace) .. 5 (fatal)");
// Calls below this level are removed at compile time (see Logger::log_at).
inline constexpr Level kCompileTimeMinLevel = static_cast<Level>(ETCH_MIN_LEVEL);

}  // namespace etch

// Lets "{}" print a Level: std::format("level is {}", etch::Level::kWarn) -> "level is WARN".
template <>
struct std::formatter<etch::Level> : std::formatter<std::string_view> {
  auto format(etch::Level level, auto& ctx) const {
    return std::formatter<std::string_view>::format(etch::to_string(level), ctx);
  }
};
