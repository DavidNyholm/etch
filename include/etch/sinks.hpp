// etch - sinks: where records go. Console (colour), file (rotating), memory (for tests), callback.
#pragma once

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <map>
#include <memory>
#include <mutex>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "json.hpp"
#include "record.hpp"

#if defined(_WIN32)
#include <io.h>  // _isatty, _fileno, _close
#if defined(_MSC_VER)
#include <fcntl.h>  // _O_* flags for the file sink's lock file
#include <share.h>  // _SH_DENYWR, _SH_DENYRW: the file sink refuses other writers

#include <sys/stat.h>  // _S_IWRITE
#endif
#else
#include <fcntl.h>  // open flags for the file sink's lock file
#include <unistd.h>  // isatty, fileno, close

#include <sys/file.h>  // flock: the file sink refuses a second writer from another process
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

class FileSink;

namespace detail {

// Every path with an open FileSink, so a second FileSink on the same file is refused instead of
// the two rotating each other's file away (or, on Windows, blocking each other's rotation),
// and the sink itself (weakly, so a sink mid-destruction is simply skipped) so an exit handler
// can flush every file sink the program still holds, wherever it lives. `owner` names the sink
// that holds the reservation, so a sink releases only its own entry; `sink` is set once the
// sink is fully open, so the exit handler never touches one still being built. Never
// destroyed: a FileSink owned by a global may be destroyed after any static would be.
struct OpenFile {
  FileSink* owner = nullptr;
  std::weak_ptr<FileSink> sink;
};
struct OpenFiles {
  std::mutex mutex;
  std::map<std::string, OpenFile> by_path;
  bool exit_handler_installed = false;
};
inline OpenFiles& open_files() {
  static OpenFiles* const files = new OpenFiles;
  return *files;
}

// The path as the sink uses it: absolute, with symlinks and ".." resolved where the filesystem
// allows, so that the lock file sits beside the real file (two names for one log must lock
// one lock file) and a change of working directory after the open changes nothing. A hard
// link is a second name the filesystem cannot resolve; two sinks through two hard links are
// not refused.
inline std::filesystem::path resolve_path(const std::filesystem::path& path) {
  std::error_code ec;
  // Absolute first: weakly_canonical leaves a relative path relative when no part of it exists
  // yet, and a sink must not follow the working directory around.
  std::filesystem::path resolved = std::filesystem::absolute(path, ec);
  if (ec) resolved = path;
  // A symlink as the last component is followed by hand: weakly_canonical leaves one that
  // points at a file not made yet unresolved, and the lock must sit beside the real file.
  constexpr int kMaxLinks = 32;  // a chain longer than this is left as a symlink: open() refuses
  for (int hop = 0; hop < kMaxLinks && std::filesystem::is_symlink(resolved, ec); ++hop) {
    const std::filesystem::path target = std::filesystem::read_symlink(resolved, ec);
    if (ec) break;
    if (target.is_absolute()) {
      resolved = target;
    } else {
      // The link's own directory, made real first: a relative target under a directory that
      // is itself a link or junction would otherwise be rewritten lexically ("alias/../x"
      // losing the junction) rather than followed.
      const std::filesystem::path parent = std::filesystem::canonical(resolved.parent_path(), ec);
      resolved = (ec ? resolved.parent_path() : parent) / target;
    }
  }
  const std::filesystem::path canonical = std::filesystem::weakly_canonical(resolved, ec);
  return ec ? resolved.lexically_normal() : canonical;
}

// Whether the name is one of etch's lock files, ".lock" in any case on Windows.
inline bool is_lock_file_name(const std::filesystem::path& path) {
  std::string extension = utf8(path.extension());
#if defined(_WIN32)
  for (char& c : extension) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
  }
#endif
  return extension == ".lock";
}

// One spelling per file: the resolved path, and on Windows case and separators folded, so
// "Logs/a.log" and "logs/../LOGS/A.LOG" collide.
inline std::string path_key(const std::filesystem::path& path) {
  std::string key = utf8(resolve_path(path));
#if defined(_WIN32)
  for (char& c : key) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    if (c == '/') c = '\\';
  }
#endif
  return key;
}

}  // namespace detail

