# etch

A small, header-only C++23 logger. It uses the standard library plus a handful of operating-system calls (console colour, the process id, naming the worker thread), and nothing else. It is a hobby project built to try the C++23 library in anger: `std::format` and `std::print`, `std::source_location`, `std::stacktrace`, `std::expected`, `std::jthread` with stop tokens, the C++20 time zone database, and a bounded ring queue with no locks on the producer path, written against the C++ memory model. There is a demo that shows every feature, a test suite under CTest, a benchmark, and a script that injects bugs to check the tests have teeth.

**Status:** version 0.3 on `beta`. Everything passes on Windows (MSVC 2026 Insiders) and on Linux (Ubuntu 24.04 under WSL 2, GCC 14.2), with zero warnings on both. A voxel game was built on it to find out what breaks in real use; what it found is fixed below.

## Build and run

Needs CMake 3.28 or newer (4.2 for the Visual Studio 2026 preset) and a compiler with the C++23 library: MSVC 2022 17.10 or newer, GCC 14 or newer, or Clang with libc++ 20 or newer (18 and 19 need `-fexperimental-library` for `std::jthread`; without a time zone database timestamps fall back to UTC).

```bash
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
build\windows\Debug\etch_demo.exe
```

On Linux use the `linux` preset in the same three commands and run `build/linux/etch_demo`. The `windows-release` and `linux-release` presets build optimised binaries (into `build\windows\Release` and `build/linux-release`), which is where `etch_bench` is worth running; `windows-ninja` uses Ninja with whatever compiler is on the path of a developer prompt.

The demo prints one line per severity in colour, four loggers with their own name colours, formatted values, a hexdump, a scope timer, a filtered line, writes a rotating log file, an async burst from four threads, a JSON Lines file and an agent-profile file with its run summary, and ends with a fatal line carrying a stacktrace. `--no-colour` turns colour off, `--colour` forces it on when piping to a file, `--burst N` sets the lines per thread in the async section. The log files land in the current directory as `etch-demo.log`, `etch-demo.log.1`, `etch-demo.log.2`, `etch-demo.async.log` and `etch-demo.jsonl`.

**Using it in a project.** Straight from GitHub:

```cmake
include(FetchContent)
FetchContent_Declare(etch GIT_REPOSITORY https://github.com/DavidNyholm/etch.git GIT_TAG main)
FetchContent_MakeAvailable(etch)
target_link_libraries(app PRIVATE etch::etch)
```

Or `add_subdirectory` on a checkout, or `cmake --install` and then `find_package(etch 0.3 REQUIRED)`, or copy `include/etch` into your include path (on GCC, link `stdc++exp` yourself for `std::stacktrace`). The target carries `/utf-8` to MSVC consumers, because `std::print` only writes non-ASCII text to the Windows console correctly under that flag (`ETCH_MSVC_UTF8=OFF` opts out). `-DETCH_MIN_LEVEL=3` on the configure line compiles trace, debug and info out of every consumer at once, which is the only safe way to set it: the value must be the same in every translation unit of a program.

## What it looks like

```cpp
#include <etch/etch.hpp>

etch::info("hello {}", "world");                          // default logger: "main", console
etch::get("render").warn("fps {:.1f}", fps);              // a named logger sharing its sinks

etch::Logger log{"game", {std::make_shared<etch::ConsoleSink>()}};
log.warn("fps dropped to {:.1f}", fps);                   // file:line captured, no macro
log.hexdump(etch::Level::kDebug, "packet", buffer);       // any contiguous container
auto timer = log.scope("load level");                     // "load level: done in 12.3 ms"
log.fatal("out of memory");                               // highlighted, flushed, stacktrace
log.with({{"fps", 41.7}, {"entities", 1204}}).warn("slow");  // structured fields
```

```
2026-10-21 16:16:48.195 INFO  [demo] [1] demo.cpp:69: frame 1442 took 16.67 ms
2026-10-21 16:16:48.195 WARN  [demo] [1] demo.cpp:73: runtime level from data: code 31
2026-10-21 16:16:48.195 INFO  [net] [1] demo.cpp:76: hello from the net subsystem
2026-10-21 16:16:48.195 DEBUG [demo] [1] demo.cpp:94: PacketHeader (24 bytes)
  0000  45 43 54 48 02 00 01 80  39 05 00 00 68 65 6c 6c  |ECTH....9...hell|
  0010  6f 2c 20 77 6f 72 6c 64                           |o, world|
2026-10-21 16:16:48.240 DEBUG [demo] [1] demo.cpp:102: pretend to load a level: done in 45.489 ms
2026-10-21 16:16:48.249 FATAL [demo] [1] demo.cpp:203: simulated fatal error, exit code would be 3
  stacktrace:
    section_fatal at examples/demo.cpp:203
    main at examples/demo.cpp:223
```

