// etch tests: the text line layout, colour, and the timestamp cache.
#include "support.hpp"

namespace {

using namespace std::chrono_literals;

// ---- line layout --------------------------------------------------------------------------

TEST(line_layout_is_exact) {
  const auto r = fixed_record();
  etch::LineStyle style{.utc = true, .thread = false};
  const auto expected = std::format("2026-01-02 03:04:05.678Z INFO  [unit] {}:{}: hello",
                                    etch::detail::base_name(r.where.file_name()), r.where.line());
  CHECK_EQ(etch::format_line(r, style), expected);

  const etch::LineStyle bare{.time = false, .thread = false, .name = false, .location = false};
  CHECK_EQ(etch::format_line(r, bare), std::string("INFO  hello"));
}

TEST(thread_column_is_the_small_thread_number) {
  auto r = fixed_record();
  r.thread_index = 42;
  const etch::LineStyle style{.time = false, .name = false, .location = false};
  CHECK_EQ(etch::format_line(r, style), std::format("INFO  [{}] hello", r.thread_index));
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
  CHECK_EQ(render(base, true), std::string("2026-03-29 23:59:59.999Z"));
  CHECK_EQ(render(base - 998ms, true), std::string("2026-03-29 23:59:59.001Z"));
  // Next millisecond rolls the second, minute, hour and day: the prefix must be recomputed.
  CHECK_EQ(render(base + 1ms, true), std::string("2026-03-30 00:00:00.000Z"));
  // Switching between UTC and local in the same second must not reuse the other's prefix.
  const auto local = render(base, false);
  CHECK_EQ(render(base, true), std::string("2026-03-29 23:59:59.999Z"));
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
  CHECK_EQ(utc, std::string("2026-01-02 03:04:05.678Z"));
#if ETCH_HAS_TIME_ZONES
  // Independent expectation: the standard's own zoned formatting of the same instant.
  if (const auto* zone = etch::detail::cached_local_zone()) {
    using namespace std::chrono;
    const auto expected = std::format("{:%F %T}", zoned_time{zone, floor<milliseconds>(r.time)});
    CHECK_EQ(local, expected);
  } else {
    CHECK_EQ(local, utc);  // no tz database: local falls back to UTC, marked the same way
  }
#else
  CHECK_EQ(local, utc);
#endif
}

TEST(text_fields_escape_quotes_backslashes_and_newlines) {
  auto r = fixed_record();
  r.fields = {{"q", std::string("say \"hi\" and\nmore \\ done")}};
  const etch::LineStyle bare{.time = false, .thread = false, .name = false, .location = false};
  CHECK_EQ(etch::format_line(r, bare),
           std::string("INFO  hello q=\"say \\\"hi\\\" and\\nmore \\\\ done\""));
}

TEST(two_timestamp_modes_on_one_thread_keep_their_own_cache) {
  // A text sink (local) and a JSON sink (UTC) format the same instant in turn; each mode must
  // reproduce its own result, which fails if one cache entry serves both.
  const auto r = fixed_record();
  std::string local1, utc1, local2, utc2;
  etch::format_time(local1, r.time, false);
  etch::format_time(utc1, r.time, true);
  etch::format_time(local2, r.time, false);
  etch::format_time(utc2, r.time, true);
  CHECK_EQ(local1, local2);
  CHECK_EQ(utc1, utc2);
  CHECK_EQ(utc1, std::string("2026-01-02 03:04:05.678Z"));
}

TEST(message_newlines_continue_indented_and_controls_are_escaped) {
  auto r = fixed_record();
  r.message = "line1\nline2\x1b[2J\x01 tab\tok";
  const etch::LineStyle bare{.time = false, .thread = false, .name = false, .location = false};
  CHECK_EQ(etch::format_line(r, bare), std::string("INFO  line1\n  line2\\e[2J\\x01 tab\tok"));
}

TEST(utc_lines_end_with_z_and_the_date_can_be_dropped) {
  const auto r = fixed_record();
  const etch::LineStyle utc{.utc = true, .thread = false, .name = false, .location = false};
  CHECK_EQ(etch::format_line(r, utc), std::string("2026-01-02 03:04:05.678Z INFO  hello"));
  const etch::LineStyle short_time{
      .date = false, .utc = true, .thread = false, .name = false, .location = false};
  CHECK_EQ(etch::format_line(r, short_time), std::string("03:04:05.678Z INFO  hello"));
}

TEST(colour_unset_lets_the_sink_decide) {
  etch::ConsoleSink on(stdout, etch::ConsoleSink::ColourMode::kAlways);
  on.set_style(etch::LineStyle{.thread = false});  // colour not mentioned: stays on
  CHECK(on.style().colour == true);
  CHECK(on.style().thread == false);
  on.set_style(etch::LineStyle{.colour = false});
  CHECK(on.style().colour == false);
  etch::ConsoleSink off(stdout, etch::ConsoleSink::ColourMode::kNever,
                        etch::LineStyle{.name = false});
  CHECK(off.style().colour == false);
  CHECK(off.style().name == false);
}

}  // namespace
