#!/usr/bin/env python3
"""tools/gui_debug.py -- talk to toy-os's serial debug console, and to
its `gui` command family in particular (see userland/wm/wm_debug.c).

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
    dbg.settle()                            # polls until queued input drains

    dbg.damage_verify(True)                 # check the damage invariant
    ... exercise whatever you changed ...
    assert dbg.damage_bugs() == []          # the assertion that matters

settle() POLLS the WM's own queue depth rather than sleeping a guessed
interval -- see the SETTLE_S comment below; the fixed sleep it replaced
raced badly enough to make a damage test report a different bug on each
run. Anything reading the wire keeps what it saw (self.log_lines), so a
kernel log line is never lost to an intervening command.

The VM must already be in GUI mode: `gui` is blocked from `sh` on
purpose (it would try to enter GUI mode from inside the console and
never return), so enter it over QMP first -- gui_flow.GuiFlow's
enter_gui() does that.
"""

import json as _json
import socket
import time

PROMPT = "dbg> "

# settle() POLLS the WM's own injected-event queue (`gui state`'s
# `pending`) rather than sleeping a fixed interval.
#
# It used to sleep 0.25s, reasoning that events drain one per WM frame at
# 100Hz so a click's four need ~40ms and a drag's eleven ~110ms. That
# reasoning has a false premise: the loop is not a 100Hz metronome. A
# drag measured at ~800ms with `gui damage verify on` (which renders
# every frame twice and diffs the whole screen), i.e. ~70ms per event,
# and it will change again with the font size, the window count or the
# display driver. The fixed sleep therefore RACED -- windows moved
# between a test's `gui windows` and the command using those
# coordinates, and the damage exerciser reported a different bug on each
# run of the same script. Anything derived from frame rate is a guess;
# the queue depth is a fact.
#
# The commands remain asynchronous BY DESIGN -- they cannot block,
# because they are dispatched from inside the very loop that drains
# them (see wm_debug.h). Polling from the host side is the way to wait.
SETTLE_S = 0.25       # post-drain grace, and the fallback when polling can't run
SPAWN_TIMEOUT_S = 15.0  # how long spawn() waits for a client's window to appear
RETRIES_ON_SPLICED_JSON = 3  # see json() -- a klog line can land mid-object
FIRST_PRESENT_S = 0.4  # grace after a client's window appears -- see spawn()
SETTLE_TIMEOUT_S = 15.0  # give up rather than hang if the queue never empties


