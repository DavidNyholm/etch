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

#if !defined(_WIN32)
#include <pthread.h>  // pthread_setname_np, so the worker is named in a debugger
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
// them. Raw pointers: each sink registers in its constructor and leaves in its destructor.
struct AsyncRegistry {
  std::mutex mutex;
  std::vector<AsyncSink*> live;
  bool exit_handler_installed = false;
};
inline AsyncRegistry& async_registry() {
  static AsyncRegistry registry;
  return registry;
}

#if defined(_WIN32)
#ifndef _WINDOWS_
extern "C" {
__declspec(dllimport) void* __stdcall GetCurrentThread();
__declspec(dllimport) long __stdcall SetThreadDescription(void*, const wchar_t*);
}
#endif
#endif
inline void name_this_thread(const char* ascii_name) noexcept {
#if defined(_WIN32)
  wchar_t wide[32] = {};
  for (std::size_t i = 0; i + 1 < sizeof(wide) / sizeof(wide[0]) && ascii_name[i] != 0; ++i) {
    wide[i] = static_cast<wchar_t>(ascii_name[i]);
  }
  SetThreadDescription(GetCurrentThread(), wide);
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
// saying how many, before the next real record, so the file shows where its gaps are.
//
// flush() returns once every record enqueued before the call is written, however much is logged
// meanwhile (progress is published per batch, so it returns when the batch holding the last of
// those records is done). When the queue goes idle the worker flushes the inner sink, so a
// crash a little later loses at most the burst in flight. Destroying the sink stops the worker
// after it has drained the queue, so nothing logged before the destructor is lost. An exception
// from the inner sink is caught, counted in write_failures() and costs that one record; the
// worker carries on.
//
// One rule for sinks behind this one: whatever they call must not drop the last reference to
// this AsyncSink (say, by replacing a logger's sink list with an empty one from inside a
// callback). The destructor would then run on the worker thread and try to join itself.
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
    register_self();
    worker_ = std::jthread([this](std::stop_token st) { run(st); });
  }

  ~AsyncSink() override {
    unregister_self();
    stop_worker();
    flush_inner();
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

  void flush() override {
    if (!passthrough_.load(std::memory_order_acquire)) {
      const std::size_t target = enqueue_pos_.load(std::memory_order_seq_cst);
      std::unique_lock lock(mutex_);
      drained_.wait(lock, [&] { return written_pos_.load(std::memory_order_acquire) >= target; });
    }
    flush_inner();
  }

  // Drains the queue, stops the worker and switches to writing straight through to the inner
  // sink on the calling thread. Used by etch::shutdown() and the exit handler; after it the
  // sink still works, synchronously. Safe to call more than once.
  void shutdown() {
    stop_worker();  // the worker sets passthrough_ as it exits, so later writes go straight through
    flush_inner();
  }

  std::size_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
  // Records the inner sink threw on, plus failed inner flushes.
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
    auto& registry = detail::async_registry();
    std::lock_guard lock(registry.mutex);
    registry.live.push_back(this);
    if (!registry.exit_handler_installed) {
      registry.exit_handler_installed = true;
      // On std::exit (or a return from main) every live async sink is drained and switched to
      // synchronous writes, so records queued before exit reach their sink and lines logged by
      // static destructors afterwards go straight through.
      std::atexit([] { shutdown_all(); });
    }
  }
  void unregister_self() {
    auto& registry = detail::async_registry();
    std::lock_guard lock(registry.mutex);
    std::erase(registry.live, this);
  }

  // Stops and joins the worker once; a no-op afterwards. Called from the worker itself (a
  // callback that reached shutdown()) it cannot join, so it only asks for the stop.
  void stop_worker() {
    worker_.request_stop();
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
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

  void enqueue(Record&& r) {
    if (passthrough_.load(std::memory_order_acquire)) {
      write_inner(std::move(r));
      return;
    }
    if (!try_enqueue(r)) {
      if (policy_ == Overflow::kDrop && r.level < keep_from_) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      std::unique_lock lock(mutex_);
      producers_waiting_.fetch_add(1, std::memory_order_relaxed);
      // Fence pairs with the worker's fence after it frees slots: either the worker sees this
      // producer waiting and notifies, or this producer sees the freed slots and gets in.
      not_full_.wait(lock, [&] {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return try_enqueue(r) || passthrough_.load(std::memory_order_acquire);
      });
      producers_waiting_.fetch_sub(1, std::memory_order_relaxed);
      if (passthrough_.load(std::memory_order_acquire)) {
        lock.unlock();
        write_inner(std::move(r));
        return;
      }
    }
    if (worker_sleeping_.load(std::memory_order_seq_cst)) {
      std::lock_guard lock(mutex_);  // so the notify cannot slip between the check and the wait
      not_empty_.notify_one();
    }
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

  // One warning that says how many records the drop policy lost since the previous line, so a
  // reader of the file knows there is a gap here and how big it is.
  void write_gap_marker(std::size_t dropped_now) noexcept {
    const std::size_t gap = dropped_now - dropped_reported_;
    dropped_reported_ = dropped_now;
    Record marker;
    marker.level = Level::kWarn;
    marker.time = std::chrono::system_clock::now();
    marker.thread = std::this_thread::get_id();
    marker.thread_index = detail::this_thread_index();
    marker.where = std::source_location::current();
    marker.logger_name = "etch";
    marker.message =
        std::format("{} records dropped by the async queue since the previous line", gap);
    marker.fields.emplace_back("dropped", gap);
    if (inner_->accepts(marker.level)) write_inner(std::move(marker));
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
      {
        std::unique_lock lock(mutex_);
        if (written_pos_.load(std::memory_order_relaxed) != batch_end) {
          written_pos_.store(batch_end, std::memory_order_release);
          drained_.notify_all();
        }
        if (empty()) {
          if (!flushed_since_write) {
            lock.unlock();
            flush_inner();  // idle: get the last burst onto disk before sleeping
            flushed_since_write = true;
            lock.lock();
            if (!empty()) continue;
          }
          worker_sleeping_.store(true, std::memory_order_seq_cst);
          const bool have_work = not_empty_.wait(lock, st, [&] { return !empty(); });
          worker_sleeping_.store(false, std::memory_order_seq_cst);
          if (!have_work) break;  // stop requested and nothing left: drained, exit
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
      const std::size_t dropped_now = dropped_.load(std::memory_order_relaxed);
      if (dropped_now != dropped_reported_) write_gap_marker(dropped_now);
      for (auto& queued : batch) write_inner(std::move(queued));
      batch.clear();
      flushed_since_write = false;
    }
    // Anything a producer could still add after this point goes straight through.
    passthrough_.store(true, std::memory_order_release);
    std::lock_guard lock(mutex_);
    not_full_.notify_all();
  }

  // Shuts every live async sink down; the exit handler.
  static void shutdown_all() noexcept {
    detail::process_exiting().store(true, std::memory_order_relaxed);
    std::vector<AsyncSink*> live;
    {
      auto& registry = detail::async_registry();
      std::lock_guard lock(registry.mutex);
      live = registry.live;
    }
    for (AsyncSink* sink : live) sink->shutdown();
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
  std::atomic<bool> passthrough_{false};     // worker gone: write() goes straight to the inner sink
  std::atomic<std::size_t> written_pos_{0};  // everything before it is in the inner sink
  std::atomic<std::size_t> producers_waiting_{0};
  std::atomic<std::size_t> dropped_{0};
  std::size_t dropped_reported_ = 0;  // worker only
  std::atomic<std::size_t> write_failures_{0};
  mutable std::mutex mutex_;
  std::condition_variable_any not_empty_;  // _any: can wait on a stop_token
  std::condition_variable not_full_;
  std::condition_variable drained_;
  std::jthread worker_;  // started last, in the constructor body, once everything it uses exists
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// Waits until every live async sink has written everything queued so far, then flushes their
// inner sinks. Call it before anything that must see the complete log (a crash handler, a
// test's assertion, a hand-off to another process).
inline void flush_all() {
  std::vector<AsyncSink*> live;
  {
    auto& registry = detail::async_registry();
    std::lock_guard lock(registry.mutex);
    live = registry.live;
  }
  for (AsyncSink* sink : live) sink->flush();
}

// Drains and stops every async worker and marks the process as exiting, after which every
// async sink writes straight through and file sinks flush every line. Call it once at the end
// of main; it also runs automatically from an exit handler. Not for use from a sink callback
// running on a worker thread.
inline void shutdown() { AsyncSink::shutdown_all(); }

}  // namespace etch
