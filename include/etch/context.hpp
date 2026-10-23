// etch - Context: fields attached to every record a thread logs while a Context object lives,
// so a request id or a frame number reaches every line without with() on each call.
#pragma once

#include <cstddef>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "record.hpp"

namespace etch {

namespace detail {

// The thread's context fields, in the order they were pushed. Heap-allocated behind a trivial
// thread_local so a log call made while the thread's thread_local objects are being destroyed
// finds a null pointer and logs without context rather than touching a dead object.
struct ContextStack {
  Fields fields;
};
struct ContextSlot {
  ContextStack* stack = nullptr;
  bool ended = false;
};
inline ContextSlot& context_slot() noexcept {
  static constinit thread_local ContextSlot slot{};
  return slot;
}
struct ContextReaper {
  ~ContextReaper() {
    ContextSlot& slot = context_slot();
    delete std::exchange(slot.stack, nullptr);
    slot.ended = true;
  }
};
inline ContextStack* context_stack(bool create) {
  ContextSlot& slot = context_slot();
  if (slot.stack == nullptr && create && !slot.ended) {
    static thread_local ContextReaper reaper;  // registered now, runs when the thread ends
    (void)reaper;
    slot.stack = new ContextStack;
  }
  return slot.stack;
}

// Adds the thread's context fields to a record's own, context first. A call's own field
// shadows a context field with the same key, and an inner context shadows an outer one, so
// JSON never carries a key twice. Costs one thread_local load when there is no context.
inline void append_context(Fields& fields) {
  const ContextStack* const stack = context_stack(/*create=*/false);
  if (stack == nullptr || stack->fields.empty()) return;
  const std::span<const Field> context = stack->fields;
  Fields merged;
  merged.reserve(context.size() + fields.size());
  for (std::size_t i = 0; i < context.size(); ++i) {
    const std::string_view key = context[i].key;
    bool shadowed = false;
    for (std::size_t j = i + 1; j < context.size() && !shadowed; ++j) {
      shadowed = context[j].key == key;
    }
    for (std::size_t j = 0; j < fields.size() && !shadowed; ++j) shadowed = fields[j].key == key;
    if (!shadowed) merged.push_back(context[i]);
  }
  for (Field& field : fields) merged.push_back(std::move(field));
  fields = std::move(merged);
}

}  // namespace detail

// Holds fields on the calling thread's context while it lives: every record that thread logs
// meanwhile carries them. Nest freely; destroy in reverse order of creation, like scopes (a
// Context destroyed out of order takes the ones made after it with it). Not movable across
// threads: it belongs to the thread that made it.
class [[nodiscard]] Context {
 public:
  explicit Context(Fields fields) {
    detail::ContextStack* const stack = detail::context_stack(/*create=*/true);
    if (stack == nullptr) return;  // the thread is ending: nothing to attach to
    mark_ = stack->fields.size();
    stack->fields.insert(stack->fields.end(), std::make_move_iterator(fields.begin()),
                         std::make_move_iterator(fields.end()));
    active_ = true;
  }
  Context(std::initializer_list<Field> fields) : Context(Fields(fields)) {}
  ~Context() {
    if (!active_) return;
    detail::ContextStack* const stack = detail::context_stack(/*create=*/false);
    if (stack != nullptr && stack->fields.size() > mark_) {
      stack->fields.erase(stack->fields.begin() + static_cast<std::ptrdiff_t>(mark_),
                          stack->fields.end());
    }
  }
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  // The calling thread's context fields right now, outermost first.
  static std::span<const Field> current() noexcept {
    const detail::ContextStack* const stack = detail::context_stack(/*create=*/false);
    return stack == nullptr ? std::span<const Field>{} : std::span<const Field>(stack->fields);
  }

 private:
  std::size_t mark_ = 0;
  bool active_ = false;
};

// auto ctx = etch::context({{"request_id", id}}); -- fields on every line until ctx goes.
inline Context context(std::initializer_list<Field> fields) { return Context(fields); }
inline Context context(Fields fields) { return Context(std::move(fields)); }

}  // namespace etch