// Appends lines to a file. Optional size-based rotation: when the file would exceed max_bytes
// it is renamed to <name>.1 (older ones shift to .2, .3 ... up to max_files) and a fresh file
// is started. Buffered; flushed on warn and above, on flush(), when destroyed, and by an exit
// handler, so a sink held by a local that std::exit() skips still gets its lines out.
//
// The file is held against other writers. A second FileSink on the path in this process is
// refused with EBUSY. A second process is refused too: the sink holds a lock file next to the
// log, <name>.lock, for as long as it lives (an exclusive flock on POSIX, an open with all
// sharing denied on Windows), taken before the log file itself is touched, so a refused open
// cannot truncate the other process's log, and kept through rotation, so no moment exists in
// which the path is free. The lock file is empty and is left in place, because deleting it
// would let a third process lock a file the second is still holding. Two processes writing one
// log would tear each other's lines and never rotate.
//
// I/O failures never throw out of a log call. They are counted in write_failures() and the sink
// keeps trying, so a disk that fills up and is freed again resumes logging. A rotation that
// cannot move the live file aside (on Windows, a viewer holding it open) keeps appending to it
// instead of truncating it, and tries again only after another max_bytes have been written.
class FileSink final : public Sink {
 public:
  using Options = FileSinkOptions;

  // Returns the open sink, or the OS error (no permission, a parent that is a file, ...), or
  // EBUSY when another FileSink has the same path open, in this process or in another.
  [[nodiscard]] static std::expected<std::shared_ptr<FileSink>, std::error_code> open(
      const std::filesystem::path& path_as_given, Options options = Options{}) {
    const std::filesystem::path path = detail::resolve_path(path_as_given);
    {
      std::error_code ec;
      if (std::filesystem::is_symlink(path, ec)) {  // a chain too long to follow: not ours to own
        return std::unexpected(std::make_error_code(std::errc::too_many_symbolic_link_levels));
      }
    }
    // ".lock" names are etch's lock files (below). A log by that name would be rotated or
    // truncated under another sink's lock, so it is refused in every process; on Windows,
    // where names do not care about case, in any spelling.
    if (detail::is_lock_file_name(path)) {
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    const std::string key = detail::path_key(path);
    detail::OpenFiles& files = detail::open_files();
    {
      std::lock_guard lock(files.mutex);
      if (!files.by_path.emplace(key, detail::OpenFile{}).second) {
        return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
      }
    }
    FileSink* raw = nullptr;
    try {
      raw = new FileSink(path, options, key);
    } catch (...) {
      std::lock_guard lock(files.mutex);
      files.by_path.erase(key);  // no object, so no destructor to release it
      throw;
    }
    {
      std::lock_guard lock(files.mutex);
      files.by_path[key].owner = raw;  // from here the destructor releases the reservation
    }
    // Including when the shared_ptr cannot be allocated (it deletes the object before throwing)
    // and on the failure returns below (the shared_ptr goes out of scope).
    std::shared_ptr<FileSink> sink(raw);
    if (options.create_directories && path.has_parent_path()) {
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);  // the opens report failure
    }
    if (const auto ec = sink->acquire_lock()) return std::unexpected(ec);
    if (const auto ec = sink->reopen(options.truncate)) return std::unexpected(ec);
    {
      // Published only now, fully built: the exit handler flushes what it finds here.
      std::lock_guard lock(files.mutex);
      files.by_path[key].sink = sink;
      if (!files.exit_handler_installed) {
        files.exit_handler_installed = true;
        std::atexit([] { flush_open_files(); });
      }
    }
    return sink;
  }

  ~FileSink() override {
    {
      std::lock_guard lock(mutex_);
      flush_locked();
      out_.close();  // release the handle before another sink may take the path
      release_lock();
    }
    detail::OpenFiles& files = detail::open_files();
    std::lock_guard lock(files.mutex);
    if (const auto it = files.by_path.find(key_);
        it != files.by_path.end() && it->second.owner == this) {
      files.by_path.erase(it);
    }
  }

  // The counters the run summary reads; shared, so they outlive the sink.
  std::shared_ptr<const detail::SinkCounters> counters() const noexcept { return counters_; }

  LineStyle style() const {
    std::lock_guard lock(mutex_);
    return style_;
  }
  void set_style(LineStyle style) {
    std::lock_guard lock(mutex_);
    style_ = std::move(style);
    if (!style_.colour.has_value()) style_.colour = false;
  }
  // Absolute, with symlinks resolved: the path the sink writes and locks, not the spelling
  // it was opened with.
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
  std::size_t write_failures() const noexcept {
    return counters_->write_failures.load(std::memory_order_relaxed);
  }

