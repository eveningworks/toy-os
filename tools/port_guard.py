"""Refuse to start a guest on a QMP or VNC port another guest already holds.

THE BUG THIS EXISTS FOR, and it cost three separate "failures" in one
session before anyone noticed the pattern:

    python3 tools/gui_regress.py &        # slots 0-3 -> QMP 4445-4448
    python3 tools/console_shell_test.py   # also defaults to QMP 4445

Everything here defaults to port 4445, and nothing stopped a second
launch. QEMU's `-qmp tcp:...,server,nowait` binds happily enough for the
FIRST guest, and the second tool then connects to the FIRST guest's
monitor -- or the first tool's session gets its socket pulled out from
under it. The symptom is not "port in use". It is a `BrokenPipeError`,
`ConnectionResetError` or a bare timeout minutes later, reported against
whichever tool happened to be mid-command:

    FAIL  scrollbar    12s  BrokenPipeError: [Errno 32] Broken pipe
    FAIL  uapp          7s  ConnectionResetError: [Errno 104] ...

Nothing in that names a port, so it reads exactly like a flake or a real
regression in the tool it lands on -- and the tool it lands on is
whichever one was unlucky, never the one that caused it. That is the
worst property a failure can have: it accuses the innocent.

The check is a BIND, not a connect. A connect only sees a monitor that
already has QEMU listening AND accepting; binding sees any listener,
including one whose single QMP connection is already taken (QEMU's
monitor accepts one client, so a second connect can hang rather than
refuse). Binding to the same address is what QEMU itself is about to do,
so this asks the exact question that is about to be answered anyway --
just early, and with a message that says what to do about it.

Set TOYOS_ALLOW_PORT_CLASH=1 to bypass it deliberately.
"""

import os
import socket
import sys
import time

BYPASS_ENV = "TOYOS_ALLOW_PORT_CLASH"


def port_is_free(port, host="127.0.0.1"):
    """True if nothing is listening on host:port.

    SO_REUSEADDR is deliberately NOT set. It would let this bind succeed
    beside an existing listener in some states, which is the opposite of
    the question being asked.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind((host, port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def _owner_hint():
    """Which of this repo's own launchers is probably holding it.

    Best-effort and deliberately vague: the point is to remind a person
    what they left running, not to be a process table. vm.py's pidfiles
    are the one thing we can read without shelling out.
    """
    running = []
    for name in sorted(os.listdir(".")):
        if name.startswith(".vm") and name.endswith(".pid"):
            try:
                with open(name) as f:
                    pid = f.read().strip()
                running.append(f"{name} (pid {pid})")
            except OSError:
                pass
    return running


def assert_ports_free(qmp_port, vnc_display=None, wait_s=6.0):
    """Refuse to launch if either port is taken. Raises SystemExit.

    WAITS BRIEFLY FIRST, which is not politeness -- it is correctness.
    A guest that has just been asked to stop holds its listening socket
    for a moment while it exits, so two back-to-back runs of
    gui_regress.py can see the previous run's ports still bound. The
    first version refused outright there and turned a clean sequential
    pair of runs into eighteen "Connection refused" failures: a guard
    that fires on a transient is worse than no guard, because it
    produces exactly the confusing downstream error it exists to
    prevent. A real clash lasts; a shutdown does not.
    """
    if os.environ.get(BYPASS_ENV) == "1":
        return

    deadline = time.time() + wait_s
    while time.time() < deadline:
        if port_is_free(qmp_port) and (vnc_display is None
                                       or port_is_free(5900 + int(vnc_display))):
            return
        time.sleep(0.25)

    clashes = []
    if not port_is_free(qmp_port):
        clashes.append(f"QMP port {qmp_port}")
    # VNC display N listens on 5900+N. A clash here does not break a
    # test the way a QMP clash does -- nothing asserts through VNC --
    # but QEMU refuses to start at all, which is a confusing failure of
    # its own.
    if vnc_display is not None and not port_is_free(5900 + int(vnc_display)):
        clashes.append(f"VNC display :{vnc_display} (port {5900 + int(vnc_display)})")

    if not clashes:
        return

    print(f"port_guard: REFUSING to start a guest -- {' and '.join(clashes)} "
          "already in use.", file=sys.stderr)
    print("", file=sys.stderr)
    print("  Another guest is running. Two tools sharing a QMP port do not", file=sys.stderr)
    print("  fail here -- they fail minutes later as a BrokenPipeError in", file=sys.stderr)
    print("  whichever one was mid-command, which is why this refuses now.", file=sys.stderr)
    print("", file=sys.stderr)
    for r in _owner_hint():
        print(f"  running: {r}", file=sys.stderr)
    print("  (gui_regress.py holds slots 0..DEFAULT_JOBS-1 while it runs --", file=sys.stderr)
    print("   up to 8, i.e. QMP 4445-4452 -- so don't hand-pick a low slot.)", file=sys.stderr)
    print("", file=sys.stderr)
    print("  Wait for it, stop it (vm.py stop), or let a free slot be picked:", file=sys.stderr)
    print("    python3 tools/<tool>.py --instance auto   # probes for the lowest free slot", file=sys.stderr)
    print("    (or launch_qemu_cmd(..., qmp_port=<free>) via find_free_instance())", file=sys.stderr)
    print(f"  {BYPASS_ENV}=1 bypasses this deliberately.", file=sys.stderr)
    raise SystemExit(2)


def find_free_instance(count=16, base_qmp=4445, base_vnc=5):
    """The lowest slot number whose QMP and VNC ports are both free, or
    None if every one of `count` slots is taken.

    Slots are `vm.py --instance N`'s numbering: QMP 4445+N, VNC :5+N.

    NOT A LOCK, and the distinction matters. Two callers picking "the
    lowest free slot" in the same instant get the same answer -- there
    is a bind/close race between probing a port and QEMU binding it,
    which is exactly why vm.py's own comment argued against probing in
    the first place. This narrows the window rather than closing it;
    assert_ports_free() at the actual launch is what catches the
    residue, and it is the half that must not be skipped.

    A caller that uses this must PRINT the slot it got. The other half
    of vm.py's objection was reproducibility -- a port that differs per
    run makes a failure harder to replay -- and reporting the number
    answers it: the re-run is `--instance <that number>`.
    """
    for n in range(count):
        if port_is_free(base_qmp + n) and port_is_free(5900 + base_vnc + n):
            return n
    return None


if __name__ == "__main__":
    # Usable on its own: `python3 tools/port_guard.py 4445` exits 0 when
    # that port is free.
    p = int(sys.argv[1]) if len(sys.argv) > 1 else 4445
    assert_ports_free(p)
    print(f"port_guard: QMP port {p} is free.")
