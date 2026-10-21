// etch tests: JSON Lines output, with a small validator for every line produced.
#include "support.hpp"

namespace {

using namespace std::chrono_literals;

// ---- JSON ---------------------------------------------------------------------------------

// A minimal JSON validator: enough to prove every line the sink writes is well-formed. Not a
// parser; it only answers yes or no.
class JsonChecker {
 public:
  explicit JsonChecker(std::string_view text) : text_(text) {}
  bool valid() {
    const bool ok = value();
    skip_ws();
    return ok && pos_ == text_.size();
  }

 private:
  bool value() {
    skip_ws();
    switch (peek()) {
      case '{':
        return object();
      case '[':
        return array();
      case '"':
        return string();
      case 't':
        return literal("true");
      case 'f':
        return literal("false");
      case 'n':
        return literal("null");
      default:
        return number();
    }
  }
  bool object() {
    ++pos_;
    skip_ws();
    if (peek() == '}') return ++pos_, true;
    for (;;) {
      skip_ws();
      if (!string()) return false;
      skip_ws();
      if (peek() != ':') return false;
      ++pos_;
      if (!value()) return false;
      skip_ws();
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == '}') return ++pos_, true;
      return false;
    }
  }
  bool array() {
    ++pos_;
    skip_ws();
    if (peek() == ']') return ++pos_, true;
    for (;;) {
      if (!value()) return false;
      skip_ws();
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == ']') return ++pos_, true;
      return false;
    }
  }
  bool string() {
    if (peek() != '"') return false;
    ++pos_;
    while (pos_ < text_.size()) {
      const unsigned char c = static_cast<unsigned char>(text_[pos_++]);
      if (c == '"') return true;
      if (c == '\\') {
        if (pos_ >= text_.size()) return false;
        const char e = text_[pos_++];
        if (e == 'u') {
          for (int i = 0; i < 4; ++i) {
            if (pos_ >= text_.size() || !std::isxdigit(static_cast<unsigned char>(text_[pos_++]))) {
              return false;
            }
          }
        } else if (std::string_view("\"\\/bfnrt").find(e) == std::string_view::npos) {
          return false;
        }
      } else if (c < 0x20) {
        return false;  // raw control characters are not allowed inside a JSON string
      }
    }
    return false;
  }
  bool number() {
    const std::size_t start = pos_;
    if (peek() == '-') ++pos_;
    if (!std::isdigit(static_cast<unsigned char>(peek()))) return false;
    if (peek() == '0') {
      ++pos_;  // a leading zero stands alone: "01" is not JSON
    } else {
      while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
    }
    if (peek() == '.') {
      ++pos_;
      if (!std::isdigit(static_cast<unsigned char>(peek()))) return false;
      while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
    }
    if (peek() == 'e' || peek() == 'E') {
      ++pos_;
      if (peek() == '+' || peek() == '-') ++pos_;
      if (!std::isdigit(static_cast<unsigned char>(peek()))) return false;
      while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
    }
    return pos_ > start;
  }
  bool literal(std::string_view word) {
    if (text_.substr(pos_, word.size()) != word) return false;
    pos_ += word.size();
    return true;
  }
  char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }
  void skip_ws() {
    while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
  }

  std::string_view text_;
  std::size_t pos_ = 0;
};

bool json_valid(std::string_view text) { return JsonChecker(text).valid(); }

TEST(json_checker_accepts_valid_and_rejects_invalid) {
  CHECK(json_valid(R"({"a":1,"b":[true,false,null,-2.5e3,"x\u00e9\n"],"c":{}})"));
  CHECK(!json_valid(R"({"a":1,})"));
  CHECK(!json_valid(R"({"a":tru})"));
  CHECK(!json_valid("{\"a\":\"line\nbreak\"}"));  // a raw newline inside a string
  CHECK(!json_valid(R"({"a":01})"));
  CHECK(!json_valid(R"([1,2)"));
}

TEST(json_escape_handles_quotes_backslashes_controls_and_utf8) {
  std::string out;
  etch::json_escape("a\"b\\c\n\t\x01 Z\xc3\xbcrich", out);
  CHECK_EQ(out, std::string("a\\\"b\\\\c\\n\\t\\u0001 Z\xc3\xbcrich"));
  CHECK(json_valid("\"" + out + "\""));
}

TEST(json_time_is_iso8601_utc) {
  std::string out;
  etch::format_time_iso8601(out, fixed_record().time);
  CHECK_EQ(out, std::string("2026-01-02T03:04:05.678Z"));
}

