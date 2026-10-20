// etch - Record: one log event, and how it is laid out as a line of text.
#pragma once

#include <chrono>
#include <format>
#include <iterator>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>

#include "level.hpp"

namespace etch {

// Everything a sink needs to know about one log call. Owns its strings, so it can outlive the
// call (the async sink queues records across threads).
struct Record {
  Level level{};
  std::chrono::system_clock::time_point time{};
  std::thread::id thread{};
  std::source_location where{};
  std::string logger_name;
  std::string message;  // already formatted; may contain newlines (hexdump, stacktrace)
};

// Which columns a sink prints. Each sink has its own copy, so the console can be terse while
// the file keeps everything.
struct LineStyle {
  bool time = true;
  bool utc = false;  // false: local time zone (looked up once, then cached)
  bool thread = true;
  bool name = true;
  bool location = true;  // file:line, file shortened to its base name
  bool colour = false;   // ANSI colour on the level and dimmed metadata
};

namespace detail {

inline std::string_view base_name(std::string_view path) noexcept {
  const auto cut = path.find_last_of("/\\");
  return cut == std::string_view::npos ? path : path.substr(cut + 1);
}

// current_zone() does a tz database lookup; do it once per process. Null when the platform has
// no tz database, in which case timestamps fall back to UTC.
inline const std::chrono::time_zone* cached_local_zone() {
  static const std::chrono::time_zone* zone = [] {
    try {
      return std::chrono::current_zone();
    } catch (...) {
      return static_cast<const std::chrono::time_zone*>(nullptr);
    }
  }();
  return zone;
}

// ANSI SGR sequences. Only used when LineStyle::colour is on.
constexpr std::string_view kSgrReset = "\x1b[0m";
constexpr std::string_view kSgrDim = "\x1b[2m";
constexpr std::string_view kSgrFatalMessage = "\x1b[1;31m";  // bold red
constexpr std::string_view level_colour(Level level) noexcept {
  switch (level) {
    case Level::kTrace:
      return "\x1b[90m";  // grey
    case Level::kDebug:
      return "\x1b[36m";  // cyan
    case Level::kInfo:
      return "\x1b[32m";  // green
    case Level::kWarn:
      return "\x1b[33m";  // yellow
    case Level::kError:
      return "\x1b[31m";  // red
    case Level::kFatal:
      return "\x1b[1;97;41m";  // bold white on red
  }
  return "";
}

}  // namespace detail

// Appends the timestamp as "YYYY-MM-DD HH:MM:SS.mmm" in local time or UTC.
//
// Measured on MSVC's standard library: formatting a zoned_time costs about 16 us, and a UTC
// sys_time about 2.5 us, while everything else in a log line costs under 0.3 us. The part down
// to the second only changes once a second, so each thread caches that prefix and only the
// milliseconds are formatted per call. thread_local keeps it lock-free.
inline void format_time(std::string& out, std::chrono::system_clock::time_point tp, bool utc) {
  using namespace std::chrono;
  struct Cache {
    seconds key{-1};
    bool utc = false;
    std::string prefix;
  };
  thread_local Cache cache;

  const auto whole_seconds = floor<seconds>(tp);
  const auto key = whole_seconds.time_since_epoch();
  if (key != cache.key || utc != cache.utc) {
    cache.prefix.clear();
    const auto* zone = utc ? nullptr : detail::cached_local_zone();
    if (zone != nullptr) {
      std::format_to(std::back_inserter(cache.prefix), "{:%F %T}", zoned_time{zone, whole_seconds});
    } else {
      std::format_to(std::back_inserter(cache.prefix), "{:%F %T}", whole_seconds);
    }
    cache.key = key;
    cache.utc = utc;
  }
  out += cache.prefix;
  std::format_to(std::back_inserter(out), ".{:03}",
                 duration_cast<milliseconds>(tp - whole_seconds).count());
}

// Renders one record as a single line (no trailing newline):
//   2026-10-20 18:19:58.582 INFO  [main] [37036] demo.cpp:42: message
inline std::string format_line(const Record& r, const LineStyle& s) {
  std::string out;
  out.reserve(96 + r.message.size());
  auto app = std::back_inserter(out);
  const bool c = s.colour;

  if (s.time) {
    if (c) out += detail::kSgrDim;
    format_time(out, r.time, s.utc);
    if (c) out += detail::kSgrReset;
    out += ' ';
  }
  if (c) out += detail::level_colour(r.level);
  out += to_string(r.level);
  if (c) out += detail::kSgrReset;
  out += ' ';
  if (s.name) std::format_to(app, "[{}] ", r.logger_name);
  if (s.thread) std::format_to(app, "[{}] ", r.thread);
  if (s.location) {
    if (c) out += detail::kSgrDim;
    std::format_to(app, "{}:{}:", detail::base_name(r.where.file_name()), r.where.line());
    if (c) out += detail::kSgrReset;
    out += ' ';
  }
  const bool highlight = c && r.level == Level::kFatal;  // so a fatal line cannot be missed
  if (highlight) out += detail::kSgrFatalMessage;
  out += r.message;
  if (highlight) out += detail::kSgrReset;
  return out;
}

}  // namespace etch
