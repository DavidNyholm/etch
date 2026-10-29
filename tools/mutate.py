"""Tests against deliberate bugs.

Injects one bug at a time into the headers, rebuilds the test binary, runs it, and reports
whether the suite caught the bug. Every file is restored afterwards, whatever happens.

    python tools/mutate.py            # uses the "windows" or "linux" preset for this host
    python tools/mutate.py --preset linux

Needs the configure step to have been run for that preset (cmake --preset <name>).
"""

from __future__ import annotations

import argparse
import os
import platform
import signal
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INCLUDE = ROOT / "include" / "etch"

# (file, description, [(text to find, replacement), ...], {platforms}?). Each replacement
# must match once. The optional set of platforms ("windows", "linux") limits a mutation to
# the builds where its text exists; elsewhere it is skipped and not counted.
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
        [
            (
                "    logger->set_level(it->second);  // asked for by name before it existed: its own level\n",
                "",
            ),
        ],
    ),
    (
        "default_logger.hpp",
        "the root level of a spec is ignored",
        [("    root.set_level(*spec.root, /*own=*/false);\n", "")],
    ),
    (
        "rate_limit.hpp",
        "once() fires every time",
        [("  state.fired = true;\n", "")],
    ),
    (
        "rate_limit.hpp",
        "every() never refills its budget",
        [("now - state.last < interval", "now - state.last >= interval || true")],
    ),
    (
        "logger.hpp",
        "the suppressed count is never reported",
        [
            (
                "    if (admission == detail::Admission::kAdmitted && suppressed > 0) {",
                "    if (admission == detail::Admission::kAdmitted && suppressed < 0) {",
            ),
        ],
    ),
    (
        "rate_limit.hpp",
        "the first held call is never noted for the shutdown report",
        [
            (
                "    return state.pending ? Admission::kHeld : Admission::kHeldFirst;",
                "    return Admission::kHeld;",
            ),
        ],
    ),
    (
        "rate_limit.hpp",
        "the shutdown report forgets to reset the count",
        [("      state.suppressed = 0;\n      state.pending.reset();\n", "")],
    ),
    (
        "rate_limit.hpp",
        "the shutdown report drops the held call's own fields",
        [("        r.fields = std::move(state.pending->fields);\n", "")],
    ),
    (
        "agent.hpp",
        "the agent log starts a run without a header record",
        [("  (*file)->write(run_started_record());  // also fixes the run id, here at setup time\n", "")],
    ),
    (
        "agent.hpp",
        "the summary carries an empty run id",
        [("  json_string(run_id(), out);\n", '  json_string("", out);\n')],
    ),
    (
        "agent.hpp",
        "the installed summary is not written on a fatal",
        [('    if (r.level == Level::kFatal) write_now("fatal");\n', "")],
    ),
    (
        "async_sink.hpp",
        "shutdown does not report the held-back counts",
        [("      detail::report_pending_rates();\n", "")],
    ),
    (
        "logger.hpp",
        "a filtered scope uses up the rate budget",
        [
            (
                "if (options.level != Level::kOff && rate_gated_) {",
                "if (rate_gated_) {",
            ),
        ],
    ),
    (
        "logger.hpp",
        "a supplied suppressed field is kept next to the generated one",
        [
            (
                "      std::erase_if(owned_, [](const Field& f) { return f.key == detail::kSuppressedField; });\n",
                "",
            ),
        ],
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
        "file_sink.hpp",
        "rotation deletes the live file instead of renaming it",
        [("std::filesystem::rename(path_, numbered(1), ec);", "std::filesystem::remove(path_, ec);")],
    ),
    (
        "file_sink.hpp",
        "file sink never rotates",
        [
            (
                "if (options_.max_bytes > 0 && would_overflow && size_ >= retry_rotation_at_) rotate();",
                "if (false && would_overflow) rotate();",
            )
        ],
    ),
    (
        "file_sink.hpp",
        "file sink opens in text mode (CRLF on Windows)",
        [("std::ios::out | std::ios::binary |", "std::ios::out |")],
    ),
    (
        "async_sink.hpp",
        "dropped records are not counted",
        [
            (
                "        note_dropped(r);\n"
                "        active_producers_.fetch_sub(1, std::memory_order_seq_cst);",
                "        active_producers_.fetch_sub(1, std::memory_order_seq_cst);",
            ),
        ],
    ),
    (
        "async_sink.hpp",
        "async ignores the inner sink's level",
        [
            (
                "    if (!inner_->accepts(r.level)) return;\n"
                "    if (detail::kept_from(r, this)) {\n"
                "      count_reentry();\n"
                "      return;\n"
                "    }\n"
                "    if (inherited()) {\n"
                "      write_inner(std::move(r));",
                "    if (detail::kept_from(r, this)) {\n"
                "      count_reentry();\n"
                "      return;\n"
                "    }\n"
                "    if (inherited()) {\n"
                "      write_inner(std::move(r));",
            ),
        ],
    ),
    (
        "async_sink.hpp",
        "an exception from the inner sink is swallowed uncounted",
        [
            (
                "    try {\n      inner_->write(std::move(r));\n    } catch (...) {\n"
                "      counters_->write_failures.fetch_add(1, std::memory_order_relaxed);\n    }",
                "    try {\n      inner_->write(std::move(r));\n    } catch (...) {\n    }",
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
                "    if (!enabled(r.level)) return;\n    r.lineage = nullptr;",
                "    r.lineage = nullptr;",
            )
        ],
    ),
    (
        "async_sink.hpp",
        "drop policy drops errors as readily as trace",
        [
            (
                "      if (policy_ == Overflow::kDrop && r.level < keep_from_) {\n        note_dropped(r);",
                "      if (policy_ == Overflow::kDrop) {\n        note_dropped(r);",
            ),
        ],
    ),
    (
        "async_sink.hpp",
        "no gap marker after dropped records",
        [("      if (marker.level < Level::kOff) write_inner(std::move(marker));", "      ;")],
    ),
    (
        "async_sink.hpp",
        "late records are written before the ring drains at shutdown",
        [
            (
                "        sync_->not_full.wait(lock, [&] { return worker_gone_.load(std::memory_order_seq_cst); });",
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
        [("const std::size_t shown = (std::min)(size, limit);", "const std::size_t shown = size;")],
    ),
    (
        "file_sink.hpp",
        "a refused rotation is retried on every line",
        [(" && size_ >= retry_rotation_at_) rotate();", ") rotate();")],
    ),
    (
        "file_sink.hpp",
        "an open file sink is never published to the exit handler",
        [("      files.by_path[key].sink = sink;\n", "")],
        {"linux"},
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
    (
        "record.hpp",
        "field keys with spaces and equals signs are written bare",
        [
            (
                "c == '_' || c == '.' || c == '-';",
                "c == '_' || c == '.' || c == '-' || c == '=' || c == ' ';",
            ),
        ],
    ),
    (
        "record.hpp",
        "bidirectional overrides pass through the text output",
        [
            (
                "(candidate >= 0x202a && candidate <= 0x202e)",
                "(candidate >= 0x202a && candidate < 0x202a)",
            ),
        ],
    ),
    (
        "record.hpp",
        "fields land after the appended dump and stack lines",
        [
            (
                "  for (const auto& field : r.fields) {\n    out += ' ';\n"
                "    append_field_text(field, out);\n  }\n  out += r.appended_text();",
                "  out += r.appended_text();\n  for (const auto& field : r.fields) {\n"
                "    out += ' ';\n    append_field_text(field, out);\n  }",
            ),
        ],
    ),
    (
        "record.hpp",
        "an enum with a formatter is logged as a number",
        [("std::is_enum_v<U> && HasFormatter<U>", "std::is_enum_v<U> && false")],
    ),
    (
        "delivery.hpp",
        "a nested write skipped by the logger is not counted",
        [("      sink->count_reentry();\n", "")],
    ),
    (
        "async_sink.hpp",
        "the async worker does not mark its sink as writing",
        [
            (
                "    const detail::WriteScope outer(this);\n"
                "    const detail::WriteScope inner(inner_.get());\n"
                "    try {\n"
                "      inner_->write(std::move(r));",
                "    const detail::WriteScope inner(inner_.get());\n"
                "    try {\n"
                "      inner_->write(std::move(r));",
            ),
        ],
    ),
    (
        "async_sink.hpp",
        "droppable records take the slots a waiting kept record needs",
        [
            (
                "    return policy_ == Overflow::kDrop && r.level < keep_from_ &&\n"
                "           producers_waiting_.load(std::memory_order_relaxed) > 0;",
                "    (void)r;\n    return false;",
            ),
        ],
    ),
    (
        "async_sink.hpp",
        "the worker never asks for the priority it was given",
        [
            (
                "      priority_raised_.store(detail::raise_this_thread_priority(), std::memory_order_release);",
                "      priority_raised_.store(true, std::memory_order_release);",
            ),
        ],
        {"windows"},
    ),
    (
        "async_sink.hpp",
        "the worker does not bracket its sweeps",
        [
            ("      bracket_inner([&] { inner_->begin_batch(); });\n", ""),
            ("      bracket_inner([&] { inner_->end_batch(); });\n", ""),
        ],
    ),
    (
        "file_sink.hpp",
        "a file sink flushes every flush_on line inside a sweep",
        [
            (
                "        flush_pending_ = true;  // the worker's end_batch() flushes once for the whole sweep",
                "        flush_locked();",
            ),
        ],
    ),
    (
        "file_sink.hpp",
        "the flush a sweep held back is never written",
        [("    if (flush_pending_) flush_locked();\n", "")],
    ),
    (
        "file_sink.hpp",
        "a file held by another process is not refused",
        [
            (
                "_O_WRONLY | _O_CREAT | _O_NOINHERIT, _SH_DENYRW, _S_IWRITE);",
                "_O_WRONLY | _O_CREAT | _O_NOINHERIT, _SH_DENYNO, _S_IWRITE);",
            ),
        ],
        {"windows"},
    ),
    (
        "file_sink.hpp",
        "a file held by another process is not refused",
        [
            (
                "if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {",
                "if (::flock(lock_fd_, LOCK_SH | LOCK_NB) != 0) {",
            ),
        ],
        {"linux"},
    ),
    (
        "file_sink.hpp",
        "the exit handler does not flush the open file sinks",
        [("    for (const auto& sink : live) sink->flush();\n", "")],
        {"linux"},
    ),
    (
        "logger.hpp",
        "a rate-gated threshold scope spends the budget when it starts",
        [
            (
                "options.min_duration > std::chrono::nanoseconds{0}",
                "options.min_duration < std::chrono::nanoseconds{0}",
            ),
        ],
    ),
    (
        "logger.hpp",
        "with().every() does not rate-gate",
        [
            (
                "    interval_ = interval;\n    once_ = false;\n    rate_gated_ = true;\n",
                "    interval_ = interval;\n    once_ = false;\n",
            ),
        ],
    ),
    (
        "default_logger.hpp",
        "a bare level skips the loggers that already exist",
        [
            (
                "    for (const auto& [name, logger] : registry.loggers) logger->inherit_level(*spec.root);\n",
                "",
            ),
        ],
    ),
    (
        "default_logger.hpp",
        "*=level leaves the loggers their own levels",
        [
            (
                "      logger->set_level(*spec.all, /*own=*/false);",
                "      logger->set_level(*spec.all);",
            ),
        ],
    ),
    (
        "default_logger.hpp",
        "a spec naming an unknown logger is not reported",
        [
            (
                "      if (!registry.loggers.empty()) {",
                "      if (registry.loggers.empty()) {",
            ),
        ],
    ),
    (
        "level.hpp",
        "a logger name with a space or a star is accepted",
        [
            (
                "return !name.empty() && name.find_first_of(kForbidden) == std::string_view::npos;",
                "return !name.empty();",
            ),
        ],
    ),
    (
        "agent.hpp",
        "an edited agent profile style is ignored",
        [
            (
                "  options.style = profile.style;  // the profile's style wins, so an edit to it is not lost\n",
                "",
            ),
        ],
    ),
    (
        "agent.hpp",
        "the summary ignores the drops of the sinks it watches",
        [("      dropped += counters->dropped.load(std::memory_order_relaxed);\n", "")],
    ),
    (
        "agent.hpp",
        "the summary file's directory is not created",
        [
            (
                "      std::filesystem::create_directories(path.parent_path(), ec);  // the open reports failure\n",
                "      (void)ec;\n",
            ),
        ],
    ),
]


BUILD_TIMEOUT_S = 600  # a mutant must not hang the whole run on a broken build
TEST_TIMEOUT_S = 120  # the suite takes about a second; a hang is a finding of its own


def kill_tree(process: subprocess.Popen[bytes]) -> None:
    """Ends the process and everything it started: a hung test can have a child of its own
    (the fresh-process probes, a forked child), which would otherwise outlive the timeout and
    hold files and locks the next mutation runs into."""
    if os.name == "nt":
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(process.pid)], capture_output=True)
    else:
        os.killpg(process.pid, signal.SIGKILL)  # the process leads its own session (below)
    process.kill()