TEST(json_line_is_exact_for_a_fixed_record) {
  auto r = fixed_record();
  r.thread_index = 7;
  r.message = "say \"hi\"";
  std::string line;
  etch::format_json(r, line);  // default JsonStyle: no path, no function name
  const auto expected = std::format(
      R"({{"ts":"2026-01-02T03:04:05.678Z","level":"info","logger":"unit","thread":{},"pid":{},)"
      R"("file":"{}","line":{},"msg":"say \"hi\""}})",
      r.thread_index, etch::detail::process_id(), etch::detail::base_name(r.where.file_name()),
      r.where.line());
  CHECK_EQ(line, expected);
  CHECK(json_valid(line));
  CHECK(line.find("path") == std::string::npos);  // the absolute source path stays private
}

TEST(json_style_adds_path_and_function_and_can_drop_thread_pid_and_use_epoch_time) {
  auto r = fixed_record();
  r.message = "m";
  etch::JsonStyle full;
  full.path = true;
  full.function = true;
  std::string line;
  etch::format_json(r, full, line);
  std::string path;
  etch::json_escape(r.where.file_name(), path);
  std::string func;
  etch::json_escape(r.where.function_name(), func);
  CHECK(line.find(std::format(R"("path":"{}","func":"{}")", path, func)) != std::string::npos);
  CHECK(json_valid(line));

  etch::JsonStyle compact;
  compact.thread = false;
  compact.pid = false;
  compact.time = etch::JsonStyle::Time::kEpochMillis;
  std::string small;
  etch::format_json(r, compact, small);
  const auto expected = std::format(
      R"({{"ts":1767323045678,"level":"info","logger":"unit","file":"{}","line":{},"msg":"m"}})",
      etch::detail::base_name(r.where.file_name()), r.where.line());
  CHECK_EQ(small, expected);
  CHECK(json_valid(small));
  std::string default_line;
  etch::format_json(r, default_line);
  CHECK(small.size() < default_line.size());  // fewer characters than the default line
  CHECK(small.size() * 2 < line.size());      // and less than half of the path+function line
}

TEST(json_value_renders_every_field_type) {
  const auto render = [](etch::FieldValue v) {
    std::string out;
    etch::json_value(v, out);
    return out;
  };
  CHECK_EQ(render(std::int64_t{-42}), std::string("-42"));
  CHECK_EQ(render(59.25), std::string("59.25"));
  CHECK_EQ(render(true), std::string("true"));
  CHECK_EQ(render(std::string("q\"q")), std::string("\"q\\\"q\""));
  CHECK_EQ(render(std::numeric_limits<double>::infinity()), std::string("null"));
  CHECK_EQ(render(std::numeric_limits<double>::quiet_NaN()), std::string("null"));
}

TEST(fields_appear_in_text_and_json) {
  Captured c;
  c.log.with({{"fps", 59.25}, {"name", "forest"}, {"ok", true}, {"n", 3}}).warn("frame {}", 7);
  const auto records = c.sink->records();
  CHECK_EQ(records.size(), 1u);
  if (records.empty()) return;
  CHECK_EQ(records[0].fields.size(), 4u);
  const etch::LineStyle bare{.time = false, .thread = false, .name = false, .location = false};
  CHECK_EQ(etch::format_line(records[0], bare),
           std::string("WARN  frame 7 fps=59.25 name=\"forest\" ok=true n=3"));
  std::string json;
  etch::format_json(records[0], json);
  CHECK(json.find(R"("fields":{"fps":59.25,"name":"forest","ok":true,"n":3})") !=
        std::string::npos);
  CHECK(json_valid(json));
}

