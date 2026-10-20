# etch

A small, header-only C++23 logger. It uses the standard library plus three operating-system calls for console colour detection, and nothing else. It is a hobby project built to try the C++23 library in anger: `std::format` and `std::print`, `std::source_location`, `std::stacktrace`, `std::expected`, `std::jthread` with stop tokens, the C++20 time zone database, and a bounded ring queue with no locks on the producer path, written against the C++ memory model. There is a demo that shows every feature, a test suite under CTest, a benchmark, and a script that injects bugs to check the tests have teeth.

**Status:** logger, sinks, async queue, demo, tests and benchmark are done and pass on Windows (MSVC 2026 Insiders). Linux is set up in CMake but untested until WSL is installed.

## Build and run

Needs CMake 3.28 or newer (4.2 for the Visual Studio 2026 preset) and a compiler with the C++23 library: MSVC 2022 17.10 or newer, GCC 14 or newer, or Clang with libc++ 20 or newer (18 and 19 need `-fexperimental-library` for `std::jthread`; without a time zone database timestamps fall back to UTC with a `Z` suffix).

```bash
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
build\windows\Debug\etch_demo.exe
```

On Linux use the `linux` preset in the same three commands and run `build/linux/etch_demo`. `--preset windows-release` builds optimised binaries into `build\windows\Release`, which is where `etch_bench.exe` is worth running.

The demo prints one line per severity in colour, four loggers with their own name colours, formatted values, a hexdump, a scope timer, a filtered line, writes a rotating log file and an async burst from four threads, and ends with a fatal line carrying a stacktrace. `--no-colour` turns colour off, `--colour` forces it on when piping to a file, `--burst N` sets the lines per thread in the async section. The log files land in the current directory as `etch-demo.log`, `etch-demo.log.1`, `etch-demo.log.2` and `etch-demo.async.log`.

To use it in a project, add the folder with `add_subdirectory` and link `etch::etch`, or copy `include/etch` into your include path (on GCC, link `stdc++exp` yourself for `std::stacktrace`).

## What it looks like

```cpp
#include <etch/etch.hpp>

etch::info("hello {}", "world");                          // default logger: "main", console

etch::Logger log{"game", {std::make_shared<etch::ConsoleSink>()}};
log.warn("fps dropped to {:.1f}", fps);                   // file:line captured, no macro
log.hexdump(etch::Level::kDebug, "packet", buffer);       // any contiguous container
auto timer = log.scope("load level");                     // "load level: done in 12.3 ms"
log.fatal("out of memory");                               // highlighted, flushed, stacktrace
```

```
2026-10-20 19:38:13.091 INFO  [demo] [30392] demo.cpp:84: frame 1442 took 16.67 ms
2026-10-20 19:38:13.091 WARN  [demo] [30392] demo.cpp:88: runtime level from data: code 31
2026-10-20 19:38:13.091 INFO  [net] [30392] demo.cpp:75: hello from the net subsystem
2026-10-20 19:38:13.091 DEBUG [demo] [30392] demo.cpp:94: PacketHeader (24 bytes)
  0000  45 43 54 48 02 00 01 80  39 05 00 00 68 65 6c 6c  |ECTH....9...hell|
  0010  6f 2c 20 77 6f 72 6c 64                           |o, world|
2026-10-20 19:38:13.136 DEBUG [demo] [30392] demo.cpp:102: pretend to load a level: done in 45.489 ms
2026-10-20 19:38:13.454 FATAL [demo] [30392] demo.cpp:176: simulated fatal error, exit code would be 3
  stacktrace:
    examples\demo.cpp(176): etch_demo!`anonymous namespace'::section_fatal+0xBF
    examples\demo.cpp(197): etch_demo!main+0x33E
