// etch tests: the agent profile, open_agent_log and SummarySink.
#include "support.hpp"

namespace {

using namespace std::chrono_literals;

TEST(agent_profile_is_compact_json_with_drops_above_warn_kept) {
  const auto p = etch::agent_profile();
  CHECK(p.style.format == etch::LineFormat::kJson);
  CHECK(p.style.json.time == etch::JsonStyle::Time::kEpochMillis);
  CHECK(!p.style.json.thread && !p.style.json.pid && !p.style.json.path && !p.style.json.function);
  CHECK(p.file.style.format == etch::LineFormat::kJson);
  CHECK(p.overflow == etch::AsyncSink::Overflow::kDrop);
  CHECK(p.keep_from == etch::Level::kWarn);
}

TEST(an_edit_to_the_agent_profile_style_reaches_the_file) {
  const auto path = scratch_dir() / "agent-style.jsonl";
  auto profile = etch::agent_profile();
  profile.style.json.thread = true;
  auto agent = etch::open_agent_log(path, profile);
  CHECK(agent.has_value());
  if (!agent) return;
  {
    etch::Logger log{"agent", {*agent}};
    log.warn("hello");
    (*agent)->flush();
  }
  const auto lines = read_lines(path);
  CHECK_EQ(lines.size(), 2u);  // the run header, then the line
  for (const auto& line : lines) CHECK(line.find("\"thread\":") != std::string::npos);
}

// Blocks every write until released, so a ring in front of it fills up.
struct HeldSink final : etch::Sink {
  std::atomic<bool> released{false};
  void write(const etch::Record&) override {
    while (!released.load()) std::this_thread::sleep_for(1ms);
  }
};

// The number after "dropped": in a summary object.
std::size_t dropped_in(const std::string& json) {
  const auto key = json.find("\"dropped\":");
  if (key == std::string::npos) return static_cast<std::size_t>(-1);
  return static_cast<std::size_t>(std::stoull(json.substr(key + 10)));
}

TEST(the_summary_keeps_the_drops_of_a_sink_that_is_gone) {
  auto summary = std::make_shared<etch::SummarySink>();
  std::size_t dropped_while_alive = 0;
  {
    auto held = std::make_shared<HeldSink>();
    auto async = std::make_shared<etch::AsyncSink>(held, 2, etch::AsyncSink::Overflow::kDrop,
                                                   etch::Level::kFatal);
    summary->watch(async);
    etch::Logger log{"dropper", {async}};
    for (int i = 0; i < 50; ++i) log.warn("flood {}", i);  // below keep_from: dropped when full
    held->released = true;
    async->flush();
    dropped_while_alive = async->dropped();
    CHECK(dropped_while_alive > 0);
  }
  // The sink is gone (a log reopened under load does this) and nothing read the summary while
  // it lived; its drops are still there, in full.
  CHECK_EQ(dropped_in(summary->json()), dropped_while_alive);
}

TEST(write_to_makes_the_summary_directory) {
  etch::SummarySink summary;
  const auto path = scratch_dir() / "nested" / "dir" / "summary.json";
  CHECK(!summary.write_to(path));
  CHECK(std::filesystem::exists(path));
}

TEST(open_agent_log_writes_compact_json_lines) {
  const auto path = scratch_dir() / "agent.jsonl";
  auto async = etch::open_agent_log(
      path, {.file = {.truncate = true, .style = etch::agent_profile().style}});
  CHECK(async.has_value());
  if (!async) return;
  etch::Logger log{"agent", {*async}};
  log.warn("hello {}", 1);
  log.with({{"fps", 60}}).error("slow");
  (*async)->flush();
  const auto lines = read_lines(path);
  CHECK_EQ(lines.size(), 3u);  // the run header, then the two lines
  for (const auto& line : lines) {
    CHECK(line.starts_with("{\"ts\":1"));  // epoch milliseconds, not an ISO string
    CHECK(line.find("\"thread\"") == std::string::npos);
  }
  if (lines.size() == 3) {
    CHECK(lines[0].find("\"msg\":\"run started\"") != std::string::npos);
    CHECK(lines[0].find(std::format("\"run_id\":\"{}\"", etch::run_id())) != std::string::npos);
    CHECK(lines[0].find("\"pid\":") != std::string::npos);  // on the header line only
    CHECK(lines[0].find(std::format("\"etch\":\"{}\"", etch::kVersion)) != std::string::npos);
    CHECK(lines[1].find("\"pid\"") == std::string::npos);
    CHECK(lines[2].find(R"("fields":{"fps":60})") != std::string::npos);
  }
  auto refused = etch::open_agent_log(path);  // the file is already open by the first sink
  CHECK(!refused.has_value());
}

// Game testbed, finding 6: a file that several runs appended to had no run boundary, and the
// summary could not be tied to a run. Each open writes a header; the id is one per process.
TEST(each_run_of_an_agent_log_starts_with_a_header_and_the_run_id_is_stable) {
  const auto path = scratch_dir() / "two-runs.jsonl";
  CHECK_EQ(etch::run_id().size(), 16u);
  CHECK_EQ(etch::run_id(), etch::run_id());
  for (int run = 0; run < 2; ++run) {
    auto async = etch::open_agent_log(path);
    CHECK(async.has_value());
    if (!async) return;
    etch::Logger log{"agent", {*async}};
    log.warn("run {}", run);
  }  // closed: the second open appends
  const auto lines = read_lines(path);
  CHECK_EQ(lines.size(), 4u);
  if (lines.size() < 4) return;
  CHECK(lines[0].find("run started") != std::string::npos);
  CHECK(lines[2].find("run started") != std::string::npos);
  CHECK_EQ(lines[0].substr(lines[0].find("run_id")), lines[2].substr(lines[2].find("run_id")));
}

TEST(the_summary_carries_the_run_id_the_pid_and_what_wrote_it) {
  etch::SummarySink summary;
  const auto json = summary.json();
  CHECK(json.starts_with(std::format("{{\"etch_summary\":1,\"run_id\":\"{}\",\"pid\":{},",
                                     etch::run_id(), etch::detail::process_id())));
  CHECK(json.find("\"written_at\":\"call\"") != std::string::npos);
  CHECK(summary.json("exit").find("\"written_at\":\"exit\"") != std::string::npos);
}

// Child probe: the installed summary writes its file the moment a fatal arrives (a run that
// aborts after its fatal never reaches exit), and the exit handler writes the final object.
// The parent gives the path, since each process has a scratch directory of its own.
TEST(zz_child_probe_the_installed_summary_is_written_on_a_fatal) {
  if (!in_child()) return;
  const std::filesystem::path path = etch::detail::env("ETCH_TEST_SUMMARY_PATH");
  CHECK(!path.empty());
  if (path.empty()) return;
  auto summary = etch::SummarySink::install(path);
  etch::warn("before the fatal");
  CHECK(!std::filesystem::exists(path));  // nothing written yet: no fatal, no exit
  etch::fatal("the run falls over");
  const auto at_fatal = read_lines(path);
  CHECK_EQ(at_fatal.size(), 1u);
  if (at_fatal.empty()) return;
  CHECK(at_fatal[0].find("\"written_at\":\"fatal\"") != std::string::npos);
  CHECK(at_fatal[0].find("\"fatal\":1") != std::string::npos);  // the fatal itself is counted
  CHECK(at_fatal[0].find(std::format("\"run_id\":\"{}\"", etch::run_id())) != std::string::npos);
  // Kept beside the file for the parent: the exit handler replaces the original.
  std::filesystem::copy_file(path, path.string() + ".fatal",
                             std::filesystem::copy_options::overwrite_existing);
  etch::warn("after the fatal");  // the exit handler's object will have it; this one does not
  CHECK(at_fatal[0].find("\"records\":2") != std::string::npos);
}

// The run id in a summary object, or an empty string.
std::string run_id_in(const std::string& json) {
  const auto key = json.find("\"run_id\":\"");
  if (key == std::string::npos) return {};
  const auto start = key + 10;
  const auto end = json.find('"', start);
  return end == std::string::npos ? std::string{} : json.substr(start, end - start);
}

bool is_hex_id(const std::string& id) {
  return id.size() == 16 &&
         std::ranges::all_of(id, [](unsigned char c) { return std::isxdigit(c) != 0; });
}

// Two processes, each: the fatal summary and the exit summary carry one id, a valid one (the
// id must outlive the statics the exit handler runs after); and the two processes' ids differ.
TEST(the_installed_summary_is_written_on_a_fatal_and_again_at_exit) {
  std::vector<std::string> ids;
  for (int run = 0; run < 2; ++run) {
    const auto path = scratch_dir() / std::format("fatal-summary-{}.json", run);
    const std::string path_utf8 = etch::utf8(path);
    CHECK_EQ(run_test_in_child("zz_child_probe_the_installed_summary_is_written_on_a_fatal",
                               {{"ETCH_TEST_SUMMARY_PATH", path_utf8.c_str()}}),
             0);
    const auto at_exit = read_lines(path);
    const auto at_fatal = read_lines(path.string() + ".fatal");
    CHECK_EQ(at_exit.size(), 1u);
    CHECK_EQ(at_fatal.size(), 1u);
    if (at_exit.empty() || at_fatal.empty()) return;
    CHECK(at_exit[0].find("\"written_at\":\"exit\"") != std::string::npos);
    CHECK(at_exit[0].find("\"records\":3") != std::string::npos);
    const auto id = run_id_in(at_exit[0]);
    CHECK(is_hex_id(id));
    CHECK_EQ(run_id_in(at_fatal[0]), id);
    ids.push_back(id);
  }
  CHECK(ids.size() == 2 && ids[0] != ids[1]);
}

TEST(summary_counts_levels_loggers_and_remembers_first_and_last_error) {
  auto summary = std::make_shared<etch::SummarySink>();
  etch::Logger app{"app", {summary}};
  etch::Logger net = app.child("net");
  app.warn("w1");
  net.error("first bad thing");
  app.warn("w2");
  net.fatal("last bad thing");  // fatal counts as an error for first/last
  CHECK_EQ(summary->total(), 4u);
  CHECK_EQ(summary->count(etch::Level::kWarn), 2u);
  CHECK_EQ(summary->count(etch::Level::kError), 1u);
  CHECK_EQ(summary->count(etch::Level::kFatal), 1u);
  CHECK_EQ(summary->count(etch::Level::kOff), 0u);
  const auto first = summary->first_error();
  const auto last = summary->last_error();
  CHECK(first.has_value() && last.has_value());
  if (!first || !last) return;
  CHECK_EQ(first->message, std::string("first bad thing"));
  CHECK_EQ(first->logger, std::string("net"));
  CHECK_EQ(last->message, std::string("last bad thing"));
  CHECK(last->level == etch::Level::kFatal);
  CHECK(first->line > 0);
}

TEST(summary_watches_async_and_file_sinks_and_writes_a_file) {
  const auto path = scratch_dir() / "watched.jsonl";
  auto file = etch::FileSink::open(path, {.truncate = true});
  CHECK(file.has_value());
  if (!file) return;
  std::atomic<int> delivered{0};
  auto slow = std::make_shared<etch::CallbackSink>([&](const etch::Record&) {
    const auto until = std::chrono::steady_clock::now() + 200us;
    while (std::chrono::steady_clock::now() < until) {
    }
    ++delivered;
  });
  auto async = std::make_shared<etch::AsyncSink>(slow, 2, etch::AsyncSink::Overflow::kDrop,
                                                 etch::Level::kOff);
  auto summary = std::make_shared<etch::SummarySink>();
  summary->watch(async);
  summary->watch(*file);
  etch::Logger log{"w", {async, *file, summary}};
  for (int i = 0; i < 60; ++i) log.warn("noise {}", i);
  async->flush();
  CHECK(async->dropped() > 0);
  const auto json = summary->json();
  CHECK(json.find(std::format("\"dropped\":{}", async->dropped())) != std::string::npos);
  CHECK(json.find("\"write_failures\":0") != std::string::npos);
  CHECK(json.find("\"records\":60") != std::string::npos);
  const auto out = scratch_dir() / "summary.json";
  CHECK(!summary->write_to(out));
  const auto lines = read_lines(out);
  CHECK_EQ(lines.size(), 1u);
  if (!lines.empty()) CHECK(lines[0].starts_with("{\"etch_summary\":1"));
}

}  // namespace
