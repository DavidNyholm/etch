// etch - JSON Lines output: one JSON object per record, for programs rather than people.
//
// A line looks like this with the default JsonStyle (one line in the file):
//   {"ts":"2026-10-20T19:38:13.091Z","level":"info","logger":"demo","thread":1,"pid":4120,
//    "file":"demo.cpp","line":84,"msg":"frame 1442 ..."}
// followed, when present, by "fields":{...}, "duration_ns":N, "bytes":"48656c..." (the data
// behind a hexdump as lowercase hex) and "stack":["frame (file.cpp:12)", ...]. JsonStyle adds
// "path" and "func" (and full paths in frames), drops "thread" and "pid", or makes "ts" epoch
// milliseconds. Text is UTF-8; an invalid byte sequence becomes U+FFFD, so every line decodes.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <variant>

#include "level.hpp"
#include "record.hpp"

#if defined(_WIN32)
#include <process.h>  // _getpid
#else
#include <unistd.h>  // getpid
#endif

namespace etch {

namespace detail {

// Cached on Windows, where a process id never changes. Asked for each time on POSIX, because a
// fork() would otherwise keep reporting the parent's id from the child.
inline int process_id() noexcept {
#if defined(_WIN32)
  static const int pid = _getpid();
  return pid;
#else
  return static_cast<int>(getpid());
#endif
}

// utf8_sequence_length lives in record.hpp, shared with the text escaper.

inline void append_replacement_character(std::string& out) {
  out += '\\';
  out += "ufffd";
}

// A stack frame as one short string: "section_fatal (demo.cpp:176)", or just the description
// for a frame without debug information.
inline void frame_text(const Frame& f, bool full_path, std::string& out) {
  out += f.description;
  if (f.file.empty()) return;
  out += " (";
  out += full_path ? std::string_view(f.file) : base_name(f.file);
  out += ':';
  std::format_to(std::back_inserter(out), "{}", f.line);
  out += ')';
}

}  // namespace detail

// Appends the body of a JSON string (no surrounding quotes). The quote, the backslash and every
// control character are escaped, so the record stays one line whatever the message contains.
// Input is expected to be UTF-8; an invalid byte sequence is replaced by U+FFFD (written as
// its six-character JSON escape), so a bad path or a binary blob logged as text cannot make
// the file undecodable.
inline void json_escape(std::string_view text, std::string& out) {
  constexpr unsigned char kLastControl = 0x1f;
  for (std::size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c >= 0x80) {
      const std::size_t length = detail::utf8_sequence_length(text, i);
      if (length == 0) {
        detail::append_replacement_character(out);
        ++i;
      } else {
        out.append(text.data() + i, length);
        i += length;
      }
      continue;
    }
    ++i;
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c <= kLastControl) {
          out += '\\';
          out += "u00";
          out += detail::kHexDigits[c >> 4];
          out += detail::kHexDigits[c & 0x0f];
        } else {
          out += static_cast<char>(c);
        }
    }
  }
}

inline void json_string(std::string_view text, std::string& out) {
  out += '"';
  json_escape(text, out);
  out += '"';
}

// A field value as a JSON value. A double that is not finite has no JSON spelling: null (NaN,
// +inf and -inf all become null; log the distinction as a string if it matters). Field keys are
// written in order and not checked for duplicates; a duplicate key is reader-defined in JSON,
// so do not attach the same key twice.
inline void json_value(const FieldValue& value, std::string& out) {
  std::visit(
      [&](const auto& v) {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) {
          json_string(v, out);
        } else if constexpr (std::is_same_v<T, bool>) {
          out += v ? "true" : "false";
        } else if constexpr (std::is_same_v<T, double>) {
          if (std::isfinite(v)) {
            std::format_to(std::back_inserter(out), "{}", v);
          } else {
            out += "null";
          }
        } else {
          std::format_to(std::back_inserter(out), "{}", v);
        }
      },
      value);
}

// ISO 8601 in UTC with milliseconds: "2026-10-20T19:38:13.091Z". Reuses the per-second cache.
inline void format_time_iso8601(std::string& out, std::chrono::system_clock::time_point tp) {
  constexpr std::size_t kDateLength = 10;  // "2026-10-20"
  const std::size_t start = out.size();
  format_time(out, tp, /*utc=*/true);  // ends with "Z" already
  if (out.size() > start + kDateLength) out[start + kDateLength] = 'T';
}

// Appends one record as a single JSON object, no trailing newline.
inline void format_json(const Record& r, const JsonStyle& j, std::string& out) {
  auto app = std::back_inserter(out);
  out += "{\"ts\":";
  if (j.time == JsonStyle::Time::kEpochMillis) {
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(r.time.time_since_epoch());
    std::format_to(app, "{}", ms.count());
  } else {
    out += '"';
    format_time_iso8601(out, r.time);
    out += '"';
  }
  out += ",\"level\":";
  json_string(level_name(r.level), out);
  out += ",\"logger\":";
  json_string(r.logger_name, out);
  if (j.thread) std::format_to(app, ",\"thread\":{}", r.thread_index);
  if (j.pid) std::format_to(app, ",\"pid\":{}", detail::process_id());
  out += ",\"file\":";
  json_string(detail::base_name(r.where.file_name()), out);
  std::format_to(app, ",\"line\":{}", r.where.line());
  if (j.path) {
    out += ",\"path\":";
    json_string(r.where.file_name(), out);
  }
  if (j.function) {
    out += ",\"func\":";
    json_string(r.where.function_name(), out);
  }
  // Only the caller's text: the dump lines and the frames travel as data below.
  out += ",\"msg\":";
  json_string(r.user_message(), out);
  if (!r.fields.empty()) {
    out += ",\"fields\":{";
    bool first = true;
    for (const auto& field : r.fields) {
      if (!first) out += ',';
      first = false;
      json_string(field.key, out);
      out += ':';
      json_value(field.value, out);
    }
    out += '}';
  }
  if (r.duration.has_value()) std::format_to(app, ",\"duration_ns\":{}", r.duration->count());
  if (!r.bytes.empty()) {
    out += ",\"bytes\":\"";
    for (const std::byte b : r.bytes) {
      const auto v = std::to_integer<unsigned>(b);
      out += detail::kHexDigits[v >> 4];
      out += detail::kHexDigits[v & 0x0f];
    }
    out += '"';
  }
  if (!r.stack.empty()) {
    out += ",\"stack\":[";
    bool first = true;
    std::string frame;
    for (const auto& f : r.stack) {
      if (!first) out += ',';
      first = false;
      frame.clear();
      detail::frame_text(f, j.path, frame);
      json_string(frame, out);
    }
    out += ']';
  }
  out += '}';
}

inline void format_json(const Record& r, std::string& out) { format_json(r, JsonStyle{}, out); }

// What every sink calls: the text line or the JSON object, as the style says.
inline void format_record(const Record& r, const LineStyle& s, std::string& out) {
  if (s.format == LineFormat::kJson) {
    format_json(r, s.json, out);
  } else {
    format_line(r, s, out);
  }
}

}  // namespace etch
