// etch - delivery: handing a record to a list of sinks, and what happens when a sink's write
// logs something itself. A record logged while a sink is writing on this thread is set aside
// and delivered once the outermost delivery on this thread has returned and no sink mutex is
// held; the sinks that were writing at the time, and the ones the record it was logged under
// was already kept from, never get it and count the skip.
#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "record.hpp"
#include "sinks.hpp"

namespace etch {

namespace detail {

// Delivering a nested record at once would re-enter the writing sink's own mutex, or, with two
// such sinks on two threads each waiting for the other, deadlock across threads. Deferring it
// avoids both. Excluding the writing sinks, and handing those exclusions down to whatever the
// nested record's delivery logs in turn (its lineage, which an async queue carries along with
// the record), is what makes any chain of sinks feeding each other end: each step adds the
// sinks it passed through, and a sink is never reached twice by the same chain.

using Lineage = std::vector<const Sink*>;

// The chain of sinks whose write is in progress on this thread, innermost first: an intrusive
// stack of scopes on the callers' own stacks, so there is no depth limit and nothing to free.
class WriteScope {
 public:
  explicit WriteScope(const Sink* sink) noexcept : sink_(sink), previous_(head()) { head() = this; }
  ~WriteScope() { head() = previous_; }
  WriteScope(const WriteScope&) = delete;
  WriteScope& operator=(const WriteScope&) = delete;

  static bool any() noexcept { return head() != nullptr; }
  static void collect(Lineage& out) {
    for (const WriteScope* scope = head(); scope != nullptr; scope = scope->previous_) {
      out.push_back(scope->sink_);
    }
  }

 private:
  static WriteScope*& head() noexcept {
    static constinit thread_local WriteScope* head = nullptr;
    return head;
  }
  const Sink* sink_;
  WriteScope* previous_;
};

// A record waiting for the sink it was logged from to finish writing. Its exclusions are its
// lineage (Record::lineage).
struct DeferredRecord {
  std::shared_ptr<const SinkList> sinks;
  Record record;
};
// The thread's deferred records. `pending` is heap-held behind this trivial thread_local (like
// the context stack) and freed when the thread ends; after that, `fallback` is storage the
// outermost delivery scope lends from its own stack frame, so a log call made by a thread-local
// destructor is still set aside rather than delivered into a sink that is writing. `ancestry`
// is the lineage of the record being delivered right now, inherited by anything logged during
// that delivery.
struct DeferredSlot {
  std::vector<DeferredRecord>* pending = nullptr;
  std::vector<DeferredRecord>* fallback = nullptr;
  const Lineage* ancestry = nullptr;
  bool draining = false;
  bool ended = false;
};
inline DeferredSlot& deferred_slot() noexcept {
  static constinit thread_local DeferredSlot slot{};
  return slot;
}
struct DeferredReaper {
  ~DeferredReaper() {
    DeferredSlot& slot = deferred_slot();
    delete std::exchange(slot.pending, nullptr);
    slot.ended = true;
  }
};
inline std::vector<DeferredRecord>* deferred_records() {
  DeferredSlot& slot = deferred_slot();
  if (slot.pending == nullptr && !slot.ended) {
    static thread_local DeferredReaper reaper;  // registered now, runs when the thread ends
    (void)reaper;
    slot.pending = new std::vector<DeferredRecord>;
  }
  return slot.pending != nullptr ? slot.pending : slot.fallback;
}

// The exclusions a record logged right now must carry: the ancestry of the record being
// delivered, plus the sinks writing on this thread.
inline std::shared_ptr<const Lineage> current_lineage() {
  auto lineage = std::make_shared<Lineage>();
  if (const Lineage* ancestry = deferred_slot().ancestry) *lineage = *ancestry;
  WriteScope::collect(*lineage);
  return lineage;
}

// Sets a record aside for delivery once the sinks writing on this thread have returned. False
// only when nothing can hold it (no delivery scope lends storage on this thread, which cannot
// happen while a sink is writing); the caller then delivers at once.
inline bool defer(std::shared_ptr<const SinkList> sinks, Record record) {
  std::vector<DeferredRecord>* const pending = deferred_records();
  if (pending == nullptr) return false;
  record.lineage = current_lineage();
  pending->push_back(DeferredRecord{std::move(sinks), std::move(record)});
  return true;
}

// Whether the record must not reach this sink: it was logged from inside its write, at some
// step of its chain. Checked by every delivery and by every forwarding (an async sink into its
// inner sink), since the rule holds wherever a record moves. Sinks are told apart by address;
// a sink destroyed while a record naming it is still queued, and a new sink at the same
// address, would be kept from that record too (false suppression, nothing worse).
inline bool kept_from(const Record& r, const Sink* sink) noexcept {
  return r.lineage != nullptr &&
         std::find(r.lineage->begin(), r.lineage->end(), sink) != r.lineage->end();
}

// While a record is being delivered, whatever is logged inherits its lineage.
class AncestryScope {
 public:
  // Keeps the installed set alive itself: a sink may destroy the record it was given before it
  // logs in response.
  explicit AncestryScope(const Record& record) noexcept
      : previous_(deferred_slot().ancestry),
        installed_(record.ancestry != nullptr ? record.ancestry : record.lineage) {
    if (installed_ != nullptr) deferred_slot().ancestry = installed_.get();
  }
  ~AncestryScope() { deferred_slot().ancestry = previous_; }
  AncestryScope(const AncestryScope&) = delete;
  AncestryScope& operator=(const AncestryScope&) = delete;

