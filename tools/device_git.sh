#!/usr/bin/env bash
# tools/device_git.sh -- run a git command against this repo when it's
# reached over the Cowork device bridge (mcp__remote-devices__device_bash).
#
# Why this exists: git commands run this way leave behind stale lock
# files. Git creates a lock (.git/index.lock for add/status,
# .git/HEAD.lock for commit, .git/objects/maintenance.lock and
# .git/objects/<xx>/tmp_obj_* for anything that writes objects, ...),
# then tries to unlink() it when the command finishes -- but the device
# bridge blocks unlink() the same way it blocks `rm` on mounted files,
# so the delete silently fails (git itself still succeeds; you just get
# a "warning: unable to unlink ... Operation not permitted" per file).
# The *next* git command that needs that same lock then fails hard --
# e.g. "fatal: Unable to create '.../index.lock': File exists" or
# "fatal: cannot lock ref 'HEAD': Unable to create '.../HEAD.lock'" --
# which looks exactly like a genuinely stuck git process, but isn't
# one. First version of this script only swept index.lock; a real
# `commit` also leaves HEAD.lock and objects/maintenance.lock behind,
# which bit a session that only cleared index.lock and then hit the
# HEAD.lock failure on the very next command. This version sweeps
# every *.lock file under .git/ (find -name '*.lock'), not just the
# well-known ones by name, so a lock in a spot not enumerated above
# doesn't repeat the same mistake.
#
# Fix: `mv` (rename) is allowed through the bridge even though `rm`
# (unlink) isn't, so rename every stale lock out of the way before
# running the real command instead of trying to delete it. See
# CLAUDE.md's "Working in the cloud sandbox vs. the user's machine"
# section for the full writeup.
#
# Usage: same as git itself, run from anywhere inside the repo, e.g.
#   tools/device_git.sh status
#   tools/device_git.sh add -A
#   tools/device_git.sh commit -m "..."
#   tools/device_git.sh push origin main --tags
#
# Safe to use for every git invocation over the device bridge, not just
# ones you expect to write -- it's a no-op when there are no stale
# locks. The leftover .git/objects/<xx>/tmp_obj_* temp files (same root
# cause, but not locks and not blocking) are harmless clutter, not
# swept here -- they don't stop any future git command from working.

set -euo pipefail

repo_root="$(git rev-parse --show-toplevel 2>/dev/null || true)"
if [ -z "$repo_root" ]; then
  echo "device_git.sh: not inside a git repo (run this from within the toy-os checkout)" >&2
  exit 1
fi

trash_dir="$repo_root/.git/_to_delete"
cleared=0
while IFS= read -r -d '' lock_file; do
  mkdir -p "$trash_dir"
  rel="${lock_file#"$repo_root"/.git/}"
  stale_name="$(echo "$rel" | tr '/' '_').stale_$$_$RANDOM"
  mv "$lock_file" "$trash_dir/$stale_name"
  echo "device_git.sh: cleared stale $rel -> .git/_to_delete/$stale_name" >&2
  cleared=$((cleared + 1))
done < <(find "$repo_root/.git" -name '*.lock' -print0 2>/dev/null)

exec git "$@"
