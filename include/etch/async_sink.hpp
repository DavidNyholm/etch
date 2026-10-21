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

#include "sinks.hpp"

#if defined(__linux__)
#include <pthread.h>  // pthread_setname_np (a GNU extension, which g++ enables by default)
#endif

namespace etch {

#if defined(_MSC_VER)
// C4324: "structure was padded due to alignment specifier". The padding is the point: the hot
// counters below are placed on separate cache lines so producers and the worker do not false-share.
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

class AsyncSink;

namespace detail {

// Every live AsyncSink, so flush_all() and shutdown() can reach them, and so an exit handler
// can drain them: a std::exit() while an async sink still holds records would otherwise lose
// them. The registry is never destroyed (a sink owned by a global may be destroyed after any
// static would be). Each sink has an entry with its own lock: a traversal snapshots the entries
// under the registry lock, releases it, then locks one entry at a time while it talks to that
// sink. The sink's destructor takes its entry lock to clear the pointer, so it cannot be
// destroyed mid-call, and nothing holds the registry lock while a worker is waited for, so a
// callback that constructs or destroys another async sink does not deadlock.
struct AsyncEntry {
  std::mutex mutex;
  AsyncSink* sink = nullptr;  // null once the sink is gone
};
struct AsyncRegistry {
  std::mutex mutex;
  std::vector<std::shared_ptr<AsyncEntry>> live;
  bool exit_handler_installed = false;
};
inline AsyncRegistry& async_registry() {
  static AsyncRegistry* const registry = new AsyncRegistry;
  return *registry;
}

#if defined(_WIN32)
#ifndef _WINDOWS_
extern "C" {
__declspec(dllimport) void* __stdcall GetModuleHandleW(const wchar_t*);
__declspec(dllimport) void* __stdcall GetProcAddress(void*, const char*);
__declspec(dllimport) void* __stdcall GetCurrentThread();
}
#endif
#endif
// Names the calling thread for debuggers and profilers. Best effort: on Windows the call is
// looked up at run time (it exists from Windows 10 1607), so an older system just has an
// unnamed worker rather than a missing import.
inline void name_this_thread(const char* ascii_name) noexcept {
#if defined(_WIN32)
  using SetThreadDescriptionFn = long(__stdcall*)(void*, const wchar_t*);
  static const auto set_description = reinterpret_cast<SetThreadDescriptionFn>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));
  if (set_description == nullptr) return;
  wchar_t wide[32] = {};
  for (std::size_t i = 0; i + 1 < sizeof(wide) / sizeof(wide[0]) && ascii_name[i] != 0; ++i) {
    wide[i] = static_cast<wchar_t>(ascii_name[i]);
  }
  set_description(GetCurrentThread(), wide);
#elif defined(__linux__)
  pthread_setname_np(pthread_self(), ascii_name);  // at most 15 characters on Linux
#else
  (void)ascii_name;
#endif
}

}  // namespace detail

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
// records arriving during that drain are not kept in order with the ring's contents.
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
        keep_from_(keep_from) {
    if (inner_ == nullptr) throw std::invalid_argument("etch::AsyncSink: null inner sink");
    for (std::size_t i = 0; i <= mask_; ++i) {
      slots_[i].sequence.store(i, std::memory_order_relaxed);
    }
    worker_ = std::jthread([this](std::stop_token st) { run(st); });
    worker_id_ = worker_.get_id();  // read once here, never from the jthread during a join
    try {
      register_self();
    } catch (...) {
      shutdown();
      throw;
    }
  }

  ~AsyncSink() override {
    shutdown();
    unregister_self();
  }

  AsyncSink(const AsyncSink&) = delete;
  AsyncSink& operator=(const AsyncSink&) = delete;

  // The inner sink's level applies as if it were used directly; filtered records never enter
  // the queue.
  void write(const Record& r) override {
    if (inner_->accepts(r.level)) enqueue(Record(r));
  }
  void write(Record&& r) override {
    if (inner_->accepts(r.level)) enqueue(std::move(r));
  }

  // Waits until everything enqueued before the call is written and every drop so far is
  // reported, then flushes the inner sink. During a shutdown it waits for the worker to finish
  // instead: closing admission is not delivery.
  void flush() override {
    const std::size_t target = enqueue_pos_.load(std::memory_order_seq_cst);
    const std::size_t drops = dropped_.load(std::memory_order_seq_cst);
    {
      std::unique_lock lock(mutex_);
      if (drops != dropped_reported_.load(std::memory_order_acquire)) not_empty_.notify_one();
      drained_.wait(lock, [&] {
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
    {
      std::lock_guard lock(mutex_);
      not_full_.notify_all();  // blocked producers re-check closed_ and write through
    }
    if (worker_id_ == std::this_thread::get_id()) {
      worker_.request_stop();  // a callback on the worker: it cannot wait for itself
      return;
    }
    std::lock_guard shutdown_lock(shutdown_mutex_);
    if (shut_down_) return;
    shut_down_ = true;
    // Every producer that saw the queue open finishes its enqueue before the worker drains.
    // (Producer: active +1, then read closed_. Here: write closed_, then read active. Both
    // sequentially consistent, so a producer we do not wait for is one that saw closed_.)
    while (active_producers_.load(std::memory_order_seq_cst) > 0) std::this_thread::yield();
    worker_.request_stop();
    if (worker_.joinable()) worker_.join();
    {
      std::lock_guard lock(mutex_);
      written_pos_.store(enqueue_pos_.load(std::memory_order_acquire), std::memory_order_release);
      drained_.notify_all();  // a flush() that was waiting on the worker
    }
    flush_inner();
  }

  std::size_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
  // Records the inner sink threw on, plus failed inner flushes and gap markers.
  std::size_t write_failures() const noexcept {
    return write_failures_.load(std::memory_order_relaxed);
  }
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

  void register_self() {
    entry_ = std::make_shared<detail::AsyncEntry>();
    entry_->sink = this;
    auto& registry = detail::async_registry();
    std::lock_guard lock(registry.mutex);
    registry.live.push_back(entry_);
    if (!registry.exit_handler_installed) {
      registry.exit_handler_installed = true;
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
      entry_->sink = nullptr;
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
      if (entry->sink != nullptr) fn(*entry->sink);
    }
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
  // Sequentially consistent on purpose: a dropping producer increments dropped_ (seq_cst) and
  // then reads worker_sleeping_; the worker writes worker_sleeping_ and then reads this. That
  // is a Dekker pair, so neither side can miss the other and a gap is never left unreported.
  bool gap_unreported() const noexcept {
    return dropped_.load(std::memory_order_seq_cst) !=
           dropped_reported_.load(std::memory_order_seq_cst);
  }

  void wake_worker() {
    if (worker_sleeping_.load(std::memory_order_seq_cst)) {
      std::lock_guard lock(mutex_);  // so the notify cannot slip between the check and the wait
      not_empty_.notify_one();
    }
  }

  void enqueue(Record&& r) {
    // Announce first, then look at closed_: shutdown() does the reverse, so one of the two
    // always sees the other (see shutdown()).
    active_producers_.fetch_add(1, std::memory_order_seq_cst);
    if (closed_.load(std::memory_order_seq_cst)) {
      // Shutdown has begun. This record is not entered into the ring (so the drain is bounded),
      // but it is not written before the ring's contents either: wait for the worker to finish,
      // then deliver it in turn. Not counted as active, or shutdown would wait for us.
      active_producers_.fetch_sub(1, std::memory_order_seq_cst);
      {
        std::unique_lock lock(mutex_);
        not_full_.wait(lock, [&] { return worker_gone_.load(std::memory_order_seq_cst); });
      }
      write_inner(std::move(r));
      return;
    }
    bool queued = try_enqueue(r);
    if (!queued) {
      if (policy_ == Overflow::kDrop && r.level < keep_from_) {
        dropped_.fetch_add(1, std::memory_order_seq_cst);
        active_producers_.fetch_sub(1, std::memory_order_seq_cst);
        wake_worker();  // so the gap gets reported even if nothing else arrives
        return;
      }
      std::unique_lock lock(mutex_);
      producers_waiting_.fetch_add(1, std::memory_order_relaxed);
      // Fence pairs with the worker's fence after it frees slots: either the worker sees this
      // producer waiting and notifies, or this producer sees the freed slots and gets in.
      not_full_.wait(lock, [&] {
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

  void write_inner(Record&& r) noexcept {
    try {
      inner_->write(std::move(r));
    } catch (...) {
      write_failures_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void flush_inner() noexcept {
    try {
      inner_->flush();
    } catch (...) {
      write_failures_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  // One warning that says how many records the drop policy lost since the previous report, so
  // a reader of the file knows there is a gap here and how big it is. Worker only. Building the
  // marker allocates, so the whole thing is guarded: a failure is counted and the report still
  // advances, otherwise the worker would retry forever.
  bool report_gap() noexcept {
    const std::size_t dropped_now = dropped_.load(std::memory_order_seq_cst);
    const std::size_t reported = dropped_reported_.load(std::memory_order_relaxed);
    if (dropped_now == reported) return false;
    try {
      const std::size_t gap = dropped_now - reported;
      Record marker;
      // At least warn, and never below what the inner sink accepts, so it cannot be filtered.
      marker.level = std::max(Level::kWarn, inner_->get_level());
      marker.time = std::chrono::system_clock::now();
      marker.thread = std::this_thread::get_id();
      marker.thread_index = detail::this_thread_index();
      marker.where = std::source_location::current();
      marker.logger_name = "etch";
      marker.message =
          std::format("{} records dropped by the async queue since the previous report", gap);
      marker.fields.emplace_back("dropped", gap);
      if (marker.level < Level::kOff) inner_->write(std::move(marker));
    } catch (...) {
      write_failures_.fetch_add(1, std::memory_order_relaxed);
    }
    dropped_reported_.store(dropped_now, std::memory_order_seq_cst);
    return true;  // something was written (or tried): the inner sink wants a flush
  }

  void run(std::stop_token st) {
    detail::name_this_thread("etch-async");
    const std::size_t batch_limit = std::min(capacity(), kMaxBatch);
    std::vector<Record> batch;
    batch.reserve(batch_limit);
    std::size_t batch_end = 0;  // dequeue position after the batch most recently written
    bool flushed_since_write = true;
    for (;;) {
      for (int i = 0; i < kYieldsBeforeSleep && empty(); ++i) std::this_thread::yield();
      if (report_gap()) flushed_since_write = false;
      {
        std::unique_lock lock(mutex_);
        if (written_pos_.load(std::memory_order_relaxed) != batch_end) {
          written_pos_.store(batch_end, std::memory_order_release);
        }
        drained_.notify_all();  // written_pos_ and dropped_reported_ may both have moved
        if (empty()) {
          if (!flushed_since_write) {
            lock.unlock();
            flush_inner();  // idle: get the last burst onto disk before sleeping
            flushed_since_write = true;
            lock.lock();
            if (!empty()) continue;
          }
          worker_sleeping_.store(true, std::memory_order_seq_cst);
          const bool have_work =
              not_empty_.wait(lock, st, [&] { return !empty() || gap_unreported(); });
          worker_sleeping_.store(false, std::memory_order_seq_cst);
          if (!have_work) break;  // stop requested and nothing left in the ring
          if (empty()) continue;  // woken for a gap report only
        }
      }
      Record r;
      while (batch.size() < batch_limit && try_dequeue(r)) batch.push_back(std::move(r));
      batch_end = dequeue_pos_.load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_seq_cst);  // pairs with the producer's fence
      if (producers_waiting_.load(std::memory_order_relaxed) > 0) {
        std::lock_guard lock(mutex_);
        not_full_.notify_all();
      }
      if (batch.empty()) {
        std::this_thread::yield();  // a producer claimed a slot but has not published it yet
        continue;
      }
      if (report_gap()) flushed_since_write = false;
      for (auto& queued : batch) write_inner(std::move(queued));
      batch.clear();
      flushed_since_write = false;
    }
    // Exit handshake: announce, then keep draining until no producer is still inside and the
    // ring is empty. A producer that is inside either enqueues (its record is drained here, since
    // it publishes before leaving active) or, blocked on a full ring, reads worker_gone_ after
    // its own announcement and writes through. (Write worker_gone_ then read active here; a
    // producer announces then reads worker_gone_: both sequentially consistent.)
    worker_gone_.store(true, std::memory_order_seq_cst);
    for (;;) {
      {
        std::lock_guard lock(mutex_);
        not_full_.notify_all();  // blocked producers: a slot may be free, or we are gone
      }
      Record r;
      while (try_dequeue(r)) write_inner(std::move(r));
      if (active_producers_.load(std::memory_order_seq_cst) == 0 && empty()) break;
      std::this_thread::yield();
    }
    report_gap();
    flush_inner();  // the final drain's lines, and the last gap marker, reach their file
    worker_finished_.store(true, std::memory_order_seq_cst);
    std::lock_guard lock(mutex_);
    not_full_.notify_all();  // late producers waiting for worker_gone_
    drained_.notify_all();   // a flush() waiting on the worker during shutdown
  }

  // Shuts every live async sink down; the exit handler. The registry lock is held throughout,
  // so no sink can be destroyed under our feet.
  static void shutdown_all() noexcept {
    detail::process_exiting().store(true, std::memory_order_relaxed);
    for_each_live([](AsyncSink& sink) { sink.shutdown(); });
  }

  friend void flush_all();
  friend void shutdown();

  std::shared_ptr<Sink> inner_;
  std::size_t mask_;
  std::unique_ptr<Slot[]> slots_;
  Overflow policy_;
  Level keep_from_;
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
  std::atomic<std::size_t> dropped_{0};
  std::atomic<std::size_t> dropped_reported_{0};
  std::atomic<std::size_t> write_failures_{0};
  mutable std::mutex mutex_;
  std::condition_variable_any not_empty_;  // _any: can wait on a stop_token
  std::condition_variable not_full_;
  std::condition_variable drained_;
  std::mutex shutdown_mutex_;
  bool shut_down_ = false;  // under shutdown_mutex_
  std::jthread worker_;     // started in the constructor body, once everything it uses exists
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

}  // namespace etch
