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

#include "context.hpp"
#include "delivery.hpp"
#include "level.hpp"
#include "record.hpp"
#include "sink_set.hpp"
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
  const std::size_t shown = (std::min)(size, limit);  // (std::min): <windows.h> may follow etch
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

// The count of held-back calls rides on the line that gets through, as a field of this name,
// replacing any field of that name the caller supplied.
inline constexpr std::string_view kSuppressedField = "suppressed";

// A rate gate carried by a scope timer with a threshold: decided at the end, by the scopes
// that turned out slow (see Logger::Entry::scope).
struct RateGate {
  std::chrono::nanoseconds interval{0};
  bool once = false;
  bool gated = false;
};

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
             Fields fields = {}, detail::RateGate gate = {});
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
  detail::RateGate gate_;
  bool active_;
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

// Defined outside Logger so it can be a default argument of the constructor; GCC and Clang
// reject a nested aggregate with default member initializers in that position.
struct LoggerOptions {
  bool stacktrace_on_fatal = true;  // append std::stacktrace to every fatal message
};


namespace detail {

// One call site of every() or once(): when it last got through and how many calls were held
// back since. Keyed by the call's source location (the file name is a string literal, so a
// view of it stays valid for the life of the program). Never destroyed: a static destructor
// may log.
struct RateKey {
  std::string_view file;  // compared by contents: the same header line in two translation
                          // units can carry the same text at two addresses
  std::uint_least32_t line = 0;
  std::uint_least32_t column = 0;
  auto operator<=>(const RateKey&) const = default;
};
struct RateState {
  std::chrono::steady_clock::time_point last{};
  std::uint64_t suppressed = 0;
  bool fired = false;
};
struct RateRegistry {
  std::mutex mutex;
  std::map<RateKey, RateState> sites;
};
inline RateRegistry& rate_registry() {
  static RateRegistry* const registry = new RateRegistry;
  return *registry;
}

// Whether the call at `where` may go through: the first one always, then one per `interval`
// (once() is an interval that never elapses). `suppressed` receives how many calls were held
// back since the last one that went through, so the line that gets through can say so.
inline bool admit_rate(const std::source_location& where, std::chrono::nanoseconds interval,
                       bool once, std::uint64_t& suppressed) {
  RateRegistry& registry = rate_registry();
  std::lock_guard lock(registry.mutex);
  RateState& state = registry.sites[RateKey{where.file_name(), where.line(), where.column()}];
  const auto now = std::chrono::steady_clock::now();
  if (state.fired && (once || now - state.last < interval)) {
    ++state.suppressed;
    return false;
  }
  state.fired = true;
  state.last = now;
  suppressed = std::exchange(state.suppressed, 0);
  return true;
}

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
        level_state_(other.level_state_.load(std::memory_order_relaxed)) {}
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

  // A level set here is the logger's own: a bare level in a later level spec ("warn" from
  // ETCH_LEVEL or apply_level_spec) leaves it alone, where a logger that only inherited its
  // level from the default logger follows. own = false is what "*=level" uses: it sets the
  // level and gives the ownership up, so the next bare level reaches the logger again.
  void set_level(Level level, bool own = true) noexcept {
    level_state_.store(pack_level(level, own), std::memory_order_relaxed);
  }
  // A level handed down by a bare level spec: taken unless the logger has a level of its own,
  // decided and applied in one atomic step, so a set_level() racing with it is never half
  // overwritten.
  void inherit_level(Level level) noexcept {
    std::uint16_t state = level_state_.load(std::memory_order_relaxed);
    while ((state & kOwnLevelBit) == 0 &&
           !level_state_.compare_exchange_weak(state, pack_level(level, /*own=*/false),
                                               std::memory_order_relaxed)) {
    }
  }
  Level get_level() const noexcept {
    return static_cast<Level>(level_state_.load(std::memory_order_relaxed) & kLevelMask);
  }
  bool has_own_level() const noexcept {
    return (level_state_.load(std::memory_order_relaxed) & kOwnLevelBit) != 0;
  }
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

