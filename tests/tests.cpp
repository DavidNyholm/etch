// etch test suite. Built twice: as etch_tests (ETCH_MIN_LEVEL=0) and as etch_tests_minlevel
// (ETCH_MIN_LEVEL=3), so the compile-time floor is exercised for real.
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <etch/etch.hpp>

#include "check.hpp"

namespace {

using namespace std::chrono_literals;

// A logger whose output we can read back.
struct Captured {
  std::shared_ptr<etch::MemorySink> sink = std::make_shared<etch::MemorySink>();
  etch::Logger log;
  explicit Captured(std::string name = "unit", etch::Logger::Options options = {})
      : log(std::move(name), {sink}, options) {}
  std::vector<std::string> messages() const { return sink->messages(); }
};

// One directory per test process, so the two CTest binaries can run in parallel without
// deleting each other's files. main() removes it at the end.
std::filesystem::path scratch_dir() {
  static const auto dir = [] {
    auto d = std::filesystem::temp_directory_path() /
             std::format("etch-tests-{:08x}", std::random_device{}());
    std::filesystem::create_directories(d);
    return d;
  }();
  return dir;
}

std::vector<std::string> read_lines(const std::filesystem::path& p) {
  std::ifstream in(p);
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);) lines.push_back(line);
  return lines;
}

template <class L>
concept ScopeCallable = requires(L logger) { std::forward<L>(logger).scope("x"); };

constexpr std::string_view kEsc = "\x1b[";

// ---- levels -------------------------------------------------------------------------------

TEST(level_names_are_five_wide_and_round_trip) {
  for (auto l : {etch::Level::kTrace, etch::Level::kDebug, etch::Level::kInfo, etch::Level::kWarn,
                 etch::Level::kError, etch::Level::kFatal}) {
    CHECK_EQ(etch::to_string(l).size(), 5u);
    CHECK(etch::parse_level(etch::to_string(l)) == l);
  }
}

TEST(parse_level_accepts_prefixes_and_case) {
  CHECK(etch::parse_level("warn") == etch::Level::kWarn);
  CHECK(etch::parse_level("WARNING") == etch::Level::kWarn);
  CHECK(etch::parse_level("Err") == etch::Level::kError);
  CHECK(etch::parse_level("  info  ") == etch::Level::kInfo);
  CHECK(!etch::parse_level("in").has_value());  // too short to be unambiguous
  CHECK(!etch::parse_level("nope").has_value());
  CHECK(!etch::parse_level("").has_value());
  CHECK(!etch::parse_level("information").has_value());  // longer than the name
}

TEST(level_has_a_std_formatter) {
  CHECK_EQ(std::format("<{}>", etch::Level::kError), std::string("<ERROR>"));
  CHECK_EQ(std::format("<{:>7}>", etch::Level::kInfo), std::string("<  INFO >"));
}

// ---- logger -------------------------------------------------------------------------------

TEST(runtime_level_filters_lower_levels) {
  Captured c;
  c.log.set_level(etch::Level::kWarn);
  c.log.info("dropped");
  c.log.warn("kept");
  c.log.error("kept too");
  CHECK_EQ(c.messages(), (std::vector<std::string>{"kept", "kept too"}));
  CHECK(!c.log.enabled(etch::Level::kInfo));
  CHECK(c.log.enabled(etch::Level::kWarn));
}

TEST(sink_level_filters_independently_of_logger) {
  auto quiet = std::make_shared<etch::MemorySink>();
  auto loud = std::make_shared<etch::MemorySink>();
  quiet->set_level(etch::Level::kError);
  etch::Logger log{"unit", {quiet, loud}};
  log.warn("w");
  log.error("e");
  CHECK_EQ(quiet->messages(), (std::vector<std::string>{"e"}));
  CHECK_EQ(loud->messages(), (std::vector<std::string>{"w", "e"}));
}

TEST(compile_time_min_level_removes_calls) {
  Captured c;
  c.log.set_level(etch::Level::kTrace);
  c.log.trace("t");
  c.log.debug("d");
  c.log.info("i");
  c.log.warn("w");
  c.log.error("e");
  c.log.log(etch::Level::kInfo, "runtime i");
  c.log.log(etch::Level::kError, "runtime e");
  const int floor = static_cast<int>(etch::kCompileTimeMinLevel);
  int expected = 0;
  for (int l = 0; l < 5; ++l) expected += (l >= floor) ? 1 : 0;         // trace .. error
  expected += (static_cast<int>(etch::Level::kInfo) >= floor) ? 1 : 0;  // runtime i
  expected += 1;                                                        // runtime e
  CHECK_EQ(static_cast<int>(c.messages().size()), expected);
  CHECK_EQ(floor, ETCH_MIN_LEVEL);
}

