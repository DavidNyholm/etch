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

// Child probe: tries a truncating open of a FileSink on the path the parent holds, and expects
// what ETCH_TEST_EXPECT says ("refused" or "opened").
TEST(zz_child_probe_file_held_by_another_process) {
  if (!in_child()) return;
  const std::string path(etch::detail::env("ETCH_TEST_HELD_PATH"));
  const auto second = etch::FileSink::open(path, {.truncate = true});
  if (etch::detail::env("ETCH_TEST_EXPECT") == "refused") {
    CHECK(!second.has_value());
    CHECK(!second.has_value() && second.error() == std::errc::device_or_resource_busy);
  } else {
    CHECK(second.has_value());
  }
}

TEST(a_file_held_by_another_process_is_refused_untouched_until_it_is_released) {
  const auto path = scratch_dir() / "held.log";
  const std::string path_text = path.string();
  auto file = etch::FileSink::open(path, {.truncate = true});
  CHECK(file.has_value());
  if (!file) return;
  CHECK((*file)->holds_lock());
  CHECK(std::filesystem::exists(scratch_dir() / "held.log.lock"));
  {
    etch::Logger log{"held", {*file}};  // scoped: a logger keeps its sinks alive
    log.warn("kept");
  }
  (*file)->flush();
  const auto before = read_bytes(path);
  CHECK(before.size() > 1);
  // The other process asks for a truncating open: refused, and the file is untouched.
  CHECK_EQ(run_test_in_child(
               "zz_child_probe_file_held_by_another_process",
               {{"ETCH_TEST_HELD_PATH", path_text.c_str()}, {"ETCH_TEST_EXPECT", "refused"}}),
           0);
  CHECK(read_bytes(path) == before);
  file->reset();
  CHECK_EQ(run_test_in_child(
               "zz_child_probe_file_held_by_another_process",
               {{"ETCH_TEST_HELD_PATH", path_text.c_str()}, {"ETCH_TEST_EXPECT", "opened"}}),
           0);
  CHECK_EQ(std::filesystem::file_size(path), 0u);  // the other process truncated it this time
}

#if !defined(_WIN32)
TEST(a_file_reached_through_a_symlink_is_the_same_file_to_the_lock) {
  const auto real = scratch_dir() / "real.log";
  const auto alias = scratch_dir() / "alias.log";
  auto first = etch::FileSink::open(real, {.truncate = true});
  CHECK(first.has_value());
  if (!first) return;
  std::error_code ec;
  std::filesystem::create_symlink(real, alias, ec);
  CHECK(!ec);
  if (ec) return;
  const auto second = etch::FileSink::open(alias, {.truncate = true});
  CHECK(!second.has_value() && second.error() == std::errc::device_or_resource_busy);
  CHECK((*first)->path() == std::filesystem::weakly_canonical(real));
  CHECK_EQ(std::filesystem::file_size(real), 0u);  // nothing was written to it
}

TEST(a_symlink_to_a_file_not_made_yet_is_the_same_file_to_the_lock) {
  const auto real = scratch_dir() / "dangling-real.log";
  const auto alias = scratch_dir() / "dangling-alias.log";
  std::error_code ec;
  std::filesystem::create_symlink(real, alias, ec);  // points at nothing yet
  CHECK(!ec);
  if (ec) return;
  auto through_alias = etch::FileSink::open(alias, {.truncate = true});
  CHECK(through_alias.has_value());
  if (!through_alias) return;
  CHECK((*through_alias)->path() == std::filesystem::weakly_canonical(real));
  etch::Logger log{"dangling", {*through_alias}};
  log.warn("kept");
  (*through_alias)->flush();
  // The real name now: the same file, so refused, and its line untouched.
  const auto by_real_name = etch::FileSink::open(real, {.truncate = true});
  CHECK(!by_real_name.has_value() && by_real_name.error() == std::errc::device_or_resource_busy);
  CHECK_EQ(read_lines(real).size(), 1u);
}
#endif

#if !defined(_WIN32)
TEST(a_symlink_chain_too_long_to_follow_is_refused_rather_than_locked_halfway) {
  constexpr int kLinks = 40;  // past the resolver's limit
  const auto real = scratch_dir() / "chain-real.log";
  std::filesystem::path next = real;
  std::error_code ec;
  for (int i = kLinks - 1; i >= 0 && !ec; --i) {
    const auto link = scratch_dir() / std::format("chain-{}.log", i);
    std::filesystem::create_symlink(next, link, ec);
    next = link;
  }
  CHECK(!ec);
  if (ec) return;
  const auto sink = etch::FileSink::open(next, {.truncate = true});  // chain-0 -> ... -> real
  CHECK(!sink.has_value() && sink.error() == std::errc::too_many_symbolic_link_levels);
  CHECK(!std::filesystem::exists(real));  // nothing was made along the way
}
#endif

