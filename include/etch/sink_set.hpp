// etch - SinkSet: the sink list a logger and its children share, replaced whole and read by
// log calls through a per-thread cache so that logging never takes the lock.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

#include "sinks.hpp"

namespace etch {

using SinkList = std::vector<std::shared_ptr<Sink>>;

namespace detail {

// The sink list a logger and its children share. The list itself is an immutable snapshot,
// replaced whole by set() and add(), and the sinks are called with no logger lock held, so a
// sink (or its callback) may replace the list, and a replaced async sink joins its worker
// outside any lock.
//
// A log call does not take the lock or touch the snapshot's reference count: both are atomic
// writes to a cache line every logging thread shares, and with four threads they cost more
// than the call itself. Instead each thread keeps its own copies of the last few lists it
// used, in a cache only it reads. A call pins its copy with one compare-and-swap on a line no
// other thread writes, and the sinks are called from that copy.
//
// Lifetimes are the same as with the lock: when set(), add() or the destructor returns, no
// thread cache still holds the old list. The replacing thread walks every thread's cache
// (there is a registry of them) and revokes the entries for this set: an entry nobody is
// using is cleared on the spot; one pinned by a call in flight is marked, and that call
// clears it when it ends. The per-entry state machine makes the two sides agree without a
// lock: Free -> Pinned (a call takes an empty slot), Pinned -> Idle (the call ends),
// Idle -> Pinned (the next call), Idle -> Revoking -> Free (a revoker clears an idle entry),
// Pinned -> PinnedRevoked (a revoker marks a busy entry) -> Free (its call clears it).
class SinkSet {
 public:
  explicit SinkSet(SinkList sinks) : list_(share(std::move(sinks))) {}
  ~SinkSet() { revoke(); }
  SinkSet(const SinkSet&) = delete;
  SinkSet& operator=(const SinkSet&) = delete;

  // The list, for callers that need to own it (flush, sinks()).
  std::shared_ptr<const SinkList> snapshot() const {
    std::shared_lock lock(mutex_);
    return list_;
  }
  void set(SinkList sinks) {
    auto next = share(std::move(sinks));
    std::shared_ptr<const SinkList> retired;  // destroyed after the lock is released
    std::unique_lock lock(mutex_);
    retired = std::exchange(list_, std::move(next));
    revoke();  // under the lock: a call that fills its cache after this reads the new list
  }
  void add(std::shared_ptr<Sink> sink) {
    if (sink == nullptr) return;
    std::shared_ptr<const SinkList> retired;
    std::unique_lock lock(mutex_);
    SinkList next = *list_;
    next.push_back(std::move(sink));
    retired = std::exchange(list_, share(std::move(next)));
    revoke();
  }

  // How many sets one thread remembers at once: a logger and a few children, or a few
  // independent loggers, before a slot is reused.
  static constexpr std::size_t kCachedSets = 4;

 private:
  enum class State : std::uint8_t { kFree, kIdle, kPinned, kPinnedRevoked, kRevoking };

  struct Entry {
    std::atomic<State> state{State::kFree};
    std::atomic<const SinkSet*> set{nullptr};  // compared, never dereferenced by a revoker
    std::shared_ptr<const SinkList> list;      // touched only by whoever holds the state
  };
  struct Cache {
    std::array<Entry, kCachedSets> entries;
    std::size_t next_victim = 0;  // round robin over the slots when all are taken
  };

 public:
  // The list for one log call on this thread, valid while the Use object lives. A sink that
  // replaces the list during the call does not disturb the caller's iteration: the copy in use
  // stays pinned, and the next call picks up the new list.
  class Use {
   public:
    explicit Use(const SinkSet& set) {
      Cache* const cache = thread_cache();
      if (cache == nullptr) {  // the thread is ending; its cache is gone
        owned_ = set.snapshot();
        return;
      }
      for (Entry& e : cache->entries) {
        if (e.set.load(std::memory_order_relaxed) != &set) continue;
        State expected = State::kIdle;
        if (e.state.compare_exchange_strong(expected, State::kPinned, std::memory_order_acq_rel)) {
          pinned_ = &e;
          return;
        }
        // Pinned by an outer call on this thread (a sink logging through its own logger), or
        // being revoked: this call takes the locked path.
        owned_ = set.snapshot();
        return;
      }
      if (Entry* e = take_slot(*cache)) {
        // Order matters: the set is published before the list is read under the lock, so a
        // replacement that runs after the read finds the entry and revokes it.
        e->set.store(&set, std::memory_order_relaxed);
        {
          std::shared_lock lock(set.mutex_);
          e->list = set.list_;
        }
        pinned_ = e;
        return;
      }
      owned_ = set.snapshot();
    }
    ~Use() {
      if (pinned_ == nullptr) return;
      State expected = State::kPinned;
      if (pinned_->state.compare_exchange_strong(expected, State::kIdle,
                                                 std::memory_order_acq_rel)) {
        return;
      }
      // Revoked while in use: this call clears the entry. The list may be the last owner of
      // a replaced sink, so its destructor runs here, after the sinks were called.
      pinned_->list.reset();
      pinned_->set.store(nullptr, std::memory_order_relaxed);
      pinned_->state.store(State::kFree, std::memory_order_release);
    }
    Use(const Use&) = delete;
    Use& operator=(const Use&) = delete;

