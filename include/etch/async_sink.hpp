// etch - AsyncSink: a lock-free bounded queue and a worker thread in front of any other sink,
// plus the process-wide flush_all() and shutdown() that drain every live async sink.
#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <source_location>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

#include "async_registry.hpp"
#include "delivery.hpp"
#include "sinks.hpp"

namespace etch {

#if defined(_MSC_VER)
// C4324: "structure was padded due to alignment specifier". The padding is the point: the hot
// counters below are placed on separate cache lines so producers and the worker do not false-share.
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

// Moves the cost of writing (disk, console) off the logging thread. The record is already
// formatted when it arrives, so the queue only carries finished strings.
//
// The queue is a bounded ring of records with a sequence number per slot (Dmitry Vyukov's
// design). A producer claims a slot with one compare-and-swap and moves its record in; it never
// takes a lock and never allocates, so the calling thread pays a few atomic operations whether
// one thread logs or eight do. The worker takes everything that is ready, writes it, and only
// then looks again.
//
// The mutex exists for the slow paths only: putting the worker to sleep when the queue is empty,
// blocking a producer when it is full, and waiting in flush(). The sleep handshake is the usual
// one: the worker announces it is going to sleep, then re-checks the queue; a producer enqueues,
// then checks the announcement. Both sides use sequentially consistent operations, so at least
// one of them sees the other's write and no wake-up is lost.
//
// When the queue is full the overflow policy decides. kBlock waits for room: nothing is ever
// lost. kDrop drops the record and counts it, except at or above keep_from (warn by default),
// where it blocks instead: a game loop is never stalled by trace lines, and an error is never
// thrown away. Whenever records were dropped, the worker writes one warning from logger "etch"
// saying how many, before the next real record, when it goes idle, and at shutdown, so the file
// shows where its gaps are and flush() never returns with a gap unreported.
//
// flush() returns once every record enqueued before the call is written, however much is logged
// meanwhile. When the queue goes idle the worker flushes the inner sink, so a crash a little
// later loses at most the burst in flight. shutdown() closes the queue to new records (a
// producer that arrives afterwards writes straight through, synchronously), waits for the
// producers already inside, drains the ring, stops the worker and flushes; the destructor does
// the same. An exit handler shuts every live async sink down, so a std::exit loses nothing that
// was queued. An exception from the inner sink is caught, counted in write_failures() and costs
// that one record; the worker carries on.
//
// Rules for sinks behind this one: whatever they call must not drop the last reference to this
// AsyncSink (the destructor would then run on the worker thread and try to join itself), and
// must not call this sink's flush() or shutdown(). shutdown() called from the worker thread
// itself only closes the queue and asks the worker to stop; it cannot wait for itself, and
// records arriving during that drain are not kept in order with the ring's contents. A record
// logged on the worker thread itself (by the inner sink, through a logger that feeds this
// sink) is written straight through rather than queued, since the worker cannot wait for a
// slot it alone frees.
//
// A sink copied into the child of a fork() has no worker (only the forking thread survives),
// and its locks, condition variables and thread object are copies of the parent's in whatever
// state the fork found them. In the child such a sink writes every record straight through to
// the inner sink, flush() flushes the inner sink, shutdown() waits for nothing, and the
// destructor lets the copied worker, locks and condition variables go rather than destroy them
// (joining or destroying a copied thread or condition variable is undefined, and hangs on real
// systems). What the ring held at the fork is the parent's to write. A mutex the parent held
// at the instant of the fork is copied held, so call etch::prepare_fork() right before the
// fork with no other thread using etch: it returns once every worker is asleep holding
// nothing. A sink made after the fork is an ordinary sink with a worker of its own.
class AsyncSink final : public Sink {
 public:
  enum class Overflow { kBlock, kDrop };
  // Slots are allocated up front (a Record plus a sequence number, about 200 bytes on MSVC x64,
  // so the default ring is under a megabyte); hence a modest default.
  static constexpr std::size_t kDefaultCapacity = 4096;
  // Upper bound for the ring: std::bit_ceil must have a representable result, and 2^30 slots
  // is already 128 GB of slot storage.
  static constexpr std::size_t kMaxCapacity = std::size_t{1} << 30;
  // The worker takes at most this many records per sweep. Producers refill slots as fast as
  // the worker frees them, so an unbounded sweep could collect forever and never write. The
  // bound also sizes the batch storage once, so the worker never allocates while running.
  static constexpr std::size_t kMaxBatch = 1024;
  // Before sleeping, the worker yields this many times while the queue stays empty. A burst of
  // messages then never pays a wake-up per message; an idle application still gets a sleeping
  // worker within a few microseconds.
  static constexpr int kYieldsBeforeSleep = 64;

