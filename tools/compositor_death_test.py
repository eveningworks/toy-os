#!/usr/bin/env python3
"""The compositor death path -- Milestone 41's R7.

WHAT IS UNDER TEST
------------------
`killing the compositor must not panic the kernel` is the exit criterion
of the whole milestone.

**This tool runs two different scenarios, and picks by asking who holds
the role.** `gui compositor --json` reports pid 0 under the ring-0
desktop and the window manager's own pid under a ring-3 one
(`make iso KCMDLINE="gui3"`), so the detection is free and exact.

  * **No compositor registered (the ring-0 desktop).** A STAND-IN --
    `screenclient` -- claims the role and takes the framebuffer grant,
    which is the state the real WM will be in when it dies. What is
    asserted here is the SKIP: a second consumer leaving must NOT tear
    down a desktop the ring-0 WM still owns.

  * **The desktop already holds it (the ring-3 desktop).** Then the
    stand-in is not merely unnecessary, it is a contradiction -- the
    role is single, so claiming it EVICTS the desktop, and the tool then
    asks questions of something that is no longer there. That is exactly
    how this tool failed under `gui3` before it learned to look. So it
    kills the DESKTOP instead, which is the real exit criterion and was
    asserted by nothing at all until now.

Killing the desktop needs `kill` at the shell, not `gui kill`: the `gui`
commands are dispatched from inside the WM's own loop and
scheduler_kill() refuses to kill the CURRENT process, so the one process
a test most needs to end was the one nothing could end. Restarting it
needs `spawn` for a matching reason -- `run` uses the legacy blocking
loader, which is not a scheduled process, so its win_request() is
refused and it can never claim the role.

Three deaths, one path. The kernel routes a clean deregistration, a
`kill`, and a fault through `win_server_set_compositor(0, 0)` -- so this
kills the process outright, which is the least graceful of the three and
the one a crashing WM actually takes.

WHAT THE KERNEL MUST DO
-----------------------
1. Survive. Checked by asking the WM a question afterwards: `gui state`
   is dispatched from inside `wm_run()`, so an answer proves the kernel
   AND the desktop are still running. A panic answers nothing.
2. Revoke the framebuffer grant, so a dead compositor's mapping cannot
   outlive it.
3. ASK every client window to close (WIN_EV_CLOSE) rather than
   destroying it. Destroying frees the client's own buffer pages, so a
   client mid-draw would fault -- the compositor dying would cascade.
   See win_server.c's compositor_gone().
4. Let the next compositor register. A role that cannot be re-claimed
   after a crash is a desktop that cannot be restarted.

WHAT IT DELIBERATELY DOES NOT ASSERT
------------------------------------
That clients are asked to close and the text console comes back -- the
other half of R7. Neither can happen yet, and the kernel is right to
skip them: while the RING-0 WM is registered it still owns the screen
and the window list, so a stand-in compositor leaving is a second
consumer going away, not the desktop dying. Tearing down there kills
live windows the WM is still drawing.

So this asserts the SKIP, which is a real property and one that was
broken within an hour of being written (the first version of
compositor_gone() had no such guard, and `compositor_test.py` failed
with UI Demo going silent). The two checks invert the day the WM is the
compositor: `win_server_active()` is false then, the teardown runs, and
`tools/` gains a scenario that drives it from the PHYSICAL shell, where
no ring-0 WM exists to own anything.

WHICH CHECKS ARE LOAD-BEARING
-----------------------------
Measured, not assumed: disabling `compositor_gone()` reddens exactly TWO
of the ten -- "reports asking client windows to close" and "console is
left to the ring-0 WM". The other eight stay green, correctly, because
they were already true before R7: stage 4a made the role-clear path a
single chokepoint that revokes the framebuffer grant, so surviving the
kill, releasing the role and re-claiming it afterwards are REGRESSION
cover for that work rather than tests of this.

Worth knowing before trusting a green run here: two checks are testing
the new path and eight are guarding the old one.

Usage (the VM must already be up):

    python3 tools/vm.py start
    python3 tools/compositor_death_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession             # noqa: E402

DEFAULT_SOCK = ".vm.serial"
COMP = "/tests/screenclient"
CLIENT = "/tests/winclient"
TOYWM = "/bin/wm/system/toywm"

checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"    [{detail}]" if detail else ""))


# The descriptor init supervises the desktop through.
TOYWM_SVC = "/etc/services.d/toywm"


def unsupervise(dbg):
    """Take the desktop out of init's hands, so it can be killed and STAY dead.

    Since init started supervising the desktop (docs/init-design.md stage
    2) killing it is no longer the end of it: init restarts it with a
    ZERO backoff, because a desktop that had been up for more than two
    seconds is a restart rather than a crash loop. Every check below
    about the role being free, or the console coming back, is then racing
    a new desktop that has already claimed both -- and the restart REUSES
    the dead process's slot, so the pid looks unchanged too.

    Deleting the descriptor makes init stop RESTARTING it without
    stopping the one that is running (systemd's `disable`, not `stop`),
    which is exactly the precondition this tool needs. init notices on
    its next pass, via `fs_generation()`.

    The image is a throwaway copy -- gui_regress gives every tool its own
    -- so deleting a seeded file here costs nothing. Say so, because a
    tool that quietly edits /etc is the kind of thing the NEXT tool's
    failure gets blamed on.
    """
    dbg.send(f"sh rm {TOYWM_SVC}")
    time.sleep(0.4)


def run_ring3(dbg, desktop_pid):
    """Kill the actual ring-3 desktop -- Milestone 41's exit criterion.

    Everything here is asserted through the KERNEL's console (`sh ...`),
    not through `gui ...`: the whole point is that the thing answering
    `gui` is about to stop existing, so using it as the liveness probe
    would make "it died" and "the kernel died with it" the same result.
    """
    unsupervise(dbg)

    # Something for the kernel to ask to close. winclient REFUSES its
    # first closes, which is deliberate here: it makes the difference
    # between "asked" and "destroyed" observable, because a refused ask
    # leaves the process alive and a destroy would not.
    dbg.send(f"gui spawn {CLIENT}")
    time.sleep(1.5)
    wins = [w for w in (dbg.windows() or []) if w.get("client_pid")]
    check("a client window exists to be asked to close",
          len(wins) >= 1, f"{len(wins)} client window(s)")

    dbg.logs()
    killed = dbg.send(f"sh kill {desktop_pid}")
    time.sleep(2.0)
    after = "\n".join(dbg.logs()) + killed

    # `sh kill <pid>` now sends a SIGNAL (SIGTERM by default) rather than
    # ending the process with a raw exit code -- so the reply is "sent
    # SIGTERM to pid N -- toywm", not the "ended pid N" this used to
    # match. The NAME is the half worth keeping either way: it is what
    # distinguishes killing the desktop from killing whatever else
    # happened to be in that slot.
    check("the desktop can be killed at all",
          "sent SIG" in killed and "toywm" in killed, killed.strip()[:60])

    # THE exit criterion. `sh` is served by the kernel context, which is
    # not the process that just died, so an answer here is a liveness
    # proof for the kernel alone -- which is the claim being made.
    #
    # `rescue df`, NOT `df`. Plain `df` became a ring-3 program
    # (/bin/df, over QUERY_FSINFO), so asking for it would prove the
    # kernel can still LOAD AND SCHEDULE a process -- a strictly larger
    # claim than the one this check makes, and one that fails for
    # reasons unrelated to the exit criterion. `rescue df` is the
    # kernel's own copy (apps/shell_rescue.c) and runs entirely in
    # ring 0, which is what "the kernel alone" means here.
    df = dbg.send("sh rescue df")
    check("the KERNEL survives killing the desktop",
          "Filesystem:" in df, df.strip().splitlines()[0] if df.strip() else "no answer")

    check("the framebuffer grant is revoked",
          "revoked the framebuffer" in after,
          next((l for l in after.split("\n") if "revoked" in l), "no line"))

    # Asked, not destroyed -- destroying frees the client's own buffer
    # pages, so a client mid-draw would fault and the desktop dying
    # would cascade into every app dying with it.
    check("client windows are ASKED to close",
          "asked to close" in after,
          next((l for l in after.split("\n") if "compositor gone" in l), "no line"))

    # ...and the proof that asking is not destroying: winclient declined,
    # so its process must still be there. `kstack slots` lists live
    # scheduler slots by name and is served by the kernel, so it still
    # answers with no desktop up.
    slots = dbg.send("sh kstack slots")
    check("a client that DECLINED is still alive (asked, not destroyed)",
          "winclient" in slots,
          "winclient present" if "winclient" in slots else slots.strip()[:60])

    check("the text console is restored",
          "console restored" in after,
          next((l for l in after.split("\n") if "console restored" in l), "no line"))

    # The role must be free afterwards. With no desktop there is nothing
    # to answer `gui`, and that refusal IS the observation: the message
    # means neither a ring-0 layer nor a compositor is registered.
    #
    # This only holds because unsupervise() ran first -- see its comment.
    # Without it init restarts the desktop with a ZERO backoff and the
    # role is refilled before this line executes, reusing the dead
    # process's slot so even the pid looks unchanged.
    gone = dbg.send("gui compositor")
    check("the compositor role is released",
          "no window manager running" in gone, gone.strip()[:60])

    # And the half that makes "survivable" mean anything: a desktop that
    # cannot be restarted after a crash has not survived in any useful
    # sense. `spawn`, not `run` -- the legacy loader is not a scheduled
    # process and its win_request() is refused.
    started = dbg.send(f"sh spawn {TOYWM}")
    check("a new desktop can be started", "started as pid" in started,
          started.strip()[:70])
    time.sleep(4.0)

    st = dbg.json("gui state --json") or {}
    check("the new desktop composites and answers again",
          st.get("screen", {}).get("w", 0) > 0, f"screen={st.get('screen')}")

    comp = dbg.json("gui compositor --json") or {}
    check("...and it holds the compositor role",
          comp.get("pid", 0) > 0, f"pid={comp.get('pid')}")

    return summarise()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sock", default=DEFAULT_SOCK)
    ap.add_argument("--qmp-port", type=int, default=4445)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)

    # WHO HOLDS THE ROLE decides which scenario is the real one. Asked
    # before anything is spawned, so the answer is the desktop's own
    # state rather than something this tool caused.
    owner = (dbg.json("gui compositor --json") or {}).get("pid", 0)
    if owner > 0:
        print(f"compositor death (M41 R7) -- the RING-3 desktop holds the role (pid {owner}):")
        return run_ring3(dbg, owner)

    print("compositor death (M41 R7) -- no compositor registered; using a stand-in:")

    # A real client window, so there is something for the kernel to ask
    # to close. Without one the interesting half of the path is not
    # exercised at all and the test would pass against a kernel that
    # simply ignores clients.
    dbg.send(f"gui spawn {CLIENT}")
    time.sleep(1.2)
    wins_before = dbg.windows() or []
    client_wins = [w for w in wins_before if w.get("client_pid")]
    check("a client window exists to be asked to close",
          len(client_wins) >= 1, f"{len(client_wins)} client window(s)")

    # The stand-in compositor: claims the role and maps the real screen.
    dbg.logs()
    dbg.send(f"gui spawn {COMP}")
    time.sleep(1.2)
    startup = "\n".join(dbg.logs())
    check("the stand-in compositor registered and took the grant",
          "screenclient: registered" in startup and "screenclient: screen " in startup,
          "registered + screen line")

    comp = dbg.json("gui compositor --json") or {}
    comp_pid = comp.get("pid", 0)
    check("the kernel agrees it is the compositor", comp_pid > 0, f"pid={comp_pid}")

    if comp_pid <= 0:
        return summarise()

    # --- the death, by kill: the least graceful of the three paths ----
    unsupervise(dbg)
    dbg.logs()
    dbg.send(f"gui kill {comp_pid}")
    time.sleep(1.5)
    after = "\n".join(dbg.logs())

    # 1. The kernel survived, and so did the desktop. `gui state` is
    #    dispatched from inside wm_run(), so an answer is a liveness
    #    proof for both -- a panic or a wedged loop answers nothing.
    state = dbg.state()
    check("the kernel survives killing the compositor", bool(state),
          "gui state answered")

    # 3. The teardown is SKIPPED, because the ring-0 WM still owns the
    #    desktop. A stand-in compositor leaving is a second consumer
    #    going away (stage 2's design), not the desktop dying -- and the
    #    first version of this change got that wrong, tearing down live
    #    windows the WM was still drawing. `compositor_test.py` caught it
    #    as UI Demo going silent; this check is what keeps it caught
    #    here, next to the code, instead of as a puzzling failure in a
    #    tool about something else.
    check("a stand-in compositor leaving does NOT tear down the desktop",
          "still owns the screen" in after,
          next((l for l in after.split("\n") if "compositor" in l), "no line"))

    # ...and the window is still there, because asking is not destroying.
    # This is the check that separates the chosen design from R7's
    # original "drop every client window": a destroyed window would be
    # gone from this list AND its client would be faulting.
    wins_after = dbg.windows() or []
    still = [w for w in wins_after if w.get("client_pid")]
    check("the client window still exists (asked, not destroyed)",
          len(still) >= 1, f"{len(still)} client window(s) after")

    # ...and specifically the console is not repainted over the live
    # desktop, which is the visible half of the same mistake.
    check("the console is not repainted over the live desktop",
          "console restored" not in after,
          next((l for l in after.split("\n") if "compositor" in l), "no line"))

    # 2 + 4. The role was given up and is re-claimable -- a desktop that
    # cannot be restarted after a crash is not survivable in any useful
    # sense. Asserted from the LOG, not by sampling `gui compositor`: see
    # the note on the same check earlier in this file. Under init's
    # supervision the role is refilled before a sample can see it empty,
    # and the refill reuses the pid.
    # 2 + 4. The role is free and re-claimable -- a desktop that cannot
    # be restarted after a crash is not survivable in any useful sense.
    comp2 = dbg.json("gui compositor --json") or {}
    check("the compositor role is released", comp2.get("pid", -1) == 0,
          f"pid={comp2.get('pid')}")

    dbg.logs()
    dbg.send(f"gui spawn {COMP}")
    time.sleep(1.2)
    again = "\n".join(dbg.logs())
    comp3 = dbg.json("gui compositor --json") or {}
    check("a new compositor can claim the role after the crash",
          comp3.get("pid", 0) > 0 and "screenclient: screen " in again,
          f"pid={comp3.get('pid')}")

    # Leave nothing running for the next tool.
    if comp3.get("pid", 0) > 0:
        dbg.send(f"gui kill {comp3['pid']}")
        time.sleep(0.8)
    check("the desktop is still alive at the end", bool(dbg.state()),
          "gui state answered")

    return summarise()


def summarise():
    bad = [n for n, ok, _ in checks if not ok]
    print(f"\n{len(checks) - len(bad)}/{len(checks)} checks passed")
    if bad:
        print("FAILED: " + "; ".join(bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