def run(args: list[str], timeout: int, cwd: Path = ROOT) -> subprocess.CompletedProcess[bytes]:
    # Bytes, not text: a mutant can make the test binary print arbitrary bytes, which must not
    # abort the whole run with a decoding error.
    start_own_session = {} if os.name == "nt" else {"start_new_session": True}
    with subprocess.Popen(
        args, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **start_own_session
    ) as process:
        try:
            out, err = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            kill_tree(process)
            out, err = process.communicate()
            raise subprocess.TimeoutExpired(args, timeout, output=out, stderr=err) from None
    return subprocess.CompletedProcess(args, process.returncode, out, err)


def failing_tests(output: str | bytes | None) -> list[str]:
    """The tests the binary reported as failed, plus one entry for an assertion that failed in a
    test that never finished (the binary hung or died inside it)."""
    if output is None:
        return []
    if isinstance(output, bytes):
        output = output.decode("utf-8", "replace")
    names: list[str] = []
    assertion_pending = False
    for line in output.splitlines():
        if line.startswith("    FAILED"):
            assertion_pending = True
        elif line.startswith("  FAIL"):
            names.append(line.split(maxsplit=1)[1])
            assertion_pending = False
        elif line.startswith("  ok"):
            assertion_pending = False
    if assertion_pending:
        names.append("(an assertion in a test that did not finish)")
    return names