  explicit AsyncSink(std::shared_ptr<Sink> inner, std::size_t capacity = kDefaultCapacity,
                     Overflow policy = Overflow::kBlock, Level keep_from = Level::kWarn)
      : inner_(std::move(inner)),
        mask_(std::bit_ceil(std::clamp(capacity, std::size_t{2}, kMaxCapacity)) - 1),
        slots_(inner_ == nullptr ? nullptr : std::make_unique<Slot[]>(mask_ + 1)),
        policy_(policy),
        keep_from_(keep_from),
        counters_(std::make_shared<detail::SinkCounters>()),
        sync_(std::make_unique<Sync>()),
        generation_(detail::fork_generation().load(std::memory_order_relaxed)) {
    if (inner_ == nullptr) throw std::invalid_argument("etch::AsyncSink: null inner sink");
    for (std::size_t i = 0; i <= mask_; ++i) {
      slots_[i].sequence.store(i, std::memory_order_relaxed);
    }
    try {
      register_self();  // also starts the worker, or decides there will be none
    } catch (...) {
      shutdown();
      throw;
    }
  }

  ~AsyncSink() override {
    if (inherited()) {
      abandon_in_child();
      return;
    }
    shutdown();
    unregister_self();
  }

  AsyncSink(const AsyncSink&) = delete;
  AsyncSink& operator=(const AsyncSink&) = delete;

  // The inner sink's level applies as if it were used directly; filtered records never enter
  // the queue. A record kept from this sink by its lineage (handed over by another async sink
  // in front of this one, whose own delivery could not know) is skipped and counted here.
  void write(const Record& r) override {
    if (!inner_->accepts(r.level)) return;
    if (detail::kept_from(r, this)) {
      count_reentry();
      return;
    }
    if (inherited()) {
      write_inner(Record(r));
      return;
    }
    enqueue(Record(r));
  }
  void write(Record&& r) override {
    if (!inner_->accepts(r.level)) return;
    if (detail::kept_from(r, this)) {
      count_reentry();
      return;
    }
    if (inherited()) {
      write_inner(std::move(r));
      return;
    }
    enqueue(std::move(r));
  }

  // Waits until everything enqueued before the call is written and every drop so far is
  // reported, then flushes the inner sink. During a shutdown it waits for the worker to finish
  // instead: closing admission is not delivery.
  void flush() override {
    if (inherited()) {
      flush_inner();  // no worker to wait for; everything the child logged went through
      return;
    }
    // A worker never waits for a worker (see on_async_worker): two workers each waiting for
    // the other's queue to drain would wait forever. A fatal that an inner sink logged into
    // this sink asks this sink's worker to flush once everything queued so far is written,
    // and returns; with no worker (after shutdown) it flushes the inner sink itself.
    if (detail::on_async_worker()) {
      if (closed_.load(std::memory_order_seq_cst)) {
        flush_inner();
        return;
      }
      // The request is a position, never a flag the worker could clear under a newer request:
      // outstanding while the served position is behind it. With nothing queued ahead (the
      // record was written straight through) there is no position to wait for, and the
      // inner sink is flushed here: that waits for a write at most, never for a worker.
      const std::size_t up_to = enqueue_pos_.load(std::memory_order_seq_cst);
      if (up_to == written_pos_.load(std::memory_order_seq_cst) && empty()) {
        flush_inner();
        return;
      }
      std::size_t previous = flush_requested_pos_.load(std::memory_order_relaxed);
      while (previous < up_to && !flush_requested_pos_.compare_exchange_weak(
                                     previous, up_to, std::memory_order_seq_cst)) {
      }
      wake_worker();
      return;
    }
    const std::size_t target = enqueue_pos_.load(std::memory_order_seq_cst);
    const std::size_t drops = dropped_now();
    {
      std::unique_lock lock(sync_->mutex);
      if (drops != dropped_reported_.load(std::memory_order_acquire)) {
        sync_->not_empty.notify_one();
      }
      sync_->drained.wait(lock, [&] {
        return worker_finished_.load(std::memory_order_acquire) ||
               (written_pos_.load(std::memory_order_acquire) >= target &&
                dropped_reported_.load(std::memory_order_acquire) >= drops);
      });
    }
    flush_inner();
  }