  // Rate limiting per call site: log.every(5s).warn("still waiting") lets the first call
  // through and then one per five seconds, and the line that gets through carries a
  // `suppressed` field with how many were held back; log.once().info("...") logs the first
  // call only. The key is the call's source location, so each line of code has its own
  // budget. Fields can follow: log.every(1s).with({{"fps", fps}}).warn("slow").
  Entry every(std::chrono::nanoseconds interval) &;
  Entry once() &;

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
    r.lineage = nullptr;  // a record handed in from outside (a replay) starts a chain of its own
    deliver(std::move(r));
  }

  // etch's own word about its configuration (a level spec that could not be applied): skips
  // this logger's level, because the configuration being diagnosed may be what set it to off,
  // and is still subject to each sink's level. Not for application messages.
  template <class... Args>
  void report(Level level, FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) {
    if (level >= Level::kOff) return;
    Record r;
    r.level = level;
    r.where = fmt.where;
    r.message = std::vformat(fmt.format.get(), std::make_format_args(args...));
    deliver(std::move(r));
  }

 private:
  friend class ScopeTimer;

  // Stamps and delivers without the level check: for a scope timer's end line, whose level was
  // checked when the scope began.
  void deliver(Record r) {
    r.time = std::chrono::system_clock::now();
    r.thread = std::this_thread::get_id();
    r.thread_index = detail::this_thread_index();
    r.logger_name = name_;
    detail::append_context(r.fields);
    if (r.level == Level::kFatal && options_.stacktrace_on_fatal) append_stacktrace(r);
    // Logged from inside a sink's write: delivered once that write has returned (see Sink).
    // The locked snapshot is fine here; this is the rare path. Should nothing be able to hold
    // it, it is delivered now, to every sink but the ones it must not reach.
    if (detail::WriteScope::any()) {
      if (detail::defer(sinks_->snapshot(), r)) return;
      r.lineage = detail::current_lineage();
    }
    const detail::SinkSet::Use use(*sinks_);
    detail::deliver_to(use.list(), std::move(r));
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
      r.bytes.assign(first, first + (std::min)(size, limit));
    }
    return r;
  }

 private:
  Logger(std::string name, std::shared_ptr<detail::SinkSet> sinks, Level level, Options options)
      : name_(std::move(name)),
        sinks_(std::move(sinks)),
        options_(options),
        level_state_(pack_level(level, /*own=*/false)) {}

  // The level and whether it is the logger's own, in one word: see set_level.
  static constexpr std::uint16_t kLevelMask = 0xff;
  static constexpr std::uint16_t kOwnLevelBit = 0x100;
  static constexpr std::uint16_t pack_level(Level level, bool own) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(level) | (own ? kOwnLevelBit : 0));
  }

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
  std::atomic<std::uint16_t> level_state_{pack_level(Level::kTrace, /*own=*/false)};
};

