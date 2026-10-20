# etch

A small, header-only C++23 logger with no dependencies beyond the standard library. It is a hobby project built to try the C++23 library in anger: `std::format` and `std::print`, `std::source_location`, `std::stacktrace`, `std::expected`, `std::jthread` with stop tokens, and the C++20 time zone database. There is a demo that shows every feature and a test suite that runs under CTest.

**Status:** the logger, the demo and the tests are done and pass on Windows (MSVC 2026 Insiders). Linux is set up in CMake but untested until WSL is installed.

## Build and run

Needs CMake 3.28 or newer and a compiler with the C++23 library: MSVC 2022 17.10 or newer, or GCC 14 or newer.

```bash
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
build\windows\Debug\etch_demo.exe
```

On Linux use the `linux` preset in the same three commands and run `build/linux/etch_demo`. `--preset windows-release` builds optimised binaries into `build\windows\Release`.

The demo prints one line per severity in colour, formatted values, a hexdump, a scope timer, a filtered line, writes a rotating log file and an async burst from four threads, and ends with a fatal line carrying a stacktrace. `--no-colour` turns colour off, `--colour` forces it on when piping to a file, `--burst N` sets the lines per thread in the async section. The log files land in the current directory as `etch-demo.log`, `etch-demo.log.1`, `etch-demo.log.2` and `etch-demo.async.log`.

To use it in a project, add the folder with `add_subdirectory` and link `etch::etch`, or copy `include/etch` into your include path.

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
2026-10-20 18:36:18.079 INFO  [demo] [7948] demo.cpp:73: frame 1442 took 16.67 ms
2026-10-20 18:36:18.079 WARN  [demo] [7948] demo.cpp:74: runtime level from data: code 31
2026-10-20 18:36:18.079 DEBUG [demo] [7948] demo.cpp:80: PacketHeader (24 bytes)
  0000  45 43 54 48 02 00 01 80  39 05 00 00 68 65 6c 6c  |ECTH....9...hell|
  0010  6f 2c 20 77 6f 72 6c 64                           |o, world|
2026-10-20 18:36:18.116 DEBUG [demo] [7948] demo.cpp:88: pretend to load a level: done in 36.371 ms
2026-10-20 18:36:18.454 FATAL [demo] [7948] demo.cpp:159: simulated fatal error, exit code would be 3
  stacktrace:
    examples\demo.cpp(159): etch_demo!`anonymous namespace'::section_fatal+0xBF
    examples\demo.cpp(180): etch_demo!main+0x33E
```

## What it does

- **Six levels**, `trace` to `fatal`, with a runtime level per logger and per sink, and a compile-time floor (`ETCH_MIN_LEVEL`) that removes calls below it from the binary.
- **`std::format` messages**, checked at compile time: a placeholder that does not match its argument is a compile error, not a runtime surprise.
- **File and line without macros.** `log.info("x = {}", x)` knows where it was called from. The format string type carries a `std::source_location` taken as a default argument of a `consteval` constructor.
- **Sinks:** console (ANSI colour, auto-detected, honours `NO_COLOR`), file (append, size-based rotation keeping N old files), memory (the last N records, for tests and crash dumps) and a callback for anything else. A logger can have any number of sinks, and sinks can be shared between loggers.
- **Async sink:** a bounded queue and one worker thread in front of any other sink. When the queue is full it either blocks the caller (lose nothing) or drops the record and counts it (never stall a game loop). Destroying it drains the queue first, so nothing logged before shutdown is lost.
- **Hexdump** of any memory in the classic hex editor layout: offset, sixteen bytes with a gap after eight, ASCII column.
- **Scope timer:** `auto t = log.scope("name")` logs when it begins and how long it took when it ends.
- **Fatal** appends a `std::stacktrace` and flushes every sink. It does not abort; that is the caller's decision.
- **Errors come back as values.** Opening a file sink returns `std::expected`, so a missing directory is a return value with the OS error code, not an exception from inside a log call.

## How it works