  // Closes the queue, waits for the producers already inside, drains the ring, stops the worker
  // and flushes. A record logged while that happens waits its turn and is written after the
  // ring's contents, by the thread that logged it; after it, write() goes straight to the inner
  // sink on the calling thread, so the sink keeps working, synchronously, in order. Safe to call
  // more than once and from several threads.
  void shutdown() {
    closed_.store(true, std::memory_order_seq_cst);
    if (inherited()) {
      // Nothing of the parent's is touched: no lock (one may have been copied held), no join.
      worker_gone_.store(true, std::memory_order_seq_cst);
      worker_finished_.store(true, std::memory_order_seq_cst);
      flush_inner();
      return;
    }
    {
      std::lock_guard lock(sync_->mutex);
      sync_->not_full.notify_all();  // blocked producers re-check closed_ and write through
    }
    if (worker_ != nullptr && worker_id_ == std::this_thread::get_id()) {
      worker_->request_stop();  // a callback on the worker: it cannot wait for itself
      return;
    }
    std::lock_guard shutdown_lock(sync_->shutdown_mutex);
    if (sync_->shut_down) return;
    sync_->shut_down = true;
    // Every producer that saw the queue open finishes its enqueue before the worker drains.
    // (Producer: active +1, then read closed_. Here: write closed_, then read active. Both
    // sequentially consistent, so a producer we do not wait for is one that saw closed_.)
    while (active_producers_.load(std::memory_order_seq_cst) > 0) std::this_thread::yield();
    if (worker_ != nullptr) {
      worker_->request_stop();
      if (worker_->joinable()) worker_->join();
    }
    {
      std::lock_guard lock(sync_->mutex);
      written_pos_.store(enqueue_pos_.load(std::memory_order_acquire), std::memory_order_release);
      sync_->drained.notify_all();  // a flush() that was waiting on the worker
    }
    flush_inner();
  }

  // Returns once the ring is empty, every drop is reported and the worker is asleep holding
  // nothing (or has finished): what etch::prepare_fork() needs. True when it had to wait for
  // that, false when it was already so. With other threads still logging it may not return.
  bool wait_until_idle() {
    if (inherited()) return false;
    const auto idle = [&] {
      return worker_finished_.load(std::memory_order_acquire) ||
             (worker_sleeping_.load(std::memory_order_seq_cst) && empty() && !gap_unreported() &&
              !flush_outstanding());
    };
    std::unique_lock lock(sync_->mutex);
    if (idle()) return false;
    sync_->drained.wait(lock, idle);
    return true;
  }

  std::size_t dropped() const noexcept { return dropped_now(); }
  // Records the inner sink threw on, plus failed inner flushes and gap markers.
  std::size_t write_failures() const noexcept {
    return counters_->write_failures.load(std::memory_order_relaxed);
  }
  // The counters the run summary reads; shared, so they outlive the sink.
  std::shared_ptr<const detail::SinkCounters> counters() const noexcept { return counters_; }
  // Approximate while producers are active; exact once they have stopped.
  std::size_t queued() const noexcept {
    const std::size_t tail = enqueue_pos_.load(std::memory_order_acquire);
    const std::size_t head = dequeue_pos_.load(std::memory_order_acquire);
    return tail >= head ? tail - head : 0;  // the two loads are not one snapshot
  }
  std::size_t capacity() const noexcept { return mask_ + 1; }  // rounded up to a power of two
  const std::shared_ptr<Sink>& inner() const noexcept { return inner_; }

