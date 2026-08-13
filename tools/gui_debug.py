#!/usr/bin/env python3
"""tools/gui_debug.py -- talk to toy-os's serial debug console, and to
its `gui` command family in particular (see apps/wm/wm_debug.c).

WHY THIS EXISTS
---------------
GUI testing here used to mean QMP for everything: synthesize a mouse
move, click, screenshot, read the picture. Every coordinate in such a
test was derived by hand from a screenshot and then hardcoded --
gui_flow.py still carries MENU_TOP_Y/ITEM_H constants with a comment
recording that they drifted once and had to be re-measured. The kernel
knows all of those numbers exactly, and `gui menu --json` will just say
them.

So the division of labour is now:

    gui_debug.py  -- state and geometry as FACTS ("where is the window",
                     "what is at this point", "what did the click do"),
                     asserted on directly.
    qmp_test.py   -- pixels, and the real PS/2/keyboard input path.

Injected input (`gui click`, `gui drag`, `gui key`) enters at the WM
loop, BELOW the PS/2 driver. It exercises WM and app logic, not the
mouse driver -- keep QMP for that, and for anything whose answer is
genuinely a picture.

USAGE
-----
    import sys; sys.path.insert(0, "tools")
    from gui_debug import DebugConsole

    dbg = DebugConsole("/path/to/serial.sock")
    print(dbg.send("gui windows"))          # raw text
    wins = dbg.json("gui windows --json")   # parsed dict
    calc = dbg.window("Calculator")         # one window by title
    dbg.send("gui click %d %d" % (x, y))
    dbg.settle()                            # let queued input drain

The VM must already be in GUI mode: `gui` is blocked from `sh` on
purpose (it would try to enter GUI mode from inside the console and
never return), so enter it over QMP first -- gui_flow.GuiFlow's
enter_gui() does that.
"""

import json as _json
import socket
import time

PROMPT = "dbg> "

# Injected events drain one per WM frame at 100Hz, so a click (4 events)
# needs ~40ms and a drag (11) ~110ms. 250ms covers either with room to
# spare; the commands are asynchronous BY DESIGN -- they cannot block,
# because they are dispatched from inside the very loop that drains
# them (see wm_debug.h).
SETTLE_S = 0.25


class DebugConsole:
    def __init__(self, sock_path, timeout=6.0):
        self.sock_path = sock_path
        self.timeout = timeout
        self._s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._s.settimeout(timeout)
        self._s.connect(sock_path)
        self._sync()

    def _read_to_prompt(self):
        buf = ""
        deadline = time.time() + self.timeout
        while time.time() < deadline and not buf.endswith(PROMPT):
            try:
                buf += self._s.recv(65536).decode("utf-8", errors="replace")
            except socket.timeout:
                break
        return buf

    def _sync(self):
        """Land on a fresh prompt. Anything printed before we connected is
        gone (the socket is nowait), so synchronise rather than assume."""
        self._s.sendall(b"\n")
        self._read_to_prompt()

    def send(self, command):
        """Run one debug-console command, return its output as text."""
        self._s.sendall((command + "\n").encode())
        buf = self._read_to_prompt().replace("\r", "")
        if buf.startswith(command):
            buf = buf[len(command):]
        if buf.endswith(PROMPT):
            buf = buf[: -len(PROMPT)]
        return buf.strip("\n")

    def json(self, command):
        """Run a `--json` command and return the parsed object.

        Tolerates the kernel prefixing its own klog lines (`wm: opened
        ...`) by taking the last line that parses -- klog output and
        command output share this wire, and a test asserting on values
        should not break because the kernel logged something."""
        text = self.send(command)
        last_error = None
        for line in reversed([l for l in text.splitlines() if l.strip()]):
            try:
                return _json.loads(line)
            except ValueError as e:
                last_error = e
        raise ValueError(f"no JSON in response to {command!r}: {text!r} ({last_error})")

    # -- conveniences over the raw commands ------------------------------

    def events(self, prefix="uidemo:"):
        """Kernel log lines emitted since the last command, as a list.

        klog output and command output share this wire, and injected
        input is asynchronous -- so the lines an event produces arrive
        AFTER the command that queued it returns, and land in the next
        read. Sending an empty line collects them. click()/drag()/key()
        below already do this and return the result, which is usually
        what you want; call this directly to sweep up anything else."""
        return [l for l in self.send("").splitlines() if l.startswith(prefix)]

    def settle(self, seconds=SETTLE_S):
        """Wait for queued synthetic input to drain. Call after click/drag
        /key before asserting on the result."""
        time.sleep(seconds)

    def windows(self):
        return self.json("gui windows --json")["windows"]

    def window(self, title):
        """The window whose title matches, or None. Titles are unique in
        practice except for multi_instance apps (Calculator), where this
        returns the lowest in z-order -- use windows() directly there."""
        for w in self.windows():
            if w["title"] == title:
                return w
        return None

    def probe(self, x, y):
        return self.json(f"gui probe {x} {y} --json")

    def menu(self):
        return self.json("gui menu --json")

    def taskbar(self):
        return self.json("gui taskbar --json")

    def state(self):
        return self.json("gui state --json")

    def open_app(self, name):
        return self.send(f"gui open {name}")

    def click(self, x, y, settle=True):
        """Click, wait for it to drain, and return the log lines it
        produced -- the assertion a GUI test actually wants."""
        self.send(f"gui click {x} {y}")
        if not settle:
            return []
        self.settle()
        return self.events()

    def drag(self, x0, y0, x1, y1, settle=True):
        self.send(f"gui drag {x0} {y0} {x1} {y1}")
        if not settle:
            return []
        self.settle(SETTLE_S * 2)  # a drag queues ~11 events, not 4
        return self.events()

    def key(self, k, settle=True):
        self.send(f"gui key {k}")
        if not settle:
            return []
        self.settle()
        return self.events()

    def menu_row(self, label):
        """Centre point of the Start menu row with this label, straight
        from the kernel's own geometry -- the thing gui_flow.py's
        hardcoded MENU_TOP_Y/ITEM_H were an approximation of."""
        m = self.menu()
        for row in m["rows"]:
            if row["label"] == label:
                return (m["x"] + m["w"] // 2, row["cy"])
        raise KeyError(f"no Start menu row named {label!r}")

    def close(self):
        self._s.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("socket", help="QEMU serial unix socket (see vm.py's .vm.serial)")
    ap.add_argument("commands", nargs="+", help="debug-console commands, e.g. 'gui windows'")
    args = ap.parse_args()

    with DebugConsole(args.socket) as dbg:
        for c in args.commands:
            if len(args.commands) > 1:
                print(f"--- {c} ---")
            print(dbg.send(c))
