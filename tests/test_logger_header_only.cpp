// Tests: a translation unit that includes only <etch/logger.hpp> still has the default logger,
// get() and the free functions, as it always had. No support.hpp here on purpose: that would
// pull in the umbrella header and prove nothing.
#include <memory>
#include <string>
#include <vector>

#include <etch/logger.hpp>

#include "check.hpp"

namespace {

TEST(logger_header_alone_provides_the_default_logger_and_free_functions) {
  auto sink = std::make_shared<etch::MemorySink>();
  const auto previous = etch::default_logger().sinks();
  etch::default_logger().set_sinks({sink});
  etch::warn("free function through logger.hpp only");
  etch::get("header-only").warn("named logger too");
  etch::once().warn("rate limited too");
  etch::default_logger().set_sinks(previous);
  CHECK_EQ(sink->messages(), (std::vector<std::string>{"free function through logger.hpp only",
                                                       "named logger too", "rate limited too"}));
}

}  // namespace