```

In a terminal the timestamp, thread and location are dimmed, the level tag is coloured, each logger name gets a stable colour of its own (hashed from the name), warnings are yellow, errors red, and a fatal line is bold red on a red tag. Only the sixteen standard colours are used, so it looks the same in Windows Terminal, conhost and any Linux terminal. The whole palette is one struct on the sink's `LineStyle`, so every colour can be changed.

## What it does

- **Six levels**, `trace` to `fatal`, with a runtime level per logger and per sink, and a compile-time floor (`ETCH_MIN_LEVEL`) that removes the call bodies below it from the binary. The argument expressions are still evaluated by the caller; wrap an expensive one in `if (log.enabled(...))`. The macro must have one value across the whole program.
- **`std::format` messages**, checked at compile time: a placeholder that does not match its argument is a compile error, not a runtime surprise.
- **File and line without macros.** `log.info("x = {}", x)` knows where it was called from. The format string type carries a `std::source_location` taken as a default argument of a `consteval` constructor.
- **Sinks:** console (ANSI colour, auto-detected, honours `NO_COLOR`), file (append, size-based rotation keeping N old files, I/O failures counted rather than thrown), memory (the last N records, for tests and crash dumps) and a callback for anything else. A logger can have any number of sinks, sinks can be shared between loggers, and the sink list can be replaced while other threads are logging.
- **Async sink:** a bounded ring queue and one worker thread in front of any other sink. A log call claims a slot with one compare-and-swap and moves the record in; it takes no lock and allocates nothing (a successful call may still briefly take a mutex to wake a sleeping worker). When the queue is full it either blocks the caller (lose nothing) or drops the record and counts it (never blocks a game loop on a full queue). `flush()` waits for exactly the records logged before it, not for the queue to go idle, and returns when the worker finishes the batch holding them. Destroying it drains the queue first, so nothing logged before shutdown is lost. A sink that throws costs that one record and is counted; the worker carries on.
- **Hexdump** of any memory in the classic hex editor layout: offset, sixteen bytes with a gap after eight, ASCII column.
- **Scope timer:** `auto t = log.scope("name")` logs when it begins and how long it took when it ends.
- **Fatal** appends a `std::stacktrace` and flushes every sink. It does not abort; that is the caller's decision.
- **Errors come back as values.** Opening a file sink returns `std::expected` with the OS error code. After that, file and console I/O never throw out of a log call: refused writes, flushes, closes and rotations are counted on the sink (`write_failures()`) and it keeps trying, reopening a file it lost. Formatting, allocation and a user callback can still throw, and that exception reaches the caller of the log call; the async sink is the exception and catches, counts and carries on.

What it does not do: defer formatting (the message is formatted on the calling thread, about 120 ns), guarantee anything about power loss, or let a sink's callback log back into the same sink (every sink holds a non-recursive mutex while writing, so that deadlocks; it is documented on `Sink`).

## Who reads what

The same logger serves two readers. A person scans a terminal or a log file after the fact. An agent (Claude Code, Codex, a CI job, a test) reads the output programmatically and must not depend on colour or timing. Set the project up so each gets what it needs.

**For a human reviewer**, the console sink in a terminal is the view, and these are the parts worth attention:

- **`warn` and above.** Set the console sink's level to `warn` once things work, and keep `info` for the file. A tinted message means something went wrong; a bold red one with a stacktrace means the process is about to stop.
- **The logger name colour.** Four subsystems logging at once are told apart by colour before you read a word. Give each subsystem its own `Logger` with a short name.
- **Scope timers** (`log.scope("load level")`) for anything that might be slow. The "done in" line is the first place to look when a frame hitches.
- **Hexdumps** when a parser or a network packet misbehaves. The ASCII column shows at a glance whether you are looking at text, a header or garbage.
- **The counters** when logging itself is suspect: `dropped()` on an async sink, `write_failures()` on a file or console sink, `rotations()` on a file sink. A non-zero number is the explanation for a missing line.

**For an agent**, use the parts that are exact and machine-checkable, and avoid the parts made for eyes:

- **`MemorySink`** in tests. `records()` gives the level, time, thread, source location and message of each record as data; `messages()` gives just the text. Assert on those, not on console output. That is how the suite in `tests/tests.cpp` works, and it is the fastest sink (about 200 ns a call).
- **`CallbackSink`** to route records into anything else: a test probe, a channel to another process, a JSON writer. The callback runs under the sink's mutex and must not log back into the same sink.
- **A file sink with colour off** (it is off by default) when output must survive the process, and a `LineStyle` with `utc = true` so timestamps compare across machines. Lines are LF-terminated on every platform and the layout is fixed: `time LEVEL [logger] [thread] file:line: message`. The level column is always five characters, so it can be sliced, and `parse_level` reads it back.
- **`NO_COLOR=1`** or `ConsoleSink::ColourMode::kNever` when capturing the console in a pipe, so no escape sequences land in the capture. The demo's `--no-colour` flag does the same.
- **Levels as data.** `log.log(level, ...)` takes the level at runtime, and `std::formatter<Level>` prints it, so a severity mapped from an error code needs no switch.
- **Never scrape the colour or the thread id.** Both are for people; the thread id differs per run and per platform.

For an agent working on etch itself: `ctest --preset windows` is the oracle, `python tools/mutate.py` checks the oracle has teeth, and `etch_bench` (Release) is how a performance claim is backed. A change to a header is done when all three say so.

## How it works

- **Format once, on the calling thread.** A log call formats the message with `std::vformat`, stamps it with the time, thread id, source location and logger name, and hands that `Record` to each sink. The record owns its strings, which is what lets the async sink move it to another thread safely. With one sink, the record is moved, not copied.
- **Sinks format the line, loggers format the message.** The columns (time, level, logger, thread, location) belong to the sink, so the console can be terse while the file keeps everything. Each sink formats into one reused buffer under its mutex, so a steady stream of lines allocates nothing.
- **The async queue is Vyukov's bounded MPMC ring**, used as multi-producer single-consumer. A producer claims a slot with one compare-and-swap and moves its record in, with no lock and no allocation. (Vyukov's design is not formally lock-free: a producer pre-empted between claiming and publishing a slot holds up the consumer until it resumes. In practice that is a few nanoseconds.) The worker takes what is ready in sweeps of at most 1,024 records, a bound that keeps its memory fixed and keeps a fast producer from feeding it forever. A mutex exists only for the slow paths: putting the worker to sleep (after a short bounded spin, so bursts never pay a wake-up per message), blocking a producer on a full queue, and waiting in `flush()`. The sleep handshake is the usual announce-then-recheck with sequentially consistent operations, and the full-queue handshake uses a pair of fences, so no wake-up can be lost.
- **No logger lock is held while sinks run.** The sink list is an immutable snapshot; a log call copies its pointer under a shared lock that is released before any sink is called. A callback can therefore replace the sinks, and a replaced async sink joins its worker outside any lock.
- **Colour on Windows without `<windows.h>`.** Enabling ANSI escape processing needs three Win32 calls. They are declared by hand, with the exact signatures, so the header does not pull a quarter of a million lines and the `min`/`max` macros into every file that logs.
- **The timestamp is cached per second.** Formatting a `std::chrono::zoned_time` costs about 16 microseconds on MSVC's standard library, more than everything else in a log line put together. The date and time down to the second change once a second, so each thread keeps that prefix in a plain char array (no destructor, so it is still valid when a static object logs from its destructor) and only formats the milliseconds per call.
- **The default logger is never destroyed**, on purpose, so objects with static storage can log from their destructors. An `atexit` handler flushes its sinks.
- **`std::print` for console output.** It is the only standard way to get UTF-8 onto a Windows console correctly; `printf` and `std::cout` mangle it.

## Performance

Measured with `etch_bench` on an AMD Ryzen 7 7800X3D, Windows 11, MSVC 2026 Insiders, Release build. These are the costs of one log call on the calling thread, averaged over 200,000 calls; run it yourself to get your own numbers.

| | per call |
|---|---|
| Format the message (`std::format`, two placeholders) | 118 ns |
| Log to a sink that does nothing | 152 to 174 ns |
| Log below the level | 1 ns |
| Log to the memory sink | about 210 ns |
| Log through the async sink into a file | about 180 to 210 ns (about 770 ns per line end to end, measured on the worker side) |
| Four threads logging at once through the async sink | about 520 to 540 ns wall time per call per thread |
| Four threads, 100,000 lines, 8.3 MB file (the demo) | about 60 ms end to end, nothing dropped |

Before the two changes described above the full line took 15.8 µs to format and a synchronous file write 20 µs; the mutex-and-condition-variable queue cost the caller 400 to 600 ns alone and 1.7 µs under four-thread contention.

Not measured: Linux, GCC, tail latency (the benchmark reports averages), or the console sink (it flushes every line on purpose, so it is bound by the terminal).

## How it is checked

Forty-nine tests, written against a tiny harness in `tests/check.hpp` so the suite needs no framework. CTest runs the suite twice: once as built, and once with `ETCH_MIN_LEVEL=3` so the compile-time floor is exercised for real rather than assumed. Each run uses its own scratch directory, so the two can run in parallel.

- **Exact output.** The line layout, the hexdump (including the last partial line and the wide offsets past 64 KiB), every colour escape, the name palette, and the duration formatting are compared against exact strings.
- **Source location.** The captured line equals the `__LINE__` of the call, and the file column is the base name.
- **Time.** UTC rendering of a fixed instant is exact; local time differs only by the zone; the per-second cache is driven across a second, minute, hour and day boundary and across a UTC/local switch in the same second.
- **File sink.** Lines on disk equal the lines logged byte for byte and the file size equals the sink's byte count (binary mode, so no CRLF surprise); append across two opens counts the old bytes; rotation at a small size keeps every line across the numbered files, drops the oldest beyond `max_files`, and with `max_files = 0` just restarts the file; a rotation whose rename is refused (on Windows, another handle holds the target open) counts the failure and keeps every line in the live file; a missing directory comes back as an error value.
- **Async.** Eight threads log 4,000 lines through a 64-slot queue: all arrive and each thread's lines stay in order. With the drop policy, dropped plus delivered equals sent. `flush()` and the destructor both wait for the queue to drain, and `flush()` returns while another thread keeps logging. The blocking policy never drops. The inner sink's level is honoured. A sink that throws is counted and the records after it still arrive.
- **Logger.** Runtime filter, sink filter, compile-time floor, `emit` honouring the level, name round trips, lenient parsing (`warning`, `err`, `INFO `), null sinks ignored, `scope()` rejected on a temporary logger (a concept checked at compile time), sinks replaced 600 times while four threads log, and a callback that adds a sink from inside a log call.

**Tests against deliberate bugs.** `tools/mutate.py` injects nineteen bugs one at a time (level filters off by one in the logger and the sink, the parser rejecting full names, the time cache ignoring the UTC flag or the second, the location column printing the full path, a lost warn colour, the memory sink keeping one record too many, rotation deleting the live file, rotation never happening, text-mode file output, uncounted drops, the async sink ignoring the inner level, an uncounted sink exception, `flush` waiting for idleness, the hexdump losing its gap, the scope timer's message changing, null sinks kept, and `emit` skipping the level check), rebuilds the tests and runs them. The suite catches every one, each with at least one failing test that names the broken behaviour. Run it after `cmake --preset windows`:

```bash
python tools/mutate.py
```

**Second opinion.** The code was reviewed by GPT-6-Astra, run through the Codex CLI at extra-high reasoning effort as a hostile senior engineer, after the first version. It found the rotation path that could truncate a file it had failed to rename, silent write failures, an exception from a sink taking the worker thread down, the thread-local timestamp cache being destroyed before static destructors run, data races on sink level and style, the logger lock being held while calling sinks, text-mode CRLF breaking the byte count, `flush` waiting for traffic that came after it, the async wrapper ignoring the inner sink's level, and a default-argument pattern GCC rejects. All of those are fixed. Most have a direct test (the refused rotation, the physical file size, the inner sink's level, the throwing sink, the flush barrier under load, sinks swapped under load); the teardown and data-race fixes are by construction and were re-reviewed rather than tested. Two of its points became documented contracts instead of code (callback reentrancy, one `ETCH_MIN_LEVEL` per program) and one is on the roadmap (deferred formatting).

A second round on the fixed code confirmed that both wake-up handshakes in the queue are correctly ordered under the C++ memory model, and found the worker's sweep being unbounded (a fast producer could refill slots as fast as the worker freed them, so it would collect forever and never write), a signed overflow in the slot arithmetic after 2^63 records, close-time failures hidden by rotation, a failed reopen disabling the file sink for good, console flush failures going uncounted, the UTC-fallback marker landing before the milliseconds, and `std::bit_ceil` on an absurd capacity. All of those are fixed; the one remaining contract is that a sink behind the async sink must not drop the async sink's last reference from inside a callback.

## Layout

```
include/etch/etch.hpp        include this; pulls in the rest
include/etch/level.hpp       Level, names, parsing, ETCH_MIN_LEVEL, std::formatter<Level>
include/etch/record.hpp      Record, Palette, LineStyle, timestamp cache, format_line
include/etch/sinks.hpp       Sink, ConsoleSink, FileSink, MemorySink, CallbackSink
include/etch/async_sink.hpp  AsyncSink: lock-free ring + worker thread
include/etch/logger.hpp      Logger, FormatWithLocation, hexdump, ScopeTimer, default logger
examples/demo.cpp            every feature, in order
bench/bench.cpp              what one call costs the calling thread
tests/check.hpp              the test harness (TEST, CHECK, CHECK_EQ)
tests/tests.cpp              the suite
tools/mutate.py              tests against deliberate bugs
```

## Built with

Written by Claude (**Fable 5.1**, model id `claude-fable-5-1`, run at high effort) in Claude Code. Reviewed by **GPT-6-Astra** (model id `gpt-6-astra`, extra-high reasoning effort) through the Codex CLI, as a second opinion whose findings Claude then fixed. Directed and tested by David. Commits carry a `Co-Authored-By` trailer naming the model that wrote them, so the history shows which model made each change.

## License

All rights reserved. The source is public to read and evaluate, not to reuse. See [LICENSE](LICENSE).

## Roadmap

1. ~~Logger, sinks, async, demo, tests~~
2. ~~Lock-free queue for the async sink, measured against the mutex one~~
3. ~~Colour palette, per-logger name colours~~
4. Build and test on Linux (WSL, GCC 14)
5. Deferred formatting: queue the arguments instead of the formatted text, so the caller pays only a copy
6. Structured output (JSON lines) as a sink option
