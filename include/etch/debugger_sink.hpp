// etch - DebuggerSink: lines into the debugger's output window (Visual Studio's Output pane,
// DebugView, x64dbg's log) through OutputDebugStringW. Optionally only while a debugger is
// attached, which then costs nothing otherwise. On other platforms it accepts records and
// writes nothing, so the same code builds everywhere.
#pragma once

#include <cstddef>
#include <mutex>
#include <string>

#include "record.hpp"
#include "sinks.hpp"

namespace etch {

#if defined(_WIN32) && !defined(_WINDOWS_)
// Declared by hand, like the console calls in sinks.hpp, so <windows.h> stays out; the types
// are the SDK's (BOOL = int, UINT = unsigned, DWORD = unsigned long, LPWSTR = wchar_t*), so a
// <windows.h> included later sees the same declarations.
extern "C" {
__declspec(dllimport) int __stdcall IsDebuggerPresent();
__declspec(dllimport) void __stdcall OutputDebugStringW(const wchar_t*);
__declspec(dllimport) int __stdcall MultiByteToWideChar(unsigned code_page, unsigned long flags,
                                                        const char* text, int text_bytes,
                                                        wchar_t* out, int out_chars);
}
#endif

class DebuggerSink final : public Sink {
 public:
  // kAlways: every line goes out, which is what DebugView needs (it captures the output of a
  // process no debugger is attached to) and costs a few microseconds per line even when
  // nothing listens. kWhenAttached: nothing is formatted or sent unless a debugger is attached
  // (a flag in the process block, a few nanoseconds), for an application that keeps this sink
  // on at all times for Visual Studio and does not use DebugView.
  enum class Output { kAlways, kWhenAttached };

  explicit DebuggerSink(LineStyle style = LineStyle{.colour = false},
                        Output output = Output::kAlways)
      : style_(style), output_(output) {
    style_.colour = false;  // a debugger window shows escape sequences as text
  }

  // A debugger reads OutputDebugStringA in the ANSI code page, which garbles anything beyond
  // it; the line is converted to UTF-16 and sent through the W call, which is exact.
  void write(const Record& r) override {
#if defined(_WIN32)
    if (output_ == Output::kWhenAttached && IsDebuggerPresent() == 0) return;
    std::lock_guard lock(mutex_);
    line_.clear();
    format_record(r, style_, line_);
    line_ += '\n';
    constexpr unsigned kUtf8 = 65001;  // CP_UTF8
    const auto bytes = static_cast<int>(line_.size());
    const int chars = MultiByteToWideChar(kUtf8, 0, line_.data(), bytes, nullptr, 0);
    if (chars <= 0)
      return;  // not valid UTF-8 at all: the text output already escaped what it could
    wide_.assign(static_cast<std::size_t>(chars) + 1, L'\0');
    MultiByteToWideChar(kUtf8, 0, line_.data(), bytes, wide_.data(), chars);
    OutputDebugStringW(wide_.c_str());
#else
    (void)r;
#endif
  }

  void set_style(LineStyle style) {
    std::lock_guard lock(mutex_);
    style_ = style;
    style_.colour = false;
  }

 private:
  std::mutex mutex_;
  LineStyle style_;
  Output output_;
  std::string line_;   // reused: a steady stream of lines allocates nothing
  std::wstring wide_;  // the same line as UTF-16, reused likewise
};

}  // namespace etch
