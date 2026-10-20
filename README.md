# etch

A small, header-only C++23 logger. It uses the standard library plus three operating-system calls for console colour detection, and nothing else. It is a hobby project built to try the C++23 library in anger: `std::format` and `std::print`, `std::source_location`, `std::stacktrace`, `std::expected`, `std::jthread` with stop tokens, the C++20 time zone database, and a bounded ring queue with no locks on the producer path, written against the C++ memory model. There is a demo that shows every feature, a test suite under CTest, a benchmark, and a script that injects bugs to check the tests have teeth.

**Status:** logger, sinks, async queue, demo, tests and benchmark are done and pass on Windows (MSVC 2026 Insiders) and on Linux (Ubuntu 24.04 under WSL 2, GCC 14.2), with zero warnings on both.

## Build and run

Needs CMake 3.28 or newer (4.2 for the Visual Studio 2026 preset) and a compiler with the C++23 library: MSVC 2022 17.10 or newer, GCC 14 or newer, or Clang with libc++ 20 or newer (18 and 19 need `-fexperimental-library` for `std::jthread`; without a time zone database timestamps fall back to UTC with a `Z` suffix).

```bash
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
build\windows\Debug\etch_demo.exe
```

On Linux use the `linux` preset in the same three commands and run `build/linux/etch_demo`. The `windows-release` and `linux-release` presets build optimised binaries (into `build\windows\Release` and `build/linux-release`), which is where `etch_bench` is worth running.

The demo prints one line per severity in colour, four loggers with their own name colours, formatted values, a hexdump, a scope timer, a filtered line, writes a rotating log file, an async burst from four threads and a JSON Lines file, and ends with a fatal line carrying a stacktrace. `--no-colour` turns colour off, `--colour` forces it on when piping to a file, `--burst N` sets the lines per thread in the async section. The log files land in the current directory as `etch-demo.log`, `etch-demo.log.1`, `etch-demo.log.2`, `etch-demo.async.log` and `etch-demo.jsonl`.

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
log.with({{"fps", 41.7}, {"entities", 1204}}).warn("slow");  // structured fields
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

With `LineStyle{.format = etch::LineFormat::kJson}` on a file or console sink, the same records come out as JSON Lines, one object per record, with everything the text view compresses: the full payload behind a hexdump, every stack frame, exact durations, and the fields as a nested object.

```
{"ts":"2026-10-20T19:38:13.091Z","level":"warn","logger":"json","thread":"30392","pid":4120,"file":"demo.cpp","line":154,"msg":"frame budget exceeded","fields":{"fps":41.7,"level_name":"forest","entities":1204}}
{"ts":"2026-10-20T19:38:13.091Z","level":"debug", ... ,"msg":"PacketHeader (24 bytes)\n  0000  45 43 ...","bytes":"4543544802000180390500006865 ..."}
```

In a terminal the timestamp, thread and location are dimmed, the level tag is coloured, each logger name gets a stable colour of its own (hashed from the name), warnings are yellow, errors red, and a fatal line is bold red on a red tag. Only the sixteen standard colours are used, so it looks the same in Windows Terminal, conhost and any Linux terminal. The whole palette is one struct on the sink's `LineStyle`, so every colour can be changed.

## What it does