TEST(a_log_named_like_a_lock_file_is_refused) {
  const auto refused = etch::FileSink::open(scratch_dir() / "not-a-log.lock");
  CHECK(!refused.has_value() && refused.error() == std::errc::invalid_argument);
#if defined(_WIN32)
  const auto other_case = etch::FileSink::open(scratch_dir() / "not-a-log.LOCK");
  CHECK(!other_case.has_value() && other_case.error() == std::errc::invalid_argument);
#endif
}

TEST(rotation_does_not_move_over_a_file_another_sink_holds_as_its_log) {
  const auto path = scratch_dir() / "rotating.log";
  auto rotated_into = scratch_dir() / "rotating.log";
  rotated_into += ".1";
  constexpr std::uintmax_t kMaxBytes = 120;
  auto first =
      etch::FileSink::open(path, {.max_bytes = kMaxBytes, .max_files = 1, .truncate = true});
  auto second = etch::FileSink::open(rotated_into, {.truncate = true});
  CHECK(first.has_value() && second.has_value());
  if (!first || !second) return;
  {
    etch::Logger other{"other", {*second}};
    other.warn("the other sink's line");
    (*second)->flush();
    etch::Logger log{"rot", {*first}};
    for (int i = 0; i < 20; ++i) log.warn("line {} is long enough to need a rotation soon", i);
    (*first)->flush();
  }
  // No rotation happened (the destination was held), the first sink kept appending, and the
  // second sink's file is still its own, with its line.
  CHECK_EQ((*first)->rotations(), 0u);
  CHECK(std::filesystem::file_size(path) > kMaxBytes);
  CHECK_EQ(read_lines(rotated_into).size(), 1u);
  CHECK((*first)->write_failures() > 0);  // each refused attempt is counted, like a failed rename
}

// Child probe: rotates a log whose ".1" the parent holds as its own live log, from another
// process, and expects the rotation to be refused there.
TEST(zz_child_probe_rotation_into_a_file_another_process_holds) {
  if (!in_child()) return;
  const std::string path(etch::detail::env("ETCH_TEST_ROTATE_PATH"));
  constexpr std::uintmax_t kMaxBytes = 120;
  auto file =
      etch::FileSink::open(path, {.max_bytes = kMaxBytes, .max_files = 1, .truncate = true});
  CHECK(file.has_value());
  if (!file) return;
  etch::Logger log{"rot", {*file}};
  for (int i = 0; i < 20; ++i) log.warn("line {} is long enough to need a rotation soon", i);
  (*file)->flush();
  CHECK_EQ((*file)->rotations(), 0u);
  CHECK((*file)->write_failures() > 0);
}

TEST(rotation_does_not_move_over_a_file_another_process_holds_as_its_log) {
  const auto path = scratch_dir() / "rotating-across.log";
  auto rotated_into = scratch_dir() / "rotating-across.log";
  rotated_into += ".1";
  auto held = etch::FileSink::open(rotated_into, {.truncate = true});
  CHECK(held.has_value());
  if (!held) return;
  {
    etch::Logger other{"other", {*held}};
    other.warn("the other process's line");
  }
  (*held)->flush();
  const std::string path_text = path.string();
  CHECK_EQ(run_test_in_child("zz_child_probe_rotation_into_a_file_another_process_holds",
                             {{"ETCH_TEST_ROTATE_PATH", path_text.c_str()}}),
           0);
  CHECK_EQ(read_lines(rotated_into).size(), 1u);  // still this process's file, with its line
}

TEST(a_relative_path_whose_directory_does_not_exist_yet_is_resolved_against_the_cwd_at_open) {
  const auto previous = std::filesystem::current_path();
  std::filesystem::current_path(scratch_dir());
  auto file = etch::FileSink::open("rel-logs/app.log", {.truncate = true});
  std::filesystem::current_path(previous);  // whatever happened, the process cwd is restored
  CHECK(file.has_value());
  if (!file) return;
  CHECK((*file)->path().is_absolute());
  CHECK((*file)->path() ==
        std::filesystem::weakly_canonical(scratch_dir() / "rel-logs" / "app.log"));
  etch::Logger log{"rel", {*file}};
  log.warn("after the cwd changed back");  // lands in the scratch directory, not in `previous`
  (*file)->flush();
  CHECK_EQ(read_lines(scratch_dir() / "rel-logs" / "app.log").size(), 1u);
}

