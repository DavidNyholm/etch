// etch - Logger: the front door. Formats a message, stamps it, hands it to the sinks.
#pragma once

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
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

// Bytes shown by a hexdump unless the call says otherwise: 256 lines of text. The rest is
// summarised as a count, and only this many bytes travel with the record.
inline constexpr std::size_t kDefaultHexdumpLimit = 4096;

// Lays out a byte range like a hex editor: offset, 16 hex bytes (gap after 8), ASCII column.
//   label (20 bytes)
//     0000  48 65 6c 6c 6f 2c 20 77  6f 72 6c 64 21 00 ff 10  |Hello, world!...|
//     0010  de ad be ef                                       |....|
// Returns the text and the length of its first line (the label part).
inline std::string format_hexdump(std::string_view label, const void* data, std::size_t size,
                                  std::size_t limit = kDefaultHexdumpLimit,
                                  std::size_t* label_length = nullptr) {
  constexpr std::size_t kBytesPerLine = 16;
  constexpr std::size_t kGapAfter = 8;
  constexpr std::size_t kWideOffsetFrom = 0x10000;  // past 64 KiB, offsets need 8 digits
  constexpr unsigned char kFirstPrintable = 0x20;
  constexpr unsigned char kLastPrintable = 0x7e;

  // A null pointer is said in the label line, so it survives into the JSON message too.
  const bool null_data = data == nullptr && size > 0;
  std::string out = std::format("{} ({} bytes{})", label, size, null_data ? ", null pointer" : "");
  if (label_length != nullptr) *label_length = out.size();
  if (null_data) return out;
  const auto* bytes = static_cast<const unsigned char*>(data);
  const std::size_t shown = std::min(size, limit);
  auto app = std::back_inserter(out);
  const int offset_digits = shown >= kWideOffsetFrom ? 8 : 4;
  for (std::size_t offset = 0; offset < shown; offset += kBytesPerLine) {
    std::format_to(app, "\n  {:0{}x}  ", offset, offset_digits);
    for (std::size_t i = 0; i < kBytesPerLine; ++i) {
      if (i == kGapAfter) out += ' ';
      if (offset + i < shown) {
        std::format_to(app, "{:02x} ", bytes[offset + i]);
      } else {
        out += "   ";
      }
    }
    out += " |";
    for (std::size_t i = 0; i < kBytesPerLine && offset + i < shown; ++i) {
      const unsigned char b = bytes[offset + i];
      out += (b >= kFirstPrintable && b <= kLastPrintable) ? static_cast<char>(b) : '.';
    }
    out += '|';
  }
  if (shown < size) std::format_to(app, "\n  ... {} more bytes not shown", size - shown);
  return out;
}

// "12.345 ms" style: picks the unit so the number stays readable.
inline std::string format_duration(std::chrono::nanoseconds d) {
  using namespace std::chrono;
  if (d < 1ms) return std::format("{} us", duration_cast<microseconds>(d).count());
  if (d < 1s) return std::format("{:.3f} ms", duration<double, std::milli>(d).count());
  return std::format("{:.3f} s", duration<double>(d).count());
}

namespace detail {

// The directory etch's headers live in, from the compiler's point of view, so stack frames
// inside etch can be recognised by their source file and left out of a fatal's stacktrace.
// A view into the compiler's own string literal: nothing is constructed or destroyed, so it is
// valid at any point of process teardown.
constexpr std::string_view etch_include_dir() noexcept {
  constexpr std::string_view here = std::source_location::current().file_name();
  constexpr auto cut = here.find_last_of("/\\");
  return cut == std::string_view::npos ? std::string_view{} : here.substr(0, cut + 1);
}

// A frame whose symbol lives in namespace etch: "etch::Logger::emit(...)" (GCC) or
// "app!etch::Logger::emit+0xD0" (MSVC, module-qualified). An application function that merely
// takes an etch type, "render(etch::Logger&)", does not start that way and is kept.
inline bool is_etch_symbol(std::string_view description) noexcept {
  if (description.starts_with("etch::")) return true;
  const auto bang = description.find('!');
  return bang != std::string_view::npos && description.substr(bang + 1).starts_with("etch::");
}

inline bool same_path_prefix(std::string_view path, std::string_view prefix) noexcept {
  if (prefix.empty() || path.size() < prefix.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    char a = path[i];
    char b = prefix[i];
#if defined(_WIN32)
    // Windows paths compare without case, and the debugger may spell the drive letter either way.
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + ('a' - 'A'));
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + ('a' - 'A'));
    if (a == '/') a = '\\';
    if (b == '/') b = '\\';
#endif
    if (a != b) return false;
  }
  return true;
}