- **Six levels**, `trace` to `fatal`, with a runtime level per logger and per sink, and a compile-time floor (`ETCH_MIN_LEVEL`) that removes the call bodies below it from the binary. The argument expressions are still evaluated by the caller; wrap an expensive one in `if (log.enabled(...))`. The macro must have one value across the whole program.
- **`std::format` messages**, checked at compile time: a placeholder that does not match its argument is a compile error, not a runtime surprise.
- **File and line without macros.** `log.info("x = {}", x)` knows where it was called from. The format string type carries a `std::source_location` taken as a default argument of a `consteval` constructor.
- **Sinks:** console (ANSI colour, auto-detected, honours `NO_COLOR`), file (append, size-based rotation keeping N old files, I/O failures counted rather than thrown), memory (the last N records, for tests and crash dumps) and a callback for anything else. A logger can have any number of sinks, sinks can be shared between loggers, and the sink list can be replaced while other threads are logging.
- **Async sink:** a bounded ring queue and one worker thread in front of any other sink. A log call claims a slot with one compare-and-swap and moves the record in; it takes no lock and allocates nothing (a successful call may still briefly take a mutex to wake a sleeping worker). When the queue is full it either blocks the caller (lose nothing) or drops the record and counts it (never blocks a game loop on a full queue). `flush()` waits for exactly the records logged before it, not for the queue to go idle, and returns when the worker finishes the batch holding them. Destroying it drains the queue first, so nothing logged before shutdown is lost. A sink that throws costs that one record and is counted; the worker carries on.
- **Hexdump** of any memory in the classic hex editor layout: offset, sixteen bytes with a gap after eight, ASCII column. The record also keeps the bytes themselves, and the JSON line carries only those (plus the label), never the rendered dump text.
- **Structured fields.** `log.with({{"fps", 41.7}, {"name", "forest"}}).warn("slow frame")` attaches typed key/values (integer, double, bool, string). Text output appends them as `key=value`; JSON output gets a `fields` object.
- **JSON Lines output** from any file or console sink (`LineStyle::format`), with the same rotation and failure accounting as text. Each line is one object: ISO 8601 UTC timestamp, level, logger, thread, pid, file, line, message, and when present fields, `duration_ns`, `bytes` (hex) and `stack` (one string per frame). Strings are escaped so a message with a newline is still one line. `JsonStyle` turns the full source path and the function name on (both off by default: the path can name a home directory, and both cost a program tokens on every line), turns thread and pid off, or switches the timestamp to epoch milliseconds.
- **Scope timer:** `auto t = log.scope("name")` logs when it begins and how long it took when it ends.
- **Fatal** appends a `std::stacktrace` and flushes every sink. It does not abort; that is the caller's decision.
- **Errors come back as values.** Opening a file sink returns `std::expected` with the OS error code. After that, file and console I/O never throw out of a log call: refused writes, flushes, closes and rotations are counted on the sink (`write_failures()`) and it keeps trying, reopening a file it lost. Formatting, allocation and a user callback can still throw, and that exception reaches the caller of the log call; the async sink is the exception and catches, counts and carries on.

What it does not do: defer formatting (the message is formatted on the calling thread, about 120 ns), guarantee anything about power loss, or let a sink's callback log back into the same sink (every sink holds a non-recursive mutex while writing, so that deadlocks; it is documented on `Sink`).

## Who reads what

The same logger serves two readers. A person scans a terminal or a log file after the fact. An agent (Claude Code, Codex, a CI job, a test) reads the output programmatically and must not depend on colour or timing. Set the project up so each gets what it needs.

**For a human reviewer**, the console sink in a terminal is the view, and these are the parts worth attention:

- **`warn` and above.** Set the console sink's level to `warn` once things work, and keep `info` for the file. A tinted message means something went wrong; a bold red one with a stacktrace is `fatal`, which flushes everything and leaves stopping to the caller.
- **The logger name colour.** Four subsystems logging at once are told apart by colour before you read a word. Give each subsystem its own `Logger` with a short name.
- **Scope timers** (`log.scope("load level")`) for anything that might be slow. The "done in" line is the first place to look when a frame hitches.
- **Hexdumps** when a parser or a network packet misbehaves. The ASCII column shows at a glance whether you are looking at text, a header or garbage.
- **The counters** when logging itself is suspect: `dropped()` on an async sink, `write_failures()` on a file or console sink, `rotations()` on a file sink. A non-zero number is the explanation for a missing line.

**For an agent**, use the parts that are exact and machine-checkable, and avoid the parts made for eyes:

