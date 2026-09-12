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
#   5. background wait loops -- also informational. A backgrounded poll
#      on an artifact that never appears (`until [ -s out.log ]`) cannot
#      exit, and nothing else in a session ever mentions it again.
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
# not part of what gets cloned -- so a fresh checkout falls back to the
# GLOBAL identity, which for most people is their real name and address.
# This project scrubbed exactly that out of every prior commit with a
# history rewrite (docs/decisions.md), and nothing in git warns you
# before the first commit reintroduces it. By then it is in the history
# and only another rewrite gets it out.
#
# WHAT IS CHECKED, and why it is not the exact value: the hazard is
# "you did not decide, so your global identity leaked in". Demanding one
# specific name would also refuse anyone working on a FORK, who has
# every right to commit as themselves -- so a local identity being SET
# is the hard requirement, and this project's own convention is a note.
#
# In the gate rather than in a git hook because .git/hooks is not cloned
# either: a guard against a clone-time problem has to live somewhere
# that travels with the repository.
conv_name="toy-os"
conv_email="noreply@toy-os.local"
local_name="$(git config --local user.name 2>/dev/null || true)"
local_email="$(git config --local user.email 2>/dev/null || true)"
if [ -z "$local_name" ] || [ -z "$local_email" ]; then
  echo "preflight: this repository has no commit identity of its own." >&2
  echo "preflight: it is per-repository and a CLONE DOES NOT CARRY IT, so" >&2
  echo "preflight: commits here would use your GLOBAL identity:" >&2
  echo "preflight:   $(git config user.name 2>/dev/null || echo '(unset)') <$(git config user.email 2>/dev/null || echo '(unset)')>" >&2
  echo "preflight: set one deliberately -- this project's own convention is" >&2
  echo "preflight:   git config --local user.name  '$conv_name'" >&2
  echo "preflight:   git config --local user.email '$conv_email'" >&2
  echo "preflight: on a fork, your own name and address are fine; the point" >&2
  echo "preflight: is that it is a CHOICE rather than a leak." >&2
  exit 1
fi
if [ "$local_name" != "$conv_name" ] || [ "$local_email" != "$conv_email" ]; then
  echo "preflight: note -- committing as $local_name <$local_email>," >&2
  echo "preflight: not this project's convention ($conv_name <$conv_email>)." >&2
fi

# Glob every instance's pidfile, not a hard-coded 0-3 list: vm.py
# --instance goes to 15 and gui_regress now uses up to 8 (DEFAULT_JOBS),
# so a guest on any of those holds disk.img's write lock and must be
# caught here -- the whole point of the check. `.vm.pid` is slot 0;
# `.vm.N.pid` are the rest. nullglob via the `[ -f ]` guard: an unmatched
# glob stays literal and is skipped.
vm_pid_file=""
for f in .vm.pid .vm.*.pid; do
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

step "check_config_size.py (a config file bigger than the parser's buffer)"
python3 tools/check_config_size.py || fail "config file size check"

step "check_docs.py (dead pointers, numbered milestones, duplicated entries)"
python3 tools/check_docs.py || fail "documentation check"

step "check_dispatch.py (dispatch chains that want a table)"
python3 tools/check_dispatch.py || fail "dispatch-chain check"

step "check_syscalls.py (no two syscalls on one number)"
python3 tools/check_syscalls.py || fail "syscall number check"

step "check_widget_ops.py (widget ops tables with a slot left NULL)"
python3 tools/check_widget_ops.py || fail "widget ops check"

step "check_key_routing.py (a key-taking widget an app routes no keys to)"
python3 tools/check_key_routing.py || fail "key routing check"

step "check_drivers.py (a driver that declares itself to nothing)"
python3 tools/check_drivers.py || fail "driver declaration check"

step "check_initcalls.py (an init both declared and hand-called, or at an unwalked level)"
python3 tools/check_initcalls.py || fail "initcall check"

step "check_copy_user.py (a vmm_copy_*_user() result tested as an errno)"
python3 tools/check_copy_user.py || fail "copy-user check"

step "check_chains.py (a write_dec/write_hex chain that grew)"
python3 tools/check_chains.py || fail "chain freeze"

step "check_tool_commands.py (a tool driving a command that no longer exists)"
python3 tools/check_tool_commands.py || fail "tool command check"

step "check_tool_coverage.py (a test tool no runner runs)"
python3 tools/check_tool_coverage.py || fail "tool coverage check"

step "check_licenses.py (a vendored port or font missing from LICENSE)"
python3 tools/check_licenses.py || fail "license inventory check"

# The baked font's header is GENERATED, and regenerating it needs a font
# most checkouts do not have installed -- so a hand-edit there survives
# until somebody who DOES have it regenerates and silently reverts it.
# Comment-and-counts only; no font needed to run this.
step "genttf.py --check (the generated font header matches its generator)"
python3 tools/genttf.py --check || fail "generated font header check"

# The host-side TFS3 writer is what SEEDS disk.img, so a leak in it
# corrupts this gate's own fixture -- an overwrite that did not free its
# double-indirect tables reddened ktest's fsck checks on the second run
# against one image. Host-only and about a second; no VM, nothing built.
step "tfs3_writer_test.py (the host seeder returns every block it frees)"
python3 tools/tfs3_writer_test.py || fail "tfs3_writer overwrite check"

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

# LINGERING WAIT LOOPS. A session that backgrounds a poll on an artifact
# which never comes to exist leaves a shell spinning until the session
# ends -- `until [ -s out.log ]` cannot tell "not yet" from "never", and
# a superseded run's log is never. Five were found six hours old in one
# session, and only because someone asked what was running.
#
# Informational, never a failure: an interactive session legitimately
# has shells of its own, and this cannot tell them apart. It only says
# what is there, which is the part nobody thinks to look at.
step "background wait loops (informational -- close what is no longer needed)"
# EXCLUDING THIS SCRIPT'S OWN ANCESTRY. The match is over command
# LINES, so a shell whose command merely CONTAINS the words -- this
# comment being written, for instance -- looks exactly like a loop
# running them. The invoking shell is never the leak, so drop it and
# everything above it.
mine=""
pid=$$
while [ -n "$pid" ] && [ "$pid" != "1" ] && [ "$pid" != "0" ]; do
    mine="$mine $pid"
    pid=$(ps -o ppid= -p "$pid" 2>/dev/null | tr -d ' ')
done
# shellcheck disable=SC2009  # pgrep cannot match the loop's shape, and
# the elapsed time is half the answer -- a waiter minutes old is normal,
# one hours old is the leak.
waiters=$(ps -eo pid,etime,args 2>/dev/null \
          | grep -E '(until|while) *\[' | grep -v grep \
          | while read -r wpid rest; do
                case " $mine " in *" $wpid "*) continue ;; esac
                echo "$wpid $rest"
            done || true)
if [ -n "$waiters" ]; then
    printf '  %s\n' "$waiters"
    echo "  ^ each of these is polling for something. If the run it waits"
    echo "    on is finished or superseded, kill it -- and prefer the"
    echo "    background job's OWN completion signal over a waiter beside"
    echo "    it, which is redundant even when it works (CLAUDE.md)."
else
    echo "  none"
fi

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