class SinkSet;

}  // namespace detail

class Logger;

// How a scope timer behaves. begin = false halves the volume for per-frame scopes; min_duration
// keeps the end line only when the scope was slow ("only tell me about frames over 16 ms").
struct ScopeOptions {
  Level level = Level::kDebug;
  bool begin = true;
  std::chrono::nanoseconds min_duration{0};
};

// Logs "<what>: begin" when created and "<what>: done in <elapsed>" when it goes out of scope.
// Returned by Logger::scope(); keep it in a named variable or it ends immediately. It borrows
// the logger, which must outlive it. Whether it logs at all is decided once, when it starts, so
// a level change in between cannot produce an end without a begin or the reverse.
class ScopeTimer {
 public:
  ScopeTimer(Logger& logger, ScopeOptions options, std::string what, std::source_location where,
             Fields fields = {});
  ~ScopeTimer();
  ScopeTimer(const ScopeTimer&) = delete;
  ScopeTimer& operator=(const ScopeTimer&) = delete;

  std::chrono::nanoseconds elapsed() const noexcept {
    return std::chrono::steady_clock::now() - start_;
  }

 private:
  Logger& logger_;
  ScopeOptions options_;
  std::string what_;
  std::source_location where_;
  Fields fields_;
  bool active_;
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

// Defined outside Logger so it can be a default argument of the constructor; GCC and Clang
// reject a nested aggregate with default member initializers in that position.
struct LoggerOptions {
  bool stacktrace_on_fatal = true;  // append std::stacktrace to every fatal message
};

using SinkList = std::vector<std::shared_ptr<Sink>>;

namespace detail {

// The sink list a logger and its children share. The list itself is an immutable snapshot: a
// log call takes the shared lock only long enough to copy the snapshot's pointer, then calls
// the sinks with no logger lock held, so a sink (or its callback) may replace the list, and a
// replaced async sink joins its worker outside any lock. (A std::atomic<std::shared_ptr> would
// do the same job but measured 70 ns slower per call on MSVC than a shared lock.)
class SinkSet {
 public:
  explicit SinkSet(SinkList sinks) : list_(share(std::move(sinks))) {}

  std::shared_ptr<const SinkList> snapshot() const {
    std::shared_lock lock(mutex_);
    return list_;
  }
  void set(SinkList sinks) {
    auto next = share(std::move(sinks));
    std::shared_ptr<const SinkList> retired;  // destroyed after the lock is released
    std::unique_lock lock(mutex_);
    retired = std::exchange(list_, std::move(next));
  }
  void add(std::shared_ptr<Sink> sink) {
    if (sink == nullptr) return;
    std::shared_ptr<const SinkList> retired;
    std::unique_lock lock(mutex_);
    SinkList next = *list_;
    next.push_back(std::move(sink));
    retired = std::exchange(list_, share(std::move(next)));
  }

 private:
  static std::shared_ptr<const SinkList> share(SinkList sinks) {
    std::erase(sinks, nullptr);  // a null sink is a programming error; dropping it beats a crash
    return std::make_shared<const SinkList>(std::move(sinks));
  }

  mutable std::shared_mutex mutex_;
  std::shared_ptr<const SinkList> list_;
};

}  // namespace detail

class Logger {
 public:
  using Options = LoggerOptions;
  using SinkList = etch::SinkList;
  class Entry;

  explicit Logger(std::string name, SinkList sinks = {}, Options options = Options{})
      : name_(std::move(name)),
        sinks_(std::make_shared<detail::SinkSet>(std::move(sinks))),
        options_(options) {}

  // Movable so children can be returned by value. Do not move a logger that has live scope
  // timers or entries: they hold a reference to it.
  Logger(Logger&& other) noexcept
      : name_(std::move(other.name_)),
        sinks_(std::move(other.sinks_)),
        options_(other.options_),
        level_(other.level_.load(std::memory_order_relaxed)) {}
  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;
  Logger& operator=(Logger&&) = delete;

  const std::string& name() const noexcept { return name_; }

  // A logger for a subsystem that shares this logger's sinks, now and after any change to them,
  // and starts with this logger's level and options. etch::get(name) does the same for the
  // default logger and keeps the result for the life of the process.
  Logger child(std::string name) const {
    return Logger(std::move(name), sinks_, get_level(), options_);
  }

