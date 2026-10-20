// etch - Record: one log event, and how it is laid out as a line of text.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
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

namespace detail {

// Set by the default logger's exit handler. Sinks that buffer (the file sink) flush every line
// once it is set, so a static object logging from its destructor still gets its lines on disk.
// A function-local constant-initialised atomic has nothing to destroy, so it is safe to read at
// any point of process teardown.
inline std::atomic<bool>& process_exiting() noexcept {
  static std::atomic<bool> flag{false};
  return flag;
}

// FNV-1a: a stable hash so a logger name maps to the same colour in every run.
constexpr std::size_t fnv1a(std::string_view text) noexcept {
  constexpr std::size_t kOffset = 14695981039346656037ull;
  constexpr std::size_t kPrime = 1099511628211ull;
  std::size_t hash = kOffset;
  for (const char c : text) hash = (hash ^ static_cast<unsigned char>(c)) * kPrime;
  return hash;
}

}  // namespace detail

// ANSI SGR sequences for each part of a line, used when LineStyle::colour is on. Only the 16
// standard colours, so it looks the same in Windows Terminal, conhost, and any Linux terminal.
// The per-level arrays are indexed by Level (trace, debug, info, warn, error, fatal).
struct Palette {
  std::string_view reset = "\x1b[0m";
  std::string_view time = "\x1b[2m";      // dim
  std::string_view thread = "\x1b[2m";    // dim
  std::string_view location = "\x1b[2m";  // dim
  std::array<std::string_view, 6> level = {
      "\x1b[90m",       // trace: grey
      "\x1b[96m",       // debug: bright cyan
      "\x1b[92m",       // info: bright green
      "\x1b[93m",       // warn: bright yellow
      "\x1b[91m",       // error: bright red
      "\x1b[1;97;41m",  // fatal: bold white on red
  };
  std::array<std::string_view, 6> message = {
      "\x1b[90m",    // trace: grey, so the firehose fades behind the rest
      "",            // debug: plain
      "",            // info: plain
      "\x1b[33m",    // warn: yellow
      "\x1b[31m",    // error: red
      "\x1b[1;31m",  // fatal: bold red
  };
  // Each logger name is hashed to one of these, so "net" and "render" are told apart at a glance.
  std::array<std::string_view, 6> names = {
      "\x1b[34m", "\x1b[35m", "\x1b[36m", "\x1b[94m", "\x1b[95m", "\x1b[96m",
  };

  std::string_view name_colour(std::string_view name) const noexcept {
    return names[detail::fnv1a(name) % names.size()];
  }
  std::string_view level_colour(Level l) const noexcept {
    return level[static_cast<std::size_t>(l)];
  }
  std::string_view message_colour(Level l) const noexcept {
    return message[static_cast<std::size_t>(l)];
  }
};

// Which columns a sink prints. Each sink has its own copy, so the console can be terse while
// the file keeps everything.
struct LineStyle {
  bool time = true;
  bool utc = false;  // false: local time zone (looked up once, then cached)
  bool thread = true;
  bool name = true;
  bool location = true;  // file:line, file shortened to its base name
  bool colour = false;   // ANSI colour from the palette below
  Palette palette{};
};

namespace detail {

inline std::string_view base_name(std::string_view path) noexcept {
  const auto cut = path.find_last_of("/\\");
  return cut == std::string_view::npos ? path : path.substr(cut + 1);
}

// The tz database (std::chrono::current_zone) is a C++20 library feature that libc++ ships
// only as experimental; without it timestamps fall back to UTC and say so with a "Z" suffix.
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
#define ETCH_HAS_TIME_ZONES 1
#else
#define ETCH_HAS_TIME_ZONES 0
#endif

#if ETCH_HAS_TIME_ZONES
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
#endif

}  // namespace detail

