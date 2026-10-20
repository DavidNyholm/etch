// etch - sinks: where records go. Console (colour), file (rotating), memory (for tests), callback.
#pragma once

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <print>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "record.hpp"

#if defined(_WIN32)
#include <io.h>  // _isatty, _fileno
#else
#include <unistd.h>  // isatty, fileno
#endif

namespace etch {

// A sink receives finished records. Every sink is safe to share between loggers and threads:
// each one serialises its own writes.
class Sink {
 public:
  virtual ~Sink() = default;
  virtual void write(const Record& r) = 0;
  virtual void flush() {}

  // Records below this level are skipped by this sink (the logger has its own level too).
  void set_level(Level level) noexcept { min_level_ = level; }
  Level get_level() const noexcept { return min_level_; }
  bool accepts(Level level) const noexcept { return level >= min_level_; }

 private:
  Level min_level_ = Level::kTrace;
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
// (HANDLE = void*, DWORD = unsigned long, BOOL = int).
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
  void* handle = GetStdHandle(f == stderr ? kStdErrorHandle : kStdOutputHandle);
  unsigned long mode = 0;
  if (handle == nullptr || GetConsoleMode(handle, &mode) == 0) return false;
  if ((mode & kEnableVirtualTerminalProcessing) != 0) return true;
  return SetConsoleMode(handle, mode | kEnableVirtualTerminalProcessing) != 0;
}
#else
inline bool enable_ansi_colours(std::FILE*) noexcept { return true; }
#endif

// Colour decision: a terminal, unless the NO_COLOR convention (https://no-color.org) says no.
inline bool want_colour(std::FILE* f) noexcept {
#if defined(_MSC_VER)
// MSVC flags std::getenv as "unsafe" (C4996) because the returned buffer is shared. We only
// test it for null and never keep the pointer, so the standard function is the right call.
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
  const bool no_colour_requested = std::getenv("NO_COLOR") != nullptr;
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
  if (no_colour_requested) return false;
  return is_terminal(f) && enable_ansi_colours(f);
}

}  // namespace detail

// Writes to stdout (or stderr). Colour is on automatically when the stream is a terminal and
// NO_COLOR is not set; override with the ColourMode argument or style().colour. Flushes every
// line so nothing is lost if the process dies.
class ConsoleSink final : public Sink {
 public:
  enum class ColourMode { kAutomatic, kAlways, kNever };

  explicit ConsoleSink(std::FILE* stream = stdout, ColourMode mode = ColourMode::kAutomatic)
      : stream_(stream) {
    style_.colour = mode == ColourMode::kAlways  ? true
                    : mode == ColourMode::kNever ? false
                                                 : detail::want_colour(stream);
  }

  LineStyle& style() noexcept { return style_; }
  const LineStyle& style() const noexcept { return style_; }

  void write(const Record& r) override {
    const std::string line = format_line(r, style_);
    std::lock_guard lock(mutex_);
    std::println(stream_, "{}", line);  // std::print writes UTF-8 correctly to a Windows console
    std::fflush(stream_);
  }
  void flush() override {
    std::lock_guard lock(mutex_);
    std::fflush(stream_);
  }

 private:
  std::FILE* stream_;
  LineStyle style_;
  std::mutex mutex_;
};

// Appends lines to a file. Optional size-based rotation: when the file would exceed max_bytes
// it is renamed to <name>.1 (older ones shift to .2, .3 ... up to max_files) and a fresh file
// is started. Buffered; flushed on warn and above, on flush(), and when destroyed.
class FileSink final : public Sink {
 public:
  struct Options {
    std::uintmax_t max_bytes = 0;   // 0: never rotate
    unsigned max_files = 3;         // how many rotated files to keep (.1 .. .N)
    Level flush_on = Level::kWarn;  // flush immediately at this level and above
    bool truncate = false;          // start empty instead of appending
  };

  // Returns the open sink, or the OS error (missing directory, no permission, ...).
  [[nodiscard]] static std::expected<std::shared_ptr<FileSink>, std::error_code> open(
      const std::filesystem::path& path, Options options = {}) {
    auto sink = std::shared_ptr<FileSink>(new FileSink(path, options));
    if (const auto ec = sink->reopen(options.truncate)) return std::unexpected(ec);
    return sink;
  }

  ~FileSink() override {
    std::lock_guard lock(mutex_);
    out_.flush();
  }

  LineStyle& style() noexcept { return style_; }
  const std::filesystem::path& path() const noexcept { return path_; }
  std::uintmax_t bytes_written() const {
    std::lock_guard lock(mutex_);
    return size_;
  }
  unsigned rotations() const {
    std::lock_guard lock(mutex_);
    return rotations_;
  }

  void write(const Record& r) override {
    std::string line = format_line(r, style_);
    line += '\n';
    std::lock_guard lock(mutex_);
    const bool would_overflow = size_ > 0 && size_ + line.size() > options_.max_bytes;
    if (options_.max_bytes > 0 && would_overflow) rotate();
    out_.write(line.data(), static_cast<std::streamsize>(line.size()));
    size_ += line.size();
    if (r.level >= options_.flush_on) out_.flush();
  }
  void flush() override {
    std::lock_guard lock(mutex_);
    out_.flush();
  }

 private:
  FileSink(std::filesystem::path path, Options options)
      : path_(std::move(path)), options_(options) {
    style_.colour = false;
  }

  std::error_code reopen(bool truncate) {
    out_.close();
    out_.clear();
    out_.open(path_, truncate ? std::ios::out | std::ios::trunc : std::ios::out | std::ios::app);
    if (!out_.is_open()) return std::error_code(errno, std::generic_category());
    std::error_code ec;
    const auto existing = std::filesystem::file_size(path_, ec);
    size_ = ec ? 0 : existing;
    return {};
  }

  // Caller holds mutex_. Filesystem failures here are swallowed on purpose: a logger must not
  // throw from inside a log call. The worst case is that we keep appending to the same file.
  void rotate() {
    out_.close();
    std::error_code ec;
    const auto numbered = [&](unsigned n) {
      auto p = path_;
      p += "." + std::to_string(n);
      return p;
    };
    std::filesystem::remove(numbered(options_.max_files), ec);
    for (unsigned n = options_.max_files; n > 1; --n) {
      if (std::filesystem::exists(numbered(n - 1), ec)) {
        std::filesystem::rename(numbered(n - 1), numbered(n), ec);
      }
    }
    if (options_.max_files > 0) {
      std::filesystem::rename(path_, numbered(1), ec);
    } else {
      std::filesystem::remove(path_, ec);
    }
    ++rotations_;
    reopen(true);
  }

  std::filesystem::path path_;
  Options options_;
  LineStyle style_;
  std::ofstream out_;
  std::uintmax_t size_ = 0;
  unsigned rotations_ = 0;
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
    if (records_.size() == capacity_) records_.pop_front();
    records_.push_back(r);
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
// on-screen console, a network sender, a test probe).
class CallbackSink final : public Sink {
 public:
  using Callback = std::function<void(const Record&)>;
  explicit CallbackSink(Callback fn) : fn_(std::move(fn)) {}

  void write(const Record& r) override {
    std::lock_guard lock(mutex_);
    fn_(r);
  }

 private:
  Callback fn_;
  std::mutex mutex_;
};

}  // namespace etch