    const SinkList& list() const noexcept { return pinned_ != nullptr ? *pinned_->list : *owned_; }

   private:
    // An empty slot, pinned, else an idle one with its old list dropped; null if every slot is
    // pinned by an outer call.
    static Entry* take_slot(Cache& cache) {
      for (Entry& e : cache.entries) {
        State expected = State::kFree;
        if (e.state.compare_exchange_strong(expected, State::kPinned, std::memory_order_acq_rel)) {
          return &e;
        }
      }
      for (std::size_t i = 0; i < kCachedSets; ++i) {
        Entry& e = cache.entries[(cache.next_victim + i) % kCachedSets];
        State expected = State::kIdle;
        if (e.state.compare_exchange_strong(expected, State::kPinned, std::memory_order_acq_rel)) {
          cache.next_victim = (cache.next_victim + i + 1) % kCachedSets;
          e.set.store(nullptr, std::memory_order_relaxed);
          e.list.reset();
          return &e;
        }
      }
      return nullptr;
    }

    Entry* pinned_ = nullptr;
    std::shared_ptr<const SinkList> owned_;
  };

 private:
  // Clears this set's entry in every thread's cache, or marks it for the call using it.
  void revoke() noexcept {
    Registry& registry = caches();
    std::lock_guard lock(registry.mutex);
    for (Cache* cache : registry.caches) {
      for (Entry& e : cache->entries) {
        if (e.set.load(std::memory_order_acquire) != this) continue;
        State state = e.state.load(std::memory_order_acquire);
        for (;;) {
          if (state == State::kIdle) {
            if (e.state.compare_exchange_weak(state, State::kRevoking, std::memory_order_acq_rel)) {
              e.list.reset();  // not the last owner: the caller still holds the retired list
              e.set.store(nullptr, std::memory_order_relaxed);
              e.state.store(State::kFree, std::memory_order_release);
              break;
            }
          } else if (state == State::kPinned) {
            if (e.state.compare_exchange_weak(state, State::kPinnedRevoked,
                                              std::memory_order_acq_rel)) {
              break;
            }
          } else {
            break;  // free, already marked, or being cleared by its own call
          }
        }
      }
    }
  }

  // Every live thread cache, so a replacement can reach them all. Never destroyed: a thread
  // may end after the statics are gone.
  struct Registry {
    std::mutex mutex;
    std::vector<Cache*> caches;
  };
  static Registry& caches() {
    static Registry* const registry = new Registry;
    return *registry;
  }

  // The cache is heap-allocated and reached through a trivial thread_local, so a log call made
  // while the thread's own thread_local objects are being destroyed finds no cache and takes
  // the locked path instead of touching a dead object. The reaper, a thread_local with a
  // destructor, empties and unregisters the cache when the thread ends. The entries are
  // released while the cache is still registered and the registry lock is held, so a
  // replacement running at the same moment either finds them itself or finds them gone; never
  // a cache it cannot see that still holds the old list. An idle entry's list is also owned by
  // its live set (or by the replacement in progress), so releasing it here destroys no sink
  // under the lock.
  struct ThreadSlot {
    Cache* cache = nullptr;
    bool ended = false;
  };
  static ThreadSlot& thread_slot() noexcept {
    static constinit thread_local ThreadSlot slot{};
    return slot;
  }
  struct CacheReaper {
    ~CacheReaper() {
      ThreadSlot& slot = thread_slot();
      Cache* const cache = std::exchange(slot.cache, nullptr);
      slot.ended = true;
      if (cache == nullptr) return;
      {
        Registry& registry = caches();
        std::lock_guard lock(registry.mutex);
        for (Entry& e : cache->entries) {
          State expected = State::kIdle;
          if (e.state.compare_exchange_strong(expected, State::kRevoking,
                                              std::memory_order_acq_rel)) {
            e.list.reset();
            e.set.store(nullptr, std::memory_order_relaxed);
            e.state.store(State::kFree, std::memory_order_release);
          }
        }
        std::erase(registry.caches, cache);
      }
      delete cache;  // empty by now, unless a call was in flight on a thread that is ending
    }
  };
  static Cache* thread_cache() {
    ThreadSlot& slot = thread_slot();
    if (slot.cache == nullptr && !slot.ended) {
      static thread_local CacheReaper reaper;  // registered now, runs when the thread ends
      (void)reaper;
      auto cache = std::make_unique<Cache>();
      Registry& registry = caches();
      std::lock_guard lock(registry.mutex);
      registry.caches.push_back(cache.get());
      slot.cache = cache.release();
    }
    return slot.cache;
  }

  static std::shared_ptr<const SinkList> share(SinkList sinks) {
    std::erase(sinks, nullptr);  // a null sink is a programming error; dropping it beats a crash
    return std::make_shared<const SinkList>(std::move(sinks));
  }

  mutable std::shared_mutex mutex_;
  std::shared_ptr<const SinkList> list_;
};

}  // namespace detail

}  // namespace etch