// Appends the timestamp as "YYYY-MM-DD HH:MM:SS.mmm" in local time or UTC.
//
// Measured on MSVC's standard library: formatting a zoned_time costs about 16 us, and a UTC
// sys_time about 2.5 us, while everything else in a log line costs under 0.3 us. The part down
// to the second only changes once a second, so each thread caches that prefix and only the
// milliseconds are formatted per call. thread_local keeps it lock-free. The cache is a plain
// char array, so it has no destructor: a static object logging from its destructor, after the
// thread's thread_local objects are gone, still finds valid storage.
inline void format_time(std::string& out, std::chrono::system_clock::time_point tp, bool utc) {
  using namespace std::chrono;
  constexpr std::size_t kPrefixCapacity = 32;  // "2026-01-02 03:04:05" is 19
  struct Cache {
    bool valid = false;
    bool utc = false;
    bool zulu_suffix = false;  // local time was asked for but no tz database exists: mark it
    seconds::rep key = 0;
    std::size_t length = 0;
    char prefix[kPrefixCapacity] = {};
  };
  // constinit and trivially destructible: plain thread-local memory with no dynamic
  // initialisation and no destructor, so nothing is torn down before static destructors run.
  thread_local constinit Cache cache{};

  const auto whole_seconds = floor<seconds>(tp);
  const auto key = whole_seconds.time_since_epoch().count();
  if (!cache.valid || key != cache.key || utc != cache.utc) {
    char fresh[kPrefixCapacity];
    std::format_to_n_result<char*> r{fresh, 0};
#if ETCH_HAS_TIME_ZONES
    const auto* zone = utc ? nullptr : detail::cached_local_zone();
    if (zone != nullptr) {
      r = std::format_to_n(fresh, kPrefixCapacity, "{:%F %T}", zoned_time{zone, whole_seconds});
      cache.zulu_suffix = false;
    } else
#endif
    {
      r = std::format_to_n(fresh, kPrefixCapacity, "{:%F %T}", whole_seconds);
      cache.zulu_suffix = !utc;
    }
    // Commit only after formatting succeeded, so a throw leaves the old entry intact.
    const auto length = static_cast<std::size_t>(r.out - fresh);
    std::copy_n(fresh, length, cache.prefix);
    cache.length = length;
    cache.key = key;
    cache.utc = utc;
    cache.valid = true;
  }
  out.append(cache.prefix, cache.length);
  std::format_to(std::back_inserter(out), ".{:03}",
                 duration_cast<milliseconds>(tp - whole_seconds).count());
  if (cache.zulu_suffix) out += 'Z';
}

// Appends one record as a single line (no trailing newline):
//   2026-10-20 18:19:58.582 INFO  [main] [37036] demo.cpp:42: message
// Sinks pass the same string every time, so a steady stream of lines allocates nothing.
inline void format_line(const Record& r, const LineStyle& s, std::string& out) {
  auto app = std::back_inserter(out);
  const Palette& p = s.palette;
  // With colour off every sequence is empty, so one code path serves both cases.
  const auto colour = [&](std::string_view sgr) { return s.colour ? sgr : std::string_view{}; };
  const auto wrap = [&](std::string_view sgr, auto&& append_text) {
    const auto on = colour(sgr);
    out += on;
    append_text();
    if (!on.empty()) out += p.reset;
  };

  if (s.time) {
    wrap(p.time, [&] { format_time(out, r.time, s.utc); });
    out += ' ';
  }
  wrap(p.level_colour(r.level), [&] { out += to_string(r.level); });
  out += ' ';
  if (s.name) {
    wrap(p.name_colour(r.logger_name), [&] { std::format_to(app, "[{}]", r.logger_name); });
    out += ' ';
  }
  if (s.thread) {
    wrap(p.thread, [&] { std::format_to(app, "[{}]", r.thread); });
    out += ' ';
  }
  if (s.location) {
    wrap(p.location, [&] {
      std::format_to(app, "{}:{}:", detail::base_name(r.where.file_name()), r.where.line());
    });
    out += ' ';
  }
  wrap(p.message_colour(r.level), [&] { out += r.message; });
}

inline std::string format_line(const Record& r, const LineStyle& s) {
  std::string out;
  out.reserve(96 + r.message.size());
  format_line(r, s, out);
  return out;
}

}  // namespace etch
