#!/usr/bin/env bash
# tools/device_git.sh -- run a git command against this repo when it's
# reached over the Cowork device bridge (mcp__remote-devices__device_bash).
#
# Why this exists: git commands run this way leave behind a stale
# .git/index.lock. Git creates the lock, then tries to unlink() it when
# the command finishes -- but the device bridge blocks unlink() the same
# way it blocks `rm` on mounted files, so the delete silently fails (git
# itself still succeeds; you just get a
# "warning: unable to unlink ... Operation not permitted"). The *next*
# git command that needs to write the index then fails hard with
# "fatal: Unable to create '.../index.lock': File exists" -- which looks
# exactly like a genuinely stuck git process, but isn't one.
#
# Fix: `mv` (rename) is allowed through the bridge even though `rm`
# (unlink) isn't, so rename any stale lock out of the way before running
# the real command instead of trying to delete it. See CLAUDE.md's
# "Working in the cloud sandbox vs. the user's machine" section for the
# full writeup.
#
# Usage: same as git itself, run from anywhere inside the repo, e.g.
#   tools/device_git.sh status
#   tools/device_git.sh add -A
#   tools/device_git.sh commit -m "..."
#   tools/device_git.sh push origin main --tags
#
# Safe to use for every git invocation over the device bridge, not just
# ones you expect to write -- it's a no-op when there's no stale lock.

set -euo pipefail

repo_root="$(git rev-parse --show-toplevel 2>/dev/null || true)"
if [ -z "$repo_root" ]; then
  echo "device_git.sh: not inside a git repo (run this from within the toy-os checkout)" >&2
  exit 1
fi

lock_file="$repo_root/.git/index.lock"
if [ -e "$lock_file" ]; then
  trash_dir="$repo_root/.git/_to_delete"
  mkdir -p "$trash_dir"
  stale_name="index.lock.stale_$$_$RANDOM"
  mv "$lock_file" "$trash_dir/$stale_name"
  echo "device_git.sh: cleared stale index.lock -> .git/_to_delete/$stale_name" >&2
fi

exec git "$@"
