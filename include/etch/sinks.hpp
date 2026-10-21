// etch - sinks: where records go. Console (colour), file (rotating), memory (for tests), callback.
#pragma once

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <memory>
#include <mutex>
#include <print>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
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
// Contract for anything a sink calls (a CallbackSink function, a custom sink's write): it must
// not log through a logger that feeds the same sink. The sink's mutex is held during the call
// and is not recursive, and the async worker cannot wait for itself, so the result is a deadlock.
class Sink {
 public:
  virtual ~Sink() = default;
  virtual void write(const Record& r) = 0;
  // A sink that keeps the record (the async queue) overrides this to take it without a copy.
  // The default forwards to the const& overload, so most sinks only implement that one.
  virtual void write(Record&& r) { write(static_cast<const Record&>(r)); }
  virtual void flush() {}

  // Records below this level are skipped by this sink (the logger has its own level too).
  // Level::kOff accepts nothing. Atomic because it can be changed from any thread while others
  // are logging.
  void set_level(Level level) noexcept { min_level_.store(level, std::memory_order_relaxed); }
  Level get_level() const noexcept { return min_level_.load(std::memory_order_relaxed); }
  bool accepts(Level level) const noexcept { return level >= get_level(); }

 private:
  std::atomic<Level> min_level_{Level::kTrace};
};

namespace detail {

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

// Defined outside FileSink so it can be a default argument of FileSink::open; GCC and Clang
// reject a nested aggregate with default member initializers in that position.
struct FileSinkOptions {
  std::uintmax_t max_bytes = 0;    // 0: never rotate
  unsigned max_files = 3;          // rotated files to keep (.1 .. .N); 0: just restart the file
  Level flush_on = Level::kWarn;   // flush immediately at this level and above
  bool truncate = false;           // start empty instead of appending
  bool create_directories = true;  // make missing parent directories
  LineStyle style{};               // colour defaults to off for a file
};

namespace detail {

// Every path with an open FileSink, so a second FileSink on the same file is refused instead of
// the two rotating each other's file away (or, on Windows, blocking each other's rotation).
inline std::mutex& open_paths_mutex() {
  static std::mutex m;
  return m;
}
inline std::set<std::string>& open_paths() {
  static std::set<std::string> paths;
  return paths;
}

inline std::string path_key(const std::filesystem::path& path) {
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(path, ec);
  return utf8(ec ? path : absolute.lexically_normal());
}

}  // namespace detail

// Appends lines to a file. Optional size-based rotation: when the file would exceed max_bytes
// it is renamed to <name>.1 (older ones shift to .2, .3 ... up to max_files) and a fresh file
// is started. Buffered; flushed on warn and above, on flush(), and when destroyed.
//
// I/O failures never throw out of a log call. They are counted in write_failures() and the sink
// keeps trying, so a disk that fills up and is freed again resumes logging. A rotation that
// cannot move the live file aside (on Windows, a viewer holding it open) keeps appending to it
// instead of truncating it, and tries again only after another max_bytes have been written.
class FileSink final : public Sink {
 public:
  using Options = FileSinkOptions;

  // Returns the open sink, or the OS error (no permission, a parent that is a file, ...), or
  // EBUSY when another FileSink in this process already has the same path open.
  [[nodiscard]] static std::expected<std::shared_ptr<FileSink>, std::error_code> open(
      const std::filesystem::path& path, Options options = Options{}) {
    const std::string key = detail::path_key(path);
    {
      std::lock_guard lock(detail::open_paths_mutex());
      if (!detail::open_paths().insert(key).second) {
        return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
      }
    }
    auto sink = std::shared_ptr<FileSink>(new FileSink(path, options, key));
    if (options.create_directories && path.has_parent_path()) {
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);  // reopen() reports failure
    }
    if (const auto ec = sink->reopen(options.truncate)) return std::unexpected(ec);
    return sink;
  }

  ~FileSink() override {
    {
      std::lock_guard lock(mutex_);
      flush_locked();
    }
    std::lock_guard lock(detail::open_paths_mutex());
    detail::open_paths().erase(key_);
  }

  LineStyle style() const {
    std::lock_guard lock(mutex_);
    return style_;
  }
  void set_style(LineStyle style) {
    std::lock_guard lock(mutex_);
    style_ = std::move(style);
    if (!style_.colour.has_value()) style_.colour = false;
  }
  const std::filesystem::path& path() const noexcept { return path_; }
  std::uintmax_t bytes_written() const {  // size of the live file, including what it had on open
    std::lock_guard lock(mutex_);
    return size_;
  }
  unsigned rotations() const {
    std::lock_guard lock(mutex_);
    return rotations_;
  }
  // Writes, flushes and rotation attempts that the filesystem refused. Zero means every line is
  // on disk (as far as the C++ stream can tell; it says nothing about power loss).
  std::size_t write_failures() const {
    std::lock_guard lock(mutex_);
    return write_failures_;
  }