 private:
  // Cache line size, so the hot counters below do not share a line. A fixed value rather than
  // std::hardware_destructive_interference_size: GCC warns that the std constant changes with
  // tuning flags (an ABI hazard for a header), and these are the real sizes anyway.
#if defined(__aarch64__) || defined(_M_ARM64)
  static constexpr std::size_t kCacheLine = 128;
#else
  static constexpr std::size_t kCacheLine = 64;
#endif

  struct Slot {
    std::atomic<std::size_t> sequence;
    Record record;
  };

  // The locks and condition variables, together so a sink copied by a fork can let them go
  // untouched (see the class comment); the lock-free state stays inline.
  struct Sync {
    std::mutex mutex;
    std::condition_variable_any not_empty;  // _any: can wait on a stop_token
    std::condition_variable not_full;
    std::condition_variable drained;
    std::mutex shutdown_mutex;
    bool shut_down = false;  // under shutdown_mutex
    std::mutex marker_mutex;
    detail::Lineage marker_lineage;  // of the nested records dropped since the last marker
  };

  // True in the child of a fork() for a sink made before it. One relaxed load per call.
  bool inherited() const noexcept {
    return generation_ != detail::fork_generation().load(std::memory_order_relaxed);
  }

  // The destructor's path for a sink copied by a fork: the ring's contents are the parent's,
  // the worker, locks and condition variables are not ours to destroy, and the registry entry
  // is cleared on a best-effort basis (its lock may be a copy of a held one).
  void abandon_in_child() noexcept {
    shutdown();
    if (entry_ != nullptr) {
      if (entry_->mutex.try_lock()) {
        entry_->sink.store(nullptr, std::memory_order_relaxed);
        entry_->mutex.unlock();
      } else {
        entry_->sink.store(nullptr, std::memory_order_relaxed);
      }
      auto& registry = detail::async_registry();
      if (registry.mutex.try_lock()) {
        std::erase(registry.live, entry_);
        registry.mutex.unlock();
      }
    }
    (void)sync_.release();
    (void)worker_.release();
  }

  // Registers the sink and starts its worker under the registry lock, which is also the lock
  // shutdown_all() takes to see which sinks exist: a sink either registers before that look,
  // and is shut down, or after it, and then sees the process exiting and starts no worker.
  void register_self() {
    entry_ = std::make_shared<detail::AsyncEntry>();
    entry_->sink.store(this, std::memory_order_relaxed);
    auto& registry = detail::async_registry();
    std::lock_guard lock(registry.mutex);
    if (detail::process_exiting().load(std::memory_order_seq_cst)) {
      // Made after etch::shutdown() or inside an exit handler: no exit handler will drain a
      // worker any more, so the sink is synchronous from the start, like every async sink
      // after shutdown.
      closed_.store(true, std::memory_order_seq_cst);
      worker_gone_.store(true, std::memory_order_seq_cst);
      worker_finished_.store(true, std::memory_order_seq_cst);
      sync_->shut_down = true;
    } else {
      worker_ = std::make_unique<std::jthread>([this](std::stop_token st) { run(st); });
      worker_id_ = worker_->get_id();  // read once here, never from the jthread during a join
    }
    registry.live.push_back(entry_);
    if (!registry.exit_handler_installed) {
      registry.exit_handler_installed = true;
      detail::install_fork_handler();
      // On std::exit (or a return from main) every live async sink is drained and switched to
      // synchronous writes, so records queued before exit reach their sink and lines logged by
      // static destructors afterwards go straight through.
      std::atexit([] { shutdown_all(); });
    }
  }
  void unregister_self() noexcept {
    if (entry_ == nullptr) return;
    {
      std::lock_guard lock(entry_->mutex);  // waits for a traversal that is using this sink
      entry_->sink.store(nullptr, std::memory_order_relaxed);
    }
    auto& registry = detail::async_registry();
    std::lock_guard lock(registry.mutex);
    std::erase(registry.live, entry_);
  }

  // Runs fn on every live sink, one entry lock at a time and never under the registry lock.
  template <class Fn>
  static void for_each_live(Fn&& fn) {
    std::vector<std::shared_ptr<detail::AsyncEntry>> entries;
    {
      auto& registry = detail::async_registry();
      std::lock_guard lock(registry.mutex);
      entries = registry.live;
    }
    for (const auto& entry : entries) {
      std::lock_guard lock(entry->mutex);
      if (AsyncSink* sink = entry->sink.load(std::memory_order_relaxed)) fn(*sink);
    }
  }