- **A JSON Lines file** (`LineStyle{.format = etch::LineFormat::kJson}` on a `FileSink`) when the output must be read by a program after the fact. Every line is one object with the data behind the event: the whole payload behind a hexdump as hex, every stack frame as a list, exact durations, the fields you attached. The text view renders those for eyes; the JSON view carries them once, as data, with the message reduced to its first line so nothing is written twice. For a model reading the log, the compact style is the one to use: `JsonStyle{.time = JsonStyle::Time::kEpochMillis, .thread = false, .pid = false}` leaves `ts`, `level`, `logger`, `file`, `line` and `msg`, about a fifth shorter than the default line and less than half of a line with path and function turned on. The absolute source path (which can contain a user name) stays out of the file unless you ask for it.
- **`MemorySink`** in tests. `records()` gives the level, time, thread, source location, message, fields, bytes, stack and duration of each record as data; `messages()` gives just the text. Assert on those, not on console output. That is how the suite under `tests/` works, and it is the cheapest sink in the benchmark (about 200 ns a call, formatting included).
- **`with()` fields** for anything a program will want to filter or aggregate on: `log.with({{"request_id", id}, {"ms", took}}).info("done")`. They survive as typed JSON values, not as text to re-parse.
- **`CallbackSink`** to route records into anything else: a test probe, a channel to another process, a JSON writer. The callback runs under the sink's mutex and must not log back into the same sink.
- **A file sink with colour off** (it is off by default) when output must survive the process, and a `LineStyle` with `utc = true` so timestamps compare across machines. Lines are LF-terminated on every platform and the layout is fixed: `time LEVEL [logger] [thread] file:line: message`. The level column is always five characters, so it can be sliced, and `parse_level` reads it back.
- **`NO_COLOR=1`** or `ConsoleSink::ColourMode::kNever` when capturing the console in a pipe, so no escape sequences land in the capture. The demo's `--no-colour` flag does the same.
- **Levels as data.** `log.log(level, ...)` takes the level at runtime, and `std::formatter<Level>` prints it, so a severity mapped from an error code needs no switch.
- **Never scrape the colour or the thread id.** Both are for people; the thread id differs per run and per platform.

For an agent working on etch itself: `ctest --preset windows` is the oracle, `python tools/mutate.py` checks the oracle has teeth, and `etch_bench` (Release) is how a performance claim is backed. A change to a header is done when all three say so.

## How it works

- **Format once, on the calling thread.** A log call formats the message with `std::vformat`, stamps it with the time, thread id, source location and logger name, and hands that `Record` to each sink. The record owns its strings, which is what lets the async sink move it to another thread safely. With one sink, the record is moved, not copied.
- **Sinks format the line, loggers format the message.** The columns (time, level, logger, thread, location) belong to the sink, so the console can be terse while the file keeps everything. Each sink formats into one reused buffer under its mutex, so a steady stream of lines allocates nothing.
- **The async queue is Vyukov's bounded MPMC ring**, used as multi-producer single-consumer. A producer claims a slot with one compare-and-swap and moves its record in, with no lock and no allocation. (Vyukov's design is not formally lock-free: a producer pre-empted between claiming and publishing a slot holds up the consumer until it resumes. The window is a few nanoseconds of the producer's own work, but a pre-emption inside it lasts as long as the scheduler says, so the guarantee is practical rather than formal.) The worker takes what is ready in sweeps of at most 1,024 records, a bound that keeps its memory fixed and keeps a fast producer from feeding it forever. A mutex exists only for the slow paths: putting the worker to sleep (after a short bounded spin, so bursts never pay a wake-up per message), blocking a producer on a full queue, and waiting in `flush()`. The sleep handshake is the usual announce-then-recheck with sequentially consistent operations, and the full-queue handshake uses a pair of fences, so no wake-up can be lost.
- **No logger lock is held while sinks run.** The sink list is an immutable snapshot; a log call copies its pointer under a shared lock that is released before any sink is called. A callback can therefore replace the sinks, and a replaced async sink joins its worker outside any lock.
- **Colour on Windows without `<windows.h>`.** Enabling ANSI escape processing needs three Win32 calls. They are declared by hand, with the exact signatures, so the header does not pull a quarter of a million lines and the `min`/`max` macros into every file that logs.
- **The timestamp is cached per second.** Formatting a `std::chrono::zoned_time` costs about 16 microseconds on MSVC's standard library, more than everything else in a log line put together. The date and time down to the second change once a second, so each thread keeps that prefix in a plain char array (no destructor, so it is still valid when a static object logs from its destructor) and only formats the milliseconds per call.
- **The default logger is never destroyed**, on purpose, so objects with static storage can log from their destructors. An `atexit` handler flushes its sinks.
- **`std::print` for console output.** It is the only standard way to get UTF-8 onto a Windows console correctly; `printf` and `std::cout` mangle it.
- **JSON is the same record, serialised.** The record carries the human text and the data side by side (message plus bytes, stack, duration, fields), so a text sink and a JSON sink on the same logger show the same event two ways. The JSON writer is a hundred lines with no dependency; the test suite checks every line it produces with a small validator and compares a fixed record against an exact expected string.

