// etch - sinks: where records go. The Sink interface, console (colour), memory (for tests)
// and callback sinks; the file sink is in file_sink.hpp.
#pragma once

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "json.hpp"
#include "record.hpp"

#if defined(_WIN32)
#include <io.h>  // _isatty, _fileno
#else
#include <unistd.h>  // isatty, fileno
#endif

// MSVC's std::print uses the console's Unicode path only when the translation unit's literal
// encoding is UTF-8. Without /utf-8, non-ASCII text reaches the console as raw bytes. The
// etch::etch CMake target sets the flag for consumers; this is for anyone who turned it off.
#if defined(_MSC_VER) && defined(_MSVC_EXECUTION_CHARACTER_SET) && \
    _MSVC_EXECUTION_CHARACTER_SET != 65001
#pragma message( \
    "etch: this translation unit is not compiled with /utf-8; non-ASCII text will be garbled on the Windows console")
#endif

namespace etch {

// A sink receives finished records. Every sink is safe to share between loggers and threads:
// each one serialises its own writes with a mutex.
//
// A sink may log from inside its write (a CallbackSink function, a custom sink reporting its
// own trouble) through any logger, including one that feeds it. The record is delivered once
// the write has returned, on the same thread, to every sink but the ones that were writing at
// the time; those count the skip in reentries(). Without that the sink's own mutex, which is
// not recursive, would deadlock, and two such sinks on two threads could deadlock each other.
// The one thing a sink must not do is wait for an async sink in front of it (its flush() or
// shutdown()): the worker cannot wait for itself.
class Sink {
 public:
  virtual ~Sink() = default;
  virtual void write(const Record& r) = 0;
  // A sink that keeps the record (the async queue) overrides this to take it without a copy.
  // The default forwards to the const& overload, so most sinks only implement that one.
  virtual void write(Record&& r) { write(static_cast<const Record&>(r)); }
  virtual void flush() {}
  // An async worker brackets each sweep of its ring with these, around the writes. A sink that
  // flushes at a level (FileSink) holds that flush back for the writes of that sweep until
  // end_batch(), so an error storm costs one flush per sweep instead of one per line. The
  // price: a crash inside a sweep can lose the sweep's lines at or above flush_on written
  // before it, where per-line flushing had them on disk; the window is one sweep of the
  // worker. An explicit flush() always flushes. The default does nothing; a sink that has no
  // flush to hold back needs neither.
  virtual void begin_batch() {}
  virtual void end_batch() {}

  // Records below this level are skipped by this sink (the logger has its own level too).
  // Level::kOff accepts nothing. Atomic because it can be changed from any thread while others
  // are logging.
  void set_level(Level level) noexcept { min_level_.store(level, std::memory_order_relaxed); }
  Level get_level() const noexcept { return min_level_.load(std::memory_order_relaxed); }
  bool accepts(Level level) const noexcept { return level < Level::kOff && level >= get_level(); }

  // Records this sink did not get because they were logged from inside its own write (above).
  std::size_t reentries() const noexcept { return reentries_.load(std::memory_order_relaxed); }
  void count_reentry() noexcept { reentries_.fetch_add(1, std::memory_order_relaxed); }

 private:
  std::atomic<Level> min_level_{Level::kTrace};
  std::atomic<std::size_t> reentries_{0};
};

using SinkList = std::vector<std::shared_ptr<Sink>>;

namespace detail {

// Counters a sink keeps for whoever watches it (the run summary), shared so they outlive the
// sink: a log reopened under load replaces its sinks, and their drops must not vanish.
struct SinkCounters {
  std::atomic<std::size_t> dropped{0};
  std::atomic<std::size_t> write_failures{0};
};

inline bool is_terminal(std::FILE* f) noexcept {
#if defined(_WIN32)
  return _isatty(_fileno(f)) != 0;
#else
  return isatty(fileno(f)) != 0;
#endif
}

#if defined(_WIN32)
// The three Win32 calls needed to switch the console to ANSI escape processing, declared by hand
// so this header does not drag in <windows.h> (and its min/max macros) for every user. If
// <windows.h> is already included its declarations are used; these match them exactly
// (HANDLE = void*, DWORD = unsigned long, BOOL = int). Only stdout and stderr are console
// streams; any other FILE* gets no colour.
#ifndef _WINDOWS_
extern "C" {
__declspec(dllimport) void* __stdcall GetStdHandle(unsigned long);
__declspec(dllimport) int __stdcall GetConsoleMode(void*, unsigned long*);
__declspec(dllimport) int __stdcall SetConsoleMode(void*, unsigned long);
}
#endif
inline bool enable_ansi_colours(std::FILE* f) noexcept {
  constexpr unsigned long kStdOutputHandle = static_cast<unsigned long>(-11);
  constexpr unsigned long kStdErrorHandle = static_cast<unsigned long>(-12);
  constexpr unsigned long kEnableVirtualTerminalProcessing = 0x0004;
  if (f != stdout && f != stderr) return false;
  void* handle = GetStdHandle(f == stderr ? kStdErrorHandle : kStdOutputHandle);
  unsigned long mode = 0;
  if (handle == nullptr || GetConsoleMode(handle, &mode) == 0) return false;
  if ((mode & kEnableVirtualTerminalProcessing) != 0) return true;
  return SetConsoleMode(handle, mode | kEnableVirtualTerminalProcessing) != 0;
}
#else
inline bool enable_ansi_colours(std::FILE*) noexcept { return true; }
#endif

// The value of an environment variable, or empty. Reads it and lets the pointer go, which is
// the one safe use of std::getenv.
inline std::string_view env(const char* name) noexcept {
#if defined(_MSC_VER)
// MSVC flags std::getenv as "unsafe" (C4996) because the returned buffer is shared. We only
// read it once, here, and never keep the pointer, so the standard function is the right call.
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
  const char* value = std::getenv(name);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
  return value == nullptr ? std::string_view{} : std::string_view(value);
}

// Colour decision, in the order the conventions give: NO_COLOR set to anything non-empty wins
// (https://no-color.org); then CLICOLOR_FORCE or FORCE_COLOR set to anything but "0" forces it
// on; then TERM=dumb turns it off; otherwise colour when the stream is a terminal.
inline bool want_colour(std::FILE* f) noexcept {
  if (!env("NO_COLOR").empty()) return false;
  const auto force = env("CLICOLOR_FORCE").empty() ? env("FORCE_COLOR") : env("CLICOLOR_FORCE");
  if (!force.empty() && force != "0") {
    enable_ansi_colours(f);  // best effort: a pipe does not need it
    return true;
  }
  if (env("TERM") == "dumb") return false;
  return is_terminal(f) && enable_ansi_colours(f);
}

}  // namespace detail

// Writes to stdout (or stderr). Colour is on automatically when the stream is a terminal and
// the environment does not say otherwise; override with the ColourMode argument. A LineStyle
// whose colour is unset keeps that decision, so set_style() never switches colour off by
// accident. Flushes every line so nothing is lost if the process dies.
class ConsoleSink final : public Sink {
 public:
  enum class ColourMode { kAutomatic, kAlways, kNever };

