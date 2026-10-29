// etch - the library's version, as a constant a program can log. CMake checks at configure
// time that it matches the project version, so the two cannot drift apart.
#pragma once

#include <string_view>

namespace etch {

inline constexpr std::string_view kVersion = "0.5.0";

}  // namespace etch
