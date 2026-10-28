# etch

A small, header-only C++23 logger: `std::format` messages with file and line and no macros, a lock-free async queue, a colour console, rotating files, and JSON Lines with typed fields for programs and models to read. MSVC and GCC 14, standard library only. A hobby project built to try the C++23 library in anger, tested by building a game and a server on it and by seven review rounds from a second model.

**Status:** v0.4.0 on `main`, verified on Windows (MSVC 2026 Insiders) and Linux (Ubuntu 24.04, GCC 14.2): 124 tests (one of them Windows-only), 51 injected bugs all caught, zero warnings. Day-to-day work happens on `beta`. The [wiki](https://github.com/DavidNyholm/etch/wiki) has everything beyond this page.

## Quick start

Needs CMake 3.28 or newer and a compiler with the C++23 library (MSVC 2022 17.10+, GCC 14+).

```bash
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
build\windows\Debug\etch_demo.exe
```

On Linux use the `linux` preset and run `build/linux/etch_demo`. The demo shows every feature in order and writes the text, JSON and agent log files next to it.

In your own project, straight from GitHub:

```cmake
include(FetchContent)
FetchContent_Declare(etch GIT_REPOSITORY https://github.com/DavidNyholm/etch.git GIT_TAG v0.4.0)
FetchContent_MakeAvailable(etch)
target_link_libraries(app PRIVATE etch::etch)
```

`add_subdirectory`, `find_package(etch)` after an install, and copying `include/etch` work too: see [Build and use](https://github.com/DavidNyholm/etch/wiki/Build-and-use).

## What it looks like

```cpp
#include <etch/etch.hpp>

etch::info("hello {}", "world");                          // default logger: "main", console
etch::Logger& log = etch::get("render");                  // a named logger sharing its sinks; keep the reference
log.warn("fps {:.1f}", fps);
log.hexdump(etch::Level::kDebug, "packet", buffer);       // any contiguous container
auto timer = log.scope("load level");                     // "load level: done in 12.3 ms"
log.with({{"fps", 41.7}, {"entities", 1204}}).warn("slow");  // typed fields
log.fatal("out of memory");                               // highlighted, flushed, stacktrace
```

```
2026-10-21 16:16:48.195 INFO  [demo] [1] demo.cpp:69: frame 1442 took 16.67 ms
2026-10-21 16:16:48.195 WARN  [net] [1] demo.cpp:76: slow handshake peer="10.0.0.7" ms=48
{"ts":1792611475773,"level":"warn","logger":"net","file":"demo.cpp","line":216,"msg":"slow handshake","fields":{"peer":"10.0.0.7","ms":48}}
```

The first two lines are the console (coloured in a terminal: level tag, a stable colour per logger name, warnings yellow, errors red). The third is the same event from the agent profile: one JSON object per record, the data behind a hexdump or a stacktrace carried as data, nothing a person needs and a model pays for. More in [What it looks like](https://github.com/DavidNyholm/etch/wiki/What-it-looks-like).

## What it does

- **Six levels** plus `off`, per logger and per sink, settable from `ETCH_LEVEL=warn,net=debug` without a rebuild, and a compile-time floor that removes calls from the binary.
- **Format strings checked at compile time**, with the call site's file and line captured through `std::source_location`, no macros.
- **Sinks:** console, rotating file (held against a second writer, in this process or another), memory (for tests and crash rings), callback, debugger output (Visual Studio, DebugView, x64dbg), and an **async sink** on a lock-free ring that either blocks or drops, keeps warnings and above, and marks every gap. `shutdown()` and an exit handler make sure nothing queued is lost, and a forked child keeps logging. A sink may log from inside its own write; it is skipped and counted rather than deadlocked.
- **Typed fields** (`with(...)`), per-thread **context fields** that ride on every line of a request or a frame, hexdumps, scope timers with thresholds, **rate limiting** per call site (`every(5s)`, `once()`, with the count of what was held back), named child loggers, and a fatal that carries a stacktrace with etch's own frames left out. Text output escapes keys, values and messages, so data from a network or a file cannot forge a record or drive the terminal.
- **JSON Lines** from any sink, UTF-8 validated, with an **agent profile** and a **run summary** (records per level and logger, drops, failures, first and last error) so a program reads the summary first and the log only when it must.
- **Errors as values and counters:** opening a file returns `std::expected`; I/O failures are counted on the sink, never thrown from a log call.

The full list, including what it deliberately does not do, is on [Features](https://github.com/DavidNyholm/etch/wiki/Features).

## For people and for programs

A person watches the console: `warn` and above, the logger name colours, scope timers, hexdumps, and the counters when logging itself is suspect. A program or a model reads the summary, then the JSON file, and never scrapes colour. The split, with the settings for each, is on [Who reads what](https://github.com/DavidNyholm/etch/wiki/Who-reads-what).

## Read more

- [How it works](https://github.com/DavidNyholm/etch/wiki/How-it-works): the design decisions and the measurements behind them
- [Performance](https://github.com/DavidNyholm/etch/wiki/Performance): about 150 ns per call to a sink, about 180 ns through the async sink into a file, on MSVC; less on GCC
- [How it is checked](https://github.com/DavidNyholm/etch/wiki/How-it-is-checked): the tests, the injected bugs, the pre-push scan, the game built on it, the review rounds
- [Branches and layout](https://github.com/DavidNyholm/etch/wiki/Branches-and-layout) and the [Roadmap](https://github.com/DavidNyholm/etch/wiki/Roadmap)

Every wiki page is plain Markdown at `https://raw.githubusercontent.com/wiki/DavidNyholm/etch/<Page-name>.md`, which is the form to hand to an agent.

## Built with

Written by Claude (**Fable 5.1**, model id `claude-fable-5-1`, run at high effort) in Claude Code. Reviewed by **GPT-6-Astra** (model id `gpt-6-astra`, extra-high reasoning effort) through the Codex CLI, as a second opinion whose findings Claude then fixed. Exercised by **Claude Opus 5.5**, which built a voxel game on it and reported what broke. Directed and tested by David. Commits carry a `Co-Authored-By` trailer naming the model that wrote them, so the history shows which model made each change.

## License

All rights reserved. The source is public to read and evaluate, not to reuse. See [LICENSE](LICENSE).
