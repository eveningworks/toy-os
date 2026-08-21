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
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VM = os.path.join(REPO, "tools", "vm.py")

# name -> (expected exit code, [substrings that must appear],
#          [substrings that must NOT appear])
#
# AN EXIT CODE OF None MEANS "SPAWN IT, AND JUDGE IT BY WHAT IT PRINTS".
# `run` uses the legacy loader, which has no scheduler slot -- so a test
# needing anything that goes through the window server (ugfx_font_init()
# asks for the kernel's glyph tables through SYS_WIN_REQUEST) gets
# refused there and would measure nothing. Such a test is spawned as a
# real process instead, which costs the exit code, so its own printed
# verdict has to carry the whole assertion. That is why wrap_test prints
# "0 failure(s)" rather than only exiting 0.
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
    # uui_label's word wrapping. Its load-bearing check is that a word
    # wider than the line is BROKEN rather than refused: refusing it
    # returns the same cursor and loops forever inside a draw call,
    # which hangs the compositor rather than drawing something wrong.
    ("wrap_test", None,
     ["0 failure(s)"], ["FAIL"]),
    # Error codes reaching ring 3. Its load-bearing check is that a full
    # descriptor table and a missing file are DIFFERENT answers, which
    # needs a process that has really run out of fds -- see the file.
    ("errno_test", 0,
     ["errno_test: all checks passed",
      "ENOENT and EMFILE are distinct: yes"], ["FAIL"]),
    ("fpu_test", 0,
     ["fpu_test: all checks passed"], ["FAIL"]),
    # The shared line editor's SECOND compilation. Same gap libc_test
    # covers: klineedit.c has KTESTs, and they would pass whether or not
    # ring 3 could link a byte of it.
    ("klineedit_test", 0,
     ["klineedit_test: all checks passed"], ["FAIL"]),
    # The TrueType rasterizer's SECOND compilation, and the same gap
    # klineedit_test covers: kernel/lib/ttf.c has KTESTs, and every one
    # of them would pass whether or not ring 3 could link a byte of it.
    # SKIPS ITSELF on an image built with no fonts, so it must not
    # require a pass line that only appears when there is a font.
    ("ttf_test", 0,
     ["ttf_test:"], ["FAIL"]),
    ("newsyscalls_test", 0,
     ["newsyscalls_test: all phases passed"], ["FAILED"]),
    ("file_test", 0,
     ["filetest: round trip OK"], []),
    ("write_test", 0,
     ["Hello from ring 3"], []),
    ("random_test", 0,
     ["random_test: all checks passed"], ["FAIL"]),
    # Takes every byte SYS_SBRK will give (~14 MiB), writes an
    # address-derived pattern and reads it back, so it also asserts the
    # heap bound holds -- "sbrk refused" is a REQUIRED line, not an
    # incidental one. Adds about a second.
    ("memtest", 0,
     ["memtest: PASSED", "sbrk refused as expected"], ["FAIL", "MISMATCH"]),
    ("guard_test", 0,
     ["guard_test: all checks passed"], ["FAIL"]),
    # malloc/free in ring 3 -- the kernel's own allocator over sbrk.
    # Its coalescing and reuse checks are measured through sbrk(0), an
    # independent path from the allocator's own bookkeeping.
    ("malloc_test", 0,
     ["malloc_test: all checks passed"], ["FAIL"]),
    ("fsgen_test", 0,
     ["fsgen_test: all checks passed"], ["FAIL"]),
    # lseek/fstat/O_APPEND. Its pattern is POSITION-DERIVED, so a seek
    # landing at the wrong offset reads the wrong letter -- a file of
    # identical bytes cannot tell a working seek from a dead one.
    ("seek_test", None,
     ["seek_test: all checks passed"], ["FAIL"]),
    # The C library's stream layer. Two of its required lines are load-
    # bearing and neither is the verdict: "atexit:BA" can only appear if
    # exit() ran the handlers in LIFO order AND flushed an unterminated
    # line after main() returned, which no assertion inside main() can
    # reach. The buffering check inside the file is the other one -- see
    # the test's own header.
    # Stage 3 of the C library. Two of its checks needed a second try to
    # become real: "zz" cannot catch a strtol that returns the
    # post-sign position for a failed parse (both pointers are equal for
    # that input), and nothing else in the file can see whether longjmp
    # restored rsp, because it also restores rbp. See the test's header.
    ("libc3_test", 0,
     ["libc3_test: all checks passed"], ["FAIL"]),
    # Stage 4: %f/%e/%g, strtod and math.h. Asserted as formatted TEXT
    # against literals, since that is the only thing a printf caller can
    # observe -- and the accuracy limit is real (printf_float.c), so
    # nothing here asserts a 17th significant digit.
    ("libc4_test", 0,
     ["libc4_test: all checks passed"], ["FAIL"]),
    ("stdio_test", 0,
     ["stdio_test: all checks passed", "atexit:BA"], ["FAIL"]),
    # SYS_QUERY from ring 3. Runs fine under `run`: it spawns nothing and
    # waits for nothing, so the legacy loader's missing scheduler slot
    # costs it nothing.
    ("query_test", 0,
     ["query_test: all checks passed"], ["FAIL"]),
    # Not a self-checker: it exists to prove an exit code survives the
    # round trip out of ring 3, so the CODE is the whole assertion.
    ("exit_test", 42, [], []),
]

