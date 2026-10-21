// etch tests: levels, the Logger front door, hexdump and scope timers.
#include "support.hpp"

namespace {

using namespace std::chrono_literals;

// ---- levels -------------------------------------------------------------------------------

TEST(level_names_are_five_wide_and_round_trip) {
  for (auto l : {etch::Level::kTrace, etch::Level::kDebug, etch::Level::kInfo, etch::Level::kWarn,
                 etch::Level::kError, etch::Level::kFatal}) {
    CHECK(etch::to_string(l).size() <= 5u);
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
  CHECK(etch::parse_level("information") == etch::Level::kInfo);
}

TEST(level_has_a_std_formatter) {
  CHECK_EQ(std::format("<{}>", etch::Level::kError), std::string("<ERROR>"));
  CHECK_EQ(std::format("<{:>7}>", etch::Level::kInfo), std::string("<   INFO>"));
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
  CHECK_EQ(etch::detail::base_name(records[0].where.file_name()),
           etch::detail::base_name(std::source_location::current().file_name()));
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
  const auto text = etch::format_hexdump("big", big.data(), big.size(), big.size());
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

TEST(level_off_accepts_nothing_and_parses) {
  CHECK(etch::parse_level("off") == etch::Level::kOff);
  CHECK(etch::parse_level("NONE") == etch::Level::kOff);
  CHECK(etch::parse_level("critical") == etch::Level::kFatal);
  CHECK(etch::parse_level("\tinfo\r\n") == etch::Level::kInfo);
  auto sink = std::make_shared<etch::MemorySink>();
  sink->set_level(etch::Level::kOff);
  CHECK(!sink->accepts(etch::Level::kFatal));
  etch::Logger log{"mute", {sink}};
  log.fatal("silent");
  CHECK_EQ(sink->size(), 0u);
  Captured c;
  c.log.set_level(etch::Level::kOff);
  c.log.fatal("silent too");
  CHECK_EQ(c.messages().size(), 0u);
}

enum class Colour : std::uint8_t { kRed = 1, kGreen = 2 };

TEST(field_values_convert_common_types) {
  const std::vector<int> v(3);
  const std::string_view sv = "view";
  const std::filesystem::path p = "dir/file.txt";
  const etch::Fields fields{
      {"size", v.size()},
      {"big", std::numeric_limits<std::uint64_t>::max()},
      {"view", sv},
      {"colour", Colour::kGreen},
      {"took", std::chrono::milliseconds(3)},
      {"path", p},
      {"letter", 'w'},
      {"ratio", 0.5f},
      {"negative", -7},
  };
  CHECK(std::get<std::int64_t>(fields[0].value) == 3);
  CHECK_EQ(std::get<std::string>(fields[1].value), std::string("18446744073709551615"));
  CHECK_EQ(std::get<std::string>(fields[2].value), std::string("view"));
  CHECK(std::get<std::int64_t>(fields[3].value) == 2);
  CHECK(std::get<std::int64_t>(fields[4].value) == 3'000'000);
  CHECK_EQ(std::get<std::string>(fields[5].value), etch::utf8(p));
  CHECK_EQ(std::get<std::string>(fields[6].value), std::string("w"));
  CHECK(std::get<double>(fields[7].value) == 0.5);
  CHECK(std::get<std::int64_t>(fields[8].value) == -7);
}

template <class E>
concept LvalueEntryUsable = requires(E& e) { e.info("x"); };

TEST(with_entry_is_single_use_and_carries_fields_everywhere) {
  static_assert(!LvalueEntryUsable<etch::Logger::Entry>, "an entry is used once, as a temporary");
  Captured c;
  c.log.with({{"k", 1}}).log(etch::Level::kWarn, "runtime {}", "level");
  const std::array<unsigned char, 2> data{1, 2};
  c.log.with({{"k", 2}}).hexdump(etch::Level::kWarn, "dump", data);
  {
    auto timer = c.log.with({{"k", 3}}).scope("timed", {.level = etch::Level::kWarn});
  }
  const auto records = c.sink->records();
  CHECK_EQ(records.size(), 4u);  // log, hexdump, scope begin, scope end
  if (records.size() < 4) return;
  for (const auto& r : records) CHECK_EQ(r.fields.size(), 1u);
  CHECK_EQ(records[0].message, std::string("runtime level"));
  CHECK_EQ(records[1].bytes.size(), 2u);
  CHECK(records[3].duration.has_value());
  // A filtered entry costs nothing and logs nothing.
  c.log.set_level(etch::Level::kOff);
  c.log.with({{"k", 4}}).warn("dropped");
  CHECK_EQ(c.sink->total(), 4u);
}

TEST(child_loggers_share_sinks_and_the_registry_returns_one_object_per_name) {
  auto first = std::make_shared<etch::MemorySink>();
  etch::Logger parent{"app", {first}};
  etch::Logger net = parent.child("net");
  net.warn("hello");
  auto second = std::make_shared<etch::MemorySink>();
  parent.add_sink(second);  // after the child was made: the child must see it
  net.warn("again");
  CHECK_EQ(first->messages(), (std::vector<std::string>{"hello", "again"}));
  CHECK_EQ(second->messages(), (std::vector<std::string>{"again"}));
  CHECK_EQ(first->records().back().logger_name, std::string("net"));

  etch::Logger& a = etch::get("registry-test");
  etch::Logger& b = etch::get("registry-test");
  CHECK(&a == &b);
  CHECK_EQ(a.name(), std::string("registry-test"));
  CHECK(a.sinks().size() == etch::default_logger().sinks().size());
}

TEST(scope_options_threshold_begin_flag_and_level_change) {
  Captured c;
  {
    auto quiet = c.log.scope(
        "fast",
        {.level = etch::Level::kWarn, .begin = false, .min_duration = std::chrono::hours(1)});
  }
  CHECK_EQ(c.messages().size(), 0u);  // no begin, and too fast for an end
  {
    auto timer = c.log.scope("steady", etch::Level::kWarn);
    c.log.set_level(etch::Level::kOff);  // the end must still appear: decided at the begin
  }
  c.log.set_level(etch::Level::kTrace);
  const auto m = c.messages();
  CHECK_EQ(m.size(), 2u);
  if (m.size() == 2) CHECK(m[1].starts_with("steady: done in "));
}

TEST(hexdump_limit_caps_text_and_bytes_and_null_is_reported) {
  Captured c;
  const std::vector<unsigned char> big(10000, 0x5a);
  c.log.hexdump(etch::Level::kWarn, "big", big);
  c.log.hexdump(etch::Level::kWarn, "nothing", nullptr, 16);
  const auto records = c.sink->records();
  CHECK_EQ(records.size(), 2u);
  if (records.size() < 2) return;
  CHECK(records[0].message.ends_with("... 5904 more bytes not shown"));
  CHECK_EQ(records[0].bytes.size(), etch::kDefaultHexdumpLimit);
  CHECK_EQ(records[0].user_message(), std::string_view("big (10000 bytes)"));
  CHECK(records[1].message.find("(null pointer)") != std::string::npos);
  CHECK(records[1].bytes.empty());
}

TEST(fatal_keeps_the_whole_user_message_and_leaves_out_etch_frames) {
  Captured c{"unit", {.stacktrace_on_fatal = true}};
  c.log.fatal("first line\nsecond line");
  const auto records = c.sink->records();
  CHECK_EQ(records.size(), 1u);
  if (records.empty()) return;
  CHECK_EQ(records[0].user_message(), std::string_view("first line\nsecond line"));
  CHECK(records[0].appended_text().starts_with("\n  stacktrace:"));
  const std::string_view own = etch::detail::etch_include_dir();
  for (const auto& frame : records[0].stack) {
    CHECK(!etch::detail::same_path_prefix(frame.file, own));
  }
}

TEST(thread_numbers_are_small_and_stable) {
  Captured c;
  c.log.warn("a");
  c.log.warn("b");
  std::jthread([&] { c.log.warn("c"); }).join();
  const auto records = c.sink->records();
  CHECK_EQ(records.size(), 3u);
  if (records.size() < 3) return;
  CHECK(records[0].thread_index > 0);
  CHECK_EQ(records[0].thread_index, records[1].thread_index);
  CHECK(records[2].thread_index != records[0].thread_index);
}

TEST(null_arguments_throw_at_construction) {
  bool threw = false;
  try {
    etch::AsyncSink bad(nullptr);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  threw = false;
  try {
    etch::ConsoleSink bad(nullptr);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  threw = false;
  try {
    etch::CallbackSink bad(etch::CallbackSink::Callback{});
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}

}  // namespace