  std::size_t dropped_now() const noexcept {
    return counters_->dropped.load(std::memory_order_seq_cst);
  }

  bool try_enqueue(Record& r) noexcept {
    std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
    for (;;) {
      Slot& slot = slots_[pos & mask_];
      const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
      // Unsigned subtraction, then a (modular, well-defined) conversion: no signed overflow.
      const auto diff = static_cast<std::intptr_t>(seq - pos);
      if (diff == 0) {
        if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_seq_cst)) {
          slot.record = std::move(r);
          slot.sequence.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false;  // the slot still holds a record the worker has not taken: full
      } else {
        pos = enqueue_pos_.load(std::memory_order_relaxed);  // another producer took it
      }
    }
  }

  // Single consumer, so no compare-and-swap on the dequeue side.
  bool try_dequeue(Record& out) noexcept {
    const std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
    Slot& slot = slots_[pos & mask_];
    const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
    if (static_cast<std::intptr_t>(seq - (pos + 1)) < 0) return false;
    out = std::move(slot.record);
    slot.sequence.store(pos + mask_ + 1, std::memory_order_release);
    dequeue_pos_.store(pos + 1, std::memory_order_seq_cst);
    return true;
  }

  // Position-based on purpose: the producer's claim (a seq_cst compare-and-swap on
  // enqueue_pos_) is the write the sleep handshake relies on, so "not empty" can mean "claimed
  // but not yet published", which the worker handles by trying again.
  bool empty() const noexcept {
    return enqueue_pos_.load(std::memory_order_seq_cst) ==
           dequeue_pos_.load(std::memory_order_seq_cst);
  }
  // Sequentially consistent on purpose: a dropping producer increments dropped (seq_cst) and
  // then reads worker_sleeping_; the worker writes worker_sleeping_ and then reads this. That
  // is a Dekker pair, so neither side can miss the other and a gap is never left unreported.
  bool gap_unreported() const noexcept {
    return dropped_now() != dropped_reported_.load(std::memory_order_seq_cst);
  }

  void wake_worker() {
    if (worker_sleeping_.load(std::memory_order_seq_cst)) {
      std::lock_guard lock(sync_->mutex);  // so the notify cannot slip between check and wait
      sync_->not_empty.notify_one();
    }
  }

  void enqueue(Record&& r) {
    if (detail::on_async_worker()) {
      // This sink's own worker cannot wait for a slot it alone frees, and another sink's
      // worker must not wait either (see on_async_worker). So: queued if there is room and
      // the queue is open; dropped and counted if the drop policy says so, as for any
      // producer; otherwise written straight through, ahead of what is queued. The producer
      // count stays up until the record is in the inner sink, so a shutdown meanwhile waits
      // for it.
      active_producers_.fetch_add(1, std::memory_order_seq_cst);
      bool settled = false;
      if (worker_id_ != std::this_thread::get_id() && !closed_.load(std::memory_order_seq_cst)) {
        if (try_enqueue(r)) {
          settled = true;
        } else if (policy_ == Overflow::kDrop && r.level < keep_from_) {
          note_dropped(r);
          settled = true;
        }
      }
      if (!settled) write_inner(std::move(r));
      active_producers_.fetch_sub(1, std::memory_order_seq_cst);
      if (settled) wake_worker();
      return;
    }
    // Announce first, then look at closed_: shutdown() does the reverse, so one of the two
    // always sees the other (see shutdown()).
    active_producers_.fetch_add(1, std::memory_order_seq_cst);
    if (closed_.load(std::memory_order_seq_cst)) {
      // Shutdown has begun. This record is not entered into the ring (so the drain is bounded),
      // but it is not written before the ring's contents either: wait for the worker to finish,
      // then deliver it in turn. Not counted as active, or shutdown would wait for us.
      active_producers_.fetch_sub(1, std::memory_order_seq_cst);
      {
        std::unique_lock lock(sync_->mutex);
        sync_->not_full.wait(lock, [&] { return worker_gone_.load(std::memory_order_seq_cst); });
      }
      write_inner(std::move(r));
      return;
    }
    bool queued = try_enqueue(r);
    if (!queued) {
      if (policy_ == Overflow::kDrop && r.level < keep_from_) {
        note_dropped(r);
        active_producers_.fetch_sub(1, std::memory_order_seq_cst);
        wake_worker();  // so the gap gets reported even if nothing else arrives
        return;
      }
      std::unique_lock lock(sync_->mutex);
      producers_waiting_.fetch_add(1, std::memory_order_relaxed);
      // Fence pairs with the worker's fence after it frees slots: either the worker sees this
      // producer waiting and notifies, or this producer sees the freed slots and gets in.
      sync_->not_full.wait(lock, [&] {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        queued = try_enqueue(r);
        // Only a worker that has gone (shut down from its own thread) ends the wait without a
        // slot; a normal shutdown keeps the worker draining until every producer inside is done.
        return queued || worker_gone_.load(std::memory_order_seq_cst);
      });
      producers_waiting_.fetch_sub(1, std::memory_order_relaxed);
      lock.unlock();
      if (!queued) {  // the worker is gone: deliver it ourselves
        active_producers_.fetch_sub(1, std::memory_order_seq_cst);
        write_inner(std::move(r));
        return;
      }
    }
    active_producers_.fetch_sub(1, std::memory_order_seq_cst);
    wake_worker();
  }

