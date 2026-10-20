// etch - Logger: the front door. Formats a message, stamps it, hands it to the sinks.
#pragma once

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <format>
#include <iterator>
#include <memory>
#include <ranges>
#include <shared_mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "level.hpp"
#include "record.hpp"
#include "sinks.hpp"

#if __has_include(<stacktrace>)
#include <stacktrace>
#endif
#if defined(__cpp_lib_stacktrace)
#define ETCH_HAS_STACKTRACE 1
#else
#define ETCH_HAS_STACKTRACE 0
#endif

namespace etch {

// A format string that also remembers where it was written. Because the constructor is
// consteval, std::format_string checks the placeholders against the argument types at compile
// time, and source_location::current() as a default argument captures the caller's file and
// line. Together they give "log.info("x = {}", x)" a location without any macro.
template <class... Args>
struct FormatWithLocation {
  std::format_string<Args...> format;
  std::source_location where;

  template <class S>
    requires std::convertible_to<const S&, std::string_view>
  consteval FormatWithLocation(const S& text,
                               std::source_location loc = std::source_location::current())
      : format(text), where(loc) {}
};

// Lays out a byte range like a hex editor: offset, 16 hex bytes (gap after 8), ASCII column.
//   label (20 bytes)
//     0000  48 65 6c 6c 6f 2c 20 77  6f 72 6c 64 21 00 ff 10  |Hello, world!...|
//     0010  de ad be ef                                       |....|
inline std::string format_hexdump(std::string_view label, const void* data, std::size_t size) {
  constexpr std::size_t kBytesPerLine = 16;
  constexpr std::size_t kGapAfter = 8;
  constexpr std::size_t kWideOffsetFrom = 0x10000;  // past 64 KiB, offsets need 8 digits
  constexpr unsigned char kFirstPrintable = 0x20;
  constexpr unsigned char kLastPrintable = 0x7e;

  const auto* bytes = static_cast<const unsigned char*>(data);
  std::string out = std::format("{} ({} bytes)", label, size);
  auto app = std::back_inserter(out);
  const int offset_digits = size >= kWideOffsetFrom ? 8 : 4;
  for (std::size_t offset = 0; offset < size; offset += kBytesPerLine) {
    std::format_to(app, "\n  {:0{}x}  ", offset, offset_digits);
    for (std::size_t i = 0; i < kBytesPerLine; ++i) {
      if (i == kGapAfter) out += ' ';
      if (offset + i < size) {
        std::format_to(app, "{:02x} ", bytes[offset + i]);
      } else {
        out += "   ";
      }
    }
    out += " |";
    for (std::size_t i = 0; i < kBytesPerLine && offset + i < size; ++i) {
      const unsigned char b = bytes[offset + i];
      out += (b >= kFirstPrintable && b <= kLastPrintable) ? static_cast<char>(b) : '.';
    }
    out += '|';
  }
  return out;
}

// "12.345 ms" style: picks the unit so the number stays readable.
inline std::string format_duration(std::chrono::nanoseconds d) {
  using namespace std::chrono;
  if (d < 1ms) return std::format("{} us", duration_cast<microseconds>(d).count());
  if (d < 1s) return std::format("{:.3f} ms", duration<double, std::milli>(d).count());
  return std::format("{:.3f} s", duration<double>(d).count());
}

class Logger;

// Logs "<what>: begin" when created and "<what>: done in <elapsed>" when it goes out of scope.
// Returned by Logger::scope(); keep it in a named variable or it ends immediately.
class ScopeTimer {
 public:
  ScopeTimer(Logger& logger, Level level, std::string what, std::source_location where);
  ~ScopeTimer();
  ScopeTimer(const ScopeTimer&) = delete;
  ScopeTimer& operator=(const ScopeTimer&) = delete;

  std::chrono::nanoseconds elapsed() const noexcept {
    return std::chrono::steady_clock::now() - start_;
  }

 private:
  Logger& logger_;
  Level level_;
  std::string what_;
  std::source_location where_;
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

class Logger {
 public:
  struct Options {
    bool stacktrace_on_fatal = true;  // append std::stacktrace to every fatal message
  };

  explicit Logger(std::string name, std::vector<std::shared_ptr<Sink>> sinks = {},
                  Options options = {})
      : name_(std::move(name)), sinks_(std::move(sinks)), options_(options) {}

  const std::string& name() const noexcept { return name_; }

  // Sinks can be changed while other threads log; a shared mutex keeps that safe.
  void add_sink(std::shared_ptr<Sink> sink) {
    std::unique_lock lock(sinks_mutex_);
    sinks_.push_back(std::move(sink));
  }
  void set_sinks(std::vector<std::shared_ptr<Sink>> sinks) {
    std::unique_lock lock(sinks_mutex_);
    sinks_ = std::move(sinks);
  }
  std::vector<std::shared_ptr<Sink>> sinks() const {
    std::shared_lock lock(sinks_mutex_);
    return sinks_;
  }

  void set_level(Level level) noexcept { level_.store(level, std::memory_order_relaxed); }
  Level get_level() const noexcept { return level_.load(std::memory_order_relaxed); }
  bool enabled(Level level) const noexcept {
    return level >= kCompileTimeMinLevel && level >= get_level();
  }

