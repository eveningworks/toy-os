#!/usr/bin/env python3
"""Blocking stdin (fd 0) and the standalone ring-3 shell, /bin/tosh.

WHAT IS UNDER TEST
------------------
A ring-3 process reading the physical console. Until fd 0 could be read
(kernel/tty/tty_fd.c's console_read) the only way into ring
3 from the keyboard was the non-blocking SYS_READ_KEY, so a shell would
have had to spin-poll the keyboard forever -- which is why
docs/decisions.md recorded that there was no /bin/tosh yet.

Three properties, and each has a distinct failure that the others would
not catch:

1. **A typed line reaches a ring-3 shell and runs.** Asserted through
   the FILESYSTEM, not through the screen: `file_test` (a /tests ELF on
   tosh's PATH) writes /filetest.txt, and the debug console can see
   whether it appeared. That is a round trip -- keystroke to key ring to
   a parked process's trapframe to a parsed command line to a spawn --
   and nothing short of the whole path satisfies it. Reading the console
   instead would need OCR and would prove less.

2. **The reader BLOCKS rather than spins.** `kstack slots` reports the
   scheduler state per slot; an idle tosh must be 4 (SCHED_BLOCKED). A
   spin-poll implementation passes check 1 perfectly and fails this one,
   which is the whole difference between this change and what
   SYS_READ_KEY already allowed.

3. **The console has exactly ONE reader, and the claim is released.**
   `rescue touch` is KERNEL-SHELL-ONLY, so typing it at tosh must create
   NOTHING -- if the kernel shell were still taking keys behind it, the
   file would appear. Then Ctrl-D exits tosh and the same line must now
   work, which is the release half. Without both directions "the claim
   works" and "the claim is stuck on" look identical.

THE TOSH UNDER TEST IS INIT'S CONSOLE SHELL, the `tosh` service, which
owns the console once the desktop is gone. It is DISABLED first (its
descriptor moved aside, as the desktop's is) so Ctrl-D ends it for good
-- with `Restart=always` init would start another, and the kernel shell
would never get the keyboard back. Spawning a second tosh, as this tool
once did, typed `spawn /bin/tosh` INTO the console tosh and then counted
whichever tosh it found.

PRECONDITIONS THIS TOOL ESTABLISHES ITSELF
------------------------------------------
It puts the guest on the US keyboard layout (`sh keyboard us`) before
typing anything. A QMP qcode names a PHYSICAL key by its US label, so on
this OS's `se` default every `/` this tool types arrives as `-` --
`spawn /bin/tosh` became `spawn -bin-tosh`, and the substring assertions
passed against a file genuinely called `-claimprobe.txt`. `kbd=us` on
the GRUB line is the same override for a boot nobody can type at.

The desktop owns the keyboard while it is up, so this needs the physical
console back. It takes the desktop out of init's hands the documented
way -- `rm /etc/services.d/toywm`, which is systemd's `disable` and not
`stop`, so init stops RESTARTING it -- and then kills it, exactly as
compositor_death_test.py does and for the same reason: a supervised
desktop comes straight back with a zero backoff and reclaims the
console before any check can see it gone.

It therefore EDITS /etc on the image it runs against. Run it against a
throwaway copy of disk.img, which is what gui_regress.py hands every
tool anyway.

WHICH CHECKS ARE LOAD-BEARING
-----------------------------
Measured with positive controls, not assumed -- see the tool's own
--positive-control note in the commit message. Removing the
scheduler_wake(SCHED_WAIT_KEY) call in keyboard.c's ring_push() reddens
check 1 (tosh parks on the first read and never wakes). Removing the
keyboard_claim_console() call reddens check 3a. Neither reddens the
other.

Usage (the VM must already be up):

    python3 tools/vm.py start
    python3 tools/stdin_test.py
    python3 tools/vm.py stop
"""

import argparse
import atexit
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole          # noqa: E402
from shell_flow import ShellFlow            # noqa: E402
from harness import Results  # noqa: E402

