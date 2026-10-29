// etch - FileSink: a rotating log file that no second sink or process can open, with a lock
// file beside it, a per-level flush that an async worker may batch, and an exit handler that
// flushes every file sink still open.
#pragma once

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "json.hpp"
#include "record.hpp"
#include "sinks.hpp"

#if defined(_WIN32)
#include <io.h>  // _close
#if defined(_MSC_VER)
#include <fcntl.h>  // _O_* flags for the lock file
#include <share.h>  // _SH_DENYWR, _SH_DENYRW: the file sink refuses other writers

#include <sys/stat.h>  // _S_IWRITE
#endif
#else
#include <fcntl.h>   // open flags for the lock file
#include <unistd.h>  // close

#include <sys/file.h>  // flock: the file sink refuses a second writer from another process
#endif

namespace etch {

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
    if (detail::process_exiting().load(std::memory_order_relaxed)) {
      flush_locked();  // a static destructor's line reaches the disk whatever else happens
    } else if (r.level >= options_.flush_on) {
      if (batches_open_ > 0) {
        flush_pending_ = true;  // the worker's end_batch() flushes once for the whole sweep
      } else {
        flush_locked();
      }
    }
  }
  void flush() override {
    std::lock_guard lock(mutex_);
    flush_locked();
  }
  // A count rather than a flag: one file sink can sit behind two async sinks, whose sweeps
  // may overlap.
  void begin_batch() override {
    std::lock_guard lock(mutex_);
    ++batches_open_;
  }
  void end_batch() override {
    std::lock_guard lock(mutex_);
    if (batches_open_ > 0) --batches_open_;
    if (batches_open_ == 0 && flush_pending_) flush_locked();
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
    flush_pending_ = false;
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
  unsigned batches_open_ = 0;   // async sweeps in progress (begin_batch/end_batch)
  bool flush_pending_ = false;  // a flush_on line was written inside a sweep
  mutable std::mutex mutex_;
};

}  // namespace etch
