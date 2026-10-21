// etch - Record: one log event, its structured extras, and how it is laid out as a line of text.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "level.hpp"

namespace etch {

// A path as UTF-8, whatever the platform's native encoding. path.string() on Windows uses the
// ANSI code page, which mangles or throws on characters outside it; this never does.
inline std::string utf8(const std::filesystem::path& path) {
  const auto u8 = path.u8string();
  return std::string(u8.begin(), u8.end());
}

// A structured key/value attached to a record with Logger::with().
using FieldValue = std::variant<std::int64_t, double, bool, std::string>;

namespace detail {

template <class T>
concept StringLike = std::convertible_to<const T&, std::string_view>;

template <class T>
struct IsDuration : std::false_type {};
template <class Rep, class Period>
struct IsDuration<std::chrono::duration<Rep, Period>> : std::true_type {};

// Converts any sensible value to a FieldValue. Integers become int64 (an unsigned value past
// int64's range becomes its decimal string, never a wrong number); floating point becomes
// double; bool stays bool; a char becomes a one-character string; an enum becomes its
// underlying integer; a chrono duration becomes whole nanoseconds; a path becomes UTF-8 text;
// anything std::format can print becomes that text.
template <class T>
FieldValue to_field_value(const T& v) {
  using U = std::remove_cvref_t<T>;
  if constexpr (std::is_same_v<U, FieldValue>) {
    return v;
  } else if constexpr (std::is_same_v<U, bool>) {
    return v;
  } else if constexpr (std::is_same_v<U, char>) {
    return std::string(1, v);
  } else if constexpr (std::is_enum_v<U>) {
    // Always a number, even for an enum backed by char.
    return static_cast<std::int64_t>(std::to_underlying(v));
  } else if constexpr (std::is_integral_v<U>) {
    if constexpr (std::is_signed_v<U>) {
      return static_cast<std::int64_t>(v);
    } else {
      if (v <= static_cast<U>(std::numeric_limits<std::int64_t>::max())) {
        return static_cast<std::int64_t>(v);
      }
      return std::format("{}", v);
    }
  } else if constexpr (std::is_floating_point_v<U>) {
    return static_cast<double>(v);
  } else if constexpr (IsDuration<U>::value) {
    // Whole nanoseconds, computed in long double so a huge duration clamps instead of
    // overflowing; a non-finite floating duration stays non-finite (JSON writes null).
    const long double ns = std::chrono::duration<long double, std::nano>(v).count();
    if (!std::isfinite(ns)) return static_cast<double>(ns);
    // int64's limits are not exactly representable in a 64-bit long double; compare, do not cast.
    constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
    constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
    if (ns >= static_cast<long double>(kMax)) return kMax;
    if (ns <= static_cast<long double>(kMin)) return kMin;
    return static_cast<std::int64_t>(ns);
  } else if constexpr (std::is_same_v<U, std::filesystem::path>) {
    return utf8(v);
  } else if constexpr (StringLike<U>) {
    return std::string(std::string_view(v));
  } else if constexpr (std::formattable<const U&, char>) {
    return std::format("{}", v);
  } else {
    static_assert(std::formattable<const U&, char>,
                  "etch: a field value must be a number, bool, string, char, enum, chrono "
                  "duration, filesystem path, or a type with a std::formatter");
  }
}

}  // namespace detail

struct Field {
  std::string key;
  FieldValue value;

  template <class T>
  Field(std::string_view k, const T& v) : key(k), value(detail::to_field_value(v)) {}
  Field(std::string_view k, const char* v) : key(k), value(std::string(v == nullptr ? "" : v)) {}
};
using Fields = std::vector<Field>;

// One stacktrace frame. file is empty for frames without debug information (OS frames, or a
// build without symbols).
struct Frame {
  std::string description;
  std::string file;
  std::uint32_t line = 0;
};

// Everything a sink needs to know about one log call. Owns its strings, so it can outlive the
// call (the async sink queues records across threads). The message is what a person reads; the
// members after it carry the same information as data, for the JSON output.
struct Record {
  Level level{};
  std::chrono::system_clock::time_point time{};
  std::thread::id thread{};
  std::uint32_t thread_index = 0;  // small per-process number, 1 for the first thread that logs
  std::source_location where{};
  std::string logger_name;
  std::string message;  // the caller's text, then anything etch appended (dump lines, frames)
  // Where etch's appended text starts in message; npos when all of it is the caller's. Text
  // output escapes and indents only the caller's part; JSON carries only the caller's part,
  // because the appended part travels as data (bytes, stack).
  std::size_t message_user_length = std::string::npos;
  Fields fields;                 // from Logger::with()
  std::vector<std::byte> bytes;  // the data behind a hexdump, up to its limit
  std::vector<Frame> stack;      // the stacktrace behind a fatal, one entry per frame
  std::optional<std::chrono::nanoseconds> duration;  // a scope timer's elapsed time