## Performance

Measured with `etch_bench` on an AMD Ryzen 7 7800X3D, Release builds: Windows 11 with MSVC 2026 Insiders, and Ubuntu 24.04 with GCC 14.2 under WSL 2 (4 virtual CPUs). These are the costs of one log call on the calling thread, averaged over 200,000 calls, with the measured values stored to a volatile so the optimiser cannot delete them; run it yourself to get your own numbers. "Burst" rows use a ring larger than the run, so they measure the enqueue alone; "sustained" rows use the default ring, so once the worker falls behind the caller waits, which is what an application that logs this hard would pay.

| per call | MSVC | GCC 14 |
|---|---|---|
| Format the message (`std::format`, two placeholders) | 112 ns | 77 ns |
| Log to a sink that does nothing | about 160 ns | about 115 ns |
| Log below the level | 1 ns | 0 ns |
| Log to the memory sink | about 220 ns | about 150 ns |
| Log through the async sink into a text file, burst (ring never fills) | about 195 ns | about 155 ns |
| Same, sustained with the default 4,096-slot ring (the caller waits whenever the worker is behind) | about 450 ns | about 390 ns |
| Same, sustained, JSON file | about 630 ns | about 925 ns |
| End to end per line including the worker's write, text file | about 460 to 570 ns | about 390 ns |
| Four threads logging at once through the async sink, measured inside each thread | about 500 ns | about 480 ns |
| Four threads, 100,000 lines, 8.3 MB file (the demo) | about 60 ms | about 41 ms |

The sustained rows are the honest ones for an application that logs flat out: the caller cannot outrun the worker for long, so its cost becomes the worker's cost per line. An application that logs in bursts with quiet time between them sees the burst row.

Most of the gap between the columns is the standard library: libstdc++'s `std::format` costs about half of MSVC's, and that is the largest single item in a call. The rest is the two environments (a VM with four virtual CPUs versus the host), which this benchmark does not separate.

Before the two changes described above the full line took 15.8 µs to format and a synchronous file write 20 µs; the mutex-and-condition-variable queue cost the caller 400 to 600 ns alone and 1.7 µs under four-thread contention.

Not measured: Clang or libc++, tail latency (the benchmark reports averages), or the console sink (it flushes every line on purpose, so it is bound by the terminal).

## How it is checked

Sixty tests (fifty-nine on Linux; the refused-rotation test relies on Windows file locking), written against a tiny harness in `tests/check.hpp` so the suite needs no framework and split by area under `tests/`. CTest runs the suite twice: once as built, and once with `ETCH_MIN_LEVEL=3` so the compile-time floor is exercised for real rather than assumed. Each run uses its own scratch directory, so the two can run in parallel. The same suite passes on MSVC and GCC 14, Debug and Release.