  void write(const Record& r) override {
    std::lock_guard lock(mutex_);
    line_.clear();
    format_record(r, style_, line_);  // reuses one buffer: no allocation per line
    line_ += '\n';
    // A failed reopen (after a rotation, say) must not disable logging for good: try again.
    if (!out_.is_open() && reopen(/*truncate=*/false)) {
      ++write_failures_;
      return;
    }
    const bool would_overflow = size_ > 0 && size_ + line_.size() > options_.max_bytes;
    if (options_.max_bytes > 0 && would_overflow && size_ >= retry_rotation_at_) rotate();
    out_.write(line_.data(), static_cast<std::streamsize>(line_.size()));
    if (out_) {
      size_ += line_.size();
    } else {
      ++write_failures_;
      out_.clear();  // keep the stream usable: the next write may succeed
    }
    const bool exiting = detail::process_exiting().load(std::memory_order_relaxed);
    if (r.level >= options_.flush_on || exiting) flush_locked();
  }
  void flush() override {
    std::lock_guard lock(mutex_);
    flush_locked();
  }

 private:
  FileSink(std::filesystem::path path, Options options, std::string key)
      : path_(std::move(path)), options_(std::move(options)), key_(std::move(key)) {
    detail::warm_up_time_zone();
    style_ = options_.style;
    if (!style_.colour.has_value()) style_.colour = false;
  }

  void flush_locked() {
    out_.flush();
    if (!out_) {
      ++write_failures_;
      out_.clear();
    }
  }

  // Binary mode on purpose: in text mode Windows turns every '\n' into "\r\n" behind the
  // stream's back, and size_ would no longer be the size of the file.
  std::error_code reopen(bool truncate) {
    out_.close();
    out_.clear();
    const auto mode =
        std::ios::out | std::ios::binary | (truncate ? std::ios::trunc : std::ios::app);
    errno = 0;  // so a failure reports this open, not an older unrelated error
    out_.open(path_, mode);
    if (!out_.is_open()) {
      // ofstream does not promise to set errno; fall back to a generic I/O error.
      return std::error_code(errno != 0 ? errno : EIO, std::generic_category());
    }
    std::error_code ec;
    const auto existing = std::filesystem::file_size(path_, ec);
    size_ = ec ? 0 : existing;
    return {};
  }

  // Caller holds mutex_. Moves the live file to .1 (shifting older ones up) and starts a fresh
  // one. If the live file cannot be moved it is reopened for appending, never truncated: a
  // rotation that fails must not cost the lines it was meant to preserve. The next attempt
  // waits for another max_bytes, so a viewer holding the file open costs one attempt per
  // max_bytes, not one per line.
  void rotate() {
    flush_locked();  // what close() would flush, but with the failure counted
    out_.close();
    if (out_.fail()) ++write_failures_;  // close can fail too; reopen() clears the state
    std::error_code ec;
    const auto numbered = [&](unsigned n) {
      auto p = path_;
      p += "." + std::to_string(n);
      return p;
    };
    bool live_file_preserved = false;
    if (options_.max_files > 0) {
      std::filesystem::remove(numbered(options_.max_files), ec);  // may not exist: fine
      for (unsigned n = options_.max_files; n > 1; --n) {
        if (!std::filesystem::exists(numbered(n - 1), ec)) continue;
        std::filesystem::rename(numbered(n - 1), numbered(n), ec);
        if (ec) ++write_failures_;  // that one history file is lost; the live file is not
      }
      std::filesystem::rename(path_, numbered(1), ec);
      live_file_preserved = !ec;
    }
    const bool start_fresh = live_file_preserved || options_.max_files == 0;
    if (start_fresh) {
      ++rotations_;
      retry_rotation_at_ = 0;
    } else {
      ++write_failures_;
      retry_rotation_at_ = size_ + options_.max_bytes;
    }
    if (reopen(/*truncate=*/start_fresh)) ++write_failures_;
  }

  std::filesystem::path path_;
  Options options_;
  std::string key_;  // what open() registered; erased by the destructor
  LineStyle style_;  // guarded by mutex_
  std::ofstream out_;
  std::string line_;  // guarded by mutex_
  std::uintmax_t size_ = 0;
  std::uintmax_t retry_rotation_at_ = 0;  // no rotation attempt until size_ reaches this
  unsigned rotations_ = 0;
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
