#!/usr/bin/env python3
"""A record that preflight passed on THIS tree, and a check against it.

    python3 tools/preflight_stamp.py --write   # preflight.sh, on PASS
    python3 tools/preflight_stamp.py           # does the stamp cover this tree?

WHY. `update_server.py --publish` releases a build to the machines that
track `stable`, and "publish after the tests" was only a habit. The
stamp makes it a check: preflight writes it on PASS, and --publish
refuses a build it does not cover (`--force` overrides, and says so) --
Fedora's Bodhi and apt staging repositories gate promotion on a test
result the same way.

WHAT "THIS TREE" MEANS. The git TREE hash of every tracked and untracked,
non-ignored file, built in a scratch index -- so an edit, a new file or
a checkout all change it, and nothing about the real index or stash is
touched. `.claude/` and `.mcp.json` are left out: session tooling, not
build input, and they change on their own. And the staging tree must not
have been re-seeded since (`build/.seeded` no newer than the stamp),
since the same sources can be rebuilt with a different configuration.
"""
import argparse
import os
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STAMP = os.path.join(REPO, "build", ".preflight-pass")
SEEDED = os.path.join(REPO, "build", ".seeded")
EXCLUDE = (":!.claude", ":!.mcp.json")


def tree_state():
    """The tree hash of the working tree as it is now, or None without git."""
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, GIT_INDEX_FILE=os.path.join(tmp, "index"))
        try:
            subprocess.run(["git", "read-tree", "HEAD"], cwd=REPO, env=env, check=True,
                           capture_output=True)
            # An exclude git ALREADY ignores must not be named: `git add`
            # refuses a pathspec that names an ignored file, and the stamp
            # silently went unwritten once .mcp.json was in info/exclude.
            excl = [e for e in EXCLUDE
                    if subprocess.run(["git", "check-ignore", "-q", e[2:]], cwd=REPO,
                                      capture_output=True).returncode != 0]
            subprocess.run(["git", "add", "-A", "--", ".", *excl], cwd=REPO, env=env,
                           check=True, capture_output=True)
            return subprocess.run(["git", "write-tree"], cwd=REPO, env=env, check=True,
                                  capture_output=True, text=True).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            return None


def write():
    state = tree_state()
    if not state:
        return 1
    os.makedirs(os.path.dirname(STAMP), exist_ok=True)
    with open(STAMP, "w") as fh:
        fh.write(state + "\n")
    return 0


def why_not():
    """None when the stamp covers this tree and this build, else the reason."""
    if not os.path.exists(STAMP):
        return "no preflight has passed on this checkout (build/.preflight-pass)"
    with open(STAMP) as fh:
        stamped = fh.read().strip()
    if stamped != tree_state():
        return "the tree changed since preflight passed -- run tools/preflight.sh again"
    if os.path.exists(SEEDED) and os.path.getmtime(SEEDED) > os.path.getmtime(STAMP):
        return "the build was re-seeded since preflight passed -- run tools/preflight.sh again"
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--write", action="store_true", help="record a pass for this tree")
    args = ap.parse_args()
    if args.write:
        return write()
    why = why_not()
    print(f"preflight_stamp: {why}" if why else "preflight_stamp: covers this tree and build")
    return 1 if why else 0


if __name__ == "__main__":
    sys.exit(main())
