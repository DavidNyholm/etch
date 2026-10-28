// Compile test: <windows.h> first, without NOMINMAX, then etch. The min and max macros are in
// force for every etch header; nothing in them may spell std::min, std::max or
// numeric_limits::max() in a form the macros can reach. On other platforms this is an ordinary
// test.
#if defined(_WIN32)
#include <windows.h>
#endif

#include <etch/etch.hpp>

#include "support.hpp"

namespace {

TEST(etch_compiles_when_the_windows_sdk_is_included_before_it) {
  Captured c;
  // The unsigned-to-int64 conversion and the hexdump limit are where max() and min() live.
  c.log.with({{"n", std::uint64_t{5}}, {"big", ~std::uint64_t{0}}}).warn("order");
  c.log.hexdump(etch::Level::kWarn, "bytes", "abc", 3, 2);
  CHECK_EQ(c.sink->size(), 2u);
  auto debugger = std::make_shared<etch::DebuggerSink>();
  CHECK(debugger != nullptr);
}

}  // namespace
