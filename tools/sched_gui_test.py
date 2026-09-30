#!/usr/bin/env python3
"""Prove the desktop stays ALIVE while a ring-3 process runs.

This is the end-to-end counterpart to kernel/proc/sched_test.c. That
KTEST proves the scheduler property in the abstract -- kernel code keeps
getting CPU while a process is ready. This proves the thing a user would
actually notice: the window manager keeps drawing, keeps routing input
and keeps answering, instead of freezing until the process exits.

Before the kernel context joined the scheduler's rotation, it did NOT.
`wm_run()` got no CPU at all between a spawn and that process's exit, so
the desktop was frozen for the whole run; the Terminal only looked live
because a process's output reaches the screen from inside its own
SYS_WRITE handler rather than by the WM drawing a frame. See
kernel/proc/scheduler.c's ROT_KERNEL comment.

WHY THE ASSERTION IS SHAPED THIS WAY
------------------------------------
The `gui` debug commands are dispatched from inside wm_run() itself, so
a frozen WM cannot answer one -- which makes "did the WM answer?" a
direct, host-observable liveness test, no screenshot to interpret. But
answering quickly proves nothing on its own unless the process really
was still running at the time, so every sample is paired with the WM's
own launch table (`gui state --json`'s "launched", added for exactly
this) and only samples taken while a process was genuinely live are
counted.

That pairing is the whole point: OVERLAP is the claim, not speed.

Usage (the VM must already be up -- `python3 tools/vm.py start`):

    python3 tools/vm.py start
    python3 tools/sched_gui_test.py --shot screenshots/2026-08-14
    python3 tools/vm.py stop

Injected input enters below the PS/2 driver (see gui_debug.py), so a
clean run says nothing about the real keyboard path -- it is about
scheduling and WM liveness only.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession             # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

DEFAULT_SOCK = ".vm.serial"

# The silent long-running spinner (userland/tests/spin_test.c), seeded to
# /tests. Silent matters here for the same reason it does in the KTEST:
# its output would land on the desktop's log mid-test.
#
# The trailing number is spin_test's round count (see its parse_rounds()
# comment): each sample below costs a full serial round trip, so the
# process has to live for seconds to overlap MIN_LIVE_SAMPLES of them.
# The default duration overlapped exactly 3.
SPIN_PATH = "/tests/spin_test 40"   # `gui spawn` forwards the argument

# A kernel-space app, opened only so there is a window on screen for
# `gui windows` to walk -- the second half of the liveness claim.
WINDOW_APP = "System Settings"

K_RET = "0x0d"
K_SPACE = "0x20"  # `gui key` splits on whitespace, so space must be hex

# How many samples taken WHILE a process was live are enough to call the
# WM genuinely alive alongside it. The pre-change behaviour scores 0 --
# not a low number -- because a frozen wm_run() cannot answer a single
# `gui` command. Six is comfortably above a fluke without assuming any
# particular emulation speed.
MIN_LIVE_SAMPLES = 6

# Give up rather than hang if the process never appears or never ends.
SPAWN_TIMEOUT_S = 10.0
RUN_TIMEOUT_S = 30.0


def type_text(dbg, text):
    """`gui key` one character at a time -- the WM's injected-input path
    has no send_text() equivalent, and space needs its hex form."""
    for ch in text:
        dbg.send(f"gui key {K_SPACE if ch == ' ' else ch}")
    dbg.settle()


Result = Results


def run(dbg, qmp, shot_dir, res):
    # `gui spawn` goes through scheduler_spawn() exactly as a Start-menu
    # launch does; `run` from the PHYSICAL shell takes the legacy
    # blocking path instead, which deliberately still freezes the kernel
    # context (see kernel_slot_runnable()) and would prove the opposite
    # of the point. This used to open the kernel-space Terminal and type
    # at it -- that Terminal retired in M41's stage 0, and spawning
    # directly drops the shell, its allowlist and its pending-process
    # slot out of a test that was never about any of them.
    #
    # A window still has to be on screen, because `gui windows` answering
    # is half the liveness claim below.
    dbg.open_app(WINDOW_APP)
    dbg.settle()
    win = dbg.window(WINDOW_APP)
    res.check(f"{WINDOW_APP} window opened", win is not None)
    if win is None:
        return

    if shot_dir:
        qmp.screenshot(os.path.abspath(os.path.join(shot_dir, "sched-gui-before.png")))

    # WHICH pids are already running, before spawning anything. This
    # used to take `launched[0]` and assume it was the process it had
    # just started -- true only while `launched` was empty beforehand,
    # which stopped being true once the app opened above became a RING-3
    # binary and therefore something the WM tracks for reaping. The test
    # then watched the windowed app instead of the spin, and waited 30
    # seconds for a program that is not supposed to exit.
    #
    # Measured while fixing it: the spin really is spawned and really is
    # reaped, in ~7.5s. Nothing was wrong with the scheduler or with
    # wm_reap_launched(); the test was looking at the wrong pid.
    already = set(dbg.json("gui state --json").get("launched", []))

    dbg.send(f"gui spawn {SPIN_PATH}")

    # Wait for the WM to report a NEW live process before sampling, so a
    # slow spawn can't be mistaken for a short one.
    deadline = time.time() + SPAWN_TIMEOUT_S
    pid = 0
    while time.time() < deadline:
        live = set(dbg.json("gui state --json").get("launched", []))
        fresh = live - already
        if fresh:
            pid = min(fresh)
            break
    res.check("the desktop spawned a ring-3 process", pid != 0,
              f"gui state never listed a NEW launched pid within {SPAWN_TIMEOUT_S}s "
              f"(already running: {sorted(already)})")
    if not pid:
        return

    # The measurement. Every iteration is one full round trip THROUGH
    # wm_run(); a sample counts only if the WM still reported a live
    # process when it answered.
    live_samples = 0
    windows_answered = 0
    shot_taken = False
    deadline = time.time() + RUN_TIMEOUT_S
    while time.time() < deadline:
        st = dbg.json("gui state --json")
        if pid not in st.get("launched", []):
            break
        live_samples += 1

        # Not just `gui state`: exercise a second, heavier command that
        # walks real WM structures, so this can't pass on some trivial
        # fast path that skips the loop.
        if dbg.window(WINDOW_APP) is not None:
            windows_answered += 1

        if shot_dir and not shot_taken and live_samples >= 3:
            qmp.screenshot(os.path.abspath(
                os.path.join(shot_dir, "sched-gui-during-process.png")))
            shot_taken = True

    res.check(f"WM answered while a process was live (>= {MIN_LIVE_SAMPLES})",
              live_samples >= MIN_LIVE_SAMPLES,
              f"only {live_samples} samples overlapped a live process")
    res.check("`gui windows` also answered during the run",
              windows_answered >= MIN_LIVE_SAMPLES,
              f"only {windows_answered} of {live_samples}")

    # It has to actually finish -- a WM that stays responsive because
    # the process never ran would otherwise pass everything above.
    still = dbg.json("gui state --json").get("launched", [])
    ended = pid not in still
    res.check("the process finished and the WM reaped it", ended,
              f"the spin is pid {pid}; still listed after {RUN_TIMEOUT_S}s "
              f"(launched now: {sorted(still)}, pre-existing: {sorted(already)})")

    # And the desktop must still be usable afterwards, not merely alive
    # during -- a rotation bug that corrupted the kernel's saved context
    # would most plausibly surface right here.
    dbg.send("gui key 0x1b")  # Esc: dismiss anything the typing opened
    dbg.settle()
    res.check("desktop still responsive after the process exited",
              dbg.window(WINDOW_APP) is not None)

    if shot_dir:
        qmp.screenshot(os.path.abspath(os.path.join(shot_dir, "sched-gui-after.png")))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true",
                    help="the VM already shows the desktop; don't type `gui` first")
    ap.add_argument("--shot", metavar="DIR", help="write proof screenshots here")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "sched_gui_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    if args.shot:
        os.makedirs(args.shot, exist_ok=True)

    dbg = DebugConsole(args.sock)
    res = Result()
    try:
        run(dbg, qmp, args.shot, res)
    finally:
        dbg.close()

    print(f"\nsched_gui_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