- **Exact output.** The line layout, the hexdump (including the last partial line and the wide offsets past 64 KiB), every colour escape, the name palette, and the duration formatting are compared against exact strings.
- **Source location.** The captured line equals the `__LINE__` of the call, and the file column is the base name.
- **Time.** UTC rendering of a fixed instant is exact; local time equals the standard library's own zoned formatting of the same instant; the per-second cache is driven across a second, minute, hour and day boundary, and the UTC and local caches are shown to be independent (a text sink and a JSON sink on one thread must not evict each other).
- **File sink.** Lines on disk equal the lines logged byte for byte and the file size equals the sink's byte count (binary mode, so no CRLF surprise); append across two opens counts the old bytes; rotation at a small size keeps every line across the numbered files, drops the oldest beyond `max_files`, and with `max_files = 0` just restarts the file; a rotation whose rename is refused (on Windows, another handle holds the target open) counts the failure and keeps every line in the live file; a missing directory comes back as an error value.
- **Async.** Eight threads log 4,000 lines through a 64-slot queue: all arrive and each thread's lines stay in order. With the drop policy, dropped plus delivered equals sent. `flush()` and the destructor both wait for the queue to drain, and `flush()` returns while another thread keeps logging. The blocking policy never drops. The inner sink's level is honoured. A sink that throws is counted and the records after it still arrive.
- **JSON.** Escaping of quotes, backslashes, control characters and UTF-8 is exact; a fixed record serialises to an exact expected line; every field type renders (non-finite doubles become `null`); fields, hexdump bytes, a scope's duration and a fatal's stack frames all appear; and every line the file sink writes passes a small JSON validator that is itself tested against malformed input.
- **Logger.** Runtime filter, sink filter, compile-time floor, `emit` honouring the level, name round trips, lenient parsing (`warning`, `err`, `INFO `), null sinks ignored, `scope()` rejected on a temporary logger (a concept checked at compile time), sinks replaced 600 times while four threads log, and a callback that adds a sink from inside a log call.

**Tests against deliberate bugs.** `tools/mutate.py` first checks that the untouched suite is green (a failing suite would "catch" every mutant for the wrong reason), then injects twenty-three bugs one at a time (level filters off by one in the logger and the sink, the parser rejecting full names, the time cache ignoring the UTC flag or the second, the location column printing the full path, a lost warn colour, the memory sink keeping one record too many, rotation deleting the live file, rotation never happening, text-mode file output, uncounted drops, the async sink ignoring the inner level, an uncounted sink exception, `flush` waiting for idleness, the hexdump losing its gap, the scope timer's message changing, null sinks kept, `emit` skipping the level check, an unescaped JSON message, JSON without its fields or duration, and text output without its fields), rebuilds the tests and runs them, each step under a timeout so a mutant that hangs is reported rather than waited for. The suite catches every one, each with at least one failing test that names the broken behaviour. Run it after `cmake --preset windows`:

```bash
python tools/mutate.py
```

**Second opinion.** The code was reviewed by GPT-6-Astra, run through the Codex CLI at extra-high reasoning effort as a hostile senior engineer, after the first version. It found the rotation path that could truncate a file it had failed to rename, silent write failures, an exception from a sink taking the worker thread down, the thread-local timestamp cache being destroyed before static destructors run, data races on sink level and style, the logger lock being held while calling sinks, text-mode CRLF breaking the byte count, `flush` waiting for traffic that came after it, the async wrapper ignoring the inner sink's level, and a default-argument pattern GCC rejects. All of those are fixed. Most have a direct test (the refused rotation, the physical file size, the inner sink's level, the throwing sink, the flush barrier under load, sinks swapped under load); the teardown and data-race fixes are by construction and were re-reviewed rather than tested. Two of its points became documented contracts instead of code (callback reentrancy, one `ETCH_MIN_LEVEL` per program) and one is on the roadmap (deferred formatting).

A third round, asked for speed and for what a model reading the logs would want, led to: two timestamp caches instead of one (a local-time text sink and a UTC JSON sink on the same thread were evicting each other every record), the JSON message reduced to its first line when the bytes or the stack are carried as data (nothing serialised twice), escaped string values in text fields, cheaper millisecond and hex rendering, a benchmark with optimisation barriers and sustained-load rows, a baseline gate and timeouts in the mutation script, and the README claims above corrected to what the measurements show. Its larger proposals (a smaller record, a per-file header with short thread ids and delta times, deferred formatting) are on the roadmap.