A line that starts with a digit is a record; a line that starts with whitespace continues the one above. That holds for etch's own dump and stack lines and for a message with newlines in it, which continues indented. Control characters and escape sequences inside a message are written as `\e`, `\x01` and so on, so a logged chat message cannot clear your terminal or forge a record.

With `LineStyle{.format = etch::LineFormat::kJson}` on a file or console sink, the same records come out as JSON Lines, one object per record, with the data the text view renders: the payload behind a hexdump, the stack frames, exact durations, and the fields as a nested object.

```
{"ts":"2026-10-21T16:16:48.195Z","level":"warn","logger":"json","thread":1,"pid":4120,"file":"demo.cpp","line":154,"msg":"frame budget exceeded","fields":{"fps":41.7,"level_name":"forest","entities":1204}}
{"ts":"2026-10-21T16:16:48.195Z","level":"debug","logger":"json","thread":1,"pid":4120,"file":"demo.cpp","line":186,"msg":"PacketHeader (24 bytes)","bytes":"45435448020001803905000068656c6c6f2c20776f726c64"}
```

In a terminal the timestamp, thread and location are dimmed, the level tag is coloured, each logger name gets a stable colour of its own (hashed from the name), warnings are yellow, errors red, and a fatal line is bold red on a red tag. Only the sixteen standard colours are used, so it looks the same in Windows Terminal, conhost and any Linux terminal. The whole palette is one struct on the sink's `LineStyle`, so every colour can be changed.

## What it does

- **Six levels**, `trace` to `fatal`, plus `off` as a setting that mutes a sink or a logger, fatal included. A runtime level per logger and per sink, and a compile-time floor (`ETCH_MIN_LEVEL`) that removes the call bodies below it from the binary. The argument expressions are still evaluated by the caller; wrap an expensive one in `if (log.enabled(...))`. `parse_level` reads `info`, `WARNING`, `err`, `critical`, `off`, `none` and tolerates surrounding whitespace, line endings included.
- **`std::format` messages**, checked at compile time: a placeholder that does not match its argument is a compile error, not a runtime surprise.
- **File and line without macros.** `log.info("x = {}", x)` knows where it was called from. The format string type carries a `std::source_location` taken as a default argument of a `consteval` constructor.
- **Named loggers that share sinks.** `parent.child("net")` makes a logger that uses the parent's sinks, now and after any change to them. `etch::get("net")` does that for the default logger and keeps the result for the life of the process, so nine subsystems are nine one-liners.
- **Sinks:** console (ANSI colour, detected from the stream and `NO_COLOR`, `CLICOLOR_FORCE`, `FORCE_COLOR`, `TERM`), file (append, size-based rotation keeping N old files, missing directories created, I/O failures counted rather than thrown), memory (the last N records, for tests and crash dumps) and a callback for anything else. A logger can have any number of sinks, sinks can be shared between loggers, and the sink list can be replaced while other threads are logging. A second `FileSink` on a path that one already has open is refused with an error, instead of the two rotating each other's file away.
- **Async sink:** a bounded ring queue and one worker thread in front of any other sink. A log call claims a slot with one compare-and-swap and moves the record in; it takes no lock and allocates nothing (a successful call may still briefly take a mutex to wake a sleeping worker). When the queue is full, `kBlock` waits for room and loses nothing; `kDrop` drops the record and counts it, except at or above `keep_from` (warn by default), where it waits instead, so a flood of trace lines never stalls a game loop and an error is never thrown away. Whenever records were dropped, the worker writes one warning from logger `etch` saying how many, before the next real record, so the file shows where its gaps are. `flush()` waits for exactly the records logged before it. When the queue goes idle the worker flushes the inner sink, so a crash a little later loses at most the burst in flight. `etch::flush_all()` and `etch::shutdown()` reach every live async sink; an exit handler calls `shutdown()` for you, so a `std::exit` does not lose what was queued, and lines logged by static destructors afterwards go straight through, in order, after the ring's contents. A sink that throws costs that one record and is counted; the worker carries on.
- **Hexdump** of any memory in the classic hex editor layout: offset, sixteen bytes with a gap after eight, ASCII column. The first 4 KiB by default (the call takes a limit); the rest is summarised as a count. The record keeps those bytes, and the JSON line carries them as hex, plus the label, never the rendered dump text.
- **Structured fields.** `log.with({{"fps", 41.7}, {"name", "forest"}, {"took", 16ms}}).warn("slow frame")` attaches typed key/values. Any integer, floating-point number, bool, string, string view, `char`, enum, chrono duration (stored as nanoseconds), filesystem path (as UTF-8) or type with a `std::formatter` is accepted. Text output appends them as `key=value`, strings quoted and escaped; JSON output gets a `fields` object. The entry owns a copy of the fields and is used in that one expression. The values are converted when the braces are evaluated, before any level check; `with(Level::kTrace, {...})` skips the copy when that level is filtered, which is the form for a hot path. `with(...)` also offers `log(level, ...)`, `hexdump(...)` and `scope(...)`, so a dump or a timing can carry fields.
- **JSON Lines output** from any file or console sink (`LineStyle::format`), with the same rotation and failure accounting as text. Each line is one object: ISO 8601 UTC timestamp, level, logger, thread number, pid, file, line, message, and when present fields, `duration_ns`, `bytes` (hex) and `stack` (one string per frame, `section_fatal (demo.cpp:203)`). Strings are escaped so a message with a newline is still one line, and invalid UTF-8 becomes U+FFFD so every line decodes. `JsonStyle` turns the full source paths on (off by default: a path can name a home directory, and it costs a program tokens on every line), turns thread and pid off, or switches the timestamp to epoch milliseconds.
- **Scope timer:** `auto t = log.scope("name")` logs when it begins and how long it took when it ends. `ScopeOptions` drops the begin line or keeps only ends slower than a threshold (`{.min_duration = 16ms}` for "tell me about slow frames"). Whether it logs is decided once, when it starts.
- **Fatal** appends a `std::stacktrace` with etch's own frames left out (by source file, and by symbol when there is no source information), base names only, and flushes every sink. It does not abort; that is the caller's decision.
- **Errors come back as values.** Opening a file sink returns `std::expected` with the OS error code. After that, file and console I/O never throw out of a log call: refused writes, flushes, closes and rotations are counted on the sink (`write_failures()`) and it keeps trying, reopening a file it lost, and retrying a refused rotation only after another `max_bytes` rather than on every line. Null arguments (a null stream, inner sink or callback) throw `std::invalid_argument` at construction, where they belong; a null hexdump pointer is reported in the message. Formatting, allocation and a user callback can still throw, and that exception reaches the caller of the log call; the async sink is the exception and catches, counts and carries on.
- **An agent profile and a run summary.** `etch::open_agent_log("run.jsonl")` opens the file a program will read: compact JSON (epoch milliseconds, no thread, pid, path or function), behind an async sink that drops trace, debug and info rather than stall the caller, keeps warnings and above, and marks every gap. `SummarySink` counts what passed through it and remembers the first and last error; its `json()` is one object with records per level and per logger, the drops and write failures of the sinks it watches, and both errors with file and line. `SummarySink::install("summary.json")` attaches it to the default logger and writes that object at exit, after the async sinks have drained. A reader looks at the summary first and opens the full log only when it says there is something to see.
- **Small thread numbers.** Each thread gets a number on its first log line, in order of first use. It is what the text column and the JSON `thread` field show, instead of a 15-digit pthread value.
- **Shutdown that cannot lose a record.** `shutdown()` closes the queue to new records (a producer arriving afterwards waits its turn and then writes straight through, so order is kept), waits for the producers already inside, drains the ring, stops the worker and flushes. It is safe to call from several threads and more than once; called from the worker's own thread (a callback) it can only close the queue and ask the worker to stop.