  // The level is a template parameter so calls below ETCH_MIN_LEVEL compile to nothing. The
  // argument expressions are still evaluated by the caller; guard an expensive one with
  // "if (log.enabled(Level::kTrace))".
  template <Level L, class... Args>
  void log_at(FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) {
    if constexpr (L >= kCompileTimeMinLevel) {
      if (enabled(L))
        emit(L, fmt.where, std::vformat(fmt.format.get(), std::make_format_args(args...)));
    }
  }

  // Runtime level, for when the severity is data (e.g. mapped from an error code).
  template <class... Args>
  void log(Level level, FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) {
    if (enabled(level)) {
      emit(level, fmt.where, std::vformat(fmt.format.get(), std::make_format_args(args...)));
    }
  }

  template <class... Args>
  void trace(FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) {
    log_at<Level::kTrace>(fmt, std::forward<Args>(args)...);
  }
  template <class... Args>
  void debug(FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) {
    log_at<Level::kDebug>(fmt, std::forward<Args>(args)...);
  }
  template <class... Args>
  void info(FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) {
    log_at<Level::kInfo>(fmt, std::forward<Args>(args)...);
  }
  template <class... Args>
  void warn(FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) {
    log_at<Level::kWarn>(fmt, std::forward<Args>(args)...);
  }
  template <class... Args>
  void error(FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) {
    log_at<Level::kError>(fmt, std::forward<Args>(args)...);
  }
  // Fatal does not abort; it logs (with a stacktrace by default) and flushes every sink, so
  // the caller decides what happens next.
  template <class... Args>
  void fatal(FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) {
    log_at<Level::kFatal>(fmt, std::forward<Args>(args)...);
  }

  // Dumps raw memory. The pointer overload takes anything; the range overload takes any
  // contiguous container (std::vector, std::array, std::string, std::span, ...).
  void hexdump(Level level, std::string_view label, const void* data, std::size_t size,
               std::source_location where = std::source_location::current()) {
    if (enabled(level)) emit(level, where, format_hexdump(label, data, size));
  }
  template <std::ranges::contiguous_range R>
    requires std::ranges::sized_range<R>
  void hexdump(Level level, std::string_view label, const R& range,
               std::source_location where = std::source_location::current()) {
    const std::size_t bytes = std::ranges::size(range) * sizeof(std::ranges::range_value_t<R>);
    hexdump(level, label, std::ranges::data(range), bytes, where);
  }

  [[nodiscard]] ScopeTimer scope(std::string_view what, Level level = Level::kDebug,
                                 std::source_location where = std::source_location::current()) {
    return ScopeTimer(*this, level, std::string(what), where);
  }

  void flush() {
    std::shared_lock lock(sinks_mutex_);
    for (const auto& sink : sinks_) sink->flush();
  }

  // The last step of every log call: stamp the message and deliver it. Public so that helpers
  // (ScopeTimer, custom wrappers) can log a message they formatted themselves.
  void emit(Level level, const std::source_location& where, std::string message) {
    Record r{level,
             std::chrono::system_clock::now(),
             std::this_thread::get_id(),
             where,
             name_,
             std::move(message)};
    if (level == Level::kFatal && options_.stacktrace_on_fatal) append_stacktrace(r.message);
    std::shared_lock lock(sinks_mutex_);
    for (const auto& sink : sinks_) {
      if (sink->accepts(level)) sink->write(r);
    }
    if (level == Level::kFatal) {
      for (const auto& sink : sinks_) sink->flush();
    }
  }

 private:
  static void append_stacktrace(std::string& message) {
#if ETCH_HAS_STACKTRACE
    // Skip this function and emit(); the rest is the caller's chain. In optimised builds the
    // compiler may inline either one, which only changes which frames appear, never the order.
    constexpr std::size_t kOwnFrames = 2;
    message += "\n  stacktrace:";
    for (const auto& entry : std::stacktrace::current(kOwnFrames)) {
      message += "\n    ";
      message += std::to_string(entry);
    }
#else
    message += "\n  stacktrace: not available on this standard library";
#endif
  }

  std::string name_;
  std::vector<std::shared_ptr<Sink>> sinks_;
  Options options_;
  std::atomic<Level> level_{Level::kTrace};
  mutable std::shared_mutex sinks_mutex_;
};

inline ScopeTimer::ScopeTimer(Logger& logger, Level level, std::string what,
                              std::source_location where)
    : logger_(logger), level_(level), what_(std::move(what)), where_(where) {
  if (logger_.enabled(level_)) logger_.emit(level_, where_, std::format("{}: begin", what_));
}

inline ScopeTimer::~ScopeTimer() {
  if (logger_.enabled(level_)) {
    logger_.emit(level_, where_, std::format("{}: done in {}", what_, format_duration(elapsed())));
  }
}

// A process-wide logger named "main" that writes to the console. Swap its sinks with
// default_logger().set_sinks(...) to send it elsewhere.
inline Logger& default_logger() {
  static Logger instance{"main", {std::make_shared<ConsoleSink>()}};
  return instance;
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

}  // namespace etch
