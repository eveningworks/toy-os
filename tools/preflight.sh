#!/usr/bin/env bash
# tools/preflight.sh -- "am I safe to deliver?" in one command.
#
# Every session's delivery step is supposed to end with a clean
# `make clean && make all && make iso` + `boot_smoke_test.py` + `ktest_run.py` run
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
#   2b. tools/check_deps.py -- confirms every build directory's .d files
#       are actually reaching make. A clean build hides this entirely
#       (nothing is stale when everything was just compiled), which is
#       exactly why it went unnoticed for a directory move; the check
#       is cheap and structural, so it runs here rather than nowhere.
#   3. tools/boot_smoke_test.py -- confirms the built ISO boots cleanly.
#   3b. tools/ktest_run.py -- runs the in-kernel test suite and fails
#       the whole preflight if any test failed. "Does it boot" and
#       "does it work" are different questions; this asks the second.
#   3c. tools/usertest_run.py -- the same question one ring out. The
#       KTESTs run INSIDE the kernel and so cannot tell whether a
#       /tests binary still links, loads and passes its own checks;
#       that gap is what libc_test was written into, and without this
#       step it would have been run once by hand and never again.
#   4. git status --short -- just informational: lists what's dirty so
#      you can eyeball it against the file list you're about to
#      deliver. This does NOT run git through the device bridge (that's
#      tools/device_git.sh's job) -- it only inspects whatever checkout
#      this script is actually running in, local or sandbox.
#
# The closing message differs depending on whether `git config
# user.name` is already set: unset means this is very likely a Cowork
# device-bridge sandbox clone (see CLAUDE.md's "Working in the cloud
# sandbox vs. directly on the user's machine" section -- that's the
# documented, deterministic tell), so it prints the SendUserFile/
# device_commit_files reminder; set means a direct local checkout, so
# it just confirms you're clear to commit (and push, if asked).
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

step "check_deps.py (header dependency tracking)"
python3 tools/check_deps.py || fail "header dependency tracking"

step "check_layout.py (disk layout vs docs/filesystem-layout.md)"
python3 tools/check_layout.py || fail "filesystem layout check"

step "check_docs.py (dead pointers, numbered milestones, duplicated entries)"
python3 tools/check_docs.py || fail "documentation check"

step "check_dispatch.py (dispatch chains that want a table)"
python3 tools/check_dispatch.py || fail "dispatch-chain check"

step "boot_smoke_test.py"
python3 tools/boot_smoke_test.py || fail "boot smoke test"

step "ktest (in-kernel test suite)"
python3 tools/ktest_run.py || fail "kernel test suite"

# The ring-3 side of the same question. ktest runs INSIDE the kernel, so
# it cannot see whether a /tests binary still links and runs at all --
# which is exactly the gap libc_test was written into. Boots once more
# against a temporary copy of disk.img; see the tool's docstring for
# what it deliberately does NOT run and why.
step "usertest_run.py (ring-3 /tests diagnostics)"
python3 tools/usertest_run.py || fail "ring-3 userland tests"

step "git status --short (informational -- compare against your delivery file list)"
git status --short || true

echo
echo "preflight: PASS -- build, iso, boot smoke test, ktest and the"
echo "preflight: ring-3 /tests diagnostics all clean."
if [ -z "$(git config user.name 2>/dev/null)" ]; then
  echo "preflight: no git identity configured -- this looks like a Cowork"
  echo "preflight: device-bridge sandbox clone. Deliver via SendUserFile +"
  echo "preflight: device_commit_files, then tools/device_git.sh commit on"
  echo "preflight: the device checkout (see CLAUDE.md)."
else
  echo "preflight: git identity is configured -- this looks like a direct"
  echo "preflight: local checkout. Safe to commit directly; push only if"
  echo "preflight: asked (see CLAUDE.md)."
fi
