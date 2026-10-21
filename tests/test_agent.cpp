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
  CHECK(!p.scope.begin);
  CHECK(p.overflow == etch::AsyncSink::Overflow::kDrop);
  CHECK(p.keep_from == etch::Level::kWarn);
  CHECK(p.hexdump_limit < etch::kDefaultHexdumpLimit);
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
  CHECK_EQ(lines.size(), 2u);
  for (const auto& line : lines) {
    CHECK(line.starts_with("{\"ts\":1"));  // epoch milliseconds, not an ISO string
    CHECK(line.find("\"thread\"") == std::string::npos);
    CHECK(line.find("\"pid\"") == std::string::npos);
  }
  if (lines.size() == 2) CHECK(lines[1].find(R"("fields":{"fps":60})") != std::string::npos);
  auto refused = etch::open_agent_log(path);  // the file is already open by the first sink
  CHECK(!refused.has_value());
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