What it does not do: defer formatting (the message is formatted on the calling thread), guarantee anything about power loss, rate-limit (a "once per 5 seconds" warning is still yours to write), install a crash handler (an unhandled-exception handler running in normal context can call `fatal()` and get every sink flushed, async ones included; a POSIX signal handler cannot call anything in etch, since nothing here is async-signal-safe), or let a sink's callback log back into the same sink (every sink holds a non-recursive mutex while writing, so that deadlocks; it is documented on `Sink`).

## Who reads what

The same logger serves two readers. A person scans a terminal or a log file after the fact. An agent (Claude Code, Codex, a CI job, a test) reads the output programmatically and must not depend on colour or timing. Set the project up so each gets what it needs.

**For a human reviewer**, the console sink in a terminal is the view, and these are the parts worth attention:

- **`warn` and above.** Set the console sink's level to `warn` once things work, and keep `info` for the file. A tinted message means something went wrong; a bold red one with a stacktrace is `fatal`, which flushes everything and leaves stopping to the caller.
- **The logger name colour.** Four subsystems logging at once are told apart by colour before you read a word. Give each subsystem its own logger with a short name (`etch::get("render")`).
- **Scope timers** (`log.scope("load level")`) for anything that might be slow. The "done in" line is the first place to look when a frame hitches; with a `min_duration` only the slow ones appear.
- **Hexdumps** when a parser or a network packet misbehaves. The ASCII column shows at a glance whether you are looking at text, a header or garbage.
- **The counters** when logging itself is suspect: `dropped()` on an async sink, `write_failures()` on a file or console sink, `rotations()` on a file sink, and the `etch` warning that marks a gap in a file. A non-zero number is the explanation for a missing line.

