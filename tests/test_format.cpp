// etch tests: the text line layout, colour, and the timestamp cache.
#include "support.hpp"

namespace {

using namespace std::chrono_literals;

// ---- line layout --------------------------------------------------------------------------

TEST(line_layout_is_exact) {
  const auto r = fixed_record();
  etch::LineStyle style{.utc = true, .thread = false};
  const auto expected = std::format("2026-01-02 03:04:05.678 INFO  [unit] {}:{}: hello",
                                    etch::detail::base_name(r.where.file_name()), r.where.line());
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

}  // namespace
