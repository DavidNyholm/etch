// etch - for logs that a program reads: the agent profile (compact JSON, nothing a person needs
// and a model pays for), one call that opens such a log, and SummarySink, which turns a whole
// run into one JSON object so a reader can decide whether to open the full log at all.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <source_location>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "async_control.hpp"
#include "async_sink.hpp"
#include "default_logger.hpp"
#include "file_sink.hpp"
#include "json.hpp"
#include "level.hpp"
#include "logger.hpp"
#include "record.hpp"
#include "sinks.hpp"
#include "version.hpp"

namespace etch {

// The settings for a log a program will read: JSON Lines with epoch-millisecond timestamps and
// without the thread, pid, path and function fields; the drop policy with warnings and above
// kept. Same code as every other log, only the defaults differ, so a human-readable console
// and an agent file can share one logger. `style` is what open_agent_log() gives the file:
// edit it (p.style.json.thread = true) and the file follows; `file.style` is only there for a
// program that passes `file` to FileSink::open itself. Two things the profile cannot set for
// you, because they belong to the call, not the sink: a small hexdump limit (a model rarely
// needs more than the first 256 bytes; pass it to hexdump()) and scopes without begin lines
// (ScopeOptions{.begin = false}).
struct AgentProfile {
  LineStyle style{.format = LineFormat::kJson,
                  .json = JsonStyle{.time = JsonStyle::Time::kEpochMillis,
                                    .thread = false,
                                    .pid = false,
                                    .path = false,
                                    .function = false}};
  FileSinkOptions file{.flush_on = Level::kWarn, .style = style};
  AsyncSink::Overflow overflow = AsyncSink::Overflow::kDrop;
  Level keep_from = Level::kWarn;
  std::size_t queue_capacity = AsyncSink::kDefaultCapacity;
  AsyncSink::WorkerPriority worker_priority = AsyncSink::WorkerPriority::kAboveNormal;
  // Kept so that 0.4 code setting them still compiles; nothing in etch has ever read them
  // (not marked deprecated: GCC would then warn on every copy of a profile). They are advice:
  // pass the limit to hexdump() and the options to scope() yourself.
  ScopeOptions scope{.level = Level::kDebug, .begin = false};
  std::size_t hexdump_limit = 256;
};

inline AgentProfile agent_profile() { return AgentProfile{}; }

// One id per process, made at first use from the system's random source: 16 hex digits. The
// agent file's first record of a run and the run summary both carry it, together with the
// pid, so a reader can tell the runs apart in a file that several runs appended to and tie a
// summary to the run it describes. A fork copies it: the child continues the parent's run.
inline std::string_view run_id() noexcept {
  // Fixed storage with a trivial destructor, filled without allocating: the summary's exit
  // handler reads it after statics are gone, and a fatal's delivery must not be the place an
  // allocation fails. A random source that fails (it may) gives way to the clock and the pid,
  // which tell runs apart well enough for a log directory.
  static const std::array<char, 16> id = [] {
    std::uint64_t bits = 0;
    try {
      std::random_device source;
      bits = (static_cast<std::uint64_t>(source()) << 32) | source();
    } catch (...) {
      bits =
          static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
          (static_cast<std::uint64_t>(detail::process_id()) << 32);
    }
    std::array<char, 16> hex{};
    for (std::size_t i = 0; i < hex.size(); ++i) {
      hex[hex.size() - 1 - i] = "0123456789abcdef"[bits & 0xfU];
      bits >>= 4;
    }
    return hex;
  }();
  return std::string_view(id.data(), id.size());
}

// The record that starts a run in an agent file: "run started" from logger "etch", marked
// etch=run-started (the reserved field, see record.hpp), with the run id, the pid (the
// profile leaves it off every other line) and the etch version.
inline Record run_started_record(std::source_location where = std::source_location::current()) {
  Record r;
  r.level = Level::kInfo;
  r.time = std::chrono::system_clock::now();
  r.thread = std::this_thread::get_id();
  r.thread_index = detail::this_thread_index();
  r.where = where;
  r.logger_name = "etch";
  r.message = "run started";
  r.fields.emplace_back(detail::kEtchField, "run-started");  // the reserved marker first
  r.fields.emplace_back("run_id", run_id());
  r.fields.emplace_back("pid", detail::process_id());
  r.fields.emplace_back("version", kVersion);
  return r;
}

// Opens the file an agent will read: a JSON file sink in the agent profile behind an async
// sink that drops trace, debug and info rather than stall the caller, keeps warnings and
// above, and marks every gap. The file is appended to, and each run begins with a "run
// started" record (above), written straight into the file before anything is queued, so a
// reader can find the run boundaries and the run id. Returns the async sink; attach it to a
// logger.
[[nodiscard]] inline std::expected<std::shared_ptr<AsyncSink>, std::error_code> open_agent_log(
    const std::filesystem::path& path, AgentProfile profile = agent_profile()) {
  FileSinkOptions options = profile.file;
  options.style = profile.style;  // the profile's style wins, so an edit to it is not lost
  auto file = FileSink::open(path, std::move(options));
  if (!file) return std::unexpected(file.error());
  (*file)->write(run_started_record());  // also fixes the run id, here at setup time
  return std::make_shared<AsyncSink>(std::move(*file), profile.queue_capacity, profile.overflow,
                                     profile.keep_from, profile.worker_priority);
}

// Counts what passed through it and remembers the first and last error, so that at the end of
// a run one JSON object says what happened: records per level and per logger, the errors,
// and, for every async or file sink it was asked to watch, how many records were dropped or
// refused. A reader looks at this first and opens the full log only when it has to.
//
// Give it to the same logger as the real sinks (it is cheap: a few counters under a mutex),
// and watch() the sinks whose counters matter. write_to() writes the object to a file at any
// time; install() does all of that for the default logger and writes the file at exit, and
// also the moment a fatal record arrives, since a run that aborts after its fatal never
// reaches exit. The object says which ("written_at": "exit", "fatal" or "call") and carries
// the run id and pid, so it can be tied to the agent file's run.
class SummarySink final : public Sink {
 public:
  struct ErrorSeen {
    std::chrono::system_clock::time_point time;
    Level level{};
    std::string logger;
    std::string file;
    std::uint32_t line = 0;
    std::string message;
  };

