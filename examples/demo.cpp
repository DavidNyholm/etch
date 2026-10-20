// etch demo: every feature, in order, so you can see what each one looks like.
//
//   etch_demo [--no-colour] [--file <path>] [--burst <lines per thread>]
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <etch/etch.hpp>

namespace {

struct DemoArgs {
  etch::ConsoleSink::ColourMode colour = etch::ConsoleSink::ColourMode::kAutomatic;
  std::filesystem::path file = "etch-demo.log";
  int burst = 2000;
};

DemoArgs parse_args(int argc, char** argv) {
  DemoArgs args;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--no-colour" || a == "--no-color") {
      args.colour = etch::ConsoleSink::ColourMode::kNever;
    } else if (a == "--colour" || a == "--color") {
      args.colour = etch::ConsoleSink::ColourMode::kAlways;  // even when piped to a file
    } else if (a == "--file" && i + 1 < argc) {
      args.file = argv[++i];
    } else if (a == "--burst" && i + 1 < argc) {
      args.burst = std::atoi(argv[++i]);
    } else {
      std::println(stderr,
                   "usage: etch_demo [--colour | --no-colour] [--file <path>] [--burst <n>]");
      std::exit(2);
    }
  }
  return args;
}

void heading(std::string_view title) { std::println("\n== {} ==", title); }

// Something with a recognisable byte layout for the hexdump.
struct PacketHeader {
  std::uint32_t magic = 0x4854'4345;  // "ECTH" little-endian
  std::uint16_t version = 2;
  std::uint16_t flags = 0x8001;
  std::uint32_t payload_bytes = 1337;
  std::array<char, 12> tag{'h', 'e', 'l', 'l', 'o', ',', ' ', 'w', 'o', 'r', 'l', 'd'};
};

void section_levels(etch::Logger& log) {
  heading("1. One line per level (the console sink colours the level)");
  log.trace("trace: the firehose, off by default in most apps");
  log.debug("debug: values you want while developing");
  log.info("info: normal progress");
  log.warn("warn: something odd, carrying on");
  log.error("error: an operation failed");
  std::println("   (fatal is shown last, it comes with a stacktrace)");
}

void section_formatting(etch::Logger& log) {
  heading("2. std::format placeholders, checked at compile time");
  const int frame = 1442;
  const double dt = 16.6667;
  const std::uint64_t addr = 0x7ff6'a000'1000;
  log.info("frame {} took {:.2f} ms", frame, dt);
  log.info("hooked function at {:#014x}", addr);
  log.info("level values print too: {} {}", etch::Level::kWarn, etch::Level::kFatal);
  log.info("utf-8 round-trips through std::print: Z\u00fcrich, \u00e5\u00e4\u00f6, \u2603");
  log.log(etch::Level::kWarn, "runtime level from data: code {}", 0x1f);
}

void section_hexdump(etch::Logger& log) {
  heading("3. Hexdump of raw memory (hex editor layout)");
  const PacketHeader header;
  log.hexdump(etch::Level::kDebug, "PacketHeader", &header, sizeof header);
  const std::vector<std::uint16_t> samples{0x0001, 0x0203, 0xfffe, 0x8000};
  log.hexdump(etch::Level::kDebug, "samples (vector<uint16_t>)", samples);
}

void section_scope(etch::Logger& log) {
  heading("4. Scope timer");
  {
    auto timer = log.scope("pretend to load a level");
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
  }
  {
    auto timer = log.scope("tiny scope", etch::Level::kInfo);
  }
}

void section_levels_and_filters(etch::Logger& log) {
  heading("5. Runtime level filter");
  log.set_level(etch::Level::kWarn);
  log.info("this info line is filtered out (level is warn)");
  log.warn("this warn line gets through");
  log.set_level(etch::Level::kTrace);
  std::println("   compile-time floor ETCH_MIN_LEVEL = {} ({})", ETCH_MIN_LEVEL,
               etch::to_string(etch::kCompileTimeMinLevel));
}

void section_file(const DemoArgs& args) {
  heading("6. File sink with rotation");
  constexpr std::uintmax_t kMaxBytes = 4 * 1024;
  constexpr int kLines = 120;
  auto file =
      etch::FileSink::open(args.file, {.max_bytes = kMaxBytes, .max_files = 2, .truncate = true});
  if (!file) {
    std::println("   could not open {}: {}", args.file.string(), file.error().message());
    return;
  }
  etch::Logger disk{"disk", {*file}};
  for (int i = 0; i < kLines; ++i) disk.info("line {:03} of {} written to disk", i + 1, kLines);
  disk.flush();
  std::println("   wrote {} lines to {} ({} bytes in the live file, {} rotations, keeps .1 .2)",
               kLines, std::filesystem::absolute(args.file).string(), (*file)->bytes_written(),
               (*file)->rotations());
}

void section_async(const DemoArgs& args) {
  heading("7. Async sink: four threads log into a queue, one worker writes the file");
  constexpr int kThreads = 4;
  constexpr std::size_t kQueueCapacity = 1024;
  auto path = args.file;
  path.replace_extension(".async.log");
  auto file = etch::FileSink::open(path, {.truncate = true});
  if (!file) {
    std::println("   could not open {}: {}", path.string(), file.error().message());
    return;
  }
  auto async =
      std::make_shared<etch::AsyncSink>(*file, kQueueCapacity, etch::AsyncSink::Overflow::kBlock);
  etch::Logger log{"async", {async}};

  const auto start = std::chrono::steady_clock::now();
  {
    std::vector<std::jthread> workers;
    for (int t = 0; t < kThreads; ++t) {
      workers.emplace_back([&log, t, n = args.burst] {
        for (int i = 0; i < n; ++i) log.info("thread {} message {}", t, i);
      });
    }
  }
  const auto logged = std::chrono::steady_clock::now();
  async->flush();
  const auto written = std::chrono::steady_clock::now();
  std::println("   {} lines: callers done after {}, file written after {}, dropped {}",
               kThreads * args.burst, etch::format_duration(logged - start),
               etch::format_duration(written - start), async->dropped());
  std::println("   -> {}", std::filesystem::absolute(path).string());
}

void section_fatal(etch::Logger& log) {
  heading("8. Fatal: highlighted, flushed, with a stacktrace (no abort, that is your call)");
  log.fatal("simulated fatal error, exit code would be {}", 3);
}

}  // namespace

int main(int argc, char** argv) {
  const DemoArgs args = parse_args(argc, argv);
  etch::Logger log{"demo", {std::make_shared<etch::ConsoleSink>(stdout, args.colour)}};

  std::println("etch demo (C++23 logger). Lines look like:");
  std::println("  <time> <LEVEL> [<logger>] [<thread id>] <file>:<line>: <message>");

  section_levels(log);
  section_formatting(log);
  section_hexdump(log);
  section_scope(log);
  section_levels_and_filters(log);
  section_file(args);
  section_async(args);
  section_fatal(log);
  return 0;
}