**For an agent**, use the parts that are exact and machine-checkable, and avoid the parts made for eyes:

- **The run summary first.** `SummarySink::install("summary.json")` costs a few counters per record and leaves one JSON object at exit: how many records at each level and from each logger, what was dropped or refused, and the first and last error with file and line. That is the whole run in a few hundred characters; it answers "did anything go wrong" without the log being read at all.
- **A JSON Lines file** (`etch::open_agent_log(path)`, or `LineStyle{.format = etch::LineFormat::kJson}` on any `FileSink`) when the output must be read by a program after the fact. Every line is one object with the data behind the event: the payload behind a hexdump as hex, every stack frame as a short string, exact durations, the fields you attached. The text view renders those for eyes; the JSON view carries them once, as data, with the message being exactly the caller's text. For a model reading the log, the compact style is the one to use: `JsonStyle{.time = JsonStyle::Time::kEpochMillis, .thread = false, .pid = false}` leaves `ts`, `level`, `logger`, `file`, `line` and `msg`, about a fifth shorter than the default line and less than half of a line with path and function turned on. Absolute source paths (which can contain a user name) stay out of the file, frames included, unless you ask for them.
- **`MemorySink`** in tests. `records()` gives the level, time, thread, source location, message, fields, bytes, stack and duration of each record as data; `messages()` gives just the text. Assert on those, not on console output. That is how the suite under `tests/` works, and it is the cheapest sink in the benchmark (about 200 ns a call, formatting included).
- **`with()` fields** for anything a program will want to filter or aggregate on: `log.with({{"request_id", id}, {"took", elapsed}}).info("done")`. They survive as typed JSON values, not as text to re-parse.
- **`CallbackSink`** to route records into anything else: a test probe, a channel to another process, a JSON writer. The callback runs under the sink's mutex and must not log back into the same sink.
- **A file sink with colour off** (it is off by default) when output must survive the process, and a `LineStyle` with `utc = true` so timestamps compare across machines (UTC lines end in `Z`). Lines are LF-terminated on every platform and the layout is fixed: `time LEVEL [logger] [thread] file:line: message key=value`. A line starting with a digit is a record and a line starting with whitespace continues it; the level column is always five characters, so it can be sliced, and `parse_level` reads it back.
- **`NO_COLOR=1`** or `ConsoleSink::ColourMode::kNever` when capturing the console in a pipe, so no escape sequences land in the capture. The demo's `--no-colour` flag does the same.
- **Levels as data.** `log.log(level, ...)` takes the level at runtime, `std::formatter<Level>` prints it (`INFO`), and `level_name()` gives the lower-case form JSON uses, so a severity mapped from an error code needs no switch.
- **Never scrape the colour.** It is for people. The thread number is stable within a run but not across runs.

For an agent working on etch itself: `ctest --preset windows` is the oracle, `python tools/mutate.py` checks the oracle has teeth, and `etch_bench` (Release) is how a performance claim is backed. A change to a header is done when all three say so.

## How it works

- **Format once, on the calling thread.** A log call formats the message with `std::vformat`, stamps it with the time, thread number, source location and logger name, and hands that `Record` to each sink. The record owns its strings, which is what lets the async sink move it to another thread safely. With one sink, the record is moved, not copied. The record also remembers where the caller's text ends and etch's appended text (dump lines, frames) begins, which is how text output escapes only the caller's part and JSON carries only the caller's part.
- **Sinks format the line, loggers format the message.** The columns (time, level, logger, thread, location) belong to the sink, so the console can be terse while the file keeps everything. Each sink formats into one reused buffer under its mutex, so a steady stream of lines allocates nothing.
- **The async queue is Vyukov's bounded MPMC ring**, used as multi-producer single-consumer. A producer claims a slot with one compare-and-swap and moves its record in, with no lock and no allocation. (Vyukov's design is not formally lock-free: a producer pre-empted between claiming and publishing a slot holds up the consumer until it resumes. The window is a few nanoseconds of the producer's own work, but a pre-emption inside it lasts as long as the scheduler says, so the guarantee is practical rather than formal.) The worker takes what is ready in sweeps of at most 1,024 records, a bound that keeps its memory fixed and keeps a fast producer from feeding it forever. A mutex exists only for the slow paths: putting the worker to sleep (after a short bounded spin, so bursts never pay a wake-up per message), blocking a producer on a full queue, and waiting in `flush()`. The sleep handshake is the usual announce-then-recheck with sequentially consistent operations, and the full-queue handshake uses a pair of fences, so no wake-up can be lost. Every async sink is in a process-wide list, which is what lets `flush_all()`, `shutdown()` and the exit handler reach them.
- **No logger lock is held while sinks run.** The sink list is an immutable snapshot; a log call copies its pointer under a shared lock that is released before any sink is called. A callback can therefore replace the sinks, and a replaced async sink joins its worker outside any lock. A child logger holds the same sink set as its parent, so one replacement reaches every subsystem.
- **Colour on Windows without `<windows.h>`.** Enabling ANSI escape processing needs three Win32 calls (naming the worker thread needs two more). They are declared by hand, with the exact signatures, so the header does not pull a quarter of a million lines and the `min`/`max` macros into every file that logs.
- **The timestamp is cached per second.** Formatting a `std::chrono::zoned_time` costs about 16 microseconds on MSVC's standard library, more than everything else in a log line put together. The date and time down to the second change once a second, so each thread keeps that prefix in a plain char array (no destructor, so it is still valid when a static object logs from its destructor) and only formats the milliseconds per call. The time zone database itself is loaded when the first sink is constructed, so the milliseconds it takes never land on a log line.
- **The default logger is never destroyed**, on purpose, so objects with static storage can log from their destructors. An `atexit` handler flushes its sinks; another drains every async sink and switches it to synchronous writes.
- **Stack frames are filtered by file, not by count.** A fatal's trace leaves out every frame whose source file is under etch's own include directory, whatever the optimiser inlined. Frames are kept as data (description, file, line) so JSON can print base names.
- **`std::print` for console output.** It is the only standard way to get UTF-8 onto a Windows console correctly; `printf` and `std::cout` mangle it. It only works under `/utf-8`, which the CMake target passes on; a translation unit built without it gets a compile-time note.
- **JSON is the same record, serialised.** The record carries the human text and the data side by side (message plus bytes, stack, duration, fields), so a text sink and a JSON sink on the same logger show the same event two ways. The JSON writer is a hundred lines with no dependency; the test suite checks every line it produces with a small validator and compares a fixed record against an exact expected string.