  SummarySink() : started_(std::chrono::system_clock::now()) {}

  void write(const Record& r) override {
    {
      std::lock_guard lock(mutex_);
      ++total_;
      if (r.level < Level::kOff) ++by_level_[static_cast<std::size_t>(r.level)];
      ++by_logger_[r.logger_name];
      if (r.level >= Level::kError) {
        ErrorSeen seen{r.time,         r.level,
                       r.logger_name,  std::string(detail::base_name(r.where.file_name())),
                       r.where.line(), std::string(r.user_message())};
        if (!first_error_) first_error_ = seen;
        last_error_ = std::move(seen);
      }
    }
    if (r.level == Level::kFatal) write_now("fatal");
  }

  // Sinks whose own counters belong in the summary. What is kept is the sink's counters, which
  // the sink shares and which outlive it, so a sink that is gone by the time the summary is
  // written (a log reopened under load replaces its sinks) still counts in full.
  void watch(const std::shared_ptr<AsyncSink>& sink) {
    if (sink == nullptr) return;  // nothing to count
    std::lock_guard lock(mutex_);
    watched_.push_back(sink->counters());
  }
  void watch(const std::shared_ptr<FileSink>& sink) {
    if (sink == nullptr) return;
    std::lock_guard lock(mutex_);
    watched_.push_back(sink->counters());
    watched_files_.push_back(sink);  // flushed before the final summary, so its failures count
  }

  // Flushes the watched file sinks that are still alive, so a failure in their last flush is
  // in the counters before the summary is written. The installed summary does this at exit.
  void flush_watched() {
    std::vector<std::weak_ptr<FileSink>> files;
    {
      std::lock_guard lock(mutex_);
      files = watched_files_;
    }
    for (const auto& weak : files) {
      if (const auto file = weak.lock()) file->flush();
    }
  }