A second round on the fixed code confirmed that both wake-up handshakes in the queue are correctly ordered under the C++ memory model, and found the worker's sweep being unbounded (a fast producer could refill slots as fast as the worker freed them, so it would collect forever and never write), a signed overflow in the slot arithmetic after 2^63 records, close-time failures hidden by rotation, a failed reopen disabling the file sink for good, console flush failures going uncounted, the UTC-fallback marker landing before the milliseconds, and `std::bit_ceil` on an absurd capacity. All of those are fixed; the one remaining contract is that a sink behind the async sink must not drop the async sink's last reference from inside a callback.

## Layout

```
include/etch/etch.hpp        include this; pulls in the rest
include/etch/level.hpp       Level, names, parsing, ETCH_MIN_LEVEL, std::formatter<Level>
include/etch/record.hpp      Record, Palette, LineStyle, timestamp cache, format_line
include/etch/json.hpp        JSON Lines: json_escape, format_json, format_record
include/etch/sinks.hpp       Sink, ConsoleSink, FileSink, MemorySink, CallbackSink
include/etch/async_sink.hpp  AsyncSink: lock-free ring + worker thread
include/etch/logger.hpp      Logger, FormatWithLocation, hexdump, ScopeTimer, default logger
examples/demo.cpp            every feature, in order
bench/bench.cpp              what one call costs the calling thread
tests/check.hpp              the test harness (TEST, CHECK, CHECK_EQ)
tests/support.hpp            shared test helpers: capturing logger, scratch dir, fixed record
tests/test_*.cpp             the suite, one file per area (levels/logger, format, sinks, async, JSON)
tests/main.cpp               test entry point
tools/mutate.py              tests against deliberate bugs
```

## Built with

Written by Claude (**Fable 5.1**, model id `claude-fable-5-1`, run at high effort) in Claude Code. Reviewed by **GPT-6-Astra** (model id `gpt-6-astra`, extra-high reasoning effort) through the Codex CLI, as a second opinion whose findings Claude then fixed. Directed and tested by David. Commits carry a `Co-Authored-By` trailer naming the model that wrote them, so the history shows which model made each change.

## License

All rights reserved. The source is public to read and evaluate, not to reuse. See [LICENSE](LICENSE).

## Roadmap

Done so far: the logger and sinks, the lock-free async queue, colour with per-logger names, Linux (GCC 14 under WSL), JSON Lines with typed fields, three review rounds. What is left, in the order it is worth doing:

1. **Continuous integration.** A GitHub Actions matrix (MSVC, GCC 14, Clang with libc++) that configures, builds, runs CTest and the mutation script on every push. Cheap, and it turns "tested on two machines" into a claim anyone can check.
2. **Clang and libc++.** Never built. libc++ 20 should work as is; 18 and 19 need `-fexperimental-library` for `std::jthread`, and the time zone fallback path has never run for real.
3. **Benchmark percentiles.** p50, p99 and worst case per call, plus allocation counts, next to the averages. The tail is what a game loop feels; the average hides it.
4. **A run summary for agents.** One JSON object at flush and at exit: records per level and per logger, dropped records, write failures, rotations, and the first and last error with their line numbers. A model reads that first and opens the full log only when it has to.
5. **A JSON header line per file.** Schema version, pid, base timestamp and a thread-id table, so each line can carry a short thread number and a millisecond delta instead of repeating the full values. Repeated on rotation so every file stands alone.
6. **Shrink the record.** Fields, bytes, stack and duration behind one optional extras object, so the 200-byte record (and the 830 KiB default ring) drop by nearly half and the move into the queue copies less.
7. **Project-relative source paths.** A `strip_path_prefix` on `JsonStyle` so `src/net.cpp` can replace both the ambiguous base name and the absolute path.
8. **Deferred formatting.** Queue the arguments instead of the formatted text, so the caller pays only a copy. Needs an explicit snapshot rule for argument types that hold pointers or references, with eager formatting as the fallback. The biggest possible speed-up, and the biggest change; last on purpose.
