// etch - the bookkeeping behind AsyncSink: the process-wide registry of live async sinks,
// what a fork() does to them, the worker thread's own markers, and naming the worker.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#if !defined(_WIN32)
#include <pthread.h>  // pthread_atfork; on Linux also pthread_setname_np (a GNU extension)
#if defined(__linux__)
#include <unistd.h>  // gettid

#include <sys/resource.h>  // setpriority: the worker's nice value
#endif
#endif

namespace etch {

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
  std::atomic<AsyncSink*> sink{nullptr};  // null once the sink is gone
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

// Counts the forks this process has been through, so a sink can tell whether it was made in
// this process or copied from a parent by fork(): only the forking thread exists in the child,
// so a copied sink's worker is gone while its state says it runs, and its locks and condition
// variables are copies of whatever the parent held. A sink made after the fork is an ordinary
// sink. Never changes on Windows, which has no fork.
inline std::atomic<unsigned>& fork_generation() noexcept {
  static std::atomic<unsigned> generation{0};
  return generation;
}
inline void install_fork_handler() noexcept {
#if !defined(_WIN32)
  static std::atomic<bool> installed{false};
  if (installed.exchange(true)) return;  // once per process, whoever asks first
  (void)pthread_atfork(nullptr, nullptr, [] { fork_generation().fetch_add(1); });
#endif
}

// True on an async sink's worker thread. A worker never waits for a slot in a ring: two
// workers each waiting on the other's full ring would wait forever (each is the one that
// would free the other's slots), so a record a worker logs into a full ring goes straight
// through instead.
inline bool& on_async_worker() noexcept {
  static constinit thread_local bool on_worker = false;
  return on_worker;
}

// Moves whenever any async worker wakes, writes a batch or flushes. prepare_fork() reads it
// around a pass over the sinks: a pass that found every worker idle while the count stood
// still proves they were all idle at once, which no number of independent looks could.
inline std::atomic<std::uint64_t>& worker_activity() noexcept {
  static std::atomic<std::uint64_t> activity{0};
  return activity;
}

#if defined(_WIN32)
#ifndef _WINDOWS_
// Declared by hand so <windows.h> stays out, with the SDK's exact types (HANDLE = void*,
// HRESULT = long, PCWSTR = const wchar_t*), so a <windows.h> included after etch sees the
// same declarations. SetThreadDescription exists from Windows 10 1607, which is older than
// anything that builds C++23, so it is linked directly rather than looked up.
extern "C" {
__declspec(dllimport) void* __stdcall GetCurrentThread();
__declspec(dllimport) long __stdcall SetThreadDescription(void*, const wchar_t*);
__declspec(dllimport) int __stdcall SetThreadPriority(void*, int);
}
#endif
// THREAD_PRIORITY_ABOVE_NORMAL: one step above the process's normal priority class, where
// the worker still yields to anything time-critical but no longer waits behind the threads
// that fill its ring.
inline constexpr int kThreadPriorityAboveNormal = 1;
#endif
// Raises the calling thread one step above its siblings: an async worker is one thread
// among all the ones logging into it, and under CPU load a fair share is not enough for it
// to keep up (the server testbed lost 97% of an agent log that way). Windows allows it to any
// process. Linux needs CAP_SYS_NICE or an RLIMIT_NICE above the default to lower a nice value,
// so there it is tried and usually refused; the caller learns which from the result. Other
// systems: not attempted.
inline bool raise_this_thread_priority() noexcept {
#if defined(_WIN32)
  return SetThreadPriority(GetCurrentThread(), kThreadPriorityAboveNormal) != 0;
#elif defined(__linux__)
  // PRIO_PROCESS with a thread id names one thread on Linux. -5 is modest: enough to win
  // against normal threads, far from the real-time range.
  constexpr int kWorkerNice = -5;
  return setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), kWorkerNice) == 0;
#else
  return false;
#endif
}

// Names the calling thread for debuggers and profilers. Best effort: the result is ignored.
inline void name_this_thread(const char* ascii_name) noexcept {
#if defined(_WIN32)
  wchar_t wide[32] = {};
  for (std::size_t i = 0; i + 1 < sizeof(wide) / sizeof(wide[0]) && ascii_name[i] != 0; ++i) {
    wide[i] = static_cast<wchar_t>(ascii_name[i]);
  }
  (void)SetThreadDescription(GetCurrentThread(), wide);
#elif defined(__linux__)
  pthread_setname_np(pthread_self(), ascii_name);  // at most 15 characters on Linux
#else
  (void)ascii_name;
#endif
}

}  // namespace detail

}  // namespace etch
