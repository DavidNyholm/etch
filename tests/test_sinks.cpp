// etch tests: file, memory, callback and console sinks.
#include "support.hpp"

namespace {

using namespace std::chrono_literals;

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
  const auto here = etch::detail::base_name(std::source_location::current().file_name());
  const int first_line = (log.warn("first"), __LINE__);
  const int second_line = (log.warn("second {}", 2), __LINE__);
  log.flush();
  const auto lines = read_lines(path);
  CHECK_EQ(lines, (std::vector<std::string>{
                      std::format("WARN  [disk] {}:{}: first", here, first_line),
                      std::format("WARN  [disk] {}:{}: second 2", here, second_line)}));
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

}  // namespace