  void write(const Record& r) override {
    std::lock_guard lock(mutex_);
    line_.clear();
    format_record(r, style_, line_);  // reuses one buffer: no allocation per line
    line_ += '\n';
    // A failed reopen (after a rotation, say) must not disable logging for good: try again.
    if (!out_.is_open() && reopen(/*truncate=*/false)) {
      count_failure();
      return;
    }
    const bool would_overflow = size_ > 0 && size_ + line_.size() > options_.max_bytes;
    if (options_.max_bytes > 0 && would_overflow && size_ >= retry_rotation_at_) rotate();
    out_.write(line_.data(), static_cast<std::streamsize>(line_.size()));
    if (out_) {
      size_ += line_.size();
    } else {
      count_failure();
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
      : path_(std::move(path)),
        options_(std::move(options)),
        key_(std::move(key)),
        counters_(std::make_shared<detail::SinkCounters>()) {
    detail::warm_up_time_zone();
    style_ = options_.style;
    if (!style_.colour.has_value()) style_.colour = false;
  }

  void count_failure() noexcept {
    counters_->write_failures.fetch_add(1, std::memory_order_relaxed);
  }

  void flush_locked() {
    out_.flush();
    if (!out_) {
      count_failure();
      out_.clear();
    }
  }

  // The exit handler: every file sink still open is flushed, and from here on every line is
  // flushed as it is written (process_exiting), so a static destructor's lines reach the disk.
  // The sinks are taken through their weak pointers, outside the registry lock, so a sink being
  // destroyed at the same time is skipped rather than raced.
  static void flush_open_files() noexcept {
    detail::process_exiting().store(true, std::memory_order_relaxed);
    std::vector<std::shared_ptr<FileSink>> live;
    {
      detail::OpenFiles& files = detail::open_files();
      std::lock_guard lock(files.mutex);
      for (const auto& [key, entry] : files.by_path) {
        if (auto sink = entry.sink.lock()) live.push_back(std::move(sink));
      }
    }
    for (const auto& sink : live) sink->flush();
  }

  // Takes the lock file, <name>.lock, once for the sink's life. EBUSY when another process
  // holds it; the OS error when it cannot be made or locked. Only a POSIX filesystem that has
  // no flock at all (some network and virtual mounts: EOPNOTSUPP or ENOSYS) gives no lock, and
  // the sink says so in holds_lock(); every other failure, ENOLCK included, refuses the open,
  // since nothing may truncate a log it does not own.
  std::error_code acquire_lock() {
    std::filesystem::path lock_path = path_;
    lock_path += ".lock";
    errno = 0;
#if defined(_MSC_VER)
    const errno_t error = _wsopen_s(&lock_fd_, lock_path.c_str(),
                                    _O_WRONLY | _O_CREAT | _O_NOINHERIT, _SH_DENYRW, _S_IWRITE);
    if (error != 0) {
      lock_fd_ = -1;
      // EACCES covers both a sharing violation (another process holds the lock file) and a
      // real permission problem; the OS error the C runtime kept apart tells them apart.
      constexpr unsigned long kSharingViolation = 32;  // ERROR_SHARING_VIOLATION
      unsigned long os_error = 0;
      _get_doserrno(&os_error);
      if (error == EACCES && os_error == kSharingViolation) {
        return std::make_error_code(std::errc::device_or_resource_busy);
      }
      return std::error_code(error, std::generic_category());
    }
    holds_lock_ = true;
#elif !defined(_WIN32)
    constexpr mode_t kLockFileMode = 0666;
    lock_fd_ = ::open(lock_path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, kLockFileMode);
    if (lock_fd_ < 0) return std::error_code(errno != 0 ? errno : EIO, std::generic_category());
    if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
      const int lock_error = errno;
      if (lock_error == EWOULDBLOCK || lock_error == EAGAIN) {
        release_lock();
        return std::make_error_code(std::errc::device_or_resource_busy);
      }
      if (lock_error != EOPNOTSUPP && lock_error != ENOSYS) {
        release_lock();
        return std::error_code(lock_error, std::generic_category());
      }
      // No locking on this filesystem: the sink works, just without the guarantee.
    } else {
      holds_lock_ = true;
    }
#endif
    return {};
  }

  // Whether some other FileSink holds `path` as its live log: one in this process by the
  // registry (the caller holds its lock, so no sink can open the path while a rotation that
  // asked is still moving files), one in another process by its lock file, which is only
  // probed when it exists (probing must not leave lock files behind for logs nobody has). A
  // lock file that exists but cannot be opened or probed counts as held: a rotation must not
  // remove what it could not establish ownership of. The caller's own entry does not count
  // (a destination that is a symlink to the caller's log resolves to it; the link itself is
  // what the rotation moves).
  static bool held_by_a_sink(const std::filesystem::path& path, const FileSink* asking,
                             const detail::OpenFiles& files_locked) {
    if (const auto it = files_locked.by_path.find(detail::path_key(path));
        it != files_locked.by_path.end() && it->second.owner != asking) {
      return true;
    }
    std::filesystem::path lock_path = path;
    lock_path += ".lock";
    std::error_code ec;
    if (!std::filesystem::exists(lock_path, ec)) return false;
#if defined(_MSC_VER)
    int fd = -1;
    const errno_t error =
        _wsopen_s(&fd, lock_path.c_str(), _O_WRONLY | _O_NOINHERIT, _SH_DENYRW, _S_IWRITE);
    if (error != 0) return true;  // another process holds it, or it is not ours to judge
    _close(fd);
    return false;
#elif !defined(_WIN32)
    const int fd = ::open(lock_path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return true;
    const bool held = ::flock(fd, LOCK_EX | LOCK_NB) != 0;
    ::close(fd);  // releases the probe's own lock when it got one
    return held;
#else
    return false;
#endif
  }

  void release_lock() noexcept {
    if (lock_fd_ >= 0) {
#if defined(_WIN32)
      _close(lock_fd_);
#else
      ::close(lock_fd_);  // closing releases the flock
#endif
      lock_fd_ = -1;
    }
    holds_lock_ = false;
  }

 public:
  // Whether the lock file is held, so another process is refused the log. False only on a
  // POSIX filesystem without flock support.
  bool holds_lock() const noexcept { return holds_lock_; }

 private:
  // Binary mode on purpose: in text mode Windows turns every '\n' into "\r\n" behind the
  // stream's back, and size_ would no longer be the size of the file. On Windows the file is
  // also opened denying other writers, which keeps a program that is not etch from appending
  // to it; the lock file is what refuses another etch.
  std::error_code reopen(bool truncate) {
    out_.close();
    out_.clear();
    const auto mode =
        std::ios::out | std::ios::binary | (truncate ? std::ios::trunc : std::ios::app);
    errno = 0;  // so a failure reports this open, not an older unrelated error
#if defined(_MSC_VER)
    out_.open(path_.c_str(), mode, _SH_DENYWR);
#else
    out_.open(path_, mode);
#endif
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
    if (out_.fail()) count_failure();  // close can fail too; reopen() clears the state
    std::error_code ec;
    const auto numbered = [&](unsigned n) {
      auto p = path_;
      p += "." + std::to_string(n);
      return p;
    };
    // A rotation destination that another FileSink holds as its live log (in this process or,
    // by its lock file, in another) is neither removed nor renamed over: the rotation is
    // refused like a failed rename, and the live file keeps appending. The registry stays
    // locked from the question to the last rename, so no sink in this process can take a
    // destination in between (another process still can, in the moment between the probe and
    // the rename: two etch processes sharing one rotation namespace is a configuration error
    // this only narrows).
    detail::OpenFiles& files = detail::open_files();
    std::lock_guard registry_lock(files.mutex);
    bool destination_held = false;
    for (unsigned n = 1; n <= options_.max_files && !destination_held; ++n) {
      destination_held = held_by_a_sink(numbered(n), this, files);
    }
    bool live_file_preserved = false;
    if (options_.max_files > 0 && !destination_held) {
      std::filesystem::remove(numbered(options_.max_files), ec);  // may not exist: fine
      for (unsigned n = options_.max_files; n > 1; --n) {
        if (!std::filesystem::exists(numbered(n - 1), ec)) continue;
        std::filesystem::rename(numbered(n - 1), numbered(n), ec);
        if (ec) count_failure();  // that one history file is lost; the live file is not
      }
      std::filesystem::rename(path_, numbered(1), ec);
      live_file_preserved = !ec;
    }
    const bool start_fresh = live_file_preserved || options_.max_files == 0;
    if (start_fresh) {
      ++rotations_;
      retry_rotation_at_ = 0;
    } else {
      count_failure();
      retry_rotation_at_ = size_ + options_.max_bytes;
    }
    if (reopen(/*truncate=*/start_fresh)) count_failure();
  }

  std::filesystem::path path_;
  Options options_;
  std::string key_;  // what open() registered; erased by the destructor
  std::shared_ptr<detail::SinkCounters> counters_;
  LineStyle style_;  // guarded by mutex_
  std::ofstream out_;
  int lock_fd_ = -1;  // the lock file's descriptor, held for the sink's life; -1 when none
  bool holds_lock_ = false;
  std::string line_;  // guarded by mutex_
  std::uintmax_t size_ = 0;
  std::uintmax_t retry_rotation_at_ = 0;  // no rotation attempt until size_ reaches this
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