  // Counts a drop. A nested record that is dropped takes its lineage into the next gap marker,
  // so the marker cannot restart the chain the record came from: a sink that logs on every
  // write, markers included, behind a dropping queue fed by such another, would otherwise
  // produce a marker, a burst, a drop and a marker forever. Ordinary records have no lineage
  // and pay nothing here beyond the count.
  void note_dropped(const Record& r) noexcept {
    // The lineage lands before the count moves, so a marker built for a count the worker has
    // seen carries every lineage behind it; the set only grows (extra exclusions are
    // harmless, missing ones are not), so no snapshot of count and set has to agree. A
    // dropped marker from another queue contributes its ancestry the same way.
    if (r.lineage != nullptr || r.ancestry != nullptr) {
      try {
        std::lock_guard lock(sync_->marker_mutex);
        for (const auto& source : {r.lineage, r.ancestry}) {
          if (source == nullptr) continue;
          for (const Sink* sink : *source) {
            if (std::find(sync_->marker_lineage.begin(), sync_->marker_lineage.end(), sink) ==
                sync_->marker_lineage.end()) {
              sync_->marker_lineage.push_back(sink);
            }
          }
        }
      } catch (...) {
        // No memory for the exclusion: counted as a failure; a marker without it could
        // restart a chain once, which the next drop's merge then ends.
        counters_->write_failures.fetch_add(1, std::memory_order_relaxed);
      }
    }
    counters_->dropped.fetch_add(1, std::memory_order_seq_cst);
  }

