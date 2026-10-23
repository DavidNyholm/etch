"""Tests against deliberate bugs.

Injects one bug at a time into the headers, rebuilds the test binary, runs it, and reports
whether the suite caught the bug. Every file is restored afterwards, whatever happens.

    python tools/mutate.py            # uses the "windows" or "linux" preset for this host
    python tools/mutate.py --preset linux

Needs the configure step to have been run for that preset (cmake --preset <name>).
"""

from __future__ import annotations

import argparse
import platform
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INCLUDE = ROOT / "include" / "etch"

# (file, description, [(text to find, replacement), ...]). Each replacement must match once.
MUTATIONS = [
    (
        "level.hpp",
        "parser rejects full-length names",
        [("text.size() > full.size()", "text.size() >= full.size()")],
    ),
    (
        "record.hpp",
        "time cache ignores the utc flag",
        [("Cache& cache = caches[utc ? 1 : 0];", "Cache& cache = caches[0];")],
    ),
    (
        "record.hpp",
        "time cache never invalidates on a new second",
        [("if (!cache.valid || key != cache.key) {", "if (!cache.valid) {")],
    ),
    (
        "record.hpp",
        "location column prints the full path",
        [("detail::base_name(r.where.file_name())", "r.where.file_name()")],
    ),
    (
        "record.hpp",
        "warn message loses its colour",
        [('"\\x1b[33m",    // warn: yellow', '"",    // warn: yellow')],
    ),
    (
        "sink_set.hpp",
        "a replaced sink list is never revoked from the thread caches",
        [
            (
                "if (e.set.load(std::memory_order_acquire) != this) continue;",
                "if (e.set.load(std::memory_order_acquire) == this) continue;",
            ),
        ],
    ),
    (
        "sink_set.hpp",
        "a call that was revoked mid-flight keeps the old list",
        [
            (
                "pinned_->list.reset();\n      pinned_->set.store(nullptr",
                "pinned_->set.store(nullptr",
            ),
        ],
    ),
    (
        "context.hpp",
        "context fields never reach the record",
        [
            (
                "if (stack == nullptr || stack->fields.empty()) return;",
                "if (stack == nullptr || !stack->fields.empty()) return;",
            ),
        ],
    ),
    (
        "context.hpp",
        "an inner context does not shadow the outer one",
        [("shadowed = context[j].key == key;", "shadowed = false;")],
    ),
    (
        "context.hpp",
        "context fields stay after the context ends",
        [("if (!active_) return;", "if (true) return;")],
    ),
    (
        "level.hpp",
        "level spec takes the logger name as the level",
        [
            (
                "equals == std::string_view::npos ? entry : trim(entry.substr(equals + 1));",
                "equals == std::string_view::npos ? entry : trim(entry.substr(0, equals));",
            ),
        ],
    ),
    (
        "default_logger.hpp",
        "a named level is not applied to a logger made later",
        [("    logger->set_level(it->second);\n", "")],
    ),
    (
        "default_logger.hpp",
        "the root level of a spec is ignored",
        [("if (spec.root) root.set_level", "if (false) root.set_level")],
    ),
    (
        "logger.hpp",
        "once() fires every time",
        [("  state.fired = true;\n", "")],
    ),
    (
        "logger.hpp",
        "every() never refills its budget",
        [("now - state.last < interval", "now - state.last >= interval || true")],
    ),
    (
        "logger.hpp",
        "the suppressed count is never reported",
        [("if (suppressed > 0) {", "if (suppressed < 0) {")],
    ),
    (
        "logger.hpp",
        "a filtered scope uses up the rate budget",
        [
            (
                "if (!active_ || !logger_.enabled(options.level) || !admit(where)) options.level",
                "if (!active_ || !admit(where) || !logger_.enabled(options.level)) options.level",
            ),
        ],
    ),
    (
        "logger.hpp",
        "a supplied suppressed field is kept next to the generated one",
        [("      std::erase_if(owned_, [](const Field& f) { return f.key == kSuppressedField; });\n", "")],
    ),
    (
        "sinks.hpp",
        "sink level filter off by one",
        [
            (
                "return level < Level::kOff && level >= get_level();",
                "return level < Level::kOff && level > get_level();",
            ),
        ],
    ),
    (
        "sinks.hpp",
        "memory sink keeps one record too many",
        [
            (
                "if (records_.size() > capacity_) records_.pop_front();",
                "if (records_.size() > capacity_ + 1) records_.pop_front();",
            )
        ],
    ),
    (
        "sinks.hpp",
        "rotation deletes the live file instead of renaming it",
        [("std::filesystem::rename(path_, numbered(1), ec);", "std::filesystem::remove(path_, ec);")],
    ),
    (
        "sinks.hpp",
        "file sink never rotates",
        [
            (
                "if (options_.max_bytes > 0 && would_overflow && size_ >= retry_rotation_at_) rotate();",
                "if (false && would_overflow) rotate();",
            )
        ],
    ),
    (
        "sinks.hpp",
        "file sink opens in text mode (CRLF on Windows)",
        [("std::ios::out | std::ios::binary |", "std::ios::out |")],
    ),
    (
        "async_sink.hpp",
        "dropped records are not counted",
        [
            ("dropped_.fetch_add(1, std::memory_order_seq_cst);", ";"),
        ],
    ),
    (
        "async_sink.hpp",
        "async ignores the inner sink's level",
        [("if (inner_->accepts(r.level)) enqueue(std::move(r));", "enqueue(std::move(r));")],
    ),
    (
        "async_sink.hpp",
        "an exception from the inner sink is swallowed uncounted",
        [
            (
                "  void write_inner(Record&& r) noexcept {\n    try {\n      inner_->write(std::move(r));\n    } catch (...) {\n      write_failures_.fetch_add(1, std::memory_order_relaxed);\n    }\n  }",
                "  void write_inner(Record&& r) noexcept {\n    try {\n      inner_->write(std::move(r));\n    } catch (...) {\n    }\n  }",
            ),
        ],
    ),
    (
        "async_sink.hpp",
        "flush waits for the queue to be idle instead of for its own records",
        [
            (
                "written_pos_.load(std::memory_order_acquire) >= target &&",
                "empty() && written_pos_.load(std::memory_order_acquire) >= dequeue_pos_.load(std::memory_order_acquire) &&",
            ),
        ],
    ),
    (
        "logger.hpp",
        "logger level filter off by one",
        [
            (
                "return level < Level::kOff && level >= kCompileTimeMinLevel && level >= get_level();",
                "return level < Level::kOff && level >= kCompileTimeMinLevel && level > get_level();",
            ),
        ],
    ),
    ("logger.hpp", "hexdump loses the gap after eight bytes", [("if (i == kGapAfter) out += ' ';", "")]),
    (
        "logger.hpp",
        "scope timer begin message changed",
        [('std::format("{}: begin", what_)', 'std::format("{}: start", what_)')],
    ),
    ("sink_set.hpp", "null sinks are kept in the list", [("std::erase(sinks, nullptr);", "")]),
    (
        "json.hpp",
        "JSON message is not escaped",
        [("json_string(r.user_message(), out);", "out += r.user_message();")],
    ),
    (
        "json.hpp",
        "JSON drops the fields",
        [("  if (!r.fields.empty()) {", "  if (false) {")],
    ),
    (
        "json.hpp",
        "JSON drops the duration",
        [("  if (r.duration.has_value())", "  if (false)")],
    ),
    (
        "record.hpp",
        "text output drops the fields",
        [("  for (const auto& field : r.fields) {", "  for (const auto& field : Fields{}) {")],
    ),
    (
        "logger.hpp",
        "emit skips the level check",
        [
            (
                "    if (!enabled(r.level)) return;\n    deliver(std::move(r));",
                "    deliver(std::move(r));",
            )
        ],
    ),
    (
        "async_sink.hpp",
        "drop policy drops errors as readily as trace",
        [("policy_ == Overflow::kDrop && r.level < keep_from_", "policy_ == Overflow::kDrop")],
    ),
    (
        "async_sink.hpp",
        "no gap marker after dropped records",
        [("      if (marker.level < Level::kOff) inner_->write(std::move(marker));", "      ;")],
    ),
    (
        "async_sink.hpp",
        "late records are written before the ring drains at shutdown",
        [
            (
                "        not_full_.wait(lock, [&] { return worker_gone_.load(std::memory_order_seq_cst); });",
                "",
            )
        ],
    ),
    (
        "logger.hpp",
        "etch's own frames are not filtered from the stacktrace",
        [
            ("      if (detail::same_path_prefix(frame.file, own)) continue;\n", ""),
            ("      if (detail::is_etch_symbol(frame.description)) continue;\n", ""),
        ],
    ),
    (
        "logger.hpp",
        "scope end line is filtered by a later level change",
        [
            (
                "    logger_.deliver(std::move(r));\n  } catch (...) {",
                "    logger_.emit(std::move(r));\n  } catch (...) {",
            )
        ],
    ),
    (
        "logger.hpp",
        "hexdump ignores its limit",
        [("const std::size_t shown = std::min(size, limit);", "const std::size_t shown = size;")],
    ),
    (
        "sinks.hpp",
        "a refused rotation is retried on every line",
        [(" && size_ >= retry_rotation_at_) rotate();", ") rotate();")],
    ),
    (
        "sinks.hpp",
        "a second FileSink on the same path is not refused",
        [("if (!detail::open_paths().insert(key).second) {", "if (false) {")],
    ),
    (
        "sinks.hpp",
        "set_style turns console colour off",
        [("    if (!style_.colour.has_value()) style_.colour = colour_;", "")],
    ),
    (
        "record.hpp",
        "a newline in a message starts a new line unindented",
        [('      out += "\\n  ";', '      out += "\\n";')],
    ),
    (
        "record.hpp",
        "UTC lines lose their Z marker",
        [("  if (cache.zulu_suffix) out += 'Z';", "")],
    ),
    (
        "json.hpp",
        "invalid UTF-8 passes into the JSON",
        [
            (
                "      if (length == 0) {\n        detail::append_replacement_character(out);",
                "      if (length == 0) {\n        out += static_cast<char>(c);",
            )
        ],
    ),
    (
        "level.hpp",
        "kOff still accepts fatal",
        [
            (
                '  if (is_prefix_of("off") || is_prefix_of("none")) return Level::kOff;',
                '  if (is_prefix_of("off") || is_prefix_of("none")) return Level::kFatal;',
            )
        ],
    ),
    (
        "agent.hpp",
        "the summary does not record errors",
        [("    if (r.level >= Level::kError) {", "    if (false) {")],
    ),
    (
        "agent.hpp",
        "the agent profile keeps the thread and pid fields",
        [
            (
                "                                    .thread = false,",
                "                                    .thread = true,",
            )
        ],
    ),
]