 private:
  const Lineage* previous_;
  std::shared_ptr<const Lineage> installed_;
};

inline void deliver_to(const SinkList& sinks, Record r);

// Delivers what was set aside, in order. A record delivered here can itself set more aside
// (its sinks write too); the loop picks those up, and the nested delivery scopes do not drain
// again meanwhile. A sink that throws on one of these records loses it: the log call it came
// from has returned, so there is nobody to throw to.
inline void drain_deferred() noexcept {
  DeferredSlot& slot = deferred_slot();
  std::vector<DeferredRecord>* const pending =
      slot.pending != nullptr ? slot.pending : slot.fallback;
  if (pending == nullptr || slot.draining) return;
  slot.draining = true;
  // A cursor, not erase-from-the-front: a sink that logs thousands of nested records would
  // otherwise pay for moving the rest down on every one. Deliveries can append meanwhile.
  for (std::size_t next = 0; next < pending->size(); ++next) {
    DeferredRecord record = std::move((*pending)[next]);
    try {
      const AncestryScope ancestry(record.record);
      deliver_to(*record.sinks, std::move(record.record));
    } catch (...) {
      // Nowhere to report: the call that logged it is gone.
    }
  }
  pending->clear();
  slot.draining = false;
}

// Counts the deliveries in progress on this thread (a logger handing a record to its sinks,
// an async worker handing one to its inner sink) and drains the deferred records when the
// outermost one ends: after it, no sink mutex is held on this thread. The outermost scope
// also lends its own storage for records set aside after the thread's heap storage is gone.
class DeliveryScope {
 public:
  // The thread-local slot points at this object's own vector for exactly the object's life
  // (the destructor clears it before returning), which is what GCC's -Wdangling-pointer
  // cannot see and warns about in optimised builds.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
#endif
  DeliveryScope() noexcept {
    if (depth()++ == 0) deferred_slot().fallback = &local_;
  }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
  // The outermost scope drains while it still counts as in progress, so a delivery the drain
  // makes nests inside it rather than starting a new outermost scope of its own (which would
  // lend its own, shorter-lived storage and lose what was set aside into it).
  ~DeliveryScope() {
    if (depth() == 1) {
      drain_deferred();
      deferred_slot().fallback = nullptr;
    }
    --depth();
  }
  DeliveryScope(const DeliveryScope&) = delete;
  DeliveryScope& operator=(const DeliveryScope&) = delete;

 private:
  static std::size_t& depth() noexcept {
    static constinit thread_local std::size_t depth = 0;
    return depth;
  }
  std::vector<DeferredRecord> local_;  // used only once the thread's own storage is gone
};

// Hands a record to every sink in the list that accepts it, each marked as writing meanwhile.
// A sink in the record's lineage (one the record was logged from inside of, at some step) is
// skipped and counts the skip. Every sink but the last gets a copy; the last takes the record
// itself, so the common single-sink setup never copies the message. A fatal flushes every
// sink afterwards.
inline void deliver_to(const SinkList& sinks, Record r) {
  const DeliveryScope delivery;
  const Level level = r.level;
  const std::shared_ptr<const Lineage> lineage = r.lineage;
  const auto is_excluded = [&](const Sink* sink) {
    return lineage != nullptr &&
           std::find(lineage->begin(), lineage->end(), sink) != lineage->end();
  };
  Sink* last = nullptr;
  for (const auto& sink : sinks) {
    if (!sink->accepts(level)) continue;
    if (is_excluded(sink.get())) {
      sink->count_reentry();
      continue;
    }
    if (last != nullptr) {
      const WriteScope writing(last);
      last->write(r);
    }
    last = sink.get();
  }
  if (last != nullptr) {
    const WriteScope writing(last);
    last->write(std::move(r));
  }
  if (level == Level::kFatal) {
    for (const auto& sink : sinks) {
      if (!is_excluded(sink.get())) sink->flush();
    }
  }
}

}  // namespace detail

}  // namespace etch