  std::string_view user_message() const noexcept {
    return std::string_view(message).substr(0, message_user_length);
  }
  std::string_view appended_text() const noexcept {
    return message_user_length == std::string::npos
               ? std::string_view{}
               : std::string_view(message).substr(message_user_length);
  }
};

enum class LineFormat { kText, kJson };

// What a JSON line carries beyond the always-present ts, level, logger, file, line and msg.
// The defaults are the cheap, safe set: a program reading the log pays tokens for every key,
// and the compiler-provided full path can name a user's home directory.
struct JsonStyle {
  enum class Time { kIso8601, kEpochMillis };
  Time time = Time::kIso8601;  // kEpochMillis: "ts":1760985493091, shorter and easy to subtract
  bool thread = true;          // the small per-process thread number
  bool pid = true;
  bool path = false;      // full source paths (the record's and the stack frames'); off: base names
  bool function = false;  // compiler-decorated function name; long, and rarely needed
};

namespace detail {

// Set by the exit handlers. Sinks that buffer (the file sink) flush every line once it is set,
// so a static object logging from its destructor still gets its lines on disk. A function-local
// constant-initialised atomic has nothing to destroy, so it is safe to read at any point of
// process teardown.
inline std::atomic<bool>& process_exiting() noexcept {
  static std::atomic<bool> flag{false};
  return flag;
}

// A small number per thread, handed out in order of first use: readable in a text column and
// cheap in JSON, unlike std::thread::id, which is a 15-digit pthread value on Linux.
inline std::uint32_t this_thread_index() noexcept {
  static std::atomic<std::uint32_t> next{0};
  thread_local constinit std::uint32_t index = 0;
  if (index == 0) index = next.fetch_add(1, std::memory_order_relaxed) + 1;
  return index;
}

// FNV-1a: a stable hash so a logger name maps to the same colour in every run.
constexpr std::size_t fnv1a(std::string_view text) noexcept {
  constexpr std::size_t kOffset = 14695981039346656037ull;
  constexpr std::size_t kPrime = 1099511628211ull;
  std::size_t hash = kOffset;
  for (const char c : text) hash = (hash ^ static_cast<unsigned char>(c)) * kPrime;
  return hash;
}

inline std::string_view base_name(std::string_view path) noexcept {
  const auto cut = path.find_last_of("/\\");
  return cut == std::string_view::npos ? path : path.substr(cut + 1);
}

}  // namespace detail

// ANSI SGR sequences for each part of a line, used when the line style's colour is on. Only the
// 16 standard colours, so it looks the same in Windows Terminal, conhost, and any Linux
// terminal. The per-level arrays are indexed by Level (trace .. fatal).
struct Palette {
  std::string_view reset = "\x1b[0m";
  std::string_view time = "\x1b[2m";      // dim
  std::string_view thread = "\x1b[2m";    // dim
  std::string_view location = "\x1b[2m";  // dim
  std::array<std::string_view, kLevelCount> level = {
      "\x1b[90m",       // trace: grey
      "\x1b[96m",       // debug: bright cyan
      "\x1b[92m",       // info: bright green
      "\x1b[93m",       // warn: bright yellow
      "\x1b[91m",       // error: bright red
      "\x1b[1;97;41m",  // fatal: bold white on red
  };
  std::array<std::string_view, kLevelCount> message = {
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
    return level[std::min(static_cast<std::size_t>(l), kLevelCount - 1)];
  }
  std::string_view message_colour(Level l) const noexcept {
    return message[std::min(static_cast<std::size_t>(l), kLevelCount - 1)];
  }
};

// Which columns a sink prints. Each sink has its own copy, so the console can be terse while
// the file keeps everything.
struct LineStyle {
  bool time = true;
  bool date = true;  // false: time of day only, "18:19:58.582"
  bool utc = false;  // false: local time zone (looked up once, then cached); true adds a "Z"
  bool thread = true;
  bool name = true;
  bool location = true;  // file:line, file shortened to its base name
  // ANSI colour from the palette below. Unset means "the sink decides": the console sink keeps
  // the colour it detected (or was told), the file sink uses none. So set_style() with a fresh
  // LineStyle never switches the console to grey by accident.
  std::optional<bool> colour = std::nullopt;  // explicit: GCC warns on omitted members otherwise
  Palette palette{};
  LineFormat format = LineFormat::kText;  // kJson: one JSON object per record, see json.hpp
  JsonStyle json{};
};

namespace detail {

// The tz database (std::chrono::current_zone) is a C++20 library feature that libc++ ships
// only as experimental; without it timestamps fall back to UTC and say so with a "Z" suffix.
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
#define ETCH_HAS_TIME_ZONES 1
#else
#define ETCH_HAS_TIME_ZONES 0
#endif

#if ETCH_HAS_TIME_ZONES
// current_zone() does a tz database lookup that takes milliseconds the first time; do it once
// per process. The sinks call this from their constructors so the cost lands at setup, not on
// the first log line. Null when the platform has no tz database.
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

inline void warm_up_time_zone() noexcept {
#if ETCH_HAS_TIME_ZONES
  (void)cached_local_zone();
#endif
}

}  // namespace detail

// Appends the timestamp as "YYYY-MM-DD HH:MM:SS.mmm" (or "HH:MM:SS.mmm" without the date) in
// local time or UTC. UTC lines end with "Z" so the two cannot be confused.
//
// Measured on MSVC's standard library: formatting a zoned_time costs about 16 us, and a UTC
// sys_time about 2.5 us, while everything else in a log line costs under 0.3 us. The part down
// to the second only changes once a second, so each thread caches that prefix and only the
// milliseconds are formatted per call. thread_local keeps it lock-free.
inline void format_time(std::string& out, std::chrono::system_clock::time_point tp, bool utc,
                        bool date = true) {
  using namespace std::chrono;
  constexpr std::size_t kPrefixCapacity = 32;  // "2026-01-02 03:04:05" is 19
  constexpr std::size_t kDateLength = 11;      // "2026-01-02 " including the space
  struct Cache {
    bool valid = false;
    bool zulu_suffix = false;  // UTC, or local time asked for with no tz database to give it
    seconds::rep key = 0;
    std::size_t length = 0;
    char prefix[kPrefixCapacity] = {};
  };
  // One cache per mode, so a local-time text sink and a UTC JSON sink on the same thread do not
  // evict each other on every record. constinit and trivially destructible: plain thread-local
  // memory with no dynamic initialisation and no destructor, so nothing is torn down before
  // static destructors run.
  thread_local constinit Cache caches[2]{};
  Cache& cache = caches[utc ? 1 : 0];

  const auto whole_seconds = floor<seconds>(tp);
  const auto key = whole_seconds.time_since_epoch().count();
  if (!cache.valid || key != cache.key) {
    char fresh[kPrefixCapacity];
    std::format_to_n_result<char*> r{fresh, 0};
    bool zulu = true;
#if ETCH_HAS_TIME_ZONES
    const auto* zone = utc ? nullptr : detail::cached_local_zone();
    if (zone != nullptr) {
      r = std::format_to_n(fresh, kPrefixCapacity, "{:%F %T}", zoned_time{zone, whole_seconds});
      zulu = false;
    } else
#endif
    {
      r = std::format_to_n(fresh, kPrefixCapacity, "{:%F %T}", whole_seconds);
    }
    // Commit only after formatting succeeded, so a throw leaves the old entry intact.
    const auto length = static_cast<std::size_t>(r.out - fresh);
    std::copy_n(fresh, length, cache.prefix);
    cache.length = length;
    cache.key = key;
    cache.zulu_suffix = zulu;
    cache.valid = true;
  }
  const std::size_t skip = (date || cache.length < kDateLength) ? 0 : kDateLength;
  out.append(cache.prefix + skip, cache.length - skip);
  // The three millisecond digits by hand: this runs once per line, std::format would not be free.
  const auto ms = duration_cast<milliseconds>(tp - whole_seconds).count();
  out += '.';
  out += static_cast<char>('0' + ms / 100);
  out += static_cast<char>('0' + (ms / 10) % 10);
  out += static_cast<char>('0' + ms % 10);
  if (cache.zulu_suffix) out += 'Z';
}

namespace detail {

constexpr std::string_view kHexDigits = "0123456789abcdef";

// Writes one C0 control character or ESC in a form that cannot act on a terminal or pose as
// structure: the common ones as C escapes, the rest as \xNN.
inline void append_escaped_control(unsigned char c, std::string& out) {
  switch (c) {
    case '\n':
      out += "\\n";
      return;
    case '\r':
      out += "\\r";
      return;
    case '\t':
      out += "\\t";
      return;
    case 0x1b:
      out += "\\e";
      return;
    default:
      out += "\\x";
      out += kHexDigits[c >> 4];
      out += kHexDigits[c & 0x0f];
  }
}

}  // namespace detail

// A string inside a text line, quoted, with the quote, the backslash and every control
// character escaped so a value cannot pose as another field, another record, or a terminal
// command.
inline void append_quoted_text(std::string_view text, std::string& out) {
  constexpr unsigned char kLastControl = 0x1f;
  out += '"';
  for (const unsigned char c : text) {
    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else if (c <= kLastControl || c == 0x7f) {
      detail::append_escaped_control(c, out);
    } else {
      out += static_cast<char>(c);
    }
  }
  out += '"';
}

// The caller's message inside a text line. A newline becomes a newline plus two spaces, so a
// multi-line message continues on indented lines (like etch's own dump and stack lines) and can
// never produce a line that looks like a new record; other control characters and ESC are
// escaped so a message cannot drive the terminal.
inline void append_message_text(std::string_view text, std::string& out) {
  constexpr unsigned char kLastControl = 0x1f;
  for (const unsigned char c : text) {
    if (c == '\n') {
      out += "\n  ";
    } else if (c == '\t') {
      out += '\t';
    } else if (c <= kLastControl || c == 0x7f) {
      detail::append_escaped_control(c, out);
    } else {
      out += static_cast<char>(c);
    }
  }
}

// key=value as a person reads it: strings quoted and escaped, numbers and booleans bare. Keys
// are written as given; keep them to letters, digits and underscores.
inline void append_field_text(const Field& field, std::string& out) {
  out += field.key;
  out += '=';
  std::visit(
      [&](const auto& v) {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) {
          append_quoted_text(v, out);
        } else if constexpr (std::is_same_v<T, bool>) {
          out += v ? "true" : "false";
        } else {
          std::format_to(std::back_inserter(out), "{}", v);
        }
      },
      field.value);
}

