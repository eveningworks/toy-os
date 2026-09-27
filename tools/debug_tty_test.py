#!/usr/bin/env python3
"""tools/debug_tty_test.py -- the serial debug console is a terminal.

WHAT IS UNDER TEST
------------------
The debug console's port (COM2 on a `vm.py` guest) is a real tty
(`kernel/tty/serial_tty.c`), and every `sh` command is one session on
it (docs/decisions.md, "The kernel log and the debug console are two
serial ports"). Five properties, each with a failure the others miss:

1. **A command's program reads the line.** `sh cat`, then a line typed
   on the port: it comes back twice -- the discipline's echo and cat's
   own output. Before the terminal existed, cat read the physical
   keyboard and this hung.
2. **Ctrl-D ends its input**, and the prompt returns.
3. **Ctrl-C stops a running command**, in the IRQ, while the console
   itself is busy waiting on it. `spin_test` is asked for far longer
   than the check waits, and the prompt is confirmed ABSENT before the
   Ctrl-C, so a command that simply finished cannot pass.
   Two more on the same session: Ctrl-Z does NOT stop a command (it
   would have nobody to continue it -- Ctrl-C still ends it after), and
   a program that went raw and exited without restoring (tosh) does not
   leave the line raw: a command ended by CR still runs.
4. **A job left behind is hung up.** `tosh -c "counter_a &"` returns
   while its background job keeps printing; from the prompt on, the port
   must carry NONE of it (it goes to the VGA console). The job is
   confirmed still running across the quiet window, so its silence means
   something. And `sh spawn` DETACHES: a spawned job's output is not even
   in spawn's own reply (SPAWN_DETACH).
5. **A spawned reader does not take the next command**: `sh spawn
   /bin/cat`, then `sh ps` must run -- a reader holding this terminal
   would eat its line.

POSITIVE CONTROLS, verified: delete the `tty_hangup(g_tty)` call in
kernel/core/debug_console.c's dbg_cmd_sh() -- the left-behind check goes
red; drop `o.flags = SPAWN_DETACH` from userland/bin/spawn.c -- the
detach check goes red; drop `kernel_session` from ldisc.c's
signal_char() -- the Ctrl-C check goes red (the rest cascade, the spin
still holding the console); let SUSP through in a kernel session, or
drop serial_tty_reset() -- the Ctrl-Z or the CR check goes red.

It ATTACHES to a running guest, and types raw bytes at the socket
rather than going through DebugConsole, because it has to speak while a
command is still running.

    python3 tools/vm.py start
    python3 tools/debug_tty_test.py
"""

import argparse
import os
import random
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import port_guard  # noqa: E402

PROMPT = b"dbg> "
SPIN = b"sh spin_test 900000000\n"   # minutes on TCG; Ctrl-C must end it


class Port:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(0.2)
        self.s.connect(path)

    def read(self, seconds, until=None):
        """What arrives in `seconds`, or up to `until` if it shows first."""
        buf = b""
        end = time.time() + seconds
        while time.time() < end:
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                continue
            if not d:
                break
            buf += d
            if until is not None and buf.endswith(until):
                break
        return buf.replace(b"\r", b"").decode("utf-8", "replace")

    def send(self, data, seconds=3.0, until=PROMPT):
        self.s.sendall(data)
        return self.read(seconds, until)


passed = 0
failed = []


def check(name, ok, detail=""):
    global passed
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"    [{detail}]" if detail else ""))
    if ok:
        passed += 1
    else:
        failed.append(name)
    return ok


