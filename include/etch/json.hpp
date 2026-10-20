// etch - JSON Lines output: one JSON object per record, for programs rather than people.
//
// A line looks like this (wrapped here; it is one line in the file):
//   {"ts":"2026-10-20T19:38:13.091Z","level":"info","logger":"demo","thread":"30392",
//    "pid":4120,"file":"demo.cpp","line":84,"path":"...","func":"...","msg":"frame 1442 ..."}
// followed, when present, by "fields":{...}, "duration_ns":N, "bytes":"48656c..." (the whole
// payload behind a hexdump as lowercase hex) and "stack":[...] (one string per frame).
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

inline int process_id() noexcept {
#if defined(_WIN32)
  static const int pid = _getpid();
#else
  static const int pid = static_cast<int>(getpid());
#endif
  return pid;
}

constexpr std::string_view json_level_name(Level level) noexcept {
  switch (level) {
    case Level::kTrace:
      return "trace";
    case Level::kDebug:
      return "debug";
    case Level::kInfo:
      return "info";
    case Level::kWarn:
      return "warn";
    case Level::kError:
      return "error";
    case Level::kFatal:
      return "fatal";
  }
  return "unknown";
}

}  // namespace detail

// Appends the body of a JSON string (no surrounding quotes). UTF-8 passes through untouched;
// the quote, the backslash and every control character are escaped, so the record stays one
// line whatever the message contains.
inline void json_escape(std::string_view text, std::string& out) {
  constexpr unsigned char kLastControl = 0x1f;
  constexpr std::string_view kHex = "0123456789abcdef";
  for (const unsigned char c : text) {
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
          out += kHex[c >> 4];
          out += kHex[c & 0x0f];
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

// A field value as a JSON value. A double that is not finite has no JSON spelling: null.
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
  format_time(out, tp, /*utc=*/true);
  if (out.size() > start + kDateLength) out[start + kDateLength] = 'T';
  out += 'Z';
}

// Appends one record as a single JSON object, no trailing newline.
inline void format_json(const Record& r, std::string& out) {
  auto app = std::back_inserter(out);
  out += "{\"ts\":";
  out += '"';
  format_time_iso8601(out, r.time);
  out += '"';
  out += ",\"level\":";
  json_string(detail::json_level_name(r.level), out);
  out += ",\"logger\":";
  json_string(r.logger_name, out);
  out += ",\"thread\":";
  json_string(std::format("{}", r.thread), out);
  std::format_to(app, ",\"pid\":{}", detail::process_id());
  out += ",\"file\":";
  json_string(detail::base_name(r.where.file_name()), out);
  std::format_to(app, ",\"line\":{}", r.where.line());
  out += ",\"path\":";
  json_string(r.where.file_name(), out);
  out += ",\"func\":";
  json_string(r.where.function_name(), out);
  out += ",\"msg\":";
  json_string(r.message, out);
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
    for (const std::byte b : r.bytes) std::format_to(app, "{:02x}", std::to_integer<unsigned>(b));
    out += '"';
  }
  if (!r.stack.empty()) {
    out += ",\"stack\":[";
    bool first = true;
    for (const auto& frame : r.stack) {
      if (!first) out += ',';
      first = false;
      json_string(frame, out);
    }
    out += ']';
  }
  out += '}';
}

// What every sink calls: the text line or the JSON object, as the style says.
inline void format_record(const Record& r, const LineStyle& s, std::string& out) {
  if (s.format == LineFormat::kJson) {
    format_json(r, out);
  } else {
    format_line(r, s, out);
  }
}

}  // namespace etch