TEST(message_uses_std_format) {
  Captured c;
  c.log.set_level(etch::Level::kWarn);  // use warn: survives the min-level build too
  const std::string name = "lvalue";
  c.log.warn("{} + {} = {}", 1, 2, 3);
  c.log.warn("{:#06x} {:.1f} {{literal}} {}", 255, 2.25, name);
  c.log.warn("no args");
  c.log.warn("level {}", etch::Level::kDebug);
  CHECK_EQ(c.messages(), (std::vector<std::string>{"1 + 2 = 3", "0x00ff 2.2 {literal} lvalue",
                                                   "no args", "level DEBUG"}));
}

TEST(source_location_is_the_call_site) {
  Captured c;
  const unsigned expected_line = (c.log.warn("here"), __LINE__);  // one expression: same line
  const auto records = c.sink->records();
  CHECK_EQ(records.size(), 1u);
  if (records.empty()) return;
  CHECK_EQ(records[0].where.line(), expected_line);
  CHECK_EQ(etch::detail::base_name(records[0].where.file_name()), std::string_view("tests.cpp"));
  CHECK_EQ(records[0].logger_name, std::string("unit"));
  CHECK(records[0].thread == std::this_thread::get_id());
  CHECK(records[0].level == etch::Level::kWarn);
}

TEST(record_carries_the_time_of_the_call) {
  Captured c;
  const auto before = std::chrono::system_clock::now();
  c.log.warn("now");
  const auto after = std::chrono::system_clock::now();
  const auto records = c.sink->records();
  CHECK_EQ(records.size(), 1u);
  if (records.empty()) return;
  CHECK(records[0].time >= before && records[0].time <= after);
}

TEST(multiple_loggers_can_share_a_sink) {
  auto sink = std::make_shared<etch::MemorySink>();
  etch::Logger a{"alpha", {sink}};
  etch::Logger b{"beta", {sink}};
  a.warn("from a");
  b.warn("from b");
  const auto records = sink->records();
  CHECK_EQ(records.size(), 2u);
  if (records.size() < 2) return;
  CHECK_EQ(records[0].logger_name, std::string("alpha"));
  CHECK_EQ(records[1].logger_name, std::string("beta"));
}

TEST(default_logger_free_functions) {
  auto sink = std::make_shared<etch::MemorySink>();
  const auto previous = etch::default_logger().sinks();
  etch::default_logger().set_sinks({sink});
  etch::warn("free {}", "function");
  etch::default_logger().set_sinks(previous);
  CHECK_EQ(sink->messages(), (std::vector<std::string>{"free function"}));
  CHECK_EQ(etch::default_logger().name(), std::string("main"));
}

TEST(fatal_appends_a_stacktrace_when_enabled) {
  Captured with{"unit", {.stacktrace_on_fatal = true}};
  Captured without{"unit", {.stacktrace_on_fatal = false}};
  with.log.fatal("boom {}", 1);
  without.log.fatal("boom {}", 2);
  const auto a = with.messages();
  const auto b = without.messages();
  CHECK_EQ(a.size(), 1u);
  CHECK_EQ(b.size(), 1u);
  if (a.empty() || b.empty()) return;
  CHECK(a[0].starts_with("boom 1\n  stacktrace:"));
  CHECK_EQ(b[0], std::string("boom 2"));
}

// ---- line layout --------------------------------------------------------------------------

etch::Record fixed_record() {
  using namespace std::chrono;
  etch::Record r;
  r.level = etch::Level::kInfo;
  r.time = sys_days{2026y / January / 2} + 3h + 4min + 5s + 678ms;
  r.logger_name = "unit";
  r.message = "hello";
  r.where = std::source_location::current();
  return r;
}