## Performance

Measured with `etch_bench` on an AMD Ryzen 7 7800X3D, Release builds: Windows 11 with MSVC 2026 Insiders, and Ubuntu 24.04 with GCC 14.2 under WSL 2 (4 virtual CPUs). These are the costs of one log call on the calling thread, averaged over 200,000 calls, with the measured values stored to a volatile so the optimiser cannot delete them; run it yourself to get your own numbers. "Burst" rows use a ring larger than the run, so they measure the enqueue alone; "sustained" rows use the default ring, so once the worker falls behind the caller waits, which is what an application that logs this hard would pay.

| per call | MSVC | GCC 14 |
|---|---|---|
| Format the message (`std::format`, two placeholders) | about 112 ns | about 77 ns |
| Log to a sink that does nothing | about 145 to 160 ns | about 115 to 140 ns |
| Log below the level | 1 ns | 0 ns |
| Log below the level with fields, `with(level, {...})` form | about 10 ns (the values are still built) | about 10 ns |
| Log to the memory sink | about 200 ns | about 130 to 150 ns |
| Log through the async sink into a text file, burst (ring never fills) | about 175 to 195 ns | about 150 ns |
| Same, sustained with the default 4,096-slot ring (the caller waits whenever the worker is behind) | about 450 to 580 ns | about 390 to 440 ns |
| Same, sustained, JSON file | about 600 ns | about 730 to 925 ns |
| End to end per line including the worker's write, text file | about 460 to 620 ns | about 390 to 450 ns |
| Four threads logging at once through the async sink, measured inside each thread | about 450 to 500 ns | about 360 to 480 ns |
| Four threads, 100,000 lines, 8.3 MB file (the demo) | about 60 ms | about 41 ms |

The sustained rows are the honest ones for an application that logs flat out: the caller cannot outrun the worker for long, so its cost becomes the worker's cost per line. An application that logs in bursts with quiet time between them sees the burst row. The ranges are run-to-run variation on a desktop with other things running.

Most of the gap between the columns is the standard library: libstdc++'s `std::format` costs about half of MSVC's, and that is the largest single item in a call. The rest is the two environments (a VM with four virtual CPUs versus the host), which this benchmark does not separate.

Before the two changes described above the full line took 15.8 µs to format and a synchronous file write 20 µs; the mutex-and-condition-variable queue cost the caller 400 to 600 ns alone and 1.7 µs under four-thread contention.

Not measured: Clang or libc++, tail latency (the benchmark reports averages), or the console sink (it flushes every line on purpose, so it is bound by the terminal).

## How it is checked

Ninety-nine tests (ninety-eight on Linux; the refused-rotation test relies on Windows file locking), written against a tiny harness in `tests/check.hpp` so the suite needs no framework and split by area under `tests/`. CTest runs the suite twice: once as built, and once with `ETCH_MIN_LEVEL=3` so the compile-time floor is exercised for real rather than assumed. Each run uses its own scratch directory, so the two can run in parallel. The same suite passes on MSVC and GCC 14, Debug and Release.

