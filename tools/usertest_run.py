#!/usr/bin/env python3
"""Run the self-checking ring-3 diagnostics in /tests, as one pass/fail table.

WHY THIS EXISTS. `make test` runs the in-kernel KTESTs; `gui_regress.py`
runs the GUI tools. Between them sat a gap: the `/tests` binaries are
real ring-3 programs that check real things, and nothing ever ran them
except a person typing `run <name>` at a shell. `libc_test` was written
into exactly that gap -- it covers what a KTEST structurally cannot
reach (the C-name headers and kfmt.o being LINKABLE from ring 3, not the
k_* logic underneath, which has KTESTs already and would pass either
way) -- and it would have been checked once and then never again.

WHAT IT ASSERTS. Each entry names an exit code AND the output that must
appear. Both halves matter: a program that dies before printing anything
can still exit 0 through a path that never ran the checks, and a program
that prints "all checks passed" while returning non-zero is equally
wrong. `exit_test` is in here for a related reason -- its expected code
is 42, so a kernel that lost the exit code entirely and reported 0 for
everything would redden it while every other test stayed green.

THE EXCLUSIONS ARE THE INTERESTING PART, so they are listed in EXCLUDED
below with a reason each rather than silently omitted. The short version:
a test that deliberately faults, one that blocks forever on the serial
port, one that needs a windowed desktop, and one whose result depends on
being spawned by a parent are all things a "just run everything" harness
would report as failures of the code under test rather than of its own
assumptions. That is not hypothetical -- `pipe_test` exits 3 under `run`
(its waitpid finds no parent) and passes perfectly well under the KTEST
that spawns it properly.

Usage:
    python3 tools/usertest_run.py                 # against a copy of disk.img
    python3 tools/usertest_run.py -k libc         # only matching tests
    python3 tools/usertest_run.py --list          # what's in it, and what isn't
    python3 tools/usertest_run.py --instance 2    # alongside another VM

Works against a COPY of disk.img by default (several of these write
files), so it never disturbs an image the user has a QEMU open on.
Exits 0 if every test passed, 1 otherwise, 2 if it could not run.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VM = os.path.join(REPO, "tools", "vm.py")

# name -> (expected exit code, [substrings that must appear],
#          [substrings that must NOT appear])
#
# A FORBIDDEN substring is matched only against lines the test itself
# printed -- lines carrying its own name -- never against everything the
# serial console said during the run. The kernel logs into the same
# stream, and "FAIL"/"FAILED" are words it uses for its own reasons
# (`atac: FLUSH FAILED` among them), so an unscoped match turns unrelated
# kernel noise into a failed test. That is not hypothetical: CI reported
# newsyscalls_test as FAILED on a run whose captured output ended with
# "all phases passed" and exit code 0.
TESTS = [
    ("libc_test", 0,
     ["libc_test: all checks passed"], ["FAIL"]),
    ("fpu_test", 0,
     ["fpu_test: all checks passed"], ["FAIL"]),
    ("newsyscalls_test", 0,
     ["newsyscalls_test: all phases passed"], ["FAILED"]),
    ("file_test", 0,
     ["filetest: round trip OK"], []),
    ("write_test", 0,
     ["Hello from ring 3"], []),
    ("random_test", 0,
     ["random_test: all checks passed"], ["FAIL"]),
    ("guard_test", 0,
     ["guard_test: all checks passed"], ["FAIL"]),
    ("fsgen_test", 0,
     ["fsgen_test: all checks passed"], ["FAIL"]),
    # Not a self-checker: it exists to prove an exit code survives the
    # round trip out of ring 3, so the CODE is the whole assertion.
    ("exit_test", 42, [], []),
]

# Deliberately not run here. Each line is a reason, not an apology --
# adding one of these without solving the reason would produce a red
# table that says nothing about the code under test.
EXCLUDED = [
    ("crash_test",       "faults on purpose; the point is the kernel's recovery"),
    ("nx_test",          "faults on purpose (jumps into a data page) -- see shell_flow.py"),
    ("stack_smash_test", "faults on purpose (trips the stack canary)"),
    ("stackovf_test",    "runs off the stack on purpose; the assertion is the KERNEL's "
                          "report, not an exit code -- see its own top comment"),
    ("write_bad_test",   "hands the kernel a bad pointer on purpose"),
    ("waitany_test",     "SYS_WAITPID(-1) needs the caller to BE a scheduled process; "
                          "`run` uses the legacy loader, which has no pid, so nothing it "
                          "spawns has a parent and wait-any has nobody to ask about. "
                          "Drive it with `spawn /tests/waitany_test` instead"),
    ("pipe_test",        "needs a parent to spawn it and reap it; exits 3 under `run`, "
                          "and kernel/proc/pipe_test.c's KTEST covers it properly"),
    ("cputime_test",     "must be SCHEDULER-spawned to have a procs[] slot at all; "
                          "under `run` (the legacy process_run_ring3 path) it has none, "
                          "so it cannot find itself and nothing is billed to it either. "
                          "kernel/proc/cputime_test.c's KTEST spawns it properly"),
    ("echo_test",        "blocks forever reading a serial port with nothing on the far end"),
    ("gui_test",         "takes over the real screen; see apps/README.md on its scope"),
    ("win_test",         "modal window, outside the window list"),
    ("winclient",        "windowed TWP client -- tools/winclient_test.py"),
    ("uiclient",         "windowed ugfx client -- tools/uiclient_test.py"),
    ("hangclient",       "wedges on purpose -- tools/forcequit_test.py"),
    ("event_test",       "waits on window events that only a desktop delivers"),
    ("counter_a",        "runs until descheduled; a scheduler fixture, not a checker"),
    ("counter_b",        "as counter_a"),
    ("spin_test",        "spins forever on purpose"),
    ("fpu_race",         "a scheduler fixture -- kernel/proc/sched_test.c asserts on it"),
    ("socket_test",      "needs a network peer"),
]

EXIT_RE = re.compile(r"Exit code:\s*(-?\d+)")


def vm(args, *argv, check=True):
    cmd = [sys.executable, VM, "--disk", args.disk]
    if args.instance:
        cmd += ["--instance", str(args.instance)]
    cmd += list(argv)
    r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    if check and r.returncode != 0:
        print(r.stdout + r.stderr, file=sys.stderr)
    return r


def run_one(args, name):
    r = vm(args, "exec", f"run {name}", "--label", check=False)
    out = r.stdout + r.stderr
    m = EXIT_RE.search(out)
    return (m.group(1) if m else None), out


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disk", default=None,
                    help="image to boot (default: a temporary copy of disk.img)")
    ap.add_argument("--instance", type=int, default=0,
                    help="vm.py slot, for running alongside another VM")
    ap.add_argument("-k", metavar="SUBSTR", default="",
                    help="only tests whose name contains SUBSTR")
    ap.add_argument("--list", action="store_true",
                    help="print what is run and what is deliberately not")
    args = ap.parse_args()

    if args.list:
        print("Run by this tool:")
        for name, code, want, _ in TESTS:
            note = f"exit {code}" + (f", expects {want[0]!r}" if want else "")
            print(f"  {name:<18} {note}")
        print("\nDeliberately NOT run:")
        for name, why in EXCLUDED:
            print(f"  {name:<18} {why}")
        return 0

    selected = [t for t in TESTS if args.k in t[0]]
    if not selected:
        print(f"usertest_run: no test matches -k {args.k!r}")
        return 2

    tmp = None
    if not args.disk:
        src = os.path.join(REPO, "disk.img")
        if not os.path.exists(src):
            print("usertest_run: no disk.img -- run `make iso` first")
            return 2
        # A copy, not the real image: several of these write files, and
        # the user may have their own QEMU holding a write lock on it.
        tmp = tempfile.NamedTemporaryFile(suffix=".img", delete=False)
        tmp.close()
        subprocess.run(["cp", "--reflink=auto", src, tmp.name], check=True)
        args.disk = tmp.name

    try:
        if vm(args, "start").returncode != 0:
            print("usertest_run: could not start the VM")
            return 2
        results = []
        for name, want_code, want, forbid in selected:
            code, out = run_one(args, name)
            problems = []
            if code is None:
                problems.append("no exit code (did it hang?)")
            elif int(code) != want_code:
                problems.append(f"exit {code}, wanted {want_code}")
            for s in want:
                if s not in out:
                    problems.append(f"missing {s!r}")
            # Only the test's OWN lines -- see the TESTS table comment.
            mine = [l for l in out.splitlines() if name.split("_")[0] in l]
            # A scoped check that found no lines to scope to cannot fail,
            # which is indistinguishable from passing. Say so.
            if forbid and not mine:
                problems.append("no output lines carry this test's name -- "
                                "its forbidden-substring checks could not run")
            for s in forbid:
                hits = [l for l in mine if s in l]
                if hits:
                    # Quote the offending line. Printing only the tail of
                    # the output (below) hid it completely the one time
                    # this fired, which turned a one-line diagnosis into
                    # an investigation.
                    problems.append(f"saw {s!r} in: {hits[0].strip()[:80]}")
            results.append((name, problems, out))
    finally:
        vm(args, "stop", check=False)
        if tmp:
            os.unlink(tmp.name)

    failed = 0
    print()
    for name, problems, out in results:
        if problems:
            failed += 1
            print(f"  FAIL  {name:<18} {'; '.join(problems)}")
            # The test's OWN lines first, all of them: a self-checking
            # binary says which phase failed, and that line is usually
            # nowhere near the end of the output. Printing only the tail
            # hid it completely on a CI failure and turned "which phase?"
            # into a second round trip.
            own = [l for l in out.splitlines() if name.split("_")[0] in l]
            for line in own[-20:]:
                print(f"          | {line}")
            tail = [l for l in out.strip().splitlines()[-4:] if l not in own]
            for line in tail:
                print(f"          . {line}")
        else:
            print(f"  ok    {name}")
    total = len(results)
    print(f"\nusertest_run: {total - failed}/{total} passed"
          f"{'' if not failed else f', {failed} FAILED'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