def kill_all(p, ps, name):
    """Tidy up: end every process called `name` in a `ps` listing."""
    for ln in ps.splitlines():
        cols = ln.split()
        if cols and cols[-1] == name and cols[0].isdigit():
            p.send(f"sh kill {cols[0]}\n".encode(), 3.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "debug_tty_test")

    print("debug console as a terminal")
    p = Port(args.sock)
    p.send(b"\n")   # land on a fresh prompt

    # 1 and 2: a program reads the port, and Ctrl-D ends it.
    probe = f"tty-probe-{random.randrange(10**6)}"
    p.send(b"sh cat\n", 1.0, until=None)
    out = p.send(probe.encode() + b"\n", 1.5, until=None)
    check("a command's program reads a line typed on the port",
          out.count(probe) == 2, repr(out[-120:]))
    out = p.send(b"\x04", 3.0)
    check("Ctrl-D ends its input and the prompt returns",
          out.endswith(PROMPT.decode()), repr(out[-80:]))

    # 3: Ctrl-C while the console is busy running the command.
    early = p.send(SPIN, 1.5)
    running = not early.endswith(PROMPT.decode())
    check("the spinning command is still running before Ctrl-C", running,
          repr(early[-80:]))
    out = p.send(b"\x03", 4.0)
    check("Ctrl-C stops it and the prompt returns",
          running and out.endswith(PROMPT.decode()) and "130" in out,
          repr(out[-80:]))

    # 3b: Ctrl-Z is NOT a stop here -- a stopped command has nobody to
    # continue it, and the console would wait on it for ever. The
    # recovery that proves it: Ctrl-C still ends the job afterwards.
    p.send(SPIN, 1.5)
    p.send(b"\x1a", 1.0, until=None)
    out = p.send(b"\x03", 4.0)
    check("Ctrl-Z does not strand a command (Ctrl-C still ends it)",
          out.endswith(PROMPT.decode()) and "130" in out, repr(out[-80:]))

    # 3c: a program that goes raw and exits without restoring does not
    # leave the line raw: tosh does exactly that. A command ended by CR --
    # what a terminal emulator's Enter sends -- must still run after it.
    p.send(b"sh tosh\n", 2.0, until=None)
    p.send(b"\x04", 4.0)   # Ctrl-D on an empty line: tosh exits
    out = p.send(b"sh echo cr-ok\r", 4.0)
    check("the line is back to canonical after a raw-mode program",
          "cr-ok" in out.replace("sh echo cr-ok", ""), repr(out[-80:]))

    # 4: a job LEFT BEHIND by a command -- a shell's background job,
    # which inherited the terminal -- is hung up when the command
    # returns: from the prompt on, not one byte of it reaches the port.
    # ANY byte counts, because counter_a writes a bare 'A' with no
    # newline and a leak glues onto whatever comes next.
    p.send(b"sh tosh -c \"/tests/counter_a &\"\n", 4.0)
    quiet = p.read(2.5)
    ps = p.send(b"sh ps\n", 3.0)
    alive = "counter_a" in ps
    check("the left-behind job is still running after the quiet window", alive,
          "counter_a in ps" if alive else "it exited too soon to tell")
    check("a left-behind job's output stays off the port once the prompt is back",
          alive and quiet == "", f"{len(quiet)} byte(s) on the port: {quiet[:40]!r}")
    kill_all(p, ps, "counter_a")

    # 4b: `sh spawn` DETACHES (SPAWN_DETACH): the job never has this
    # terminal, so none of its output is in spawn's own reply either.
    out = p.send(b"sh spawn /tests/counter_a\n", 3.0)
    tail = out.split("started as pid", 1)[-1]
    check("a spawned job starts on the machine console, not this terminal",
          "started as pid" in out and "A" not in tail.split("\n", 1)[-1],
          repr(out[-80:]))
    kill_all(p, p.send(b"sh ps\n", 3.0), "counter_a")

    # 5: a spawned READER does not take the next command's line. The
    # next command is `ps` itself: a reader holding this terminal would
    # eat it, and there would be no listing at all.
    out = p.send(b"sh spawn /bin/cat\n", 3.0)
    time.sleep(0.5)
    ps = p.send(b"sh ps\n", 3.0)
    check("a spawned reader does not take the next command's line",
          "PID" in ps, "ps ran" if "PID" in ps else "ps never ran -- its line was eaten")
    kill_all(p, ps, "cat")

    total = passed + len(failed)
    print(f"\ndebug_tty_test: {passed} passed, {len(failed)} failed (of {total})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