  explicit ConsoleSink(std::FILE* stream = stdout, ColourMode mode = ColourMode::kAutomatic,
                       LineStyle style = LineStyle{})
      : stream_(stream) {
    if (stream == nullptr) throw std::invalid_argument("etch::ConsoleSink: null stream");
    switch (mode) {
      case ColourMode::kAutomatic:
        colour_ = detail::want_colour(stream);
        break;
      case ColourMode::kAlways:
        detail::enable_ansi_colours(stream);  // best effort: a pipe does not need it
        colour_ = true;
        break;
      case ColourMode::kNever:
        colour_ = false;
        break;
    }
    detail::warm_up_time_zone();  // the tz database load lands here, not on the first line
    set_style(std::move(style));
  }

  // Both go through the mutex, so the style can be changed while other threads are logging.
  LineStyle style() const {
    std::lock_guard lock(mutex_);
    return style_;
  }
  void set_style(LineStyle style) {
    std::lock_guard lock(mutex_);
    style_ = std::move(style);
    if (!style_.colour.has_value()) style_.colour = colour_;
  }

  // Lines the stream refused (std::print throws std::system_error, fflush returns EOF).
  std::size_t write_failures() const {
    std::lock_guard lock(mutex_);
    return write_failures_;
  }

  void write(const Record& r) override {
    std::lock_guard lock(mutex_);
    line_.clear();
    format_record(r, style_, line_);  // reuses one buffer: no allocation per line
    try {
      std::println(stream_, "{}", line_);  // std::print writes UTF-8 correctly to a Windows console
    } catch (const std::exception&) {
      ++write_failures_;
    }
    if (std::fflush(stream_) != 0) ++write_failures_;
  }
  void flush() override {
    std::lock_guard lock(mutex_);
    if (std::fflush(stream_) != 0) ++write_failures_;
  }

 private:
  std::FILE* stream_;
  bool colour_ = false;  // what the stream and the environment decided, or what the caller said
  LineStyle style_;      // guarded by mutex_
  std::string line_;     // guarded by mutex_
  std::size_t write_failures_ = 0;
  mutable std::mutex mutex_;
};

// Keeps the last N records in memory. Made for tests and for dumping recent history after a
// crash; records() returns a snapshot you can assert on.
class MemorySink final : public Sink {
 public:
  static constexpr std::size_t kDefaultCapacity = 1024;

  explicit MemorySink(std::size_t capacity = kDefaultCapacity)
      : capacity_(capacity > 0 ? capacity : 1) {}

  void write(const Record& r) override {
    std::lock_guard lock(mutex_);
    records_.push_back(r);  // append first: if the copy throws, nothing is lost
    if (records_.size() > capacity_) records_.pop_front();
    ++total_;
  }

  std::vector<Record> records() const {
    std::lock_guard lock(mutex_);
    return {records_.begin(), records_.end()};
  }
  std::vector<std::string> messages() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    out.reserve(records_.size());
    for (const auto& r : records_) out.push_back(r.message);
    return out;
  }
  std::size_t size() const {
    std::lock_guard lock(mutex_);
    return records_.size();
  }
  std::size_t total() const {  // including records evicted by the capacity limit
    std::lock_guard lock(mutex_);
    return total_;
  }
  void clear() {
    std::lock_guard lock(mutex_);
    records_.clear();
    total_ = 0;
  }

 private:
  std::size_t capacity_;
  std::deque<Record> records_;
  std::size_t total_ = 0;
  mutable std::mutex mutex_;
};

// Hands every record to a function. The quickest way to plug in something custom (a game's
// on-screen console, a network sender, a test probe). The function runs under this sink's
// mutex; see the contract on Sink.
class CallbackSink final : public Sink {
 public:
  using Callback = std::function<void(const Record&)>;
  explicit CallbackSink(Callback fn) : fn_(std::move(fn)) {
    if (!fn_) throw std::invalid_argument("etch::CallbackSink: empty callback");
  }

  void write(const Record& r) override {
    std::lock_guard lock(mutex_);
    fn_(r);
  }

 private:
  Callback fn_;
  std::mutex mutex_;
};

}  // namespace etch

// FileSink has always come with this header; it lives in its own file now, included last
// because it needs the complete Sink.
#include "file_sink.hpp"