TEST(json_carries_hexdump_bytes_scope_duration_and_fatal_stack) {
  Captured c{"unit", {.stacktrace_on_fatal = true}};
  const std::array<unsigned char, 4> data{0x01, 0xab, 0x00, 0xff};
  c.log.hexdump(etch::Level::kWarn, "pkt", data);
  {
    auto timer = c.log.scope("work", etch::Level::kWarn);
  }
  c.log.fatal("boom");
  const auto records = c.sink->records();
  CHECK_EQ(records.size(), 4u);  // hexdump, scope begin, scope end, fatal
  if (records.size() < 4) return;
  std::vector<std::string> lines;
  for (const auto& r : records) {
    std::string json;
    etch::format_json(r, json);
    CHECK(json_valid(json));
    lines.push_back(json);
  }
  CHECK(lines[0].find(R"("bytes":"01ab00ff")") != std::string::npos);
  CHECK_EQ(records[0].bytes.size(), 4u);
  CHECK(lines[0].find(R"x("msg":"pkt (4 bytes)")x") != std::string::npos);  // label only
  CHECK(lines[0].find("0000  01 ab") == std::string::npos);  // the dump text is not repeated
  CHECK(lines[1].find("duration_ns") == std::string::npos);  // begin has no duration
  CHECK(lines[2].find(R"("duration_ns":)") != std::string::npos);
  CHECK(records[2].duration.has_value());
#if ETCH_HAS_STACKTRACE
  CHECK(!records[3].stack.empty());
  CHECK(lines[3].find(R"("stack":[")") != std::string::npos);
  CHECK(lines[3].find(R"("msg":"boom")") != std::string::npos);  // frames only in "stack"
#endif
}

TEST(file_sink_json_format_writes_one_valid_object_per_line) {
  const auto path = scratch_dir() / "lines.jsonl";
  auto file = etch::FileSink::open(path, {.truncate = true});
  CHECK(file.has_value());
  if (!file) return;
  (*file)->set_style(etch::LineStyle{.format = etch::LineFormat::kJson});
  etch::Logger log{"j", {*file}};
  log.warn("plain");
  log.warn("quotes \" and a\nnewline and a tab\t");
  log.with({{"k", "v"}}).error("with field");
  log.flush();
  const auto lines = read_lines(path);
  CHECK_EQ(lines.size(), 3u);
  for (const auto& line : lines) {
    CHECK(json_valid(line));
    CHECK(line.starts_with("{\"ts\":"));
    CHECK(line.find("\"msg\":") != std::string::npos);
  }
  if (lines.size() == 3) CHECK(lines[2].find(R"("fields":{"k":"v"})") != std::string::npos);
}

TEST(console_sink_can_force_colour_off_and_on) {
  etch::ConsoleSink off(stdout, etch::ConsoleSink::ColourMode::kNever);
  etch::ConsoleSink on(stdout, etch::ConsoleSink::ColourMode::kAlways);
  CHECK(off.style().colour == false);
  CHECK(on.style().colour == true);
}

TEST(json_replaces_invalid_utf8_and_keeps_valid_sequences) {
  std::string out;
  etch::json_escape(
      "a\xff\xfe"
      "b Z\xc3\xbc"
      "rich \xe2\x98\x83",
      out);
  CHECK_EQ(out, std::string("a\\ufffd\\ufffdb Z\xc3\xbc"
                            "rich \xe2\x98\x83"));
  CHECK(json_valid("\"" + out + "\""));
  out.clear();
  etch::json_escape("\xed\xa0\x80", out);  // a UTF-16 surrogate encoded in UTF-8: not allowed
  CHECK(out.find("ufffd") != std::string::npos);
}

TEST(json_msg_keeps_user_newlines_but_not_the_dump_or_the_frames) {
  Captured c{"unit", {.stacktrace_on_fatal = true}};
  const std::array<unsigned char, 4> data{1, 2, 3, 4};
  c.log.hexdump(etch::Level::kWarn, "two\nlines", data);
  c.log.fatal("why\nbecause");
  const auto records = c.sink->records();
  CHECK_EQ(records.size(), 2u);
  if (records.size() < 2) return;
  std::string dump;
  etch::format_json(records[0], dump);
  CHECK(dump.find(R"x("msg":"two\nlines (4 bytes)")x") != std::string::npos);
  CHECK(dump.find("0000") == std::string::npos);
  std::string fatal;
  etch::format_json(records[1], fatal);
  CHECK(fatal.find(R"("msg":"why\nbecause")") != std::string::npos);
  CHECK(fatal.find("stacktrace:") == std::string::npos);
  CHECK(json_valid(dump) && json_valid(fatal));
#if ETCH_HAS_STACKTRACE
  // Frames carry base names only unless JsonStyle.path asks for the full path.
  const auto stack_at = fatal.find("\"stack\":[");
  if (stack_at != std::string::npos) {
    const auto stack = fatal.substr(stack_at);
    CHECK(stack.find("\\\\") == std::string::npos);  // no escaped backslash: no Windows path
    CHECK(stack.find("/home/") == std::string::npos);
  }
#endif
}

TEST(summary_json_is_valid_and_has_every_level) {
  auto summary = std::make_shared<etch::SummarySink>();
  etch::Logger log{"s", {summary}};
  log.warn("one");
  log.error("two \"quoted\"");
  const auto json = summary->json();
  CHECK(json_valid(json));
  for (const auto name : {"trace", "debug", "info", "warn", "error", "fatal"}) {
    CHECK(json.find(std::format("\"{}\":", name)) != std::string::npos);
  }
  CHECK(json.find(R"("first_error":{)") != std::string::npos);
  CHECK(json.find(R"("msg":"two \"quoted\""})") != std::string::npos);
}

}  // namespace