- **Exact output.** The line layout, the hexdump (including the last partial line, the wide offsets past 64 KiB, the limit and the null-pointer case), every colour escape, the name palette, message escaping and continuation, the `Z` on UTC lines, the time-of-day style, and the duration formatting are compared against exact strings.
- **Source location.** The captured line equals the `__LINE__` of the call, and the file column is the base name.
- **Time.** UTC rendering of a fixed instant is exact; local time equals the standard library's own zoned formatting of the same instant; the per-second cache is driven across a second, minute, hour and day boundary, and the UTC and local caches are shown to be independent (a text sink and a JSON sink on one thread must not evict each other).
- **File sink.** Lines on disk equal the lines logged byte for byte and the file size equals the sink's byte count (binary mode, so no CRLF surprise); append across two opens counts the old bytes; rotation at a small size keeps every line across the numbered files, drops the oldest beyond `max_files`, and with `max_files = 0` just restarts the file; a rotation whose rename is refused (on Windows, another handle holds the target open) counts a handful of attempts rather than one per line and keeps every line in the live file; a second sink on the same path is refused, also when the path is spelled differently (`..`, and case on Windows), and allowed again once the first is gone; a failed open leaves no reservation behind; missing directories are created; a parent that is a file comes back as an error value.
- **Async.** Eight threads log 4,000 lines through a 64-slot queue: all arrive and each thread's lines stay in order. With the drop policy, dropped plus delivered equals sent; an error survives a flood of droppable lines and a gap marker reports how many were lost. `flush()` and the destructor both wait for the queue to drain, `flush()` returns while another thread keeps logging, `shutdown()` drains and then writes straight through, a producer blocked on a full ring when shutdown begins gets every record delivered once and in order, and a `flush()` that overlaps a shutdown returns only once the worker has finished; two threads may call `shutdown()` at once; `flush_all()` does not deadlock when a sink's callback constructs another async sink; the gap count reported by the time `flush()` returns equals the drop count exactly; an idle worker flushes the inner sink by itself; `flush_all()` is safe while async sinks are created and destroyed on another thread. The blocking policy never drops. The inner sink's level is honoured. A sink that throws is counted and the records after it still arrive.
- **JSON.** Escaping of quotes, backslashes, control characters and UTF-8 is exact, and invalid UTF-8 (including an encoded surrogate) becomes U+FFFD; a fixed record serialises to an exact expected line; every field type renders (non-finite doubles become `null`); fields, hexdump bytes, a scope's duration and a fatal's stack frames all appear; the message keeps the caller's newlines but never the dump text or the frames; frames carry base names only; and every line the file sink writes passes a small JSON validator that is itself tested against malformed input.
- **Agent profile and summary.** The profile is compact JSON with drops above warn kept; `open_agent_log` writes epoch-millisecond lines without thread or pid and refuses a path already open; the summary counts per level and per logger, remembers the first and last error with file and line, reports the drops and write failures of the sinks it watches, writes one valid JSON object to a file, and names every level.
- **Logger.** Runtime filter, sink filter, `off` (as a setting, and rejected as a record level), compile-time floor, `emit` honouring the level, name round trips, lenient parsing, field conversions for nine value types and the edge cases (a ready `FieldValue`, a null C string, a duration past `int64`, a NaN duration, a char-backed enum, a custom formatter), the gated `with(level, {...})` that logs nothing when filtered, the single-use entry (a compile-time check) carrying fields into `log`, `hexdump` and `scope`, a filtered entry logging nothing, child loggers seeing later sink changes, the registry returning one object per name, scope options (no begin, a threshold, an end that survives a level change), etch's own frames absent from a stacktrace and the remaining frames showing base names only, a null hexdump pointer named in the JSON message, stable thread numbers, null arguments throwing, sinks replaced 600 times while four threads log, and a callback that adds a sink from inside a log call.