TEST(line_layout_is_exact) {
  const auto r = fixed_record();
  etch::LineStyle style{.utc = true, .thread = false};
  const auto expected =
      std::format("2026-01-02 03:04:05.678 INFO  [unit] tests.cpp:{}: hello", r.where.line());
  CHECK_EQ(etch::format_line(r, style), expected);

  const etch::LineStyle bare{.time = false, .thread = false, .name = false, .location = false};
  CHECK_EQ(etch::format_line(r, bare), std::string("INFO  hello"));
}

TEST(thread_column_matches_std_format_of_the_id) {
  auto r = fixed_record();
  r.thread = std::this_thread::get_id();
  const etch::LineStyle style{.time = false, .name = false, .location = false};
  CHECK_EQ(etch::format_line(r, style), std::format("INFO  [{}] hello", r.thread));
}

TEST(colour_wraps_the_level_and_nothing_leaks_when_off) {
  const auto r = fixed_record();
  etch::LineStyle style{.utc = true, .thread = false, .colour = true};
  const auto coloured = etch::format_line(r, style);
  CHECK(coloured.find("\x1b[92mINFO \x1b[0m") != std::string::npos);
  style.colour = false;
  CHECK(etch::format_line(r, style).find(kEsc) == std::string::npos);
}

TEST(fatal_line_highlights_the_message_in_colour) {
  auto r = fixed_record();
  r.level = etch::Level::kFatal;
  const etch::LineStyle style{
      .time = false, .thread = false, .name = false, .location = false, .colour = true};
  CHECK_EQ(etch::format_line(r, style),
           std::string("\x1b[1;97;41mFATAL\x1b[0m \x1b[1;31mhello\x1b[0m"));
}

TEST(time_cache_tracks_the_second_and_the_zone_flag) {
  using namespace std::chrono;
  const auto base = sys_days{2026y / March / 29} + 23h + 59min + 59s + 999ms;
  const auto render = [](system_clock::time_point tp, bool utc) {
    std::string s;
    etch::format_time(s, tp, utc);
    return s;
  };
  // Same second, different milliseconds: the cached prefix is reused, the suffix is not.
  CHECK_EQ(render(base, true), std::string("2026-03-29 23:59:59.999"));
  CHECK_EQ(render(base - 998ms, true), std::string("2026-03-29 23:59:59.001"));
  // Next millisecond rolls the second, minute, hour and day: the prefix must be recomputed.
  CHECK_EQ(render(base + 1ms, true), std::string("2026-03-30 00:00:00.000"));
  // Switching between UTC and local in the same second must not reuse the other's prefix.
  const auto local = render(base, false);
  CHECK_EQ(render(base, true), std::string("2026-03-29 23:59:59.999"));
  CHECK_EQ(render(base, false), local);
}

TEST(logger_name_gets_a_stable_palette_colour) {
  const etch::Palette palette;
  const auto net = palette.name_colour("net");
  CHECK_EQ(net, palette.name_colour("net"));
  CHECK(std::find(palette.names.begin(), palette.names.end(), net) != palette.names.end());
  auto r = fixed_record();
  const etch::LineStyle style{.time = false, .thread = false, .location = false, .colour = true};
  const auto expected =
      std::format("\x1b[92mINFO \x1b[0m {}[unit]\x1b[0m hello", palette.name_colour("unit"));
  CHECK_EQ(etch::format_line(r, style), expected);
}

TEST(warn_and_error_messages_are_coloured_info_is_plain) {
  auto r = fixed_record();
  const etch::LineStyle style{
      .time = false, .thread = false, .name = false, .location = false, .colour = true};
  r.level = etch::Level::kWarn;
  CHECK_EQ(etch::format_line(r, style), std::string("\x1b[93mWARN \x1b[0m \x1b[33mhello\x1b[0m"));
  r.level = etch::Level::kError;
  CHECK_EQ(etch::format_line(r, style), std::string("\x1b[91mERROR\x1b[0m \x1b[31mhello\x1b[0m"));
  r.level = etch::Level::kInfo;
  CHECK_EQ(etch::format_line(r, style), std::string("\x1b[92mINFO \x1b[0m hello"));
}

TEST(appending_format_line_matches_the_returning_one) {
  const auto r = fixed_record();
  const etch::LineStyle style{.utc = true, .colour = true};
  std::string out = "prefix:";
  etch::format_line(r, style, out);
  CHECK_EQ(out, "prefix:" + etch::format_line(r, style));
}