class DebugConsole:
    def __init__(self, sock_path, timeout=6.0):
        self.sock_path = sock_path
        self.timeout = timeout
        self._s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._s.settimeout(timeout)
        self._s.connect(sock_path)
        # Every line the console has emitted, kept because the kernel's
        # own klog shares this wire with command output and a caller
        # asking a question must not destroy an answer to a different
        # one. `wm: DAMAGE BUG` lines were being lost exactly this way:
        # events() filters to the `uidemo:` prefix and dropped them, and
        # settle()'s polling now issues reads of its own that would
        # otherwise consume them. See logs() / damage_bugs().
        self.log_lines = []
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
        out = buf.strip("\n")
        self.log_lines.extend(l for l in out.splitlines() if l.strip())
        return out

    def json(self, command):
        """Run a `--json` command and return the parsed object.

        Tolerates the kernel prefixing its own klog lines (`wm: opened
        ...`) by taking the last line that parses -- klog output and
        command output share this wire, and a test asserting on values
        should not break because the kernel logged something.

        A klog line can also land in the MIDDLE of the JSON line rather
        than before it -- the console has no per-writer buffering, so a
        process exiting while this command is printing splices its
        message straight through the object. Nothing can parse that, so
        the command is simply re-asked: the splice is a collision, not a
        property of the answer. Re-asking rather than trying to unpick
        the fragment keeps this from silently "repairing" genuinely
        malformed output."""
        last_error = None
        for _ in range(RETRIES_ON_SPLICED_JSON):
            text = self.send(command)
            for line in reversed([l for l in text.splitlines() if l.strip()]):
                try:
                    return _json.loads(line)
                except ValueError as e:
                    last_error = e
        raise ValueError(f"no JSON in response to {command!r}: {text!r} ({last_error})")

    # -- conveniences over the raw commands ------------------------------

    def events(self, prefix="uidemo:"):
        """Kernel log lines emitted since the last command, as a list.

        TRAP, and it has bitten twice: this filters the output of the ONE
        command it issues. A line printed while some OTHER command was in
        flight is consumed by that command's read and is invisible here
        -- spawn() polls `gui windows --json` while waiting for a window,
        so an app's own startup lines routinely land in one of those. Use
        logs() when you need everything the app said; it reads send()'s
        accumulated buffer instead. uidemo_test.py errored at startup
        roughly one run in six until it switched.

        klog output and command output share this wire, and injected
        input is asynchronous -- so the lines an event produces arrive
        AFTER the command that queued it returns, and land in the next
        read. Sending an empty line collects them. click()/drag()/key()
        below already do this and return the result, which is usually
        what you want; call this directly to sweep up anything else."""
        return [l for l in self.send("").splitlines() if l.startswith(prefix)]

    def logs(self, match="", clear=True):
        """Every console line seen so far containing `match`, newest last.

        Unlike events(), this does NOT filter to a prefix and does not
        lose lines to an intervening command -- send() accumulates
        everything into self.log_lines. Sweeps the wire first, so a
        message the kernel emitted since the last command is included.
        """
        self.send("")
        hits = [l for l in self.log_lines if match in l]
        if clear:
            self.log_lines = [l for l in self.log_lines if match not in l]
        return hits

    # A verdict the WM itself declared void: the comparison measured
    # nothing, so the line is a note and not a finding. See
    # wm_render_frame()'s verification block for each one's reasoning --
    # the big one is that a client window's content lives in another
    # process's memory and cannot be held still for three renders.
    VOID_VERDICTS = ("verdict void",)

    def damage_bugs(self, clear=True, include_void=False):
        """`wm: DAMAGE BUG` reports seen so far -- the assertion for any
        test run under `gui damage verify on`.

        Assert this is empty after exercising whatever you changed. The
        WM reports each distinct failure once (see wm_render.c), so a
        long exercise yields one line per distinct bug, not per frame.

        VOID VERDICTS ARE EXCLUDED BY DEFAULT, and that is the whole
        difference between a useful signal and a wall of noise: the WM
        prints a line whenever the two renders differ, then says whether
        the difference means anything. Counting the voided ones as
        failures made damage_sweep.py report 22 violations on a desktop
        with no damage bug in it at all. Pass include_void=True to see
        them -- worth doing when a real one is suspected of hiding among
        them.
        """
        hits = self.logs("DAMAGE BUG", clear=clear)
        if include_void:
            return hits
        return [h for h in hits if not any(v in h for v in self.VOID_VERDICTS)]

    def damage_bugs_void(self, clear=False):
        """The reports the WM declared void, for a caller that wants to
        show them without failing on them."""
        return [h for h in self.logs("DAMAGE BUG", clear=clear)
                if any(v in h for v in self.VOID_VERDICTS)]

    def settle(self, seconds=SETTLE_S):
        """Wait for queued synthetic input to actually drain, then pause
        `seconds` for the frame it caused to finish.

        Polls `gui state`'s `pending` count -- the number of injected
        events the WM has not delivered yet -- instead of sleeping a
        guessed interval. See this module's SETTLE_S comment for why the
        guess was wrong and what it broke.

        Falls back to a plain sleep against an older kernel whose `gui
        state --json` predates the `pending` field, so a mismatched
        checkout degrades to the old behaviour rather than erroring.
        """
        deadline = time.time() + SETTLE_TIMEOUT_S
        while time.time() < deadline:
            try:
                pending = self.state().get("pending")
            except (ValueError, KeyError):
                pending = None
            if pending is None:      # kernel without the field -- old behaviour
                time.sleep(seconds)
                return
            if pending == 0:
                break
            time.sleep(0.02)
        else:
            raise TimeoutError(
                f"injected input still pending after {SETTLE_TIMEOUT_S}s -- "
                "is the WM loop still running?")
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

    def processes(self):
        """Every process as a dict: pid, ppid, pgid, state, cpu, name.

        Parsed from `/bin/ps`, which is run through the KERNEL's shell
        over this socket -- a different reader from whatever ring-3 shell
        is under test, so a broken one cannot make this agree with it.

        `state` is the column verbatim (`ready`, `run`, `block(pipe)`,
        `stopped`, `zombie`), and `cpu` is seconds as a float. **`cpu` is
        how you tell a suspended process from an idle one**: a label says
        what the kernel thinks, and only a number that stops advancing
        says the scheduler agrees. Two tools hand-rolled this parser in
        one session before it moved here.

        A ZOMBIE IS INCLUDED. Filter it out for "is it still running"
        questions and ask about it separately for "was everything
        reaped" -- mixing the two makes every count ambiguous between
        them.
        """
        out = self.send("sh ps") or ""
        rows = []
        for line in out.splitlines():
            f = line.split()
            # pid and ppid are the first two columns and both numeric;
            # the name is last, because it is the only column that can
            # contain nothing surprising. Anything else on this socket
            # (kernel log lines, the echoed command) fails that shape.
            if len(f) < 7 or not f[0].isdigit() or not f[1].isdigit():
                continue
            try:
                cpu = float(f[4])
            except ValueError:
                continue
            rows.append({"pid": int(f[0]), "ppid": int(f[1]), "pgid": int(f[2]),
                         "state": f[3], "cpu": cpu, "name": f[-1]})
        return rows

    def processes_named(self, prefix, include_zombies=False):
        """Every process whose name starts with `prefix`. Zombies are
        excluded by default -- see processes()."""
        return [p for p in self.processes()
                if p["name"].startswith(prefix)
                and (include_zombies or p["state"] != "zombie")]

    def probe(self, x, y):
        return self.json(f"gui probe {x} {y} --json")

    def menu(self):
        return self.json("gui menu --json")

    def ctxmenu(self):
        """The open right-click menu's geometry and rows, or open=False.

        Same shape as menu(). Use ctxmenu_row(label) to get a row's
        centre rather than computing one from item_h -- a derived row
        index is exactly what docs/gui-guidelines.md says not to do.
        """
        return self.json("gui ctxmenu --json")

    def ctxmenu_row(self, label):
        """(x, cy) of the context-menu row with this label, or None."""
        m = self.ctxmenu()
        if not m.get("open"):
            return None
        for r in m.get("rows", []):
            if r.get("label") == label:
                return (m["x"] + m["w"] // 2, r["cy"])
        return None

    def taskbar(self):
        return self.json("gui taskbar --json")

    def state(self):
        return self.json("gui state --json")

    def cursor(self):
        """Where the kernel thinks the REAL cursor is, as (x, y).

        The one authoritative answer. QMPSession tracks the cursor
        client-side by accumulating the relative deltas it sent, which is
        all it can do -- there is no absolute-position query in the PS/2
        path -- and that estimate drifts whenever the guest doesn't apply
        a delta in full.
        """
        c = self.state()["cursor"]
        return (c["x"], c["y"])

    def warp_cursor(self, qmp, x, y, tries=8, tol=1):
        """Move the REAL cursor to (x, y) and confirm it arrived.

        Needed because QMPSession.goto() is open-loop: it sends chunked
        relative deltas and then ASSUMES the cursor is where it aimed.
        Measured, a large jump lands roughly a third of the way -- asking
        for (611, 378) from (640, 150) ended up at (630, 226) -- and
        because the client-side estimate was updated anyway, a second
        goto() then sends a zero delta and never corrects. The failure
        mode is a hover test that reports no hover on a control the
        cursor never reached.
        
        So: aim, ask the kernel where it actually got to, and re-aim at
        the remaining error until it's there. Returns the final position.

        Use this for anything that needs the cursor PARKED (hover states,
        pixel probes). `gui move` cannot do that job -- injected input
        overrides the mouse for one WM iteration only, after which the
        real pointer takes over again and the hover is recomputed away.
        """
        for _ in range(tries):
            qmp.goto(x, y)
            self.settle()
            cx, cy = self.cursor()
            if abs(cx - x) <= tol and abs(cy - y) <= tol:
                return (cx, cy)
            # Correct against the truth, not against the estimate.
            qmp.pos[0], qmp.pos[1] = cx, cy
        return self.cursor()

    def open_app(self, name):
        """Open a desktop app by NAME, and RAISE if there is no such app.

        **THE NAME IS THE DESKTOP ENTRY'S, INCLUDING ITS CASE** --
        "Terminal", not "terminal". `gui open` answers a wrong name with
        `no app named "x". Known apps: ...` and returns normally, so a
        caller that ignored the reply got a desktop with no window and a
        test that failed several checks later on something unrelated.
        That cost a real debugging round; raising with the guest's own
        list of known apps turns it into one line.

        The reply is still returned on success, so existing callers that
        read it are unaffected.
        """
        out = self.send(f"gui open {name}") or ""
        if "no app named" in out:
            known = [ln.strip() for ln in out.splitlines() if ln.strip()]
            raise ValueError(f"gui open {name!r} was refused by the guest: "
                             + " | ".join(known[:12]))
        return out

    def spawn(self, path, title=None, timeout=SPAWN_TIMEOUT_S):
        """Run a ring-3 binary and (optionally) wait for its window.

        `gui spawn` with no Terminal in the loop -- which is the only way
        to start a client since Milestone 41's stage 0 retired the
        kernel-space Terminal. Every tool used to `gui open Terminal` and
        type `run <name>` at it; that idiom now opens the RING-3 terminal,
        whose window does not exist yet when the keys arrive, so the keys
        went nowhere and the test failed at its first check.

        Returns the window dict once it appears, or None on timeout (with
        `title` None, returns immediately after spawning).
        """
        self.send(f"gui spawn {path}")
        if title is None:
            return None
        deadline = time.time() + timeout
        while time.time() < deadline:
            win = self.window(title)
            if win is not None:
                # A window in the WM's list is not yet a window with
                # PIXELS: the client still has to draw and present its
                # first frame. A caller that samples immediately reads
                # desktop through the window's rect. Settle, then a short
                # grace -- and if the app reports its own layout line,
                # wait for THAT instead (see uterm_test.py).
                self.settle()
                time.sleep(FIRST_PRESENT_S)
                return win
            time.sleep(0.2)
        return None

    def capture_panic(self, seconds=4.0):
        """Text the guest printed while dying, after something fatal.

        A panicking kernel never returns a prompt, so the ordinary
        command/response cycle cannot complete and every read looks like
        a timeout. What DOES work is sending an empty line and taking
        whatever arrives before the read gives up -- which is exactly
        the panic block.

        Do NOT open a second connection to the serial socket to do this:
        the console already holds it and the new one receives nothing.
        That mistake cost three attempts before this existed.

        Returns the captured text (possibly empty, if nothing panicked).
        Pair it with tools/panic_resolve.py to name the addresses -- or
        just read it, since the kernel bakes a symbol table in now.
        """
        time.sleep(seconds)
        prev = self.timeout
        try:
            self.timeout = seconds
            return self.send("")
        finally:
            self.timeout = prev

    def damage_verify(self, on=True):
        """Turn the damage-invariant checker on/off. Pair with
        damage_bugs() after exercising whatever you changed -- see
        docs/gui-guidelines.md's damage-invariant section.

        Costs a second full render plus a full-screen diff per frame, so
        the WM loop slows by roughly an order of magnitude while it's on.
        settle() polls rather than sleeps precisely so that doesn't
        matter to a test's timing.
        """
        return self.send(f"gui damage verify {'on' if on else 'off'}")

    def move(self, x, y, settle=True):
        """Move the cursor with nothing held, for hover testing.

        click() moves the cursor too, but also presses and releases, so
        the hover state is gone by the time you can look at it. Verifying
        hover by pixel value is what docs/gui-guidelines.md asks for, and
        it needs the cursor parked with no button down.
        """
        self.send(f"gui move {x} {y}")
        if settle:
            self.settle()
        return []

    def click(self, x, y, settle=True):
        """Click, wait for it to drain, and return the log lines it
        produced -- the assertion a GUI test actually wants."""
        self.send(f"gui click {x} {y}")
        if not settle:
            return []
        self.settle()
        return self.events()

    def rclick(self, x, y, settle=True):
        """Right-click -- what opens a context menu. See ctxmenu()."""
        self.send(f"gui rclick {x} {y}")
        if settle:
            self.settle()
        return []

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

    def wheel(self, notches, settle=True):
        """Positive scrolls up (reveals older content), matching
        mouse.h's mouse_get_wheel_delta()."""
        self.send(f"gui wheel {notches}")
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


def wait_for_desktop(sock, timeout=8.0):
    """Block until the ring-3 desktop is answering on the serial console,
    or `timeout` elapses. Returns True if it came up.

    The readiness signal is `gui state` returning a real screen -- the
    WM only answers that once it holds the compositor role. This replaces
    the fixed `time.sleep(2..3)` every GUI tool used after typing "gui":
    init starts the desktop at boot (the graphical target), so it is
    almost always already up by the time a tool connects, and a poll
    returns in one round trip instead of waiting out a worst-case guess.

    Opens a short-lived DebugConsole per poll and closes it before
    returning, so it never overlaps the caller's own console (two on one
    serial socket steal each other's replies -- see capture_panic()).
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with DebugConsole(sock, timeout=1.5) as dbg:
                st = dbg.state()
                if isinstance(st, dict) and st.get("screen"):
                    return True
        except Exception:
            pass
        time.sleep(0.15)
    return False


def enter_gui(qmp, sock=".vm.serial", timeout=8.0):
    """Enter GUI mode and wait for the desktop to actually be up.

    Types "gui" + Enter at the shell (a no-op if init already started the
    desktop, which it does under the graphical target -- the common case)
    and then polls readiness via wait_for_desktop(), rather than sleeping
    a fixed 2-3s. `sock` must be the SAME serial socket the caller's
    DebugConsole will use (pass args.sock). Returns wait_for_desktop()'s
    result; a caller's own settle()/first assertion catches a real
    failure, so this does not raise on timeout.
    """
    qmp.send_text("gui")
    qmp.send_key("ret")
    return wait_for_desktop(sock, timeout)


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