- **Format once, on the calling thread.** A log call formats the message with `std::vformat`, stamps it with the time, thread id, source location and logger name, and hands that `Record` to each sink. The record owns its strings, which is what lets the async sink move it to another thread safely.
- **Sinks format the line, loggers format the message.** The columns (time, level, logger, thread, location) belong to the sink, so the console can be terse while the file keeps everything.
- **Colour on Windows without `<windows.h>`.** Enabling ANSI escape processing needs three Win32 calls. They are declared by hand, with the exact signatures, so the header does not pull a quarter of a million lines and the `min`/`max` macros into every file that logs.
- **The timestamp is cached per second.** Formatting a `std::chrono::zoned_time` costs about 16 microseconds on MSVC's standard library, more than everything else in a log line put together. The date and time down to the second change once a second, so each thread keeps that prefix and only formats the milliseconds per call.
- **`std::print` for console output.** It is the only standard way to get UTF-8 onto a Windows console correctly; `printf` and `std::cout` mangle it.
- **One mutex per sink, a shared mutex for the sink list.** Logging takes a shared lock, so threads log in parallel and only adding a sink waits. The async worker waits on a `std::condition_variable_any` with its `std::stop_token`, so stopping it is one call and needs no flag.

## Performance

Measured on an AMD Ryzen 7 7800X3D, Windows 11, MSVC 2026 Insiders, Release build. These are the costs of one log call, averaged over 100,000.

| | per call |
|---|---|
| Format the message (`std::vformat`) | 0.10 us |
| Format the full line, time included | 0.29 us (was 15.8 us before the timestamp cache) |
| Log to the memory sink | 0.20 us |
| Log to a file sink, synchronous | 0.78 us (was 20 us) |
| Async: four threads, 100,000 lines, 8.3 MB written | about 170 ms end to end, nothing dropped |

Not measured: Linux, GCC, or the console sink (it flushes every line on purpose, so it is bound by the terminal).

## How it is checked

Thirty-seven tests, written against a tiny harness in `tests/check.hpp` so the suite needs no framework. CTest runs the suite twice: once as built, and once with `ETCH_MIN_LEVEL=3` so the compile-time floor is exercised for real rather than assumed.

- **Exact output.** The line layout, the hexdump (including the last partial line and the wide offsets past 64 KiB), the colour escapes, and the duration formatting are compared against exact strings.
- **Source location.** The captured line equals the `__LINE__` of the call, and the file column is the base name.
- **Time.** UTC rendering of a fixed instant is exact; local time differs only by the zone; the per-second cache is driven across a second, minute, hour and day boundary and across a UTC/local switch in the same second.
- **File sink.** Lines on disk equal the lines logged byte for byte; append across two opens counts the old bytes; rotation at a small size keeps every line across the numbered files and drops the oldest beyond `max_files`; a missing directory comes back as an error value.
- **Async.** Eight threads log 4,000 lines through a 64-slot queue: all arrive and each thread's lines stay in order. With the drop policy, dropped plus delivered equals sent. `flush()` and the destructor both wait for the queue to drain. The blocking policy never drops.
- **Levels.** Runtime filter, sink filter, compile-time floor, name round trips, lenient parsing (`warning`, `err`, `INFO `).

**Tests against deliberate bugs.** Twelve bugs were injected one at a time (a level filter off by one in the logger and again in the sink, the parser rejecting full names, the timestamp cache ignoring the UTC flag, the location column printing the full path, the memory sink keeping one record too many, rotation deleting the live file instead of renaming it, rotation never happening, dropped records not being counted, the async worker taking records from the wrong end of the queue, the hexdump losing its gap, and the scope timer's message changing) and the suite caught every one, each with at least one failing test that names the broken behaviour.

## Layout

```
include/etch/etch.hpp        include this; pulls in the rest
include/etch/level.hpp       Level, names, parsing, ETCH_MIN_LEVEL, std::formatter<Level>
include/etch/record.hpp      Record, LineStyle, timestamp cache, format_line
include/etch/sinks.hpp       Sink, ConsoleSink, FileSink, MemorySink, CallbackSink
include/etch/async_sink.hpp  AsyncSink: bounded queue + worker thread
include/etch/logger.hpp      Logger, FormatWithLocation, hexdump, ScopeTimer, default logger
examples/demo.cpp            every feature, in order
tests/check.hpp              the test harness (TEST, CHECK, CHECK_EQ)
tests/tests.cpp              the suite
```

## Built with

Written by Claude (**Fable 5.1**, model id `claude-fable-5-1`, run at high effort) in Claude Code. Directed and tested by David. Commits carry a `Co-Authored-By` trailer naming the model, so the history shows which model made each change.

## License

All rights reserved. The source is public to read and evaluate, not to reuse. See [LICENSE](LICENSE).

## Roadmap

1. ~~Logger, sinks, async, demo, tests~~
2. Build and test on Linux (WSL, GCC 14)
3. A lock-free queue for the async sink, measured against the mutex one
4. Structured output (JSON lines) as a sink option
