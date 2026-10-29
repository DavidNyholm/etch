// etch - the process-wide calls on the async sinks: flush_all() before anything that must see
// the whole log, shutdown() at the end of main (also run from an exit handler), prepare_fork()
// right before a fork().
#pragma once

#include <atomic>
#include <cstdint>

#include "async_registry.hpp"
#include "async_sink.hpp"

namespace etch {

// Waits until every live async sink has written everything queued so far, then flushes their
// inner sinks. Call it before anything that must see the complete log (an exception handler
// in normal context, a test's assertion, a hand-off to another process). Not from inside a
// sink, and not from a signal handler: it locks, allocates and waits.
inline void flush_all() {
  AsyncSink::for_each_live([](AsyncSink& sink) { sink.flush(); });
}

// Drains and stops every async worker and marks the process as exiting, after which every
// async sink writes straight through and file sinks flush every line. Call it once at the end
// of main; it also runs automatically from an exit handler. Not from inside a sink.
inline void shutdown() { AsyncSink::shutdown_all(); }

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
