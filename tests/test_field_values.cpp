// etch tests: field value conversion for types that bring their own std::formatter.
#include "support.hpp"

enum class Mob { kPig = 0, kZombie = 1 };
enum class Plain { kA = 7 };
struct Vec2 {
  int x = 0;
  int y = 0;
};

// Written on top of a standard formatter, as the etch docs show for Level.
template <>
struct std::formatter<Mob> : std::formatter<std::string_view> {
  auto format(Mob m, auto& ctx) const {
    return std::formatter<std::string_view>::format(m == Mob::kPig ? "pig" : "zombie", ctx);
  }
};

// The common hand-written form: parse(), and format() taking std::format_context by name.
template <>
struct std::formatter<Vec2> {
  constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
  auto format(const Vec2& v, std::format_context& ctx) const {
    return std::format_to(ctx.out(), "({}, {})", v.x, v.y);
  }
};

namespace {

TEST(an_enum_with_a_formatter_is_logged_by_name_and_without_one_as_a_number) {
  const etch::Field named("kind", Mob::kZombie);
  CHECK(std::get_if<std::string>(&named.value) != nullptr);
  CHECK(std::get<std::string>(named.value) == "zombie");
  const etch::Field plain("p", Plain::kA);
  CHECK(std::get_if<std::int64_t>(&plain.value) != nullptr);
  CHECK(std::get<std::int64_t>(plain.value) == 7);
}

TEST(a_formatter_written_for_format_context_is_accepted) {
  const etch::Field f("pos", Vec2{3, -4});
  CHECK(std::get_if<std::string>(&f.value) != nullptr);
  CHECK(std::get<std::string>(f.value) == "(3, -4)");
  Captured c;
  c.log.with({{"pos", Vec2{1, 2}}, {"kind", Mob::kPig}}).warn("spawn");
  const auto records = c.sink->records();
  CHECK_EQ(records.size(), 1u);
  if (records.empty()) return;
  const etch::LineStyle bare{.time = false, .thread = false, .name = false, .location = false};
  CHECK_EQ(etch::format_line(records[0], bare),
           std::string("WARN  spawn pos=\"(1, 2)\" kind=\"pig\""));
}

}  // namespace
