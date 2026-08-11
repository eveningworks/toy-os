#!/usr/bin/env python3
"""tools/deliver.py -- builds the delivery manifest for shipping a
change from this sandbox checkout to the user's real machine.

What this does NOT do: it cannot call SendUserFile / device_commit_files
/ tools/device_git.sh itself -- those are Cowork-session tools, only
callable by the assistant driving this session, not by a plain script
running in the sandbox. What it DOES do is remove the actual error-prone
part of that step: figuring out which files changed, mapping each one
to its device path, catching the Makefile/*.yml protected-files
exception (see CLAUDE.md's "Working in the cloud sandbox" section)
*before* a `device_commit_files` call silently rejects it, and
generating a ready-to-fill commit message skeleton -- instead of
re-deriving all of that by hand, and only finding out about the
protected-file rejection after checking device_commit_files' `rejected`
array.

Usage:
    python3 tools/deliver.py                 # uses `git status --short`
    python3 tools/deliver.py file1 file2 ...  # explicit file list instead

Output: a manifest to stdout, one block per file:
    - repo-relative path
    - device path (under ~/CodingProjects/toy-os/, override with
      --device-root if the user's checkout ever lives somewhere else)
    - PROTECTED flag + the .new-suffix workaround reminder, for
      Makefile / anything under .github/workflows/
  ...followed by a ready-to-copy commit message skeleton (one line per
  file, left blank for you to fill in) and the exact tools/device_git.sh
  add/commit invocation with every path already filled in.

This is a planning aid, not a mechanical pipe -- the assistant still
makes the actual SendUserFile/device_commit_files/device_git.sh tool
calls, using this manifest instead of re-deriving the path list and
protected-file check by hand each time.
"""

import argparse
import subprocess
import sys

PROTECTED_PATTERNS = (
    "Makefile",
)


def is_protected(path):
    if path in PROTECTED_PATTERNS:
        return True
    if path.startswith(".github/workflows/") and (path.endswith(".yml") or path.endswith(".yaml")):
        return True
    return False


def git_changed_files():
    out = subprocess.run(
        ["git", "status", "--short"], capture_output=True, text=True, check=True
    ).stdout
    files = []
    for line in out.splitlines():
        if not line.strip():
            continue
        # "XY path" or "XY orig -> new" for renames -- take the final path.
        path = line[3:]
        if " -> " in path:
            path = path.split(" -> ", 1)[1]
        files.append(path)
    return files


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*", help="explicit repo-relative paths; defaults to `git status --short`")
    ap.add_argument("--device-root", default="~/CodingProjects/toy-os",
                     help="device-side checkout root (default: ~/CodingProjects/toy-os)")
    args = ap.parse_args()

    files = args.files if args.files else git_changed_files()
    if not files:
        print("deliver.py: no changed files (git status --short is clean, and none given explicitly).")
        return 0

    protected = [f for f in files if is_protected(f)]
    normal = [f for f in files if not is_protected(f)]

    print(f"# Delivery manifest -- {len(files)} file(s)\n")

    if normal:
        print("## Normal files -- SendUserFile, then device_commit_files as-is\n")
        for f in normal:
            print(f"  {f}")
            print(f"    -> {args.device_root}/{f}")
        print()

    if protected:
        print("## PROTECTED -- device_commit_files will REJECT these outright\n")
        print("   (Makefile / .github/workflows/*.yml -- see CLAUDE.md)")
        print("   Workaround: deliver as '<name>.new' via SendUserFile +")
        print("   device_commit_files, then apply over device_bash:")
        print("     cp <name>.new <name> && diff <name>.new <name> && mv <name>.new _to_delete/\n")
        for f in protected:
            new_name = f + ".new"
            print(f"  {f}  (PROTECTED)")
            print(f"    deliver as -> {args.device_root}/{new_name}")
            print(f"    then on device: cp {new_name} {f} && rm -f {new_name}  # via device_bash, mv into _to_delete/ if rm is blocked")
        print()

    print("## Commit message skeleton (fill in the one-liners, keep subject short)\n")
    print("<subject line: short summary>\n")
    for f in files:
        print(f"- {f}: ")
    print("\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n")

    print("## tools/device_git.sh commands (run over the device bridge)\n")
    print("  bash tools/device_git.sh add \\")
    for i, f in enumerate(files):
        sep = " \\" if i < len(files) - 1 else ""
        print(f"    {f}{sep}")
    print("  bash tools/device_git.sh commit -m \"...\"  # use the skeleton above")

    return 0


if __name__ == "__main__":
    sys.exit(main())