  // Null entries are dropped. Changes are seen by every logger sharing this sink list.
  void add_sink(std::shared_ptr<Sink> sink) { sinks_->add(std::move(sink)); }
  void set_sinks(SinkList sinks) { sinks_->set(std::move(sinks)); }
  SinkList sinks() const { return *sinks_->snapshot(); }

  void set_level(Level level) noexcept { level_.store(level, std::memory_order_relaxed); }
  Level get_level() const noexcept { return level_.load(std::memory_order_relaxed); }
  // kOff is a setting, never a record level: nothing is logged "at off".
  bool enabled(Level level) const noexcept {
    return level < Level::kOff && level >= kCompileTimeMinLevel && level >= get_level();
  }

  // The level is a template parameter so calls below ETCH_MIN_LEVEL compile to nothing. The
  // argument expressions are still evaluated by the caller; guard an expensive one with
  // "if (log.enabled(Level::kTrace))".
  template <Level L, class... Args>
  void log_at(FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) {
    if constexpr (L >= kCompileTimeMinLevel) {
      if (enabled(L)) {
        emit(L, fmt.where, std::vformat(fmt.format.get(), std::make_format_args(args...)));
      }
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

  // Attaches structured fields to one log call: log.with({{"fps", 59.2}}).warn("slow frame").
  // Text output appends them as key=value; JSON output gets them as a "fields" object. The
  // entry owns a copy of the fields and is meant for that one expression. The values are
  // converted when the braces are evaluated, before any level check; the overload that takes a
  // level skips the copy when that level is filtered, which is the cheap form for a hot path:
  // log.with(Level::kTrace, {{"frame", n}}).trace("...").
  Entry with(std::initializer_list<Field> fields) &;
  Entry with(Level gate, std::initializer_list<Field> fields) &;
  Entry with(Fields fields) &;

  // Dumps raw memory. The pointer overload takes anything; the range overload takes any
  // contiguous container (std::vector, std::array, std::string, std::span, ...). The text is
  // the hex editor view of the first `limit` bytes; the record also carries those bytes
  // themselves for the JSON output.
  void hexdump(Level level, std::string_view label, const void* data, std::size_t size,
               std::size_t limit = kDefaultHexdumpLimit,
               std::source_location where = std::source_location::current()) {
    if (!enabled(level)) return;
    emit(hexdump_record(level, label, data, size, limit, where));
  }
  template <std::ranges::contiguous_range R>
    requires std::ranges::sized_range<R>
  void hexdump(Level level, std::string_view label, const R& range,
               std::size_t limit = kDefaultHexdumpLimit,
               std::source_location where = std::source_location::current()) {
    const std::size_t bytes = std::ranges::size(range) * sizeof(std::ranges::range_value_t<R>);
    hexdump(level, label, std::ranges::data(range), bytes, limit, where);
  }

  // Lvalue-only: the timer keeps a reference to this logger, so a temporary would dangle.
  [[nodiscard]] ScopeTimer scope(std::string_view what, ScopeOptions options,
                                 std::source_location where = std::source_location::current()) & {
    return ScopeTimer(*this, options, std::string(what), where);
  }
  [[nodiscard]] ScopeTimer scope(std::string_view what, Level level = Level::kDebug,
                                 std::source_location where = std::source_location::current()) & {
    return ScopeTimer(*this, ScopeOptions{.level = level}, std::string(what), where);
  }

  void flush() {
    const auto sinks = sinks_->snapshot();
    for (const auto& sink : *sinks) sink->flush();
  }

  // The last step of every log call: stamp the message and deliver it. Public so that helpers
  // (ScopeTimer, custom wrappers) can log a message they formatted themselves. Applies the
  // logger's level like every other entry point. An exception thrown by a sink propagates out
  // of the log call, and the sinks after it are skipped; AsyncSink is the one that catches.
  void emit(Level level, const std::source_location& where, std::string message) {
    Record r;
    r.level = level;
    r.where = where;
    r.message = std::move(message);
    emit(std::move(r));
  }

  // Same, for a record that already carries extras (fields, bytes, a duration). Fills in the
  // time, thread and logger name.
  void emit(Record r) {
    if (!enabled(r.level)) return;
    deliver(std::move(r));
  }

 private:
  friend class ScopeTimer;

  // Stamps and delivers without the level check: for a scope timer's end line, whose level was
  // checked when the scope began.
  void deliver(Record r) {
    const Level level = r.level;
    r.time = std::chrono::system_clock::now();
    r.thread = std::this_thread::get_id();
    r.thread_index = detail::this_thread_index();
    r.logger_name = name_;
    if (level == Level::kFatal && options_.stacktrace_on_fatal) append_stacktrace(r);
    const auto sinks = sinks_->snapshot();
    // Every sink but the last gets a copy; the last takes the record itself, so the common
    // single-sink setup never copies the message.
    Sink* last = nullptr;
    for (const auto& sink : *sinks) {
      if (!sink->accepts(level)) continue;
      if (last != nullptr) last->write(r);
      last = sink.get();
    }
    if (last != nullptr) last->write(std::move(r));
    if (level == Level::kFatal) {
      for (const auto& sink : *sinks) sink->flush();
    }
  }

 public:
  static Record hexdump_record(Level level, std::string_view label, const void* data,
                               std::size_t size, std::size_t limit, std::source_location where) {
    Record r;
    r.level = level;
    r.where = where;
    r.message = format_hexdump(label, data, size, limit, &r.message_user_length);
    if (data != nullptr) {
      const auto* first = static_cast<const std::byte*>(data);
      r.bytes.assign(first, first + std::min(size, limit));
    }
    return r;
  }

 private:
  Logger(std::string name, std::shared_ptr<detail::SinkSet> sinks, Level level, Options options)
      : name_(std::move(name)), sinks_(std::move(sinks)), options_(options), level_(level) {}

  // Appends the frames to the message for people and keeps them as data for the JSON output.
  // Frames inside etch's own headers are left out, whatever the compiler inlined, by looking at
  // each frame's source file rather than counting.
  static void append_stacktrace(Record& r) {
    if (r.message_user_length == std::string::npos) r.message_user_length = r.message.size();
#if ETCH_HAS_STACKTRACE
    r.message += "\n  stacktrace:";
    const std::string_view own = detail::etch_include_dir();
    for (const auto& entry : std::stacktrace::current()) {
      Frame frame{entry.description(), entry.source_file(),
                  static_cast<std::uint32_t>(entry.source_line())};
      // Two signals for "this frame is etch": its source file lives under etch's include
      // directory, or its symbol is in namespace etch (a build without source information).
      if (detail::same_path_prefix(frame.file, own)) continue;
      if (detail::is_etch_symbol(frame.description)) continue;
      if (frame.description.empty() && frame.file.empty()) continue;  // nothing to show
      r.message += "\n    ";
      r.message += frame.description;
      if (!frame.file.empty()) {
        r.message += " at ";
        r.message += detail::base_name(frame.file);  // the full path stays out of text too
        r.message += ':';
        r.message += std::to_string(frame.line);
      }
      r.stack.push_back(std::move(frame));
    }
#else
    r.message += "\n  stacktrace: not available on this standard library";
#endif
  }

  std::string name_;
  std::shared_ptr<detail::SinkSet> sinks_;
  Options options_;
  std::atomic<Level> level_{Level::kTrace};
};

inline ScopeTimer::ScopeTimer(Logger& logger, ScopeOptions options, std::string what,
                              std::source_location where, Fields fields)
    : logger_(logger),
      options_(options),
      what_(std::move(what)),
      where_(where),
      fields_(std::move(fields)),
      active_(logger.enabled(options.level)) {
  if (active_ && options_.begin) {
    Record r;
    r.level = options_.level;
    r.where = where_;
    r.message = std::format("{}: begin", what_);
    r.fields = fields_;
    logger_.deliver(std::move(r));
  }
}

inline ScopeTimer::~ScopeTimer() {
  if (!active_) return;
  try {
    const auto took = elapsed();
    if (took < options_.min_duration) return;
    Record r;
    r.level = options_.level;
    r.where = where_;
    r.message = std::format("{}: done in {}", what_, format_duration(took));
    r.fields = std::move(fields_);
    r.duration = took;
    logger_.deliver(std::move(r));
  } catch (...) {
    // A destructor must not throw; a sink that does (a user callback) loses this one line.
  }
}

// The result of Logger::with(): the same calls as the logger, with the fields attached to the
// record. Every method is rvalue-qualified, so an entry is used once, in the expression that
// made it: log.with({{"k", v}}).info("..."). (That is also what keeps it cheap: the fields are
// a view of the braced list until the level check passes.)
class Logger::Entry {
 public:
  Entry(Logger& logger, std::initializer_list<Field> fields) : logger_(logger), owned_(fields) {}
  Entry(Logger& logger, Fields fields) : logger_(logger), owned_(std::move(fields)) {}
  // Gated: when the level is filtered the fields are not even copied.
  Entry(Logger& logger, Level gate, std::initializer_list<Field> fields)
      : logger_(logger), active_(logger.enabled(gate)) {
    if (active_) owned_ = Fields(fields);
  }
  Entry(const Entry&) = delete;
  Entry& operator=(const Entry&) = delete;

  template <Level L, class... Args>
  void log_at(FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) && {
    if constexpr (L >= kCompileTimeMinLevel) {
      if (!active_ || !logger_.enabled(L)) return;
      Record r;
      r.level = L;
      r.where = fmt.where;
      r.message = std::vformat(fmt.format.get(), std::make_format_args(args...));
      r.fields = take();
      logger_.emit(std::move(r));
    }
  }
  template <class... Args>
  void log(Level level, FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) && {
    if (!active_ || !logger_.enabled(level)) return;
    Record r;
    r.level = level;
    r.where = fmt.where;
    r.message = std::vformat(fmt.format.get(), std::make_format_args(args...));
    r.fields = take();
    logger_.emit(std::move(r));
  }
  template <class... Args>
  void trace(FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) && {
    std::move(*this).template log_at<Level::kTrace>(fmt, std::forward<Args>(args)...);
  }
  template <class... Args>
  void debug(FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) && {
    std::move(*this).template log_at<Level::kDebug>(fmt, std::forward<Args>(args)...);
  }
  template <class... Args>
  void info(FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) && {
    std::move(*this).template log_at<Level::kInfo>(fmt, std::forward<Args>(args)...);
  }
  template <class... Args>
  void warn(FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) && {
    std::move(*this).template log_at<Level::kWarn>(fmt, std::forward<Args>(args)...);
  }
  template <class... Args>
  void error(FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) && {
    std::move(*this).template log_at<Level::kError>(fmt, std::forward<Args>(args)...);
  }
  template <class... Args>
  void fatal(FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) && {
    std::move(*this).template log_at<Level::kFatal>(fmt, std::forward<Args>(args)...);
  }

  void hexdump(Level level, std::string_view label, const void* data, std::size_t size,
               std::size_t limit = kDefaultHexdumpLimit,
               std::source_location where = std::source_location::current()) && {
    if (!active_ || !logger_.enabled(level)) return;
    Record r = Logger::hexdump_record(level, label, data, size, limit, where);
    r.fields = take();
    logger_.emit(std::move(r));
  }
  template <std::ranges::contiguous_range R>
    requires std::ranges::sized_range<R>
  void hexdump(Level level, std::string_view label, const R& range,
               std::size_t limit = kDefaultHexdumpLimit,
               std::source_location where = std::source_location::current()) && {
    const std::size_t bytes = std::ranges::size(range) * sizeof(std::ranges::range_value_t<R>);
    std::move(*this).hexdump(level, label, std::ranges::data(range), bytes, limit, where);
  }

  // A scope timer whose begin and end lines carry the fields.
  [[nodiscard]] ScopeTimer scope(std::string_view what, ScopeOptions options = ScopeOptions{},
                                 std::source_location where = std::source_location::current()) && {
    if (!active_) options.level = Level::kOff;  // a filtered gate: the timer logs nothing
    return ScopeTimer(logger_, options, std::string(what), where, take());
  }

 private:
  Fields take() { return std::move(owned_); }

  Logger& logger_;
  bool active_ = true;  // false: the gating level was filtered, nothing is logged
  Fields owned_;
};

inline Logger::Entry Logger::with(std::initializer_list<Field> fields) & {
  return Entry(*this, fields);
}
inline Logger::Entry Logger::with(Level gate, std::initializer_list<Field> fields) & {
  return Entry(*this, gate, fields);
}
inline Logger::Entry Logger::with(Fields fields) & { return Entry(*this, std::move(fields)); }

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
inline Logger& default_logger() {
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
    return logger;
  }();
  return *instance;
}

// A named logger that shares the default logger's sinks: etch::get("render").info(...). The
// same name always gives the same logger, created on first use and never destroyed, so it can
// be used from static destructors like the default logger.
inline Logger& get(std::string_view name) {
  struct Registry {
    std::mutex mutex;
    std::map<std::string, std::unique_ptr<Logger>, std::less<>> loggers;
  };
  static Registry* const registry = new Registry;
  std::lock_guard lock(registry->mutex);
  if (const auto it = registry->loggers.find(name); it != registry->loggers.end()) {
    return *it->second;
  }
  auto logger = std::make_unique<Logger>(default_logger().child(std::string(name)));
  Logger& ref = *logger;
  registry->loggers.emplace(std::string(name), std::move(logger));
  return ref;
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