TEST(local_time_differs_from_utc_only_by_the_zone_offset) {
  const auto r = fixed_record();
  std::string local;
  std::string utc;
  etch::format_time(local, r.time, false);
  etch::format_time(utc, r.time, true);
  CHECK_EQ(utc, std::string("2026-01-02 03:04:05.678"));
  CHECK(local.size() == utc.size() || local == utc + "Z");  // "Z": no tz database here
  CHECK(local.find(".678") != std::string::npos);           // the sub-second part is untouched
}

// ---- hexdump and scope --------------------------------------------------------------------

TEST(hexdump_layout_is_exact) {
  const std::array<unsigned char, 20> bytes{'H',  'e',  'l',  'l',  'o',  ',', ' ',
                                            'w',  'o',  'r',  'l',  'd',  '!', 0x00,
                                            0xff, 0x10, 0xde, 0xad, 0xbe, 0xef};
  const std::string expected =
      "pkt (20 bytes)\n"
      "  0000  48 65 6c 6c 6f 2c 20 77  6f 72 6c 64 21 00 ff 10  |Hello, world!...|\n"
      "  0010  de ad be ef                                       |....|";
  CHECK_EQ(etch::format_hexdump("pkt", bytes.data(), bytes.size()), expected);
  CHECK_EQ(etch::format_hexdump("empty", bytes.data(), 0), std::string("empty (0 bytes)"));
}

TEST(hexdump_uses_wide_offsets_and_keeps_columns_aligned) {
  const std::vector<unsigned char> big(0x10001, 0xaa);
  const auto text = etch::format_hexdump("big", big.data(), big.size());
  CHECK(text.starts_with("big (65537 bytes)\n  00000000  aa aa"));
  CHECK(text.ends_with("|.|"));
  // The ASCII column must start at the same offset on a full line and on the 1-byte last line.
  const auto first_line_start = text.find('\n') + 1;
  const auto last_line_start = text.rfind('\n') + 1;
  const auto first_line =
      text.substr(first_line_start, text.find('\n', first_line_start) - first_line_start);
  const auto last_line = text.substr(last_line_start);
  CHECK_EQ(first_line.find(" |"), last_line.find(" |"));
  CHECK(last_line.starts_with("  00010000  aa "));
}

TEST(hexdump_range_overload_counts_bytes_not_elements) {
  Captured c;
  const std::vector<std::uint32_t> words{0x04030201u, 0x08070605u};
  c.log.hexdump(etch::Level::kWarn, "words", words);
  const auto m = c.messages();
  CHECK_EQ(m.size(), 1u);
  if (m.empty()) return;
  CHECK(m[0].starts_with("words (8 bytes)\n  0000  "));
  if constexpr (std::endian::native == std::endian::little) {
    CHECK(m[0].find("01 02 03 04 05 06 07 08") != std::string::npos);
  }
}

TEST(hexdump_respects_the_level) {
  Captured c;
  c.log.set_level(etch::Level::kError);
  const std::array<char, 3> abc{'a', 'b', 'c'};
  c.log.hexdump(etch::Level::kWarn, "abc", abc);
  CHECK_EQ(c.messages().size(), 0u);
}

TEST(scope_timer_logs_begin_and_end) {
  Captured c;
  {
    auto timer = c.log.scope("load", etch::Level::kWarn);
    std::this_thread::sleep_for(2ms);
    CHECK(timer.elapsed() >= 2ms);
  }
  const auto m = c.messages();
  CHECK_EQ(m.size(), 2u);
  if (m.size() < 2) return;
  CHECK_EQ(m[0], std::string("load: begin"));
  CHECK(m[1].starts_with("load: done in "));
  CHECK(m[1].ends_with(" ms") || m[1].ends_with(" us") || m[1].ends_with(" s"));
}

