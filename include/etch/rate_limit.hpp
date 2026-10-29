// etch - the rate gate behind every() and once(): one state per call site, the count of calls
// held back since the last one that got through, and the report of what is still held back
// when the process shuts down, so a site that stopped logging does not take its count with it.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "delivery.hpp"
#include "level.hpp"
#include "record.hpp"
#include "sink_set.hpp"
#include "sinks.hpp"

namespace etch::detail {

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

// What admit_rate() decided: through, held back, or held back as the first call since the
// last one that got through (the caller then records what was held, see note_pending_rate).
enum class Admission { kAdmitted, kHeld, kHeldFirst };

// The first call held back since the site last got through, kept so that at shutdown the
// count still pending can be written as one line in that call's shape: its level, logger,
// message and fields, to the sinks its logger has by then. The sink set is held weakly: a
// logger that is gone by then takes its pending count with it (its sinks may be gone too),
// and holding it strongly would keep every sink of every rate-limited logger alive.
struct PendingRate {
  Level level = Level::kInfo;
  std::string logger;
  std::string message;
  Fields fields;
  std::source_location where;
  std::thread::id thread;
  std::uint32_t thread_index = 0;
  std::weak_ptr<SinkSet> sinks;
};

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
  std::optional<PendingRate> pending;  // the first call held since `last`, once noted
};
struct RateRegistry {
  std::mutex mutex;
  std::map<RateKey, RateState> sites;
};
inline RateRegistry& rate_registry() {
  static RateRegistry* const registry = new RateRegistry;
  return *registry;
}

inline RateKey rate_key(const std::source_location& where) noexcept {
  return RateKey{where.file_name(), where.line(), where.column()};
}

// Whether the call at `where` may go through: the first one always, then one per `interval`
// (once() is an interval that never elapses). `suppressed` receives how many calls were held
// back since the last one that went through, so the line that gets through can say so.
inline Admission admit_rate(const std::source_location& where, std::chrono::nanoseconds interval,
                            bool once, std::uint64_t& suppressed) {
  RateRegistry& registry = rate_registry();
  std::lock_guard lock(registry.mutex);
  RateState& state = registry.sites[rate_key(where)];
  const auto now = std::chrono::steady_clock::now();
  if (state.fired && (once || now - state.last < interval)) {
    ++state.suppressed;
    return state.suppressed == 1 ? Admission::kHeldFirst : Admission::kHeld;
  }
  state.fired = true;
  state.last = now;
  state.pending.reset();
  suppressed = std::exchange(state.suppressed, 0);
  return Admission::kAdmitted;
}

// Records the shape of the first held call (Admission::kHeldFirst), outside the registry lock
// that admit_rate() took, so the message is formatted without it. The site may have got
// through meanwhile on another thread; then there is nothing pending and this is dropped.
inline void note_pending_rate(PendingRate pending) {
  RateRegistry& registry = rate_registry();
  std::lock_guard lock(registry.mutex);
  RateState& state = registry.sites[rate_key(pending.where)];
  if (state.suppressed == 0 || state.pending.has_value()) return;
  state.pending = std::move(pending);
}

// Writes one line per site that still holds a count back: the first held call's level, logger,
// message and fields, with the count as the `suppressed` field, to the sinks its logger has
// now; the count then starts again at zero. etch::shutdown() and the run summary's exit handler
// call it, so "nothing held back is lost silently" holds at the end of a run as well as while
// the site keeps logging. The lines are built under the registry lock and delivered outside
// it, since a sink may itself log through a rate gate. Not noexcept: a sink's exception
// propagates, as it does from any log call.
inline void report_pending_rates() {
  std::vector<std::pair<Record, std::shared_ptr<const SinkList>>> due;
  {
    RateRegistry& registry = rate_registry();
    std::lock_guard lock(registry.mutex);
    for (auto& [key, state] : registry.sites) {
      if (state.suppressed == 0 || !state.pending) continue;
      const std::shared_ptr<SinkSet> sinks = state.pending->sinks.lock();
      if (sinks != nullptr) {
        Record r;
        r.level = state.pending->level;
        r.time = std::chrono::system_clock::now();
        r.thread = state.pending->thread;
        r.thread_index = state.pending->thread_index;
        r.where = state.pending->where;
        r.logger_name = std::move(state.pending->logger);
        r.message = std::move(state.pending->message);
        r.fields = std::move(state.pending->fields);
        std::erase_if(r.fields, [](const Field& f) { return f.key == kSuppressedField; });
        r.fields.emplace_back(kSuppressedField, state.suppressed);
        due.emplace_back(std::move(r), sinks->snapshot());
      }
      state.suppressed = 0;
      state.pending.reset();
    }
  }
  for (auto& [r, sinks] : due) deliver_to(*sinks, std::move(r));
}

}  // namespace etch::detail
