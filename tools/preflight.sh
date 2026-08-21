#!/usr/bin/env bash
# tools/preflight.sh -- "am I safe to deliver?" in one command.
#
# Every session's delivery step is supposed to end with a clean
# `make clean && make all && make iso` + `boot_smoke_test.py` + `ktest_run.py` run
# against the FINAL state (see CLAUDE.md's "Delivering changes"
# section) -- but that's three separate commands run by hand, and it's
# easy to deliver after only re-running one of them, or after a stray
# uncommitted file got left out. This wraps all of it into a single
# pass/fail so there's one command to run right before committing, not
# three to remember.
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
#      you can eyeball it against the file list you are about to report.
#
# The closing message warns if `git config user.name` is unset, since a
# commit would then either fail or carry the wrong identity -- this repo
# commits as `toy-os <noreply@toy-os.local>` on purpose (see
# docs/decisions.md's history-scrub entry).
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
# GUARDED, because `set -e` is deliberately not in effect here (this is
# `set -uo pipefail`, so each step's status is checked by hand). An
# unguarded cd that failed would run the whole gate -- make clean
# included -- in whatever directory the caller happened to be in.
cd "$repo_root" || { echo "preflight: cannot cd to $repo_root" >&2; exit 1; }

skip_clean=0
for arg in "$@"; do
  case "$arg" in
    --skip-clean) skip_clean=1 ;;
    *) echo "preflight: unknown argument '$arg'" >&2; exit 1 ;;
  esac
done

step() { echo "== $1 =="; }
fail() { echo "preflight: FAIL -- $1"; exit 1; }

# A VM LEFT RUNNING FAILS THIS GATE MINUTES LATER, AND NOT WHERE YOU
# LOOK. `make iso` re-seeds disk.img while a guest holds a write lock on
# it, and the first thing to actually complain is boot_smoke_test, with
# "qemu exited early (code 1)" -- which reads as the kernel failing to
# boot rather than as a leftover from the previous command. A whole
# clean-build cycle is spent getting to that wrong conclusion.
#
# Checked here rather than in iso_guard.py because this is about a
# PROCESS, not about the ISO being stale, and because the fix is one the
# person running it has to make.
# THE COMMIT IDENTITY IS PER-REPOSITORY, AND A CLONE DOES NOT CARRY IT.
#
# `git config --local user.name/email` lives in .git/config, which is
# not part of what gets cloned -- so a fresh checkout on a new machine
# silently falls back to the GLOBAL identity, which is somebody's real
# name and address. This project scrubbed exactly that out of every
# prior commit with a history rewrite (docs/decisions.md), and nothing
# in git warns you before the first commit reintroduces it.
#
# Checked in the gate rather than in a hook, because .git/hooks is not
# cloned either -- the guard has to live somewhere that travels with the
# repository, and this is the thing everyone runs before committing.
want_name="toy-os"
want_email="noreply@toy-os.local"
have_name="$(git config user.name 2>/dev/null || true)"
have_email="$(git config user.email 2>/dev/null || true)"
if [ "$have_name" != "$want_name" ] || [ "$have_email" != "$want_email" ]; then
  echo "preflight: this repository's commit identity is not set." >&2
  echo "preflight:   have: ${have_name:-(unset)} <${have_email:-(unset)}>" >&2
  echo "preflight:   want: $want_name <$want_email>" >&2
  echo "preflight: it is per-repository and a CLONE DOES NOT CARRY IT, so a" >&2
  echo "preflight: fresh checkout falls back to your global identity -- which" >&2
  echo "preflight: is the real name and address a history rewrite once removed" >&2
  echo "preflight: from every commit here (docs/decisions.md). Run:" >&2
  echo "preflight:   git config --local user.name  '$want_name'" >&2
  echo "preflight:   git config --local user.email '$want_email'" >&2
  exit 1
fi

vm_pid_file=""
for f in .vm.pid .vm.0.pid .vm.1.pid .vm.2.pid .vm.3.pid; do
  [ -f "$f" ] || continue
  if kill -0 "$(cat "$f" 2>/dev/null)" 2>/dev/null; then vm_pid_file="$f"; break; fi
done
if [ -n "$vm_pid_file" ]; then
  echo "preflight: a vm.py guest is still running (pid $(cat "$vm_pid_file"), $vm_pid_file)." >&2
  echo "preflight: it holds a write lock on disk.img, which \`make iso\`" >&2
  echo "preflight: re-seeds -- the gate would fail several minutes from now" >&2
  echo "preflight: in boot_smoke_test, looking like a boot failure." >&2
  echo "preflight: run \`python3 tools/vm.py stop\` first." >&2
  exit 1
fi

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

step "check_widget_ops.py (widget ops tables with a slot left NULL)"
python3 tools/check_widget_ops.py || fail "widget ops check"

# The baked font's header is GENERATED, and regenerating it needs a font
# most checkouts do not have installed -- so a hand-edit there survives
# until somebody who DOES have it regenerates and silently reverts it.
# Comment-and-counts only; no font needed to run this.
step "genttf.py --check (the generated font header matches its generator)"
python3 tools/genttf.py --check || fail "generated font header check"

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
  echo "preflight: WARNING -- no git identity configured. This repo commits"
  echo "preflight: as toy-os <noreply@toy-os.local>; set it before committing"
  echo "preflight: so a real name cannot leak into history (see CLAUDE.md)."
else
  echo "preflight: safe to commit; push verified work (see CLAUDE.md)."
fi
