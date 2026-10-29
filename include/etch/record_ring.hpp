// etch - the bounded ring the async sink queues records in: slots with sequence numbers, one
// compare-and-swap per producer claim, a single consumer (Dmitry Vyukov's bounded MPMC queue,
// used MPSC). No lock and no allocation after construction, on either side.
#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "record.hpp"

namespace etch::detail {

// Cache line size, so hot counters do not share a line. A fixed value rather than
// std::hardware_destructive_interference_size: GCC warns that the std constant changes with
// tuning flags (an ABI hazard for a header), and these are the real sizes anyway.
#if defined(__aarch64__) || defined(_M_ARM64)
inline constexpr std::size_t kCacheLine = 128;
#else
inline constexpr std::size_t kCacheLine = 64;
#endif

#if defined(_MSC_VER)
// C4324: "structure was padded due to alignment specifier". The padding is the point: the
// positions below are placed on separate cache lines so producers and the consumer do not
// false-share.
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

class RecordRing {
 public:
  // Upper bound: std::bit_ceil must have a representable result, and 2^30 slots is already
  // 128 GB of slot storage.
  static constexpr std::size_t kMaxCapacity = std::size_t{1} << 30;

  // Rounds the capacity up to a power of two, at least 2. Allocates the slots at once.
  explicit RecordRing(std::size_t capacity)
      : mask_(std::bit_ceil(std::clamp(capacity, std::size_t{2}, kMaxCapacity)) - 1),
        slots_(std::make_unique<Slot[]>(mask_ + 1)) {
    for (std::size_t i = 0; i <= mask_; ++i) slots_[i].sequence.store(i, std::memory_order_relaxed);
  }

  RecordRing(const RecordRing&) = delete;
  RecordRing& operator=(const RecordRing&) = delete;

  // Moves the record in and returns true, or returns false with the record untouched when
  // every slot still holds a record the consumer has not taken. Any thread.
  bool try_push(Record& r) noexcept {
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
        return false;  // full
      } else {
        pos = enqueue_pos_.load(std::memory_order_relaxed);  // another producer took it
      }
    }
  }

  // The one consumer only, so no compare-and-swap on this side.
  bool try_pop(Record& out) noexcept {
    const std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
    Slot& slot = slots_[pos & mask_];
    const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
    if (static_cast<std::intptr_t>(seq - (pos + 1)) < 0) return false;
    out = std::move(slot.record);
    slot.sequence.store(pos + mask_ + 1, std::memory_order_release);
    dequeue_pos_.store(pos + 1, std::memory_order_seq_cst);
    return true;
  }

  // Position-based on purpose: a producer's claim (the sequentially consistent compare-and-swap
  // above) is the write that wake-up handshakes rely on, so "not empty" can mean "claimed but
  // not yet published", which a consumer handles by trying again.
  bool empty() const noexcept {
    return enqueue_pos_.load(std::memory_order_seq_cst) ==
           dequeue_pos_.load(std::memory_order_seq_cst);
  }
  // The positions as counters: everything pushed, everything popped, since construction.
  std::size_t pushed(std::memory_order order) const noexcept { return enqueue_pos_.load(order); }
  std::size_t popped(std::memory_order order) const noexcept { return dequeue_pos_.load(order); }
  std::size_t capacity() const noexcept { return mask_ + 1; }

 private:
  struct Slot {
    std::atomic<std::size_t> sequence;
    Record record;
  };

  std::size_t mask_;
  std::unique_ptr<Slot[]> slots_;
  alignas(kCacheLine) std::atomic<std::size_t> enqueue_pos_{0};
  alignas(kCacheLine) std::atomic<std::size_t> dequeue_pos_{0};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace etch::detail
