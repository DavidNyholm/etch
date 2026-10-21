"""Pre-push check: refuses to push anything that should never be public.

Scans the commits that are about to leave this machine (their added lines, file names and
messages) for local paths, home directories, the current login name as a path segment,
e-mail addresses other than GitHub's noreply one, token-shaped strings, private keys, local
tooling files and large binaries. It is a heuristic: it reads text that git shows as added
(merge commits against their first parent) and file names; it does not decode binary files. Nothing here knows your name or paths: it reads them from
the environment at run time, so the script itself contains nothing personal.

    python tools/prepush.py --install     # install as .git/hooks/pre-push (once per clone)
    python tools/prepush.py --all         # scan every commit on every branch, right now
    python tools/prepush.py --range origin/main..main   # scan a git range by hand

As a hook it is run by git with the refs on stdin; a non-zero exit aborts the push.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ZERO_SHA = "0" * 40
LARGE_FILE_BYTES = 1_000_000  # a header-only library has no business shipping anything this big

# Addresses that are fine to publish: GitHub's privacy relay and the model attribution.
ALLOWED_EMAIL_SUFFIXES = ("@users.noreply.github.com", "noreply@anthropic.com")

# Files that are local tooling or generated output, never part of the project.
BLOCKED_FILE_PATTERNS = [
    r"(^|/)AGENTS\.md$",
    r"(^|/)CLAUDE\.md$",
    r"(^|/)RESEARCH\.md$",
    r"(^|/)\.agent-style\.toml$",
    r"(^|/)ruff\.toml$",
    r"(^|/)\.clang-format$",
    r"(^|/)\.wslconfig$",
    r"(^|/)CMakeUserPresets\.json$",
    r"\.log$",
    r"\.log\.\d+$",
    r"\.jsonl$",
    r"(^|/)\.env($|\.)",
    r"(^|/)\.codex(/|$)",
    r"(^|/)\.claude(/|$)",
    r"\.pem$",
    r"\.key$",
    r"\.pfx$",
    r"\.p12$",
]

# Content that must not appear on an added line or in a commit message. (name, regex)
BLOCKED_TEXT_PATTERNS = [
    # A drive letter, a colon and a slash (a Windows absolute path); a URL scheme does not
    # match because the letter must not follow another letter and the slash is not doubled.
    ("drive-letter path", r"(?<![A-Za-z0-9])[A-Za-z]:[\\/](?![\\/])\S"),
    (
        "home directory",
        r"(/home/[A-Za-z0-9._-]+/|/Users/[A-Za-z0-9._-]+/|[\\/]Users[\\/][A-Za-z0-9._-]+[\\/])",
    ),
    ("GitHub token", r"\b(ghp|gho|ghu|ghs|ghr)_[A-Za-z0-9]{20,}\b|\bgithub_pat_[A-Za-z0-9_]{20,}\b"),
    ("OpenAI-style key", r"\bsk-[A-Za-z0-9_-]{20,}\b"),
    ("AWS access key", r"\bAKIA[0-9A-Z]{16}\b"),
    ("Slack token", r"\bxox[abpr]-[A-Za-z0-9-]{10,}\b"),
    ("Google API key", r"\bAIza[0-9A-Za-z_-]{35}\b"),
    ("private key block", r"-----BEGIN [A-Z ]*PRIVATE KEY-----"),
    (
        "credential assignment",
        r"(?i)\b(api[_-]?key|secret|token|password|passwd)\b\s*[:=]\s*[\"'][^\"']{8,}[\"']",
    ),
]
EMAIL_PATTERN = re.compile(r"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}")


def git(*args: str) -> str:
    result = subprocess.run(
        ["git", *args], cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace"
    )
    if result.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def login_name() -> str:
    for key in ("USERNAME", "USER", "LOGNAME"):
        if os.environ.get(key):
            return os.environ[key]
    return ""


def personal_patterns() -> list[tuple[str, re.Pattern[str]]]:
    """Patterns built from this machine's identity, never written into the script."""
    patterns = []
    name = login_name()
    if name:
        # Only as a path segment: the name may legitimately appear as an author credit.
        patterns.append(("login name in a path", re.compile(r"[\\/]" + re.escape(name) + r"[\\/]", re.I)))
    home = str(Path.home())
    if len(home) > 3:
        patterns.append(("home directory path", re.compile(re.escape(home), re.I)))
        patterns.append(("home directory path", re.compile(re.escape(home.replace("\\", "/")), re.I)))
    return patterns


class Findings:
    def __init__(self) -> None:
        self.items: list[str] = []

    def add(self, where: str, what: str, sample: str) -> None:
        sample = sample.strip()
        if len(sample) > 100:
            sample = sample[:100] + "..."
        self.items.append(f"  {where}: {what}: {sample}")