BUILD_TIMEOUT_S = 600  # a mutant must not hang the whole run on a broken build
TEST_TIMEOUT_S = 120  # the suite takes about a second; a hang is a finding of its own


def run(args: list[str], timeout: int, cwd: Path = ROOT) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=cwd, capture_output=True, text=True, timeout=timeout)


def build_and_test(build_args: list[str], binary: Path) -> tuple[str, list[str]]:
    """Returns (status, failing test names). status: caught, MISSED, BUILD FAILED, TIMED OUT."""
    try:
        build = run(build_args, BUILD_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        return "TIMED OUT", []
    if build.returncode != 0:
        return "BUILD FAILED", []
    try:
        test = run([str(binary)], TEST_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        return "TIMED OUT", []
    failing = [line.split(maxsplit=1)[1] for line in test.stdout.splitlines() if line.startswith("  FAIL")]
    return ("caught" if test.returncode != 0 else "MISSED"), failing


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    default_preset = "windows" if platform.system() == "Windows" else "linux"
    parser.add_argument("--preset", default=default_preset)
    opts = parser.parse_args()
    build_dir = ROOT / "build" / opts.preset
    binary = build_dir / ("Debug/etch_tests.exe" if opts.preset == "windows" else "etch_tests")
    build_args = ["cmake", "--build", str(build_dir), "--target", "etch_tests"]
    if opts.preset == "windows":
        build_args += ["--config", "Debug"]

    # Baseline gate: a suite that already fails would "catch" every mutant for the wrong reason.
    status, failing = build_and_test(build_args, binary)
    if status != "MISSED":
        print(f"baseline is not green ({status} {', '.join(failing)}); fix that first")
        return 2
    print("baseline: all tests pass")

    results = []
    for file_name, description, edits in MUTATIONS:
        path = INCLUDE / file_name
        original = path.read_text(encoding="utf-8")
        mutated = original
        applicable = True
        for old, new in edits:
            if mutated.count(old) != 1:
                applicable = False
                break
            mutated = mutated.replace(old, new)
        if not applicable:
            results.append(("NOT APPLIED", description, []))
            print(f"NOT APPLIED  {description}  (the text to mutate was not found exactly once)")
            continue
        path.write_text(mutated, encoding="utf-8", newline="\n")
        try:
            status, failing = build_and_test(build_args, binary)
            results.append((status, description, failing))
            if failing:
                detail = ", ".join(failing)
            elif status == "caught":
                detail = "(the test binary crashed)"
            elif status == "MISSED":
                detail = "(every test passed)"
            else:
                detail = ""
            print(f"{status:<12} {description}  {detail}")
        finally:
            path.write_text(original, encoding="utf-8", newline="\n")

    # Leave the build matching the restored sources, and say so if that fails.
    restored = run(build_args, BUILD_TIMEOUT_S)
    if restored.returncode != 0:
        print("WARNING: rebuilding the restored sources failed; run the build by hand")
        return 2
    caught = sum(1 for r in results if r[0] == "caught")
    print(f"\n{caught} of {len(results)} injected bugs caught")
    return 0 if caught == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