TEST(format_duration_picks_a_readable_unit) {
  CHECK_EQ(etch::format_duration(750us), std::string("750 us"));
  CHECK_EQ(etch::format_duration(12'345us), std::string("12.345 ms"));
  CHECK_EQ(etch::format_duration(2'500ms), std::string("2.500 s"));
}

// ---- sinks --------------------------------------------------------------------------------

TEST(memory_sink_keeps_the_last_n) {
  auto sink = std::make_shared<etch::MemorySink>(3);
  etch::Logger log{"unit", {sink}};
  for (int i = 1; i <= 5; ++i) log.warn("{}", i);
  CHECK_EQ(sink->size(), 3u);
  CHECK_EQ(sink->total(), 5u);
  CHECK_EQ(sink->messages(), (std::vector<std::string>{"3", "4", "5"}));
  sink->clear();
  CHECK_EQ(sink->size(), 0u);
  CHECK_EQ(sink->total(), 0u);
}

TEST(callback_sink_receives_records) {
  std::vector<std::string> seen;
  auto sink = std::make_shared<etch::CallbackSink>([&](const etch::Record& r) {
    seen.push_back(std::format("{}:{}", etch::to_string(r.level), r.message));
  });
  etch::Logger log{"unit", {sink}};
  log.warn("one");
  log.error("two");
  CHECK_EQ(seen, (std::vector<std::string>{"WARN :one", "ERROR:two"}));
}

TEST(file_sink_writes_one_line_per_record) {
  const auto path = scratch_dir() / "plain.log";
  auto file = etch::FileSink::open(path, {.truncate = true});
  CHECK(file.has_value());
  if (!file) return;
  (*file)->set_style(etch::LineStyle{.time = false, .thread = false});
  etch::Logger log{"disk", {*file}};
  const int first_line = (log.warn("first"), __LINE__);
  const int second_line = (log.warn("second {}", 2), __LINE__);
  log.flush();
  const auto lines = read_lines(path);
  CHECK_EQ(lines, (std::vector<std::string>{
                      std::format("WARN  [disk] tests.cpp:{}: first", first_line),
                      std::format("WARN  [disk] tests.cpp:{}: second 2", second_line)}));
  if (lines.size() != 2) return;
  const auto bytes_on_disk = lines[0].size() + lines[1].size() + 2;  // two newlines
  CHECK_EQ((*file)->bytes_written(), static_cast<std::uintmax_t>(bytes_on_disk));
  CHECK_EQ(std::filesystem::file_size(path), (*file)->bytes_written());  // binary mode: no CRLF
  CHECK_EQ((*file)->write_failures(), 0u);
}

TEST(file_sink_appends_by_default) {
  const auto path = scratch_dir() / "append.log";
  {
    auto file = etch::FileSink::open(path, {.truncate = true});
    CHECK(file.has_value());
    if (!file) return;
    etch::Logger log{"disk", {*file}};
    log.warn("run 1");
  }
  {
    auto file = etch::FileSink::open(path);
    CHECK(file.has_value());
    if (!file) return;
    etch::Logger log{"disk", {*file}};
    log.warn("run 2");
    CHECK((*file)->bytes_written() > std::string_view("run 1").size());  // counted the old bytes
  }
  const auto lines = read_lines(path);
  CHECK_EQ(lines.size(), 2u);
  if (lines.size() == 2) CHECK(lines[0].ends_with("run 1") && lines[1].ends_with("run 2"));
}

TEST(file_sink_reports_open_errors_with_expected) {
  const auto path = scratch_dir() / "no-such-dir" / "x.log";
  auto file = etch::FileSink::open(path);
  CHECK(!file.has_value());
  if (file) return;
  CHECK(static_cast<bool>(file.error()));
  CHECK(!file.error().message().empty());
}

TEST(file_sink_rotates_at_max_bytes_and_keeps_every_line) {
  const auto path = scratch_dir() / "rotate.log";
  constexpr std::uintmax_t kMaxBytes = 400;
  constexpr unsigned kMaxFiles = 10;
  constexpr int kLines = 60;
  auto file = etch::FileSink::open(
      path, {.max_bytes = kMaxBytes, .max_files = kMaxFiles, .truncate = true});
  CHECK(file.has_value());
  if (!file) return;
  (*file)->set_style(etch::LineStyle{.time = false, .thread = false, .location = false});
  etch::Logger log{"r", {*file}};
  for (int i = 0; i < kLines; ++i) log.warn("line {:02}", i);
  log.flush();

  CHECK((*file)->rotations() > 0);
  CHECK((*file)->rotations() < kMaxFiles);  // otherwise lines were deleted and the count below lies
  CHECK((*file)->bytes_written() <= kMaxBytes);
  std::vector<std::string> all;
  for (unsigned n = (*file)->rotations(); n >= 1; --n) {
    auto p = path;
    p += "." + std::to_string(n);
    CHECK(std::filesystem::exists(p));
    const auto part = read_lines(p);
    CHECK(!part.empty());
    all.insert(all.end(), part.begin(), part.end());
  }
  const auto live = read_lines(path);
  all.insert(all.end(), live.begin(), live.end());
  CHECK_EQ(all.size(), static_cast<std::size_t>(kLines));
  for (int i = 0; i < kLines && i < static_cast<int>(all.size()); ++i) {
    CHECK_EQ(all[i], std::format("WARN  [r] line {:02}", i));
  }
}

TEST(file_sink_rotation_drops_the_oldest_beyond_max_files) {
  const auto path = scratch_dir() / "cap.log";
  auto file = etch::FileSink::open(path, {.max_bytes = 60, .max_files = 2, .truncate = true});
  CHECK(file.has_value());
  if (!file) return;
  (*file)->set_style(etch::LineStyle{.time = false, .thread = false, .location = false});
  etch::Logger log{"c", {*file}};
  for (int i = 0; i < 40; ++i) log.warn("{:02}", i);
  log.flush();
  CHECK((*file)->rotations() > 2);
  CHECK(std::filesystem::exists(path.string() + ".1"));
  CHECK(std::filesystem::exists(path.string() + ".2"));
  CHECK(!std::filesystem::exists(path.string() + ".3"));
}

#if defined(_WIN32)
TEST(file_sink_keeps_appending_when_rotation_cannot_move_the_file) {
  const auto path = scratch_dir() / "stuck.log";
  auto file = etch::FileSink::open(path, {.max_bytes = 60, .max_files = 1, .truncate = true});
  CHECK(file.has_value());
  if (!file) return;
  (*file)->set_style(etch::LineStyle{.time = false, .thread = false, .location = false});
  etch::Logger log{"s", {*file}};
  // Windows refuses to replace or delete a file another handle holds open without delete
  // sharing, which makes the rename in rotate() fail. (Linux allows it, so this is Windows-only.)
  std::ofstream blocker(path.string() + ".1");
  blocker << "held open";
  blocker.flush();
  for (int i = 0; i < 30; ++i) log.warn("{:02}", i);
  log.flush();
  blocker.close();
  CHECK((*file)->write_failures() > 0);
  CHECK_EQ((*file)->rotations(), 0u);
  const auto lines = read_lines(path);
  CHECK_EQ(lines.size(), 30u);  // nothing lost: the live file was appended to, not truncated
  for (int i = 0; i < 30 && i < static_cast<int>(lines.size()); ++i) {
    CHECK_EQ(lines[i], std::format("WARN  [s] {:02}", i));
  }
}
#endif

TEST(console_sink_can_force_colour_off_and_on) {
  etch::ConsoleSink off(stdout, etch::ConsoleSink::ColourMode::kNever);
  etch::ConsoleSink on(stdout, etch::ConsoleSink::ColourMode::kAlways);
  CHECK(!off.style().colour);
  CHECK(on.style().colour);
}

// ---- async --------------------------------------------------------------------------------

TEST(file_sink_with_max_files_zero_restarts_the_file) {
  const auto path = scratch_dir() / "restart.log";
  auto file = etch::FileSink::open(path, {.max_bytes = 60, .max_files = 0, .truncate = true});
  CHECK(file.has_value());
  if (!file) return;
  (*file)->set_style(etch::LineStyle{.time = false, .thread = false, .location = false});
  etch::Logger log{"z", {*file}};
  for (int i = 0; i < 40; ++i) log.warn("{:02}", i);
  log.flush();
  CHECK((*file)->rotations() > 0);
  CHECK((*file)->bytes_written() <= 60u);
  CHECK(!std::filesystem::exists(path.string() + ".0"));
  CHECK(!std::filesystem::exists(path.string() + ".1"));
  CHECK_EQ((*file)->write_failures(), 0u);
}

TEST(sinks_can_be_replaced_while_other_threads_log) {
  constexpr int kThreads = 4;
  constexpr int kPerThread = 2000;
  auto always = std::make_shared<etch::MemorySink>(kThreads * kPerThread);
  auto sometimes = std::make_shared<etch::MemorySink>(kThreads * kPerThread);
  etch::Logger log{"swap", {always}};
  std::atomic<bool> go{false};
  {
    std::vector<std::jthread> workers;
    for (int t = 0; t < kThreads; ++t) {
      workers.emplace_back([&] {
        while (!go.load()) {
        }
        for (int i = 0; i < kPerThread; ++i) log.warn("{}", i);
      });
    }
    go = true;
    for (int i = 0; i < 200; ++i) {
      log.set_sinks({always, sometimes});
      log.add_sink(std::make_shared<etch::MemorySink>(1));
      log.set_sinks({always});
    }
  }
  CHECK_EQ(always->total(), static_cast<std::size_t>(kThreads * kPerThread));
  CHECK(sometimes->total() <= always->total());
  CHECK_EQ(log.sinks().size(), 1u);
}

TEST(a_sink_callback_may_change_the_logger_sinks) {
  auto late = std::make_shared<etch::MemorySink>();
  etch::Logger log{"re", {}};
  bool added = false;
  auto hook = std::make_shared<etch::CallbackSink>([&](const etch::Record&) {
    if (!added) {
      added = true;
      log.add_sink(late);  // allowed: the logger holds no lock while calling sinks
    }
  });
  log.add_sink(hook);
  log.warn("first");   // triggers the add
  log.warn("second");  // reaches both
  CHECK_EQ(late->messages(), (std::vector<std::string>{"second"}));
}

TEST(null_sinks_are_ignored_and_scope_needs_an_lvalue_logger) {
  etch::Logger log{"n", {nullptr, std::make_shared<etch::MemorySink>(), nullptr}};
  CHECK_EQ(log.sinks().size(), 1u);
  log.add_sink(nullptr);
  CHECK_EQ(log.sinks().size(), 1u);
  log.warn("does not crash");
  static_assert(!ScopeCallable<etch::Logger&&>, "scope() on a temporary logger would dangle");
  static_assert(ScopeCallable<etch::Logger&>);
}

TEST(emit_applies_the_logger_level) {
  Captured c;
  c.log.set_level(etch::Level::kError);
  c.log.emit(etch::Level::kWarn, std::source_location::current(), "filtered");
  c.log.emit(etch::Level::kError, std::source_location::current(), "kept");
  CHECK_EQ(c.messages(), (std::vector<std::string>{"kept"}));
}

TEST(async_applies_the_inner_sinks_level) {
  auto memory = std::make_shared<etch::MemorySink>();
  memory->set_level(etch::Level::kError);
  auto async = std::make_shared<etch::AsyncSink>(memory);
  etch::Logger log{"lvl", {async}};
  log.warn("w");
  log.error("e");
  async->flush();
  CHECK_EQ(memory->messages(), (std::vector<std::string>{"e"}));
  CHECK_EQ(async->queued(), 0u);
}

TEST(async_survives_an_inner_sink_that_throws) {
  std::vector<std::string> delivered;
  auto fragile = std::make_shared<etch::CallbackSink>([&](const etch::Record& r) {
    if (r.message == "boom") throw std::runtime_error("sink failed");
    delivered.push_back(r.message);
  });
  auto async = std::make_shared<etch::AsyncSink>(fragile);
  etch::Logger log{"throw", {async}};
  log.warn("a");
  log.warn("boom");
  log.warn("b");
  async->flush();
  CHECK_EQ(delivered, (std::vector<std::string>{"a", "b"}));
  CHECK_EQ(async->write_failures(), 1u);
}

TEST(async_delivers_everything_and_keeps_per_thread_order) {
  constexpr int kThreads = 8;
  constexpr int kPerThread = 500;
  auto memory = std::make_shared<etch::MemorySink>(kThreads * kPerThread);
  auto async = std::make_shared<etch::AsyncSink>(memory, 64, etch::AsyncSink::Overflow::kBlock);
  etch::Logger log{"mt", {async}};
  {
    std::vector<std::jthread> workers;
    for (int t = 0; t < kThreads; ++t) {
      workers.emplace_back([&log, t] {
        for (int i = 0; i < kPerThread; ++i) log.warn("{} {}", t, i);
      });
    }
  }
  async->flush();
  CHECK_EQ(async->dropped(), 0u);
  CHECK_EQ(async->queued(), 0u);
  const auto messages = memory->messages();
  CHECK_EQ(messages.size(), static_cast<std::size_t>(kThreads * kPerThread));
  std::array<int, kThreads> next{};
  int out_of_order = 0;
  for (const auto& m : messages) {
    const auto space = m.find(' ');
    const int t = space == std::string::npos ? -1 : std::stoi(m.substr(0, space));
    const int i = space == std::string::npos ? -1 : std::stoi(m.substr(space + 1));
    if (t < 0 || t >= kThreads || i != next[t]++) ++out_of_order;
  }
  CHECK_EQ(out_of_order, 0);
}

// A sink that takes a while per record, to make the queue fill up. Spins instead of sleeping:
// sleep_for on Windows rounds up to the scheduler tick, which would make these tests slow.
std::shared_ptr<etch::CallbackSink> slow_sink(std::atomic<int>& delivered,
                                              std::chrono::microseconds per_record) {
  return std::make_shared<etch::CallbackSink>([&delivered, per_record](const etch::Record&) {
    const auto until = std::chrono::steady_clock::now() + per_record;
    while (std::chrono::steady_clock::now() < until) {
    }
    delivered.fetch_add(1);
  });
}

TEST(async_drop_policy_counts_what_it_loses) {
  constexpr int kSent = 500;
  constexpr std::size_t kCapacity = 8;
  std::atomic<int> delivered{0};
  {
    auto async = std::make_shared<etch::AsyncSink>(slow_sink(delivered, 500us), kCapacity,
                                                   etch::AsyncSink::Overflow::kDrop);
    etch::Logger log{"drop", {async}};
    for (int i = 0; i < kSent; ++i) log.warn("{}", i);
    async->flush();
    CHECK(async->dropped() > 0);
    CHECK_EQ(static_cast<int>(async->dropped()) + delivered.load(), kSent);
  }
}

TEST(async_flush_waits_for_the_queue_to_drain) {
  std::atomic<int> delivered{0};
  auto async = std::make_shared<etch::AsyncSink>(slow_sink(delivered, 200us), 1000);
  etch::Logger log{"flush", {async}};
  for (int i = 0; i < 100; ++i) log.warn("{}", i);
  log.flush();
  CHECK_EQ(delivered.load(), 100);
  CHECK_EQ(async->queued(), 0u);
}

TEST(async_destructor_drains_before_exiting) {
  std::atomic<int> delivered{0};
  {
    auto async = std::make_shared<etch::AsyncSink>(slow_sink(delivered, 200us), 1000);
    etch::Logger log{"dtor", {async}};
    for (int i = 0; i < 50; ++i) log.warn("{}", i);
  }
  CHECK_EQ(delivered.load(), 50);
}

TEST(async_block_policy_never_drops) {
  std::atomic<int> delivered{0};
  {
    auto async = std::make_shared<etch::AsyncSink>(slow_sink(delivered, 100us), 4,
                                                   etch::AsyncSink::Overflow::kBlock);
    etch::Logger log{"block", {async}};
    for (int i = 0; i < 200; ++i) log.warn("{}", i);
    CHECK_EQ(async->dropped(), 0u);
  }
  CHECK_EQ(delivered.load(), 200);
}

TEST(async_flush_returns_while_another_thread_keeps_logging) {
  std::atomic<int> delivered{0};
  auto async = std::make_shared<etch::AsyncSink>(slow_sink(delivered, 20us), 256);
  etch::Logger log{"busy", {async}};
  std::atomic<bool> stop{false};
  std::atomic<bool> flushed{false};
  std::jthread producer([&] {
    while (!stop.load()) log.warn("noise");
  });
  // Watchdog: if flush waits for the producer to go idle, it would never return. Stop the
  // producer after a while so the test fails on the timing check instead of hanging.
  std::jthread watchdog([&] {
    for (int i = 0; i < 300 && !flushed.load(); ++i) std::this_thread::sleep_for(10ms);
    stop = true;
  });
  // Only flush once the producer is demonstrably ahead of the worker, otherwise a flush that
  // waits for an idle queue would also return at once and prove nothing.
  while (delivered.load() < 50 || async->queued() < 50) std::this_thread::yield();
  log.warn("before flush");
  const auto start = std::chrono::steady_clock::now();
  log.flush();
  const auto took = std::chrono::steady_clock::now() - start;
  flushed = true;
  stop = true;
  CHECK(took < 2s);
}

}  // namespace

int main(int argc, char** argv) {
  std::println("etch tests (ETCH_MIN_LEVEL={}), scratch files in {}", ETCH_MIN_LEVEL,
               scratch_dir().string());
  const int result = check::run(argc, argv);
  std::error_code ec;
  std::filesystem::remove_all(scratch_dir(), ec);
  return result;
}