  std::size_t total() const {
    std::lock_guard lock(mutex_);
    return total_;
  }
  std::size_t count(Level level) const {
    std::lock_guard lock(mutex_);
    return level < Level::kOff ? by_level_[static_cast<std::size_t>(level)] : 0;
  }
  std::optional<ErrorSeen> first_error() const {
    std::lock_guard lock(mutex_);
    return first_error_;
  }
  std::optional<ErrorSeen> last_error() const {
    std::lock_guard lock(mutex_);
    return last_error_;
  }

  // The whole run as one JSON object (one line, no trailing newline). `written_at` says what
  // produced it: "call" (the program asked), "fatal" or "exit" (the installed summary).
  std::string json(std::string_view written_at = "call") const {
    std::lock_guard lock(mutex_);
    std::string out = "{\"etch_summary\":1,\"run_id\":";
    auto app = std::back_inserter(out);
    json_string(run_id(), out);
    std::format_to(app, ",\"pid\":{},\"written_at\":", detail::process_id());
    json_string(written_at, out);
    out += ",\"started\":\"";
    format_time_iso8601(out, started_);
    out += "\",\"ended\":\"";
    const auto now = std::chrono::system_clock::now();
    format_time_iso8601(out, now);
    out += '"';
    const auto seconds = std::chrono::duration<double>(now - started_).count();
    std::format_to(app, ",\"duration_s\":{:.3f},\"records\":{}", seconds, total_);
    out += ",\"by_level\":{";
    for (std::size_t i = 0; i < kLevelCount; ++i) {
      if (i > 0) out += ',';
      json_string(level_name(static_cast<Level>(i)), out);
      std::format_to(app, ":{}", by_level_[i]);
    }
    out += "},\"by_logger\":{";
    bool first = true;
    for (const auto& [name, n] : by_logger_) {
      if (!first) out += ',';
      first = false;
      json_string(name, out);
      std::format_to(app, ":{}", n);
    }
    out += '}';
    std::size_t dropped = 0;
    std::size_t failures = 0;
    for (const auto& counters : watched_) {
      dropped += counters->dropped.load(std::memory_order_relaxed);
      failures += counters->write_failures.load(std::memory_order_relaxed);
    }
    std::format_to(app, ",\"dropped\":{},\"write_failures\":{},\"report_failures\":{}", dropped,
                   failures, detail::rate_report_failures().load(std::memory_order_relaxed));
    append_error(",\"first_error\":", first_error_, out);
    append_error(",\"last_error\":", last_error_, out);
    out += '}';
    return out;
  }

  // Writes the object and a newline to the file (replacing it), making missing parent
  // directories like FileSink does. When no file exists yet it is written to a temporary of
  // its own beside it (a name no other writer uses, created exclusively) and renamed into
  // place, so a reader never sees a half-written object. When a file exists it is rewritten
  // in place: that keeps its mode (a summary made 0600 stays 0600, and is never exposed
  // through a temporary with the default mode), at the price that a reader can see the
  // object half-written for the microseconds of the write, as in 0.4. Summary writes to any
  // path are serialised process-wide, so two summaries on one path cannot interleave in it.
  // Returns the OS error if any.
  std::error_code write_to(const std::filesystem::path& path,
                           std::string_view written_at = "call") const {
    std::lock_guard writing(summary_file_mutex());
    if (path.has_parent_path()) {
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);  // the open reports failure
    }
    const std::string object = json(written_at);
    std::error_code ec;
    // The entry itself, not what it points to: a symlink (dangling or not) is written through,
    // never replaced by a plain file, so a summary linked into a private directory stays
    // there. Only a name with nothing behind it gets the staged first write.
    if (std::filesystem::symlink_status(path, ec).type() != std::filesystem::file_type::not_found) {
      return write_file(path, object, /*exclusive=*/false, nullptr);
    }
    static std::atomic<unsigned> temporaries{0};
    std::filesystem::path temporary = path;
    temporary += std::format(".{}.{}.tmp", detail::process_id(),
                             temporaries.fetch_add(1, std::memory_order_relaxed));
    bool created = false;
    if (const auto write_ec = write_file(temporary, object, /*exclusive=*/true, &created)) {
      if (created) std::filesystem::remove(temporary, ec);  // ours; a name that was taken is not
      return write_ec;
    }
    std::filesystem::rename(temporary, path, ec);
    if (!ec) return {};
    std::filesystem::remove(temporary, ec);
    return write_file(path, object, /*exclusive=*/false, nullptr);  // refused rename: in place
  }