  // Both this sink and the inner one are marked as writing on the calling thread (the worker,
  // usually), so a record the inner sink logs through a logger feeding either of them is set
  // aside until this write returns and then skips them both; and the record's own lineage
  // (the sinks it was logged from inside of, before it was queued here) is handed down to
  // whatever the inner sink logs (see delivery.hpp). A record kept from the inner sink by that
  // lineage (an async sink behind this one, say, that the record came through) stops here.
  void write_inner(Record&& r) noexcept {
    if (detail::kept_from(r, inner_.get())) {
      inner_->count_reentry();
      return;
    }
    const detail::DeliveryScope delivery;
    const detail::AncestryScope ancestry(r);
    const std::shared_ptr<const detail::Lineage> lineage = r.lineage;  // outlives the write
    const detail::WriteScope outer(this);
    const detail::WriteScope inner(inner_.get());
    try {
      inner_->write(std::move(r));
    } catch (...) {
      counters_->write_failures.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void flush_inner() noexcept {
    try {
      inner_->flush();
    } catch (...) {
      counters_->write_failures.fetch_add(1, std::memory_order_relaxed);
    }
  }

  // A flush asked for on a worker thread that has not been served yet.
  bool flush_outstanding() const noexcept {
    return flush_served_pos_.load(std::memory_order_seq_cst) <
           flush_requested_pos_.load(std::memory_order_seq_cst);
  }

  // Worker only. Whether to flush now for a request made on a worker thread: whenever one is
  // outstanding. `written` is the dequeue position after the last batch; it becomes the
  // served position, so a request for records still in the ring stays outstanding and the
  // batch that writes them is flushed too, and a stream of requests flushes every batch
  // rather than waiting for a target that keeps moving.
  bool take_flush_request(std::size_t written) noexcept {
    if (!flush_outstanding()) return false;
    flush_served_pos_.store(written, std::memory_order_seq_cst);
    return true;
  }

  // One warning that says how many records the drop policy lost since the previous report, so
  // a reader of the file knows there is a gap here and how big it is. Worker only. Building the
  // marker allocates, so the whole thing is guarded: a failure is counted and the report still
  // advances, otherwise the worker would retry forever.
  bool report_gap() noexcept {
    const std::size_t dropped_so_far = dropped_now();
    const std::size_t reported = dropped_reported_.load(std::memory_order_relaxed);
    if (dropped_so_far == reported) return false;
    try {
      const std::size_t gap = dropped_so_far - reported;
      Record marker;
      // At least warn, and never below what the inner sink accepts, so it cannot be filtered.
      marker.level = (std::max)(Level::kWarn, inner_->get_level());
      marker.time = std::chrono::system_clock::now();
      marker.thread = std::this_thread::get_id();
      marker.thread_index = detail::this_thread_index();
      marker.where = std::source_location::current();
      marker.logger_name = "etch";
      marker.message =
          std::format("{} records dropped by the async queue since the previous report", gap);
      marker.fields.emplace_back("dropped", gap);
      {
        // The marker itself goes wherever the inner sink and anything behind it send it (it
        // is this queue's own word about itself); what is logged in response inherits the
        // dropped records' exclusions, so the marker cannot restart their chain.
        std::lock_guard lock(sync_->marker_mutex);
        if (!sync_->marker_lineage.empty()) {
          marker.ancestry = std::make_shared<detail::Lineage>(sync_->marker_lineage);
        }
      }
      if (marker.level < Level::kOff) write_inner(std::move(marker));
    } catch (...) {
      counters_->write_failures.fetch_add(1, std::memory_order_relaxed);
    }
    dropped_reported_.store(dropped_so_far, std::memory_order_seq_cst);
    return true;  // something was written (or tried): the inner sink wants a flush
  }

  void run(std::stop_token st) {
    detail::name_this_thread("etch-async");
    detail::on_async_worker() = true;
    const std::size_t batch_limit = (std::min)(capacity(), kMaxBatch);
    std::vector<Record> batch;
    batch.reserve(batch_limit);
    std::size_t batch_end = 0;  // dequeue position after the batch most recently written
    bool flushed_since_write = true;
    for (;;) {
      for (int i = 0; i < kYieldsBeforeSleep && empty(); ++i) std::this_thread::yield();
      if (report_gap()) flushed_since_write = false;
      {
        std::unique_lock lock(sync_->mutex);
        if (written_pos_.load(std::memory_order_relaxed) != batch_end) {
          written_pos_.store(batch_end, std::memory_order_release);
        }
        sync_->drained.notify_all();  // written_pos_ and dropped_reported_ may both have moved
        if (empty()) {
          if (!flushed_since_write || take_flush_request(batch_end)) {
            lock.unlock();
            detail::worker_activity().fetch_add(1, std::memory_order_seq_cst);
            flush_inner();  // idle, or asked by a fatal from a worker: flush before sleeping
            flushed_since_write = true;
            lock.lock();
            if (!empty()) continue;
          }
          worker_sleeping_.store(true, std::memory_order_seq_cst);
          sync_->drained.notify_all();  // wait_until_idle() watches for this
          const bool have_work = sync_->not_empty.wait(
              lock, st, [&] { return !empty() || gap_unreported() || flush_outstanding(); });
          worker_sleeping_.store(false, std::memory_order_seq_cst);
          detail::worker_activity().fetch_add(1, std::memory_order_seq_cst);
          if (!have_work) break;  // stop requested and nothing left in the ring
          if (empty()) continue;  // woken for a gap report or a flush only
        }
      }
      Record r;
      while (batch.size() < batch_limit && try_dequeue(r)) batch.push_back(std::move(r));
      batch_end = dequeue_pos_.load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_seq_cst);  // pairs with the producer's fence
      if (producers_waiting_.load(std::memory_order_relaxed) > 0) {
        std::lock_guard lock(sync_->mutex);
        sync_->not_full.notify_all();
      }
      if (batch.empty()) {
        std::this_thread::yield();  // a producer claimed a slot but has not published it yet
        continue;
      }
      if (report_gap()) flushed_since_write = false;
      detail::worker_activity().fetch_add(1, std::memory_order_seq_cst);
      for (auto& queued : batch) write_inner(std::move(queued));
      batch.clear();
      flushed_since_write = false;
      if (take_flush_request(batch_end)) {
        flush_inner();  // a fatal from a worker asked for it: after the batch it is in
        flushed_since_write = true;
      }
    }
    // Exit handshake: announce, then keep draining until no producer is still inside and the
    // ring is empty. A producer that is inside either enqueues (its record is drained here, since
    // it publishes before leaving active) or, blocked on a full ring, reads worker_gone_ after
    // its own announcement and writes through. (Write worker_gone_ then read active here; a
    // producer announces then reads worker_gone_: both sequentially consistent.)
    worker_gone_.store(true, std::memory_order_seq_cst);
    for (;;) {
      {
        std::lock_guard lock(sync_->mutex);
        sync_->not_full.notify_all();  // blocked producers: a slot may be free, or we are gone
      }
      Record r;
      while (try_dequeue(r)) write_inner(std::move(r));
      if (active_producers_.load(std::memory_order_seq_cst) == 0 && empty()) break;
      std::this_thread::yield();
    }
    report_gap();
    flush_inner();  // the final drain's lines, and the last gap marker, reach their file
    worker_finished_.store(true, std::memory_order_seq_cst);
    std::lock_guard lock(sync_->mutex);
    sync_->not_full.notify_all();  // late producers waiting for worker_gone_
    sync_->drained.notify_all();   // a flush() waiting on the worker during shutdown
  }

  // Shuts every live async sink down; the exit handler. The registry lock is held throughout,
  // so no sink can be destroyed under our feet.
  static void shutdown_all() noexcept {
    detail::process_exiting().store(true, std::memory_order_relaxed);
    for_each_live([](AsyncSink& sink) { sink.shutdown(); });
  }

  friend void flush_all();
  friend void shutdown();
  friend void prepare_fork();

  std::shared_ptr<Sink> inner_;
  std::size_t mask_;
  std::unique_ptr<Slot[]> slots_;
  Overflow policy_;
  Level keep_from_;
  std::shared_ptr<detail::SinkCounters> counters_;  // dropped, write failures
  std::unique_ptr<Sync> sync_;
  unsigned generation_;  // the fork generation this sink was made in
  alignas(kCacheLine) std::atomic<std::size_t> enqueue_pos_{0};
  alignas(kCacheLine) std::atomic<std::size_t> dequeue_pos_{0};
  alignas(kCacheLine) std::atomic<bool> worker_sleeping_{false};
  std::atomic<bool> closed_{false};            // shutdown began: new records wait, then go through
  std::atomic<bool> worker_gone_{false};       // the worker has left its loop
  std::atomic<bool> worker_finished_{false};   // ... and its final drain and flush are done
  std::shared_ptr<detail::AsyncEntry> entry_;  // this sink's registry entry
  std::thread::id worker_id_;                  // set once in the constructor
  std::atomic<std::size_t> active_producers_{0};  // producers between announcing and enqueueing
  std::atomic<std::size_t> written_pos_{0};       // everything before it is in the inner sink
  std::atomic<std::size_t> producers_waiting_{0};
  std::atomic<std::size_t> dropped_reported_{0};
  std::atomic<std::size_t> flush_requested_pos_{0};  // by a flush() on a worker thread, for
                                                     // everything queued before this position
  std::atomic<std::size_t> flush_served_pos_{0};     // the worker had written this much when
                                                     // it last flushed for such a request
  std::unique_ptr<std::jthread> worker_;  // started in the constructor body, once the rest exists
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

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