# Deliberately not run here. Each line is a reason, not an apology --
# adding one of these without solving the reason would produce a red
# table that says nothing about the code under test.
EXCLUDED = [
    ("crash_test",       "faults on purpose; the point is the kernel's recovery"),
    ("nx_test",          "faults on purpose (jumps into a data page) -- see faulttest_run.py"),
    ("stack_smash_test", "faults on purpose (trips the stack canary)"),
    ("stackovf_test",    "runs off the stack on purpose; the assertion is the KERNEL's "
                          "report, not an exit code -- see its own top comment"),
    ("write_bad_test",   "hands the kernel a bad pointer on purpose"),
    ("waitany_test",     "SYS_WAITPID(-1) needs the caller to BE a scheduled process; "
                          "`run` uses the legacy loader, which has no pid, so nothing it "
                          "spawns has a parent and wait-any has nobody to ask about. "
                          "Drive it with `spawn /tests/waitany_test` instead"),
    ("orphan_test",      "abandons children on purpose and exits at once; the "
                          "assertion is whether INIT reaps them, which is the process "
                          "table before and after -- tools/init_test.py"),
    ("sleep_test",       "SYS_SLEEP refuses a caller with no scheduler slot, and `run` "
                          "is the legacy loader, which has none. Driven by "
                          "tools/init_test.py through `spawn` instead"),
    ("pipefull_test",    "the writer must BLOCK on a full pipe, and the legacy "
                          "`run` loader has no scheduler slot to park in -- so the "
                          "write reports 0 there and the test fails against a "
                          "correct kernel; kernel/proc/fd_test.c spawns it"),
    ("pipedrain",        "the reading half of pipefull_test; on its own it waits "
                          "on a console that never reaches EOF"),
    ("catin",            "copies stdin to stdout; without a `<` it waits on the "
                          "console, which has no EOF"),
    ("cwd_test",         "its load-bearing check spawns /bin/mkdir with a bare name "
                          "and waits for it, which the legacy `run` loader cannot do "
                          "(no scheduler slot, so waitpid returns before the child has "
                          "created anything and inheritance reads as broken); "
                          "kernel/fs/cwd_test.c's KTEST spawns it properly"),
    ("fd_test",          "spawns a child and waits for it, which the legacy `run` "
                          "loader cannot do (no scheduler slot, so waitpid returns "
                          "at once and it reads the child's file too early); "
                          "kernel/proc/fd_test.c's KTEST spawns it properly"),
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

# How long a spawned test is given before its output is collected. These
# are small programs; the wait is for the scheduler to run them at all,
# not for them to do work.
SPAWN_SETTLE_S = 3.0

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


def run_one(args, name, spawned=False):
    if spawned:
        # Spawned, then dmesg'd: `spawn` returns as soon as the child
        # exists, so the child's own output is collected on the second
        # command rather than from the first. There is no exit code to
        # read -- see the TESTS table comment.
        a = vm(args, "exec", f"spawn /tests/{name}", "--label", check=False)
        # THE VERDICT IS READ BACK FROM A FILE, not from the console.
        # `spawn` returns as soon as the child exists, so its output
        # arrives while this harness is between commands -- where it is
        # dropped. Chaining `dmesg` onto the same exec collects the log
        # from BEFORE the test ran; sleeping and then asking collects it
        # from after the output was already discarded. Both were tried.
        # The test writes its own verdict to /tmp, which can be asked for
        # at any time -- waiting on the artifact rather than the timing.
        time.sleep(SPAWN_SETTLE_S)
        b = vm(args, "exec", f"cat /tmp/{name}.out", "--label", check=False)
        return None, a.stdout + a.stderr + b.stdout + b.stderr
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
            spawned = want_code is None
            code, out = run_one(args, name, spawned)
            problems = []
            if spawned:
                pass  # judged by its printed verdict alone
            elif code is None:
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