  // Attaches a summary sink to the default logger (and so to every etch::get() logger),
  // watches the given sinks, and writes the summary to `path` when the process exits, after
  // the async sinks have been drained, and at once whenever a fatal record arrives. The sink
  // lives until exit; it is also returned so the program can ask it questions or write the
  // summary earlier.
  static std::shared_ptr<SummarySink> install(std::filesystem::path path,
                                              std::vector<std::shared_ptr<Sink>> watched = {}) {
    auto summary = std::make_shared<SummarySink>();
    for (const auto& sink : watched) {
      if (auto async = std::dynamic_pointer_cast<AsyncSink>(sink)) summary->watch(async);
      if (auto file = std::dynamic_pointer_cast<FileSink>(sink)) summary->watch(file);
    }
    default_logger().add_sink(summary);
    (void)run_id();  // fixed here, at setup time, rather than inside a fatal's delivery
    auto& slot = installed();
    std::lock_guard lock(slot.mutex);
    slot.sink = summary;
    slot.path = std::move(path);
    detail::install_fork_handler();  // the generation below must move in a child
    slot.fork_generation = detail::fork_generation().load(std::memory_order_relaxed);
    if (!slot.handler_registered) {
      slot.handler_registered = true;
      // Registered after the async exit handler would be if any async sink exists already, so
      // it runs before that handler (atexit runs in reverse): it drains the async sinks itself
      // first, so the drop counts are final.
      std::atexit([] { write_at_exit(); });
    }
    return summary;
  }

 private:
  struct Installed {
    std::mutex mutex;  // the fields below
    std::shared_ptr<SummarySink> sink;
    std::filesystem::path path;
    unsigned fork_generation = 0;  // the process the summary belongs to
    bool handler_registered = false;
    std::atomic<bool> exit_handler_running{false};
  };
  // Never destroyed: the exit handler reads it after statics may be gone.
  static Installed& installed() {
    static Installed* const slot = new Installed;
    return *slot;
  }

  // The exit handler. It runs etch::shutdown(): the held-back counts are reported, every
  // async worker is closed and drained in turn (so work queued behind another async sink is
  // drained too, not merely flushed once), the counts made by that drain are reported through
  // the then-synchronous sinks, and only then are the watched files flushed and the final
  // object written. The async exit handler that follows finds nothing left to do. While this
  // runs, write_now() stands aside (it may be called on this very thread, by a held fatal the
  // report delivers); once it is done a later fatal (another exit handler's) updates the file
  // again. Nothing may throw out of an exit handler, so the whole of it is guarded.
  static void write_at_exit() noexcept {
    auto& s = installed();
    s.exit_handler_running.store(true, std::memory_order_seq_cst);
    try {
      std::lock_guard l(s.mutex);
      // The child of a fork inherits this handler and the path: the summary is the parent's
      // run, so the child leaves it alone (it installs its own if it wants one).
      if (s.sink != nullptr &&
          s.fork_generation == detail::fork_generation().load(std::memory_order_relaxed)) {
        shutdown();
        s.sink->flush_watched();
        if (const auto ec = s.sink->write_to(s.path, "exit")) report_failure(s.path, ec);
      }
    } catch (...) {
      report_failure(s.path, std::make_error_code(std::errc::not_enough_memory));
    }
    s.exit_handler_running.store(false, std::memory_order_seq_cst);
  }

