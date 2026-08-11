#!/usr/bin/env bash
# tools/preflight.sh -- "am I safe to deliver?" in one command.
#
# Every session's delivery step is supposed to end with a clean
# `make clean && make all && make iso` + `boot_smoke_test.py` run
# against the FINAL state (see CLAUDE.md's "Delivering changes"
# section) -- but that's three separate commands run by hand, and it's
# easy to deliver after only re-running one of them, or after a stray
# uncommitted file got left out. This wraps all of it into a single
# pass/fail so there's one command to run right before SendUserFile/
# device_commit_files, not three to remember.
#
# What it checks, in order (stops at the first failure):
#   1. make clean && make all -- full rebuild from scratch, no stale .o
#      files hiding a real error.
#   2. make iso -- confirms the ISO actually assembles.
#   3. tools/boot_smoke_test.py -- confirms the built ISO boots cleanly.
#   4. git status --short -- just informational: lists what's dirty so
#      you can eyeball it against the file list you're about to
#      deliver. This does NOT touch the device bridge or run git
#      through it (that's tools/device_git.sh's job over the bridge,
#      and this script may run in the cloud sandbox where plain `git`
#      is fine) -- it only inspects the sandbox checkout you're
#      building in.
#
# Usage:
#   tools/preflight.sh              # full check
#   tools/preflight.sh --skip-clean # skip `make clean` (faster re-check
#                                    # after preflight already passed
#                                    # once and only docs changed since)
#
# Exit code 0 = safe to deliver. Exit code 1 = stop, something's wrong
# -- read the last command's output above the FAIL line.

set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

skip_clean=0
for arg in "$@"; do
  case "$arg" in
    --skip-clean) skip_clean=1 ;;
    *) echo "preflight: unknown argument '$arg'" >&2; exit 1 ;;
  esac
done

step() { echo "== $1 =="; }
fail() { echo "preflight: FAIL -- $1"; exit 1; }

if [ "$skip_clean" -eq 0 ]; then
  step "make clean && make all"
  make clean >/tmp/preflight_clean.log 2>&1 || fail "make clean (see /tmp/preflight_clean.log)"
  make all >/tmp/preflight_build.log 2>&1 || fail "make all (see /tmp/preflight_build.log)"
else
  step "make all (--skip-clean: no full rebuild)"
  make all >/tmp/preflight_build.log 2>&1 || fail "make all (see /tmp/preflight_build.log)"
fi

step "make iso"
make iso >/tmp/preflight_iso.log 2>&1 || fail "make iso (see /tmp/preflight_iso.log)"

step "boot_smoke_test.py"
python3 tools/boot_smoke_test.py || fail "boot smoke test"

step "git status --short (informational -- compare against your delivery file list)"
git status --short || true

echo
echo "preflight: PASS -- build, iso, and boot smoke test all clean."
echo "preflight: remember this only checked the SANDBOX checkout -- still"
echo "preflight: deliver via SendUserFile + device_commit_files, then"
echo "preflight: tools/device_git.sh commit on the device checkout."