**Tests against deliberate bugs.** `tools/mutate.py` first checks that the untouched suite is green (a failing suite would "catch" every mutant for the wrong reason), then injects thirty-eight bugs one at a time (level filters off by one, the parser rejecting full names, `off` accepting fatal, the time cache ignoring the mode or the second, the location column printing the full path, a lost warn colour, UTC lines losing their `Z`, a newline in a message starting an unindented line, the memory sink keeping one record too many, rotation deleting the live file, rotation never happening or retried on every line, a duplicate path not refused, `set_style` turning colour off, text-mode file output, uncounted drops, errors dropped as readily as trace, a missing gap marker, the async sink ignoring the inner level, a sink exception escaping the worker, `flush` waiting for idleness, late records overtaking the ring at shutdown, the hexdump losing its gap or ignoring its limit, etch's own frames left in a stacktrace, a scope end filtered by a later level change, the scope message changing, null sinks kept, `emit` skipping the level check, an unescaped JSON message, invalid UTF-8 passed into JSON, JSON without its fields or duration, text output without its fields, a summary that ignores errors, and an agent profile that keeps the thread and pid fields), rebuilds the tests and runs them, each step under a timeout so a mutant that hangs is reported rather than waited for. The suite catches every one, each with at least one failing test that names the broken behaviour. Run it after `cmake --preset windows`:

```bash
python tools/mutate.py
```

**A check before anything leaves the machine.** `tools/prepush.py` is a git pre-push hook (install it once per clone with `python tools/prepush.py --install`). It scans the commits about to be pushed, their messages and file names, and refuses the push on drive-letter paths, home directories, the current login name used as a path segment, e-mail addresses other than GitHub's noreply one, token-shaped strings (GitHub, OpenAI, AWS, Slack, Google), private key blocks, credential assignments, local tooling files, generated logs and files over a megabyte. It learns the login name and home directory from the environment at run time, so the script itself contains nothing personal. It is a heuristic over the text git shows as added and over file names (binaries are only size-checked), not a proof. `--all` scans every commit on every branch; the published history passes it. GitHub's own secret scanning and push protection are enabled on the repository as the second layer.

**Found by building a game on it.** Claude Opus 5.5 built a small Minecraft-style voxel game (OpenGL, fifteen worker threads, an in-game log console, a crash ring) with etch as its only logging, fetched from GitHub the way a new user would get it, then ran it through normal play, a 1.6-million-line log flood, a broken shader, a deliberate crash and corrupted save files, plus about forty isolated probes and a 27-case compile matrix, on MSVC and GCC. It reported nineteen findings. Fixed in version 0.3: JSON losing a message's second line, the drop policy losing errors and leaving no trace of a gap, consumers not getting `/utf-8`, records lost on `std::exit` (process-wide shutdown and idle flushes), a refused rotation retried on every line, two file sinks on one path, absolute paths in stack frames, etch's own frames leaking into traces, common field types not compiling, a reused entry losing its fields, `set_style` switching colour off, forged records and escape sequences in text output, invalid UTF-8 in JSON, the cost of a filtered `with()`, the first line paying for the time zone load, padded level names, `off`, null arguments crashing, and the API gaps it hit (child loggers and a registry, `log`/`hexdump`/`scope` on an entry, free functions for all of them, scope thresholds, directory creation, time-of-day lines, FetchContent and `find_package`, an `ETCH_MIN_LEVEL` option). Documented rather than coded: the crash-handler pattern, rate limiting, splitting warnings to stderr (two sinks with levels do it), and that Release stacktraces need debug information to show names.

**Second opinion.** The code was reviewed by GPT-6-Astra, run through the Codex CLI at extra-high reasoning effort as a hostile senior engineer, after the first version. It found the rotation path that could truncate a file it had failed to rename, silent write failures, an exception from a sink taking the worker thread down, the thread-local timestamp cache being destroyed before static destructors run, data races on sink level and style, the logger lock being held while calling sinks, text-mode CRLF breaking the byte count, `flush` waiting for traffic that came after it, the async wrapper ignoring the inner sink's level, and a default-argument pattern GCC rejects. All of those are fixed. Most have a direct test; the teardown and data-race fixes are by construction and were re-reviewed rather than tested. Two of its points became documented contracts instead of code (callback reentrancy, one `ETCH_MIN_LEVEL` per program) and one is on the roadmap (deferred formatting).

A second round on the fixed code confirmed that both wake-up handshakes in the queue are correctly ordered under the C++ memory model, and found the worker's sweep being unbounded (a fast producer could refill slots as fast as the worker freed them, so it would collect forever and never write), a signed overflow in the slot arithmetic after 2^63 records, close-time failures hidden by rotation, a failed reopen disabling the file sink for good, console flush failures going uncounted, the UTC-fallback marker landing before the milliseconds, and `std::bit_ceil` on an absurd capacity. All of those are fixed; the one remaining contract is that a sink behind the async sink must not drop the async sink's last reference from inside a callback.

A fourth round reviewed the 0.3 batch and found the shutdown handshake able to strand or duplicate a record, registries that a global's destructor could outlive, a worker-thread self-shutdown that flushed from inside a write, concurrent `shutdown()` calls racing on the thread object, and the single-use entry holding a view of a braced list that could be named and used after the list was gone. A fifth round verified the rewrite (an admission protocol: producers announce themselves before reading the closed flag, shutdown writes the flag and waits for them; the worker announces its exit and drains until no producer is inside; every pair of operations is sequentially consistent, which the reviewer checked pair by pair) and found three more: `flush()` returning early during a shutdown, the thread id read from the `jthread` while another thread joined it, and a deadlock when a callback constructs an async sink while `flush_all()` holds the registry lock. All fixed; the registry now holds lockable entries and never holds a global lock while waiting on a worker.

A third round, asked for speed and for what a model reading the logs would want, led to: two timestamp caches instead of one (a local-time text sink and a UTC JSON sink on the same thread were evicting each other every record), the JSON message carrying only the caller's text when the bytes or the stack travel as data, escaped string values in text fields, cheaper millisecond and hex rendering, a benchmark with optimisation barriers and sustained-load rows, a baseline gate and timeouts in the mutation script, and the README claims above corrected to what the measurements show. Its larger proposals (a smaller record, a per-file header with short thread ids and delta times, deferred formatting) are on the roadmap.

## Branches

`main` holds verified states only: every commit on it has passed the full suite on Windows and Linux, the injected-bug script, and a review round. Day-to-day work happens on `beta`, which is merged into `main` once a batch of features is done and checked. If you want the latest, build `beta`; if you want something that is known to work, build `main`.

## Layout

```
include/etch/etch.hpp        include this; pulls in the rest
include/etch/level.hpp       Level (with off), names, parsing, ETCH_MIN_LEVEL, std::formatter<Level>
include/etch/record.hpp      Field conversions, Record, Palette, LineStyle, timestamp cache, format_line
include/etch/json.hpp        JSON Lines: json_escape (UTF-8 validating), format_json, format_record
include/etch/sinks.hpp       Sink, ConsoleSink, FileSink, MemorySink, CallbackSink
include/etch/async_sink.hpp  AsyncSink: lock-free ring + worker thread; flush_all, shutdown
include/etch/agent.hpp       AgentProfile, open_agent_log, SummarySink
include/etch/logger.hpp      Logger, Entry (with), ScopeTimer, hexdump, children, get(), free functions
examples/demo.cpp            every feature, in order
bench/bench.cpp              what one call costs the calling thread
tests/check.hpp              the test harness (TEST, CHECK, CHECK_EQ)
tests/support.hpp            shared test helpers: capturing logger, scratch dir, fixed record
tests/test_*.cpp             the suite, one file per area (levels/logger, format, sinks, async, JSON, agent)
tests/main.cpp               test entry point
tools/mutate.py              tests against deliberate bugs
tools/prepush.py             pre-push hook: refuses local paths, secrets and local files
```

## Built with

Written by Claude (**Fable 5.1**, model id `claude-fable-5-1`, run at high effort) in Claude Code. Reviewed by **GPT-6-Astra** (model id `gpt-6-astra`, extra-high reasoning effort) through the Codex CLI, as a second opinion whose findings Claude then fixed. Exercised by **Claude Opus 5.5**, which built a voxel game on it and reported what broke. Directed and tested by David. Commits carry a `Co-Authored-By` trailer naming the model that wrote them, so the history shows which model made each change.

## License

All rights reserved. The source is public to read and evaluate, not to reuse. See [LICENSE](LICENSE).

## Roadmap

Done so far: the logger and sinks, the lock-free async queue, colour with per-logger names, Linux (GCC 14 under WSL), JSON Lines with typed fields, five review rounds, the fixes from a game built on it, the agent profile and the run summary. What is left, in the order it is worth doing:

1. **Continuous integration.** A GitHub Actions matrix (MSVC, GCC 14, Clang with libc++) that configures, builds, runs CTest and the mutation script on every push. Cheap, and it turns "tested on two machines" into a claim anyone can check.
2. **Clang and libc++.** Never built. libc++ 20 should work as is; 18 and 19 need `-fexperimental-library` for `std::jthread`, and the time zone fallback path has never run for real.
3. **Rate limiting.** `log.every(5s).warn(...)` and `log.once().info(...)`, with the number of suppressed calls reported when the next line gets through. The one thing every game loop wrote by hand.
4. **A crash handler**, opt-in: an unhandled-exception filter and signal handler that log a fatal and flush. It needs care, because a crash inside a sink holds that sink's mutex; a try-lock with a timeout is the way through.
5. **Benchmark percentiles.** p50, p99 and worst case per call, plus allocation counts, next to the averages. The tail is what a game loop feels; the average hides it.
7. **A JSON header line per file.** Schema version, pid, base timestamp and a thread table, so each line can carry a millisecond delta instead of a full timestamp. Repeated on rotation so every file stands alone.
8. **Shrink the record.** Fields, bytes, stack and duration behind one optional extras object, so the 200-byte record (and the default ring) drop by nearly half and the move into the queue copies less.
9. **A slim header.** `etch/log.hpp` with the logger, the levels and the record only, for translation units that log and nothing else: about half of the 1.3 s per file that `etch.hpp` costs is the standard headers the sinks and JSON pull in.
10. **Deferred formatting.** Queue the arguments instead of the formatted text, so the caller pays only a copy. Needs an explicit snapshot rule for argument types that hold pointers or references, with eager formatting as the fallback. The biggest possible speed-up, and the biggest change; last on purpose.
