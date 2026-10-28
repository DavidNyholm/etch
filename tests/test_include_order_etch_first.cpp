// Compile test: etch first, then <winsock2.h> (which brings <windows.h>), with neither NOMINMAX
// nor WIN32_LEAN_AND_MEAN. A platform layer that includes the SDK late is where etch's
// hand-declared Win32 functions must agree with the SDK's own declarations. On other platforms
// this is an ordinary test.
#include <etch/etch.hpp>

#if defined(_WIN32)
#include <winsock2.h>
#endif

#include "support.hpp"

namespace {

TEST(etch_compiles_when_the_windows_sdk_is_included_after_it) {
  Captured c;
  c.log.with({{"n", std::uint64_t{5}}}).warn("order");
  CHECK_EQ(c.messages(), (std::vector<std::string>{"order"}));
#if defined(_WIN32)
  CHECK(GetCurrentThread() != nullptr);  // the SDK's declaration, the same entity etch declared
#endif
}

}  // namespace
