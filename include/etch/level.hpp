// etch - Level: the six severities, their names, and a std::formatter for them.
#pragma once

#include <cstdint>
#include <format>
#include <optional>
#include <string_view>

namespace etch {

enum class Level : std::uint8_t { kTrace = 0, kDebug, kInfo, kWarn, kError, kFatal };

// Fixed-width (5 chars) names so log lines line up in a column.
constexpr std::string_view to_string(Level level) noexcept {
  switch (level) {
    case Level::kTrace:
      return "TRACE";
    case Level::kDebug:
      return "DEBUG";
    case Level::kInfo:
      return "INFO ";
    case Level::kWarn:
      return "WARN ";
    case Level::kError:
      return "ERROR";
    case Level::kFatal:
      return "FATAL";
  }
  return "?????";
}

// Case-insensitive, accepts "info", "INFO ", "warning", "err" and so on: any prefix of the
// full name that is at least three characters. Returns nullopt for anything else.
constexpr std::optional<Level> parse_level(std::string_view text) noexcept {
  constexpr std::size_t kShortestAccepted = 3;
  while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
  while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
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
  if (is_prefix_of("info")) return Level::kInfo;
  if (is_prefix_of("warning")) return Level::kWarn;
  if (is_prefix_of("error")) return Level::kError;
  if (is_prefix_of("fatal")) return Level::kFatal;
  return std::nullopt;
}

#ifndef ETCH_MIN_LEVEL
#define ETCH_MIN_LEVEL 0  // trace: nothing is compiled out by default
#endif
static_assert(ETCH_MIN_LEVEL >= 0 && ETCH_MIN_LEVEL <= 5,
              "ETCH_MIN_LEVEL must be 0 (trace) .. 5 (fatal)");
// Calls below this level are removed at compile time (see Logger::log_at).
inline constexpr Level kCompileTimeMinLevel = static_cast<Level>(ETCH_MIN_LEVEL);

}  // namespace etch

// Lets "{}" print a Level: std::format("level is {}", etch::Level::kWarn) -> "level is WARN ".
template <>
struct std::formatter<etch::Level> : std::formatter<std::string_view> {
  auto format(etch::Level level, auto& ctx) const {
    return std::formatter<std::string_view>::format(etch::to_string(level), ctx);
  }
};
