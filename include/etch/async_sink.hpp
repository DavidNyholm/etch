// etch - AsyncSink: a lock-free bounded queue and a worker thread in front of any other sink.
#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

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
// When the queue is full the overflow policy decides: block the caller until there is room
// (never lose a line), or drop the new record and count it (never stall a game loop).
// dropped() tells you how many were lost.
//
// flush() returns once every record enqueued before the call is written, however much is logged
// meanwhile (progress is published per batch, so it returns when the batch holding the last of
// those records is done). Destroying the sink stops the worker after it has drained the queue,
// so nothing logged before the destructor is lost. An exception from the inner sink is caught,
// counted in write_failures() and costs that one record; the worker carries on.
//
// One rule for sinks behind this one: whatever they call must not drop the last reference to
// this AsyncSink (say, by replacing a logger's sink list with an empty one from inside a
// callback). The destructor would then run on the worker thread and try to join itself.
class AsyncSink final : public Sink {
 public:
  enum class Overflow { kBlock, kDrop };
  // Slots are allocated up front (about 128 bytes each), so the default stays modest.
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
                     Overflow policy = Overflow::kBlock)
      : inner_(std::move(inner)),
        mask_(std::bit_ceil(std::clamp(capacity, std::size_t{2}, kMaxCapacity)) - 1),
        slots_(std::make_unique<Slot[]>(mask_ + 1)),
        policy_(policy),
        worker_([this](std::stop_token st) { run(st); }) {
    for (std::size_t i = 0; i <= mask_; ++i) {
      slots_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  ~AsyncSink() override {
    worker_.request_stop();  // wakes the worker through its stop_token-aware wait
    worker_.join();
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
    const std::size_t target = enqueue_pos_.load(std::memory_order_seq_cst);
    std::unique_lock lock(mutex_);
    drained_.wait(lock, [&] { return written_pos_.load(std::memory_order_acquire) >= target; });
    lock.unlock();
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
  // Destructive interference size, so the hot counters do not share a cache line.
#ifdef __cpp_lib_hardware_interference_size
  static constexpr std::size_t kCacheLine = std::hardware_destructive_interference_size;
#else
  static constexpr std::size_t kCacheLine = 64;
#endif

  struct Slot {
    std::atomic<std::size_t> sequence;
    Record record;
  };

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
    if (!try_enqueue(r)) {
      if (policy_ == Overflow::kDrop) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      std::unique_lock lock(mutex_);
      producers_waiting_.fetch_add(1, std::memory_order_relaxed);
      // Fence pairs with the worker's fence after it frees slots: either the worker sees this
      // producer waiting and notifies, or this producer sees the freed slots and gets in.
      not_full_.wait(lock, [&] {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return try_enqueue(r);
      });
      producers_waiting_.fetch_sub(1, std::memory_order_relaxed);
    }
    if (worker_sleeping_.load(std::memory_order_seq_cst)) {
      std::lock_guard lock(mutex_);  // so the notify cannot slip between the check and the wait
      not_empty_.notify_one();
    }
  }

  void flush_inner() noexcept {
    try {
      inner_->flush();
    } catch (...) {
      write_failures_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void run(std::stop_token st) {
    const std::size_t batch_limit = std::min(capacity(), kMaxBatch);
    std::vector<Record> batch;
    batch.reserve(batch_limit);
    std::size_t batch_end = 0;  // dequeue position after the batch most recently written
    for (;;) {
      for (int i = 0; i < kYieldsBeforeSleep && empty(); ++i) std::this_thread::yield();
      {
        std::unique_lock lock(mutex_);
        if (written_pos_.load(std::memory_order_relaxed) != batch_end) {
          written_pos_.store(batch_end, std::memory_order_release);
          drained_.notify_all();
        }
        if (empty()) {
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
      for (auto& queued : batch) {
        try {
          inner_->write(std::move(queued));
        } catch (...) {
          write_failures_.fetch_add(1, std::memory_order_relaxed);
        }
      }
      batch.clear();
    }
  }

  std::shared_ptr<Sink> inner_;
  std::size_t mask_;
  std::unique_ptr<Slot[]> slots_;
  Overflow policy_;
  alignas(kCacheLine) std::atomic<std::size_t> enqueue_pos_{0};
  alignas(kCacheLine) std::atomic<std::size_t> dequeue_pos_{0};
  alignas(kCacheLine) std::atomic<bool> worker_sleeping_{false};
  std::atomic<std::size_t> written_pos_{0};  // everything before it is in the inner sink
  std::atomic<std::size_t> producers_waiting_{0};
  std::atomic<std::size_t> dropped_{0};
  std::atomic<std::size_t> write_failures_{0};
  mutable std::mutex mutex_;
  std::condition_variable_any not_empty_;  // _any: can wait on a stop_token
  std::condition_variable not_full_;
  std::condition_variable drained_;
  std::jthread worker_;  // last member: constructed last, so everything it uses already exists
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace etch