DEFAULT_SOCK = ".vm.serial"
TOYWM_SVC = "/etc/services.d/toywm"
# Where the descriptor waits while the test runs: on DISK, so it outlives
# a crash of this run, and put back at the end. The image is shared with
# every later tool in a sweep, and a desktop that never comes back fails
# them all with `no provider named gui` (2026-09-30).
TOYWM_SAVED = "/var/tmp/toywm.service.saved"
TOSH_SVC = "/etc/services.d/tosh"
TOSH_SAVED = "/var/tmp/tosh.service.saved"


def restore_desktop(dbg):
    dbg.send(f"sh cp {TOSH_SAVED} {TOSH_SVC}")
    dbg.send(f"sh cp {TOYWM_SAVED} {TOYWM_SVC}")
    dbg.send("sh sync")   # the sweep stops the guest straight after
PROBE = "/claimprobe.txt"
MADE = "/filetest.txt"



_res = Results()
check = _res.check
checks = _res.rows


def root_names(dbg):
    """The names in `/`, as a SET of exact entries.

    Exact, not a substring search over the raw reply: the first version
    of this tool asked `"claimprobe.txt" in output` and passed against a
    file actually called `-claimprobe.txt`, created because the harness
    was typing `-` for every `/` (see shell_flow.py's mapping comment).
    A substring check cannot tell a bug in the harness from the property
    it is asserting.
    """
    out = dbg.send("sh ls /") or ""
    names = set()
    for line in out.splitlines():
        for tok in line.split():
            if tok.endswith("/") or "." in tok:
                names.add(tok.rstrip("/"))
    return names


def kstack_slots(dbg):
    """Rows of `kstack slots` as (pid, name, state) -- the one kernel
    command that reports a scheduler state over the serial console. `ps`
    cannot serve: it is a /bin program, so its output goes to the screen
    the desktop or tosh owns, not to this socket."""
    out = dbg.send("sh kstack slots") or ""
    rows = []
    for line in out.splitlines():
        parts = line.split()
        # slot pid name state krsp ...
        if len(parts) >= 4 and parts[0].isdigit() and parts[1].isdigit() and parts[3].isdigit():
            rows.append((int(parts[1]), parts[2], int(parts[3])))
    return rows


def spend_first_line(flow):
    """A harmless line for the console to lose -- see main()."""
    flow.type_command("x")
    flow.session.send_key("ret")
    time.sleep(1.0)