inline ScopeTimer::ScopeTimer(Logger& logger, ScopeOptions options, std::string what,
                              std::source_location where, Fields fields, detail::RateGate gate)
    : logger_(logger),
      options_(options),
      what_(std::move(what)),
      where_(where),
      fields_(std::move(fields)),
      gate_(gate),
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
    if (gate_.gated) {  // a rate-gated threshold scope: the slow ones spend the budget
      std::uint64_t suppressed = 0;
      if (!detail::admit_rate(where_, gate_.interval, gate_.once, suppressed)) return;
      if (suppressed > 0) {
        std::erase_if(fields_, [](const Field& f) { return f.key == detail::kSuppressedField; });
        fields_.emplace_back(detail::kSuppressedField, suppressed);
      }
    }
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
  // Rate-gated: see Logger::every and Logger::once.
  Entry(Logger& logger, std::chrono::nanoseconds interval, bool once)
      : logger_(logger), interval_(interval), once_(once), rate_gated_(true) {}
  Entry(const Entry&) = delete;
  Entry& operator=(const Entry&) = delete;

  // Fields on a rate-gated entry: log.every(1s).with({{"fps", fps}}).warn("slow").
  Entry&& with(std::initializer_list<Field> fields) && {
    owned_.insert(owned_.end(), fields.begin(), fields.end());
    return std::move(*this);
  }
  Entry&& with(Fields fields) && {
    owned_.insert(owned_.end(), std::make_move_iterator(fields.begin()),
                  std::make_move_iterator(fields.end()));
    return std::move(*this);
  }
  // The other order: log.with({{"fps", fps}}).every(1s).warn("slow") means the same.
  Entry&& every(std::chrono::nanoseconds interval) && {
    interval_ = interval;
    once_ = false;
    rate_gated_ = true;
    return std::move(*this);
  }
  Entry&& once() && {
    interval_ = std::chrono::nanoseconds{0};
    once_ = true;
    rate_gated_ = true;
    return std::move(*this);
  }

  template <Level L, class... Args>
  void log_at(FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args) && {
    if constexpr (L >= kCompileTimeMinLevel) {
      if (!active_ || !logger_.enabled(L) || !admit(fmt.where)) return;
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
    if (!active_ || !logger_.enabled(level) || !admit(fmt.where)) return;
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
    if (!active_ || !logger_.enabled(level) || !admit(where)) return;
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

  // A scope timer whose begin and end lines carry the fields. Rate-gated with a threshold,
  // log.every(1min).scope("frame", {.min_duration = 16ms}), the gate is decided at the end and
  // only by the scopes that passed the threshold, so "the slow frames, at most once a minute"
  // means that; such a scope writes no begin line, since the decision is not known yet.
  [[nodiscard]] ScopeTimer scope(std::string_view what, ScopeOptions options = ScopeOptions{},
                                 std::source_location where = std::source_location::current()) && {
    // Level first, so a filtered scope costs nothing and does not use up the budget.
    if (!active_ || !logger_.enabled(options.level)) options.level = Level::kOff;
    detail::RateGate gate;
    if (options.level != Level::kOff && rate_gated_) {
      if (options.min_duration > std::chrono::nanoseconds{0}) {
        gate = detail::RateGate{.interval = interval_, .once = once_, .gated = true};
        options.begin = false;
      } else if (!admit(where)) {
        options.level = Level::kOff;
      }
    }
    return ScopeTimer(logger_, options, std::string(what), where, take(), gate);
  }
  [[nodiscard]] ScopeTimer scope(std::string_view what, Level level,
                                 std::source_location where = std::source_location::current()) && {
    return std::move(*this).scope(what, ScopeOptions{.level = level}, where);
  }

 private:
  Fields take() { return std::move(owned_); }

  // The rate gate, after the level check so a filtered call costs nothing and is not counted.
  // When the call goes through after others were held back, the count rides on it as the
  // `suppressed` field, replacing any field of that name the caller supplied.
  bool admit(const std::source_location& where) {
    if (!rate_gated_) return true;
    std::uint64_t suppressed = 0;
    if (!detail::admit_rate(where, interval_, once_, suppressed)) return false;
    if (suppressed > 0) {
      std::erase_if(owned_, [](const Field& f) { return f.key == detail::kSuppressedField; });
      owned_.emplace_back(detail::kSuppressedField, suppressed);
    }
    return true;
  }

  Logger& logger_;
  bool active_ = true;  // false: the gating level was filtered, nothing is logged
  Fields owned_;
  std::chrono::nanoseconds interval_{0};
  bool once_ = false;
  bool rate_gated_ = false;
};

inline Logger::Entry Logger::with(std::initializer_list<Field> fields) & {
  return Entry(*this, fields);
}
inline Logger::Entry Logger::with(Level gate, std::initializer_list<Field> fields) & {
  return Entry(*this, gate, fields);
}
inline Logger::Entry Logger::with(Fields fields) & { return Entry(*this, std::move(fields)); }
inline Logger::Entry Logger::every(std::chrono::nanoseconds interval) & {
  return Entry(*this, interval, /*once=*/false);
}
inline Logger::Entry Logger::once() & {
  return Entry(*this, std::chrono::nanoseconds{0}, /*once=*/true);
}

}  // namespace etch

// The default logger, etch::get() and the free functions have always come with this header;
// they live in their own file now, included last because they need the complete Logger.
#include "default_logger.hpp"
