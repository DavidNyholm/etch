// etch - for logs that a program reads: the agent profile (compact JSON, nothing a person needs
// and a model pays for), one call that opens such a log, and SummarySink, which turns a whole
// run into one JSON object so a reader can decide whether to open the full log at all.
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
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

#include "async_sink.hpp"
#include "default_logger.hpp"
#include "json.hpp"
#include "level.hpp"
#include "logger.hpp"
#include "record.hpp"
#include "sinks.hpp"

namespace etch {

// The settings for a log a program will read: JSON Lines with epoch-millisecond timestamps and
// without the thread, pid, path and function fields; begin lines left off scope timers; small
// hexdumps; the drop policy with warnings and above kept. Same code as every other log, only
// the defaults differ, so a human-readable console and an agent file can share one logger.
struct AgentProfile {
  LineStyle style{.format = LineFormat::kJson,
                  .json = JsonStyle{.time = JsonStyle::Time::kEpochMillis,
                                    .thread = false,
                                    .pid = false,
                                    .path = false,
                                    .function = false}};
  FileSinkOptions file{.flush_on = Level::kWarn, .style = style};
  ScopeOptions scope{.level = Level::kDebug, .begin = false};
  std::size_t hexdump_limit = 256;  // the first 16 lines; a model rarely needs more
  AsyncSink::Overflow overflow = AsyncSink::Overflow::kDrop;
  Level keep_from = Level::kWarn;
  std::size_t queue_capacity = AsyncSink::kDefaultCapacity;
};

inline AgentProfile agent_profile() { return AgentProfile{}; }

// Opens the file an agent will read: a JSON file sink in the agent profile behind an async
// sink that drops trace, debug and info rather than stall the caller, keeps warnings and
// above, and marks every gap. Returns the async sink; attach it to a logger.
[[nodiscard]] inline std::expected<std::shared_ptr<AsyncSink>, std::error_code> open_agent_log(
    const std::filesystem::path& path, AgentProfile profile = agent_profile()) {
  auto file = FileSink::open(path, profile.file);
  if (!file) return std::unexpected(file.error());
  return std::make_shared<AsyncSink>(std::move(*file), profile.queue_capacity, profile.overflow,
                                     profile.keep_from);
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

  // Sinks whose own counters belong in the summary. Held weakly: a sink that is gone by the
  // time the summary is written simply contributes nothing.
  void watch(const std::shared_ptr<AsyncSink>& sink) {
    std::lock_guard lock(mutex_);
    async_sinks_.push_back(sink);
  }
  void watch(const std::shared_ptr<FileSink>& sink) {
    std::lock_guard lock(mutex_);
    file_sinks_.push_back(sink);
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
    for (const auto& weak : async_sinks_) {
      if (const auto sink = weak.lock()) {
        dropped += sink->dropped();
        failures += sink->write_failures();
      }
    }
    for (const auto& weak : file_sinks_) {
      if (const auto sink = weak.lock()) failures += sink->write_failures();
    }
    std::format_to(app, ",\"dropped\":{},\"write_failures\":{}", dropped, failures);
    append_error(",\"first_error\":", first_error_, out);
    append_error(",\"last_error\":", last_error_, out);
    out += '}';
    return out;
  }

  // Writes the object and a newline to the file (replacing it). Returns the OS error if any.
  std::error_code write_to(const std::filesystem::path& path) const {
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
    if (!slot.handler_registered) {
      slot.handler_registered = true;
      // Registered after the async exit handler would be if any async sink exists already, so
      // it runs before that handler (atexit runs in reverse): it drains the async sinks itself
      // first, so the drop counts are final.
      std::atexit([] {
        auto& s = installed();
        std::lock_guard l(s.mutex);
        if (s.sink == nullptr) return;
        flush_all();
        (void)s.sink->write_to(s.path);  // nowhere left to report a failure at exit
      });
    }
    return summary;
  }

 private:
  struct Installed {
    std::mutex mutex;
    std::shared_ptr<SummarySink> sink;
    std::filesystem::path path;
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
  std::vector<std::weak_ptr<AsyncSink>> async_sinks_;
  std::vector<std::weak_ptr<FileSink>> file_sinks_;
  mutable std::mutex mutex_;
};

}  // namespace etch