  // Writes the installed summary's file at once, if this is it, holding the slot for the
  // whole write so that the exit handler, which takes the slot first, cannot be overtaken by
  // an older object. The async sinks are not drained first: that can wait for a worker, and
  // this may run on one (a fatal logged from a sink), so the drop counts are the counts so
  // far; the exit handler writes the final object. The slot is only tried, never waited for:
  // the exit handler holds it while it waits for the workers, and a worker blocking on it
  // here would be the deadlock; while the handler runs it stands aside anyway (the flag, read
  // before the try: the handler may be this very thread).
  void write_now(std::string_view written_at) noexcept {
    auto& slot = installed();
    // Whoever holds the slot is either the exit handler (then this stands aside: it writes
    // the final object, and may be this very thread), install() for an instant, or another
    // fatal's writer in its file I/O; waiting for the last two is bounded and never waits for
    // a worker, and it is what keeps a second fatal's counts out of an older object.
    for (;;) {
      // The flag first, every time: the handler may be this very thread, and trying a mutex
      // this thread already holds is not allowed, not even to be told no.
      if (slot.exit_handler_running.load(std::memory_order_seq_cst)) return;
      if (slot.mutex.try_lock()) break;
      std::this_thread::yield();
    }
    std::lock_guard lock(slot.mutex, std::adopt_lock);
    try {
      if (slot.sink.get() != this ||
          slot.fork_generation != detail::fork_generation().load(std::memory_order_relaxed)) {
        return;
      }
      if (const auto ec = write_to(slot.path, written_at)) report_failure(slot.path, ec);
    } catch (...) {
      // Out of memory building the object: nothing may leave a sink's write for this, and
      // the exit handler will try again.
      report_failure(slot.path, std::make_error_code(std::errc::not_enough_memory));
    }
  }

  // One writer of any summary file at a time, across every SummarySink: two summaries on one
  // path (a reinstall, a program's own write_to) must not interleave in the file.
  static std::mutex& summary_file_mutex() {
    static std::mutex* const mutex = new std::mutex;  // never destroyed: used at exit
    return *mutex;
  }

  // `created`, when given, says whether the file was opened by this call (so a failed
  // exclusive open leaves nothing to clean up). The close is checked too: a write that the
  // filesystem only refuses at close is a failure, not a published summary.
  static std::error_code write_file(const std::filesystem::path& path, const std::string& object,
                                    bool exclusive, bool* created) {
    errno = 0;
    auto mode = std::ios::out | std::ios::binary | std::ios::trunc;
    if (exclusive) mode |= std::ios::noreplace;  // a temporary is ours alone, or not opened
    std::ofstream out(path, mode);
    if (created != nullptr) *created = out.is_open();
    if (!out.is_open()) return std::error_code(errno != 0 ? errno : EIO, std::generic_category());
    out << object << '\n';
    out.close();
    return out.fail() ? std::make_error_code(std::errc::io_error) : std::error_code{};
  }

  // No logger to report to (the summary is a sink, and at exit none may be left): stderr is
  // the one place this can go, best effort, because nothing may throw out of an exit handler
  // or a sink's write.
  static void report_failure(const std::filesystem::path& path, std::error_code ec) noexcept {
    try {
      std::fprintf(stderr, "etch: could not write the run summary to %s: %s\n", utf8(path).c_str(),
                   ec.message().c_str());
    } catch (...) {
      // Even the message could not be built: nothing left to do.
    }
  }

  static void append_error(std::string_view key, const std::optional<ErrorSeen>& error,
                           std::string& out) {
    if (!error) return;
    out += key;
    out += "{\"ts\":\"";
    format_time_iso8601(out, error->time);
    out += "\",\"level\":";
    json_string(level_name(error->level), out);
    out += ",\"logger\":";
    json_string(error->logger, out);
    out += ",\"file\":";
    json_string(error->file, out);
    std::format_to(std::back_inserter(out), ",\"line\":{}", error->line);
    out += ",\"msg\":";
    json_string(error->message, out);
    out += '}';
  }

  std::chrono::system_clock::time_point started_;
  std::size_t total_ = 0;
  std::array<std::size_t, kLevelCount> by_level_{};
  std::map<std::string, std::size_t, std::less<>> by_logger_;
  std::optional<ErrorSeen> first_error_;
  std::optional<ErrorSeen> last_error_;
  std::vector<std::shared_ptr<const detail::SinkCounters>> watched_;
  std::vector<std::weak_ptr<FileSink>> watched_files_;
  mutable std::mutex mutex_;
};

}  // namespace etch
