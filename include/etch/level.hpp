// etch - Level: the six severities (plus "off" as a filter setting), their names, parsing, and a
// std::formatter for them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>

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