def check_text(findings: Findings, where: str, text: str, extra: list[tuple[str, re.Pattern[str]]]) -> None:
    for name, pattern in BLOCKED_TEXT_PATTERNS:
        for match in re.finditer(pattern, text):
            findings.add(where, name, match.group(0))
    for name, pattern in extra:
        for match in pattern.finditer(text):
            findings.add(where, name, match.group(0))
    for match in EMAIL_PATTERN.finditer(text):
        address = match.group(0)
        if not address.endswith(ALLOWED_EMAIL_SUFFIXES):
            findings.add(where, "e-mail address", address)


def check_range(findings: Findings, rev_range: str, extra: list[tuple[str, re.Pattern[str]]]) -> int:
    """Checks every commit in the range. Returns how many commits were looked at."""
    commits = git("rev-list", "--reverse", rev_range).split()
    for sha in commits:
        short = sha[:7]
        message = git("log", "-1", "--format=%B", sha)
        check_text(findings, f"{short} commit message", message, extra)
        author = git("log", "-1", "--format=%ae %ce", sha)
        for address in author.split():
            if not address.endswith(ALLOWED_EMAIL_SUFFIXES):
                findings.add(f"{short} author/committer", "e-mail address", address)
        for line in git(
            "diff-tree", "--no-commit-id", "-r", "--name-only", "--root", "-m", "--first-parent", sha
        ).splitlines():
            check_text(findings, f"{short} file name", line, extra)  # a path can leak too
            for pattern in BLOCKED_FILE_PATTERNS:
                if re.search(pattern, line):
                    findings.add(f"{short} {line}", "file that must stay local", line)
                    break
            try:
                size = int(git("cat-file", "-s", f"{sha}:{line}"))
            except RuntimeError:
                continue  # deleted in this commit
            if size > LARGE_FILE_BYTES:
                findings.add(f"{short} {line}", "large file", f"{size} bytes")
        diff = git("show", "--format=", "--unified=0", "--no-color", "-m", "--first-parent", sha)
        current_file = ""
        for raw in diff.splitlines():
            if raw.startswith("+++ "):
                current_file = raw[4:].removeprefix("b/")
            elif raw.startswith("+") and not raw.startswith("+++"):
                check_text(findings, f"{short} {current_file}", raw[1:], extra)
    return len(commits)


def ranges_from_hook_stdin() -> list[str]:
    """pre-push gives one line per ref: <local ref> <local sha> <remote ref> <remote sha>."""
    ranges = []
    for line in sys.stdin.read().splitlines():
        parts = line.split()
        if len(parts) != 4:
            continue
        _, local_sha, _, remote_sha = parts
        if local_sha == ZERO_SHA:
            continue  # a deletion pushes nothing
        if remote_sha == ZERO_SHA:
            ranges.append(local_sha)  # new branch: everything reachable (git limits it to new commits)
        else:
            ranges.append(f"{remote_sha}..{local_sha}")
    return ranges


def install_hook() -> int:
    hooks = Path(git("rev-parse", "--git-path", "hooks").strip())
    if not hooks.is_absolute():
        hooks = ROOT / hooks
    hooks.mkdir(parents=True, exist_ok=True)
    hook = hooks / "pre-push"
    hook.write_text(
        "#!/bin/sh\n"
        "# Installed by tools/prepush.py: refuses to push local paths, secrets or local files.\n"
        'exec python -I "$(git rev-parse --show-toplevel)/tools/prepush.py" "$@"\n',
        encoding="utf-8",
        newline="\n",
    )
    hook.chmod(0o755)
    print(f"installed {hook}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--install", action="store_true", help="install as the pre-push hook")
    parser.add_argument("--all", action="store_true", help="scan every commit on every branch")
    parser.add_argument("--range", help="a git range to scan by hand, e.g. origin/main..main")
    # git runs a pre-push hook as "pre-push <remote name> <remote url>"; both are ignored here,
    # the refs come on stdin.
    parser.add_argument("hook_args", nargs="*", help=argparse.SUPPRESS)
    opts = parser.parse_args()
    if opts.install:
        return install_hook()

    extra = personal_patterns()
    findings = Findings()
    if opts.all:
        ranges = ["--all"]
    elif opts.range:
        ranges = [opts.range]
    else:
        ranges = ranges_from_hook_stdin()
    looked_at = 0
    for rev_range in ranges:
        if rev_range == "--all":
            looked_at += check_range(findings, "--all", extra)
        else:
            looked_at += check_range(findings, rev_range, extra)
    if not ranges:
        print("prepush: nothing to check")
        return 0
    if findings.items:
        print(f"prepush: REFUSED, {len(findings.items)} finding(s) in {looked_at} commit(s):")
        print("\n".join(findings.items))
        print("fix the commits (amend or rebase), then push again")
        return 1
    print(f"prepush: {looked_at} commit(s) checked, nothing that must stay local")
    return 0


if __name__ == "__main__":
    sys.exit(main())