def free_the_console(dbg):
    """Stop init supervising the desktop, then kill it -- see this
    module's docstring for why both steps are needed."""
    comp = dbg.json("gui compositor --json") or {}
    pid = comp.get("pid") or 0
    dbg.send(f"sh cp {TOYWM_SVC} {TOYWM_SAVED}")    # put back by restore_desktop()
    dbg.send(f"sh rm {TOYWM_SVC}")
    # The console shell keeps running, but is not restarted when it exits.
    dbg.send(f"sh cp {TOSH_SVC} {TOSH_SAVED}")
    dbg.send(f"sh rm {TOSH_SVC}")
    time.sleep(0.5)
    if pid:
        dbg.send(f"sh kill {pid}")
    # Generous: the desktop's teardown restores the text console and
    # clears the compositor role, and a line typed before that lands
    # nowhere -- which reads exactly like the shell being deaf.
    time.sleep(3.0)
    return pid


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sock", default=DEFAULT_SOCK)
    ap.add_argument("--port", type=int, default=4445)
    args = ap.parse_args()

    dbg = DebugConsole(args.sock)
    flow = ShellFlow(qmp_port=args.port)

    print("preconditions")
    # A QMP qcode names a PHYSICAL key by its US-layout label, and this
    # OS defaults to `se`, where that key produces something else --
    # every `/` in this tool arrived as `-` until this line existed, and
    # the substring assertions passed anyway. `kbd=us` on the GRUB line
    # does the same thing for an ISO nobody can type at
    # (docs/boot-flags.md); over a live console this is the cheap way.
    # It PERSISTS to /etc, which is fine for the throwaway image this
    # tool already deletes a service descriptor from.
    dbg.send("sh keyboard us")
    time.sleep(0.4)

    pid = free_the_console(dbg)
    atexit.register(restore_desktop, dbg)   # every exit path, a crash of this script included
    check("the desktop was found and killed, freeing the console", bool(pid),
          f"toywm pid {pid}")

    # Clear both fixtures. `make iso` re-seeds by SYNC, never reformat,
    # so an earlier run's files are still there and every check below
    # would be satisfied by them rather than by anything this run did.
    dbg.send(f"sh rm {MADE}")
    dbg.send(f"sh rm {PROBE}")
    time.sleep(0.4)
    before = root_names(dbg)
    check("fixture is clean -- neither probe file exists",
          "filetest.txt" not in before and "claimprobe.txt" not in before,
          " ".join(sorted(before)))

    print("the ring-3 shell")
    rows = kstack_slots(dbg)
    tosh = [r for r in rows if r[1].startswith("tosh")]
    mine = tosh[0][0] if len(tosh) == 1 else None
    check("the console tosh is running as a ring-3 process, alone", mine is not None,
          f"{tosh}" if tosh else f"slots: {rows}")

    # 4 == SCHED_BLOCKED (kernel/proc/sched_internal.h's enum sched_state).
    check("an idle tosh is BLOCKED, not spinning", bool(tosh) and tosh[0][2] == 4,
          f"state {tosh[0][2]}" if tosh else "no tosh row")

    print("a typed line reaches it")
    # THE FIRST LINE AFTER A CONSOLE HANDOVER IS LOST (docs/bugs.md), so
    # one is spent here and after Ctrl-D -- the spawn line this tool used
    # to type absorbed it unseen.
    spend_first_line(flow)
    flow.type_command("file_test")
    flow.session.send_key("ret")
    time.sleep(2.5)
    after = root_names(dbg)
    check("a command typed at tosh ran -- filetest.txt appeared",
          "filetest.txt" in after, " ".join(sorted(after)))

    print("the console has one reader")
    # `rescue touch` is a KERNEL-SHELL-ONLY command -- the kernel's own
    # copies of the file commands live behind that one name -- so tosh
    # answers "not found" and nothing is created, UNLESS the kernel shell
    # is still reading the same keyboard.
    #
    # **IT WAS A BARE `touch` AND THE PREMISE HAD GONE STALE.** touch
    # became a /bin program, so tosh found it on PATH, ran it, and left
    # the probe file behind -- which reads as "the kernel shell is still
    # listening", i.e. exactly the failure this check exists to catch,
    # with nothing wrong. The identical defect was found and fixed in
    # console_shell_test.py (eaa7529); this tool is run on demand, so it
    # kept the stale version for longer. `rescue` is the fix because no
    # /bin program can shadow it.
    flow.type_command("rescue touch /claimprobe.txt")
    flow.session.send_key("ret")
    time.sleep(1.5)
    mid = root_names(dbg)
    check("the kernel shell did not also execute the line",
          "claimprobe.txt" not in mid, " ".join(sorted(mid)))

    # Ctrl-D exits tosh, which releases the claim through fd_release_all().
    flow.session.combo(["ctrl", "d"])
    time.sleep(1.5)
    rows = kstack_slots(dbg)
    check("tosh exited on Ctrl-D",
          mine is not None and not [r for r in rows if r[0] == mine and r[2] not in (0, 3)],
          f"slots: {rows}")

    spend_first_line(flow)
    flow.type_command("rescue touch /claimprobe.txt")
    flow.session.send_key("ret")
    time.sleep(1.5)
    end = root_names(dbg)
    check("the kernel shell has the keyboard back",
          "claimprobe.txt" in end, " ".join(sorted(end)))

    failed = [n for n, ok, _ in checks if not ok]
    print(f"\n{len(checks) - len(failed)}/{len(checks)} passed")
    if failed:
        print("failed: " + ", ".join(failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