// Child probe: a file sink held by a local that std::exit() skips. The exit handler flushes it.
TEST(zz_child_probe_exit_with_a_local_file_sink) {
  if (!in_child()) return;
  const std::string path(etch::detail::env("ETCH_TEST_EXIT_PATH"));
  auto file = etch::FileSink::open(path, {.flush_on = etch::Level::kOff, .truncate = true});
  CHECK(file.has_value());
  if (!file) std::exit(3);
  etch::Logger log{"exit", {*file}};
  log.warn("written before exit");  // buffered: nothing flushes it but the exit handler
  std::error_code ec;
  std::filesystem::remove_all(scratch_dir(), ec);  // main() will not get to do it
  std::exit(0);
}

TEST(a_file_sink_skipped_by_exit_is_flushed_by_the_exit_handler) {
  const auto path = scratch_dir() / "exit.log";
  const std::string path_text = path.string();
  CHECK_EQ(run_test_in_child("zz_child_probe_exit_with_a_local_file_sink",
                             {{"ETCH_TEST_EXIT_PATH", path_text.c_str()}}),
           0);
  const auto lines = read_lines(path);
  CHECK_EQ(lines.size(), 1u);
  CHECK(!lines.empty() && lines[0].ends_with("written before exit"));
}

TEST(callback_sink_receives_records) {
  std::vector<std::string> seen;
  auto sink = std::make_shared<etch::CallbackSink>([&](const etch::Record& r) {
    seen.push_back(std::format("{}:{}", etch::to_string(r.level), r.message));
  });
  etch::Logger log{"unit", {sink}};
  log.warn("one");
  log.error("two");
  CHECK_EQ(seen, (std::vector<std::string>{"WARN:one", "ERROR:two"}));
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
  // A parent that is a file, not a directory: create_directories cannot help and open fails.
  const auto blocker = scratch_dir() / "blocker";
  std::ofstream(blocker) << "not a directory";
  const auto path = blocker / "x.log";
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
  CHECK((*file)->write_failures() < 10);  // one attempt per max_bytes written, not per line
  CHECK_EQ((*file)->rotations(), 0u);
  const auto lines = read_lines(path);
  CHECK_EQ(lines.size(), 30u);  // nothing lost: the live file was appended to, not truncated
  for (int i = 0; i < 30 && i < static_cast<int>(lines.size()); ++i) {
    CHECK_EQ(lines[i], std::format("WARN  [s] {:02}", i));
  }
}
#endif

TEST(file_sink_refuses_a_second_sink_on_the_same_path) {
  const auto path = scratch_dir() / "once.log";
  auto first = etch::FileSink::open(path, {.truncate = true});
  CHECK(first.has_value());
  auto second = etch::FileSink::open(path);
  CHECK(!second.has_value());
  if (!second) CHECK(second.error() == std::errc::device_or_resource_busy);
  // Dropping the sink drops its registration; an expected has no reset, so assign an error.
  first = std::unexpected(std::make_error_code(std::errc::invalid_argument));
  auto third = etch::FileSink::open(path);
  CHECK(third.has_value());
}

TEST(file_sink_creates_missing_parent_directories) {
  const auto path = scratch_dir() / "deep" / "er" / "x.log";
  auto file = etch::FileSink::open(path, {.truncate = true});
  CHECK(file.has_value());
  if (!file) return;
  etch::Logger log{"d", {*file}};
  log.warn("made it");
  log.flush();
  CHECK_EQ(read_lines(path).size(), 1u);
}

TEST(file_sink_refuses_the_same_file_spelled_differently) {
  const auto dir = scratch_dir() / "alias";
  std::filesystem::create_directories(dir);
  auto first = etch::FileSink::open(dir / "x.log", {.truncate = true});
  CHECK(first.has_value());
  auto dotdot = etch::FileSink::open(dir / ".." / "alias" / "x.log");
  CHECK(!dotdot.has_value());
#if defined(_WIN32)
  auto upper = etch::FileSink::open(dir / "X.LOG");
  CHECK(!upper.has_value());
#endif
}

TEST(file_sink_reservation_is_released_when_open_fails) {
  const auto blocker = scratch_dir() / "blocker2";
  std::ofstream(blocker) << "not a directory";
  const auto path = blocker / "y.log";
  auto first = etch::FileSink::open(path);
  CHECK(!first.has_value());
  auto second = etch::FileSink::open(path);
  CHECK(!second.has_value());
  if (!second)
    CHECK(second.error() != std::errc::device_or_resource_busy);  // the real error, not "busy"
}

}  // namespace
