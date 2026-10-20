// etch - AsyncSink: a bounded queue and a worker thread in front of any other sink.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

#include "sinks.hpp"

namespace etch {

// Moves the cost of writing (disk, console) off the logging thread. The record is already
// formatted when it arrives, so the queue only carries finished strings.
//
// When the queue is full the overflow policy decides: block the caller until there is room
// (never lose a line), or drop the new record and count it (never stall a game loop).
// dropped() tells you how many were lost.
//
// Destroying the sink stops the worker after it has drained the queue, so nothing logged before
// the destructor is lost. flush() waits for the queue to empty, then flushes the inner sink.
class AsyncSink final : public Sink {
 public:
  enum class Overflow { kBlock, kDrop };
  static constexpr std::size_t kDefaultCapacity = 8192;

  explicit AsyncSink(std::shared_ptr<Sink> inner, std::size_t capacity = kDefaultCapacity,
                     Overflow policy = Overflow::kBlock)
      : inner_(std::move(inner)),
        capacity_(capacity > 0 ? capacity : 1),
        policy_(policy),
        worker_([this](std::stop_token st) { run(st); }) {}

  ~AsyncSink() override {
    worker_.request_stop();  // wakes the worker through its stop_token-aware wait
    worker_.join();
    inner_->flush();
  }

  AsyncSink(const AsyncSink&) = delete;
  AsyncSink& operator=(const AsyncSink&) = delete;

  void write(const Record& r) override {
    std::unique_lock lock(mutex_);
    if (queue_.size() >= capacity_) {
      if (policy_ == Overflow::kDrop) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      not_full_.wait(lock, [&] { return queue_.size() < capacity_; });
    }
    queue_.push_back(r);
    lock.unlock();
    not_empty_.notify_one();
  }

  void flush() override {
    std::unique_lock lock(mutex_);
    drained_.wait(lock, [&] { return queue_.empty() && !writing_; });
    lock.unlock();
    inner_->flush();
  }

  std::size_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
  std::size_t queued() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
  }
  std::size_t capacity() const noexcept { return capacity_; }
  const std::shared_ptr<Sink>& inner() const noexcept { return inner_; }

 private:
  void run(std::stop_token st) {
    std::unique_lock lock(mutex_);
    for (;;) {
      // Returns false only when stop was requested AND the queue is empty: drain, then exit.
      if (!not_empty_.wait(lock, st, [&] { return !queue_.empty(); })) break;
      Record r = std::move(queue_.front());
      queue_.pop_front();
      writing_ = true;
      lock.unlock();
      not_full_.notify_one();
      inner_->write(r);
      lock.lock();
      writing_ = false;
      if (queue_.empty()) drained_.notify_all();
    }
  }

  std::shared_ptr<Sink> inner_;
  std::size_t capacity_;
  Overflow policy_;
  std::deque<Record> queue_;
  bool writing_ = false;  // a record is out of the queue but not yet written
  std::atomic<std::size_t> dropped_{0};
  mutable std::mutex mutex_;
  std::condition_variable_any not_empty_;  // _any: can wait on a stop_token
  std::condition_variable_any not_full_;
  std::condition_variable_any drained_;
  std::jthread worker_;  // last member: constructed last, so everything it uses already exists
};

}  // namespace etch