// Appends one record as a single line (no trailing newline):
//   2026-10-20 18:19:58.582 INFO  [main] [1] demo.cpp:42: message key=value
// A line that starts with a digit is a record; a line that starts with whitespace continues the
// previous one. Sinks pass the same string every time, so a steady stream allocates nothing.
inline void format_line(const Record& r, const LineStyle& s, std::string& out) {
  auto app = std::back_inserter(out);
  const Palette& p = s.palette;
  const bool colour_on = s.colour.value_or(false);
  // The colour is asked for lazily (a callable), so with colour off nothing is computed: no
  // palette lookup and, for the logger name, no hash.
  const auto wrap = [&](auto&& sgr_of, auto&& append_text) {
    const std::string_view on = colour_on ? sgr_of() : std::string_view{};
    out += on;
    append_text();
    if (!on.empty()) out += p.reset;
  };

  if (s.time) {
    wrap([&] { return p.time; }, [&] { format_time(out, r.time, s.utc, s.date); });
    out += ' ';
  }
  wrap([&] { return p.level_colour(r.level); },
       [&] { std::format_to(app, "{:<{}}", to_string(r.level), kLevelColumnWidth); });
  out += ' ';
  if (s.name) {
    wrap([&] { return p.name_colour(r.logger_name); },
         [&] { std::format_to(app, "[{}]", r.logger_name); });
    out += ' ';
  }
  if (s.thread) {
    wrap([&] { return p.thread; }, [&] { std::format_to(app, "[{}]", r.thread_index); });
    out += ' ';
  }
  if (s.location) {
    wrap([&] { return p.location; },
         [&] {
           std::format_to(app, "{}:{}:", detail::base_name(r.where.file_name()), r.where.line());
         });
    out += ' ';
  }
  wrap([&] { return p.message_colour(r.level); },
       [&] {
         append_message_text(r.user_message(), out);
         out += r.appended_text();  // etch's own lines, already indented and safe
       });
  for (const auto& field : r.fields) {
    out += ' ';
    append_field_text(field, out);
  }
}

inline std::string format_line(const Record& r, const LineStyle& s) {
  std::string out;
  out.reserve(96 + r.message.size());
  format_line(r, s, out);
  return out;
}

}  // namespace etch
