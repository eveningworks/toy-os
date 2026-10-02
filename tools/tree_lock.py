#!/usr/bin/env python3
"""The TEST LOCK: nothing rebuilds or edits the tree while a test is using it.

    python3 tools/tree_lock.py            # who holds it; exit 1 if anyone does
    python3 tools/tree_lock.py hook       # a Claude Code PreToolUse hook (stdin: JSON)

A test that boots this build or serves its staging tree reads the LIVE
checkout for minutes: `update_server.py` serves `seed/sync` per request
and `iso_guard.py` re-checks source mtimes before every boot. A `make
all`, a source edit or a `git stash` in that window does not fail
loudly -- the server answers 503 or the next boot is refused, and the
run reports a failure the code never had. Cargo's "Blocking waiting for
file lock on build directory" and Bazel's "Another command is running"
are the shape.

**A SHARED `flock` ON `.test-lock` AT THE REPO ROOT.** Shared, because
`gui_regress.py` runs a dozen tools at once; at the root, because `make
clean` deletes `build/`. The kernel drops it when the holder exits, so a
killed test leaves no stale lock. `.test-lock.d/<pid>` names each holder
for the message and is only ever READ for that: liveness is the lock.

Taken by `hold()`: `iso_guard.py`'s boot and staging checks (so every
launcher) and `preflight.sh` for its whole run. A holder exports
`TOYOS_TEST_LOCK_HELD`, which is how the `make` a holder runs itself
(preflight, `mutate.py` inside a test) is let through. Refused while
held: `make` (Makefile), and through the hook a source edit, a building
`make` and a `git` command that moves the tree. Docs, `tools/` and
`.claude/` stay editable -- none of them is in the image.
`TOYOS_IGNORE_TEST_LOCK=1` bypasses both.
"""
import fcntl
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOCK = os.path.join(REPO, ".test-lock")
HOLDERS = os.path.join(REPO, ".test-lock.d")
HELD_ENV = "TOYOS_TEST_LOCK_HELD"
BYPASS_ENV = "TOYOS_IGNORE_TEST_LOCK"

# What may change while a test runs: nothing here reaches the image.
FREE = ("docs/", "tools/", ".claude/", "screenshots/")
# `make` as a COMMAND -- at the start, or after `;`/`&&`/`|`/`(`, past
# `timeout N`/`VAR=x` prefixes -- and not a dry run. `grep make` is not one.
BUILDING_MAKE = re.compile(
    r"(?:^|[;&|(\n]|\b(?:then|do)\b)\s*(?:timeout\s+\S+\s+|\w+=\S*\s+)*make\b(?![^;&|\n]*\s-n\b)")
MOVING_GIT = re.compile(
    r"(?:^|[;&|(\n]|\b(?:then|do)\b)\s*git\s+(?:-C\s+\S+\s+)?"
    r"(stash|checkout|switch|reset|pull|merge|rebase|restore|am|cherry-pick|revert)\b")

_fd = None


def hold(tool=None):
    """Hold the lock until this process exits. Idempotent."""
    global _fd
    if _fd is not None:
        return
    _fd = os.open(LOCK, os.O_RDWR | os.O_CREAT, 0o644)
    fcntl.flock(_fd, fcntl.LOCK_SH)
    os.makedirs(HOLDERS, exist_ok=True)
    for stale in os.listdir(HOLDERS):   # a killed holder skips atexit
        if stale.isdigit() and not _alive(int(stale)):
            try:
                os.remove(os.path.join(HOLDERS, stale))
            except OSError:
                pass
    name = tool or os.path.basename(sys.argv[0]) or "python"
    with open(os.path.join(HOLDERS, str(os.getpid())), "w") as fh:
        fh.write(" ".join([name] + sys.argv[1:])[:200] + "\n")
    os.environ[HELD_ENV] = str(os.getpid())
    import atexit
    atexit.register(_release)


def _release():
    try:
        os.remove(os.path.join(HOLDERS, str(os.getpid())))
    except OSError:
        pass


def _alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def holders():
    """[(pid, what)] of the tests holding the lock; [] when nobody does."""
    if not os.path.exists(LOCK):
        return []
    fd = os.open(LOCK, os.O_RDONLY)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return []                       # nobody holds it
    except BlockingIOError:
        pass
    finally:
        os.close(fd)
    out = []
    for name in sorted(os.listdir(HOLDERS)) if os.path.isdir(HOLDERS) else []:
        if name.isdigit() and _alive(int(name)):
            try:
                with open(os.path.join(HOLDERS, name)) as fh:
                    out.append((int(name), fh.read().strip()))
            except OSError:
                pass
    return out or [(0, "a test (it left no name)")]


def describe(who):
    return "; ".join(f"{what} (pid {pid})" if pid else what for pid, what in who)


HEREDOC = re.compile(r"<<-?\s*(['\"]?)(\w+)\1[^\n]*\n.*?\n\s*\2\s*(?:\n|$)", re.S)
QUOTED = re.compile(r"'[^']*'|\"(?:\\.|[^\"\\])*\"")


def _code_only(cmd):
    """The command without heredoc bodies or quoted strings: a script that
    WRITES the words `git stash` is not one that runs it."""
    return QUOTED.sub("''", HEREDOC.sub("\n", cmd))


def _blocked(tool, inp):
    """Why this tool call would change what a running test reads, or None."""
    if tool in ("Edit", "Write", "MultiEdit", "NotebookEdit"):
        path = inp.get("file_path") or inp.get("notebook_path") or ""
        rel = os.path.relpath(os.path.realpath(path), os.path.realpath(REPO))
        if rel.startswith("..") or rel.endswith(".md") or rel.startswith(FREE):
            return None
        return f"editing {rel}"
    if tool == "Bash":
        cmd = _code_only(inp.get("command", ""))
        if BUILDING_MAKE.search(cmd):
            return "running make"
        m = MOVING_GIT.search(cmd)
        if m:
            return f"`git {m.group(1)}`"
    return None


def hook():
    try:
        call = json.load(sys.stdin)
    except ValueError:
        return 0
    if os.environ.get(BYPASS_ENV) == "1":
        return 0
    what = _blocked(call.get("tool_name", ""), call.get("tool_input") or {})
    if not what:
        return 0
    who = holders()
    if not who:
        return 0
    reason = (f"tree_lock: {describe(who)} is using this checkout, and {what} would change "
              f"what it reads (a stale-build 503 or a refused boot, reported as a test "
              f"failure). Wait for its completion notification, or stop it first. Docs, "
              f"tools/ and .claude/ stay editable.")
    print(json.dumps({"hookSpecificOutput": {
        "hookEventName": "PreToolUse",
        "permissionDecision": "deny",
        "permissionDecisionReason": reason}}))
    return 0


def main():
    if sys.argv[1:] == ["hook"]:
        return hook()
    if os.environ.get(BYPASS_ENV) == "1" or os.environ.get(HELD_ENV):
        return 0
    who = holders()
    if who:
        print(f"tree_lock: REFUSING -- {describe(who)} is using this checkout.\n"
              f"  Wait for it, or stop it. Bypass: {BYPASS_ENV}=1", file=sys.stderr)
        return 1
    if sys.stdout.isatty():
        print("tree_lock: free")
    return 0


if __name__ == "__main__":
    sys.exit(main())