def build_and_test(build_args: list[str], binary: Path) -> tuple[str, list[str], str]:
    """Returns (status, failing test names, note). status: caught, MISSED, BUILD FAILED, TIMED OUT.

    A binary that reports failures and then hangs counts as caught: the mutant was found, and
    a later test not finishing is one more symptom of it, not a miss.
    """
    try:
        build = run(build_args, BUILD_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        return "TIMED OUT", [], ""
    if build.returncode != 0:
        return "BUILD FAILED", [], ""
    try:
        test = run([str(binary)], TEST_TIMEOUT_S)
    except subprocess.TimeoutExpired as hang:
        failing = failing_tests(hang.stdout)
        if failing:
            return "caught", failing, "(the binary then hung)"
        return "TIMED OUT", [], ""
    return ("caught" if test.returncode != 0 else "MISSED"), failing_tests(test.stdout), ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    default_preset = "windows" if platform.system() == "Windows" else "linux"
    parser.add_argument("--preset", default=default_preset)
    parser.add_argument(
        "--check",
        action="store_true",
        help="only verify that every mutation's text is found exactly once; build nothing",
    )
    parser.add_argument(
        "--only",
        default="",
        help="run only the mutations whose description contains this text",
    )
    opts = parser.parse_args()
    # The build presets map onto the configure presets' directories: "windows-release" builds
    # the Release configuration in the "windows" (Visual Studio, multi-config) directory; the
    # Ninja and Linux presets are single-config directories of their own.
    on_windows = opts.preset.startswith("windows")
    platform_name = "windows" if on_windows else "linux"
    configure_preset = "windows" if opts.preset in ("windows", "windows-release") else opts.preset
    build_dir = ROOT / "build" / configure_preset
    # --parallel: a mutant rebuilds a handful of translation units; one core per unit.
    build_args = ["cmake", "--build", str(build_dir), "--target", "etch_tests", "--parallel"]
    if opts.preset in ("windows", "windows-release"):
        configuration = "Release" if opts.preset == "windows-release" else "Debug"
        build_args += ["--config", configuration]
        binary = build_dir / configuration / "etch_tests.exe"
    elif on_windows:
        binary = build_dir / "etch_tests.exe"
    else:
        binary = build_dir / "etch_tests"

    if not opts.check:
        # Baseline gate: a suite that already fails would "catch" every mutant for the wrong
        # reason.
        status, failing, _ = build_and_test(build_args, binary)
        if status != "MISSED":
            print(f"baseline is not green ({status} {', '.join(failing)}); fix that first")
            return 2
        print("baseline: all tests pass")

    results = []
    for entry in MUTATIONS:
        file_name, description, edits, *only = entry
        if only and platform_name not in only[0]:
            continue  # a mutation of another platform's code
        if opts.only and opts.only not in description:
            continue
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
        if opts.check:
            results.append(("caught", description, []))
            print(f"applies      {description}")
            continue
        path.write_text(mutated, encoding="utf-8", newline="\n")
        try:
            status, failing, note = build_and_test(build_args, binary)
            results.append((status, description, failing))
            if failing:
                detail = ", ".join(failing) + (" " + note if note else "")
            elif status == "caught":
                detail = "(the test binary crashed)"
            elif status == "MISSED":
                detail = "(every test passed)"
            else:
                detail = ""
            print(f"{status:<12} {description}  {detail}")
        finally:
            path.write_text(original, encoding="utf-8", newline="\n")

    if opts.check:
        not_applied = sum(1 for r in results if r[0] == "NOT APPLIED")
        print(f"\n{len(results) - not_applied} of {len(results)} mutations apply")
        return 0 if not_applied == 0 else 1

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
