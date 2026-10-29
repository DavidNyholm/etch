// etch - for logs that a program reads: the agent profile (compact JSON, nothing a person needs
// and a model pays for), one call that opens such a log, and SummarySink, which turns a whole
// run into one JSON object so a reader can decide whether to open the full log at all.
#pragma once

#include <array>
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
#include <string>
#include <string_view>
#include <system_error>
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

// Opens the file an agent will read: a JSON file sink in the agent profile behind an async
// sink that drops trace, debug and info rather than stall the caller, keeps warnings and
// above, and marks every gap. Returns the async sink; attach it to a logger.
[[nodiscard]] inline std::expected<std::shared_ptr<AsyncSink>, std::error_code> open_agent_log(
    const std::filesystem::path& path, AgentProfile profile = agent_profile()) {
  FileSinkOptions options = profile.file;
  options.style = profile.style;  // the profile's style wins, so an edit to it is not lost
  auto file = FileSink::open(path, std::move(options));
  if (!file) return std::unexpected(file.error());
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
// time; install() does all of that for the default logger and writes the file at exit.
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

  // The whole run as one JSON object (one line, no trailing newline).
  std::string json() const {
    std::lock_guard lock(mutex_);
    std::string out = "{\"etch_summary\":1";
    auto app = std::back_inserter(out);
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
    std::format_to(app, ",\"dropped\":{},\"write_failures\":{}", dropped, failures);
    append_error(",\"first_error\":", first_error_, out);
    append_error(",\"last_error\":", last_error_, out);
    out += '}';
    return out;
  }

  // Writes the object and a newline to the file (replacing it), making missing parent
  // directories like FileSink does. Returns the OS error if any.
  std::error_code write_to(const std::filesystem::path& path) const {
    if (path.has_parent_path()) {
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);  // the open reports failure
    }
    errno = 0;
    std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return std::error_code(errno != 0 ? errno : EIO, std::generic_category());
    out << json() << '\n';
    out.flush();
    return out ? std::error_code{} : std::make_error_code(std::errc::io_error);
  }

  // Attaches a summary sink to the default logger (and so to every etch::get() logger),
  // watches the given sinks, and writes the summary to `path` when the process exits, after
  // the async sinks have been drained. The sink lives until exit; it is also returned so the
  // program can ask it questions or write the summary earlier.
  static std::shared_ptr<SummarySink> install(std::filesystem::path path,
                                              std::vector<std::shared_ptr<Sink>> watched = {}) {
    auto summary = std::make_shared<SummarySink>();
    for (const auto& sink : watched) {
      if (auto async = std::dynamic_pointer_cast<AsyncSink>(sink)) summary->watch(async);
      if (auto file = std::dynamic_pointer_cast<FileSink>(sink)) summary->watch(file);
    }
    default_logger().add_sink(summary);
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
      std::atexit([] {
        auto& s = installed();
        std::lock_guard l(s.mutex);
        if (s.sink == nullptr) return;
        // The child of a fork inherits this handler and the path: the summary is the parent's
        // run, so the child leaves it alone (it installs its own if it wants one).
        if (s.fork_generation != detail::fork_generation().load(std::memory_order_relaxed)) {
          return;
        }
        // What the rate gates still hold back goes to the sinks first, so the summary counts it.
        try {
          detail::report_pending_rates();
        } catch (...) {
          // Nothing may throw out of an exit handler: a sink that threw loses those lines only.
        }
        flush_all();
        s.sink->flush_watched();
        if (const auto ec = s.sink->write_to(s.path)) {
          // No logger left to report to: stderr is the one place a failure at exit can go,
          // best effort, because nothing may throw out of an exit handler.
          try {
            std::fprintf(stderr, "etch: could not write the run summary to %s: %s\n",
                         utf8(s.path).c_str(), ec.message().c_str());
          } catch (...) {
            // Even the message could not be built: nothing left to do.
          }
        }
      });
    }
    return summary;
  }

 private:
  struct Installed {
    std::mutex mutex;
    std::shared_ptr<SummarySink> sink;
    std::filesystem::path path;
    unsigned fork_generation = 0;  // the process the summary belongs to
    bool handler_registered = false;
  };
  // Never destroyed: the exit handler reads it after statics may be gone.
  static Installed& installed() {
    static Installed* const slot = new Installed;
    return *slot;
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
