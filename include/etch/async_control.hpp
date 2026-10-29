// etch - the process-wide calls on the async sinks: flush_all() before anything that must see
// the whole log, shutdown() at the end of main (also run from an exit handler), prepare_fork()
// right before a fork().
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include "async_registry.hpp"
#include "async_sink.hpp"
#include "rate_limit.hpp"
#include "record.hpp"

namespace etch {

// Waits until every live async sink has written everything queued so far, then flushes their
// inner sinks. Call it before anything that must see the complete log (an exception handler
// in normal context, a test's assertion, a hand-off to another process). Not from inside a
// sink, and not from a signal handler: it locks, allocates and waits.
inline void flush_all() {
  AsyncSink::for_each_live([](AsyncSink& sink) { sink.flush(); });
}

namespace detail {

inline void report_pending_rates_at_shutdown() noexcept {
  try {
    report_pending_rates();
  } catch (...) {
    // A sink threw on one of those lines (or memory ran out building them): at shutdown
    // there is nothing left to tell; the other lines were still attempted.
  }
}

// `at_exit`: called from an exit handler. Exit handlers report the held-back counts until the
// run summary has written its final object (its own handler sets summary_finalized after the
// write, and always reports before it, whatever order the handlers run in); after that a
// report would be a line the file cannot show, so none is made. etch::shutdown() called by
// the program reports every time.
//
// One limit, documented rather than engineered around: a std::exit() on another thread while
// the program's own shutdown() is still delivering a report (a sink of the report's that
// blocks) can write the summary before that report reaches the summary sink. Serialising the
// two would turn such a sink into a deadlock at exit instead. Call shutdown() at the end of
// main, on the thread that exits.
inline void shutdown_all(bool at_exit) noexcept {
  const bool report = !at_exit || !summary_finalized().load(std::memory_order_acquire);
  if (report) report_pending_rates_at_shutdown();
  process_exiting().store(true, std::memory_order_seq_cst);  // pairs with the first sink's look
  // One entry at a time: found under the registry lock, shut down outside it, so no
  // allocation is needed (nothing may throw here) and no sink is joined while the lock is
  // held (a callback of the sink being drained may destroy another sink, whose destructor
  // takes the lock). The entry's own lock is held across its shutdown, so two threads here
  // at once (the program's shutdown() racing an exit) take turns on each entry, and the
  // second finds it done rather than skipping a drain the first has only begun: a sink is
  // marked done by the thread that shut it down, under that lock, never by a look. A sink
  // registered while this runs is found on a later turn. With no registry at all no sink
  // was ever made, and the registry is not made here (that would allocate).
  for (;;) {
    std::shared_ptr<AsyncEntry> entry;
    {
      AsyncRegistry* const registry = async_registry_if_made();
      if (registry == nullptr) break;
      std::lock_guard lock(registry->mutex);
      for (const auto& candidate : registry->live) {
        if (!candidate->shutdown_done.load(std::memory_order_acquire)) {
          entry = candidate;
          break;
        }
      }
    }
    if (entry == nullptr) break;
    std::lock_guard entry_lock(entry->mutex);
    if (!entry->shutdown_done.load(std::memory_order_acquire)) {
      if (AsyncSink* sink = entry->sink.load(std::memory_order_relaxed)) sink->shutdown();
      entry->shutdown_done.store(true, std::memory_order_release);
    }
  }
  if (report) report_pending_rates_at_shutdown();
}

}  // namespace detail

// Drains and stops every async worker and marks the process as exiting, after which every
// async sink writes straight through and file sinks flush every line. Call it once at the end
// of main; it also runs automatically from an exit handler. Not from inside a sink.
inline void shutdown() { detail::shutdown_all(/*at_exit=*/false); }

// For a program that forks after setting up its logging: waits until every async worker has
// written everything, flushed its sink and gone to sleep, so that the fork copies no lock of
// etch's in a held state (a worker's idle flush can hold the file sink's mutex a moment after
// flush_all() returns). Call it right before fork(), with no other thread logging; the child
// then logs through the inherited sinks synchronously (see AsyncSink).
inline void prepare_fork() {
  detail::install_fork_handler();  // so the child knows itself even with no sink made yet
  // One sink going idle can wake another (an async sink behind an async sink gets the
  // forwarded records, then does its own idle flush), so one sink's idleness says nothing
  // about the others' a moment later. A pass counts only when every sink was already idle
  // and no worker did anything at all while the pass ran; with no producer logging, the
  // chain settles and such a pass comes.
  for (;;) {
    const std::uint64_t before = detail::worker_activity().load(std::memory_order_seq_cst);
    bool waited = false;
    AsyncSink::for_each_live([&](AsyncSink& sink) { waited = sink.wait_until_idle() || waited; });
    if (!waited && detail::worker_activity().load(std::memory_order_seq_cst) == before) return;
  }
}

}  // namespace etch
