// etch - DebuggerSink: lines into the debugger's output window (Visual Studio's Output pane,
// DebugView, x64dbg's log) through OutputDebugStringA. On other platforms it accepts records
// and writes nothing, so the same code builds everywhere.
#pragma once

#include <mutex>
#include <string>

#include "record.hpp"
#include "sinks.hpp"

namespace etch {

#if defined(_WIN32) && !defined(_WINDOWS_)
// Declared by hand, like the console calls in sinks.hpp, so <windows.h> stays out.
extern "C" __declspec(dllimport) void __stdcall OutputDebugStringA(const char*);
#endif

class DebuggerSink final : public Sink {
 public:
  explicit DebuggerSink(LineStyle style = LineStyle{.colour = false}) : style_(style) {
    style_.colour = false;  // a debugger window shows escape sequences as text
  }

  void write(const Record& r) override {
    std::lock_guard lock(mutex_);
    line_.clear();
    format_record(r, style_, line_);
    line_ += '\n';
#if defined(_WIN32)
    OutputDebugStringA(line_.c_str());
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
  std::string line_;  // reused: a steady stream of lines allocates nothing
};

}  // namespace etch
