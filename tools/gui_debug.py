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
import os
import atexit
import socket
import sys
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
    # POINTED AT THE LAPTOP WHEN `TOYOS_REMOTE_HOST` NAMES ONE, which
    # `gui_regress.py --host` sets for its children. The remote object is
    # a SUBCLASS (tools/remote_gui.py) that replaces the transport and
    # nothing else, so a tool reaches real hardware without an edit of
    # its own -- and the alternative, a factory call in every tool, was
    # fifty chances to forget one. It says on stderr where it is pointed.
    def __new__(cls, *args, **kwargs):
        if cls is DebugConsole and os.environ.get("TOYOS_REMOTE_HOST"):
            from remote_gui import RemoteConsole
            return super().__new__(RemoteConsole)
        return super().__new__(cls)

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
        self._stale = False   # a reply timed out; resync before the next
        self._resyncs = 0
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

    def reconnect(self, timeout=90.0):
        """Re-open this console after the GUEST went away.

        A test that proves something PERSISTED has to reboot the
        machine, and the socket dies with it -- every later command on
        this object then fails with `BrokenPipeError`, in whatever check
        happened to be next rather than at the reboot. So the console
        re-attaches to the same socket and waits for a prompt, and the
        object a test is holding keeps working.
        """
        try:
            self._s.close()
        except OSError:
            pass
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                self._s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self._s.settimeout(self.timeout)
                self._s.connect(self.sock_path)
                self.log_lines = []
                self._stale = False
                self._sync()
                return True
            except OSError as e:
                last = e
                time.sleep(2)
        print(f"gui_debug: could not re-attach to {self.sock_path}: {last}",
              file=sys.stderr)
        return False

    def send(self, command):
        """Run one debug-console command, return its output as text.

        A COMMAND THAT TIMES OUT LEAVES ITS OUTPUT AND PROMPT IN FLIGHT,
        and reading to "the next prompt" then handed them to the NEXT
        command -- every reply after it shifted by one for the rest of
        the run (measured: `echo C` answered `BBB`). Counting owed
        prompts is not enough, because a guest still booting can drop a
        typed line and never print its prompt at all. So a timeout marks
        the console out of step, and the next command first resyncs on a
        marker nothing else can print (_resync()).
        """
        if self._stale:
            self._resync()
        self._s.sendall((command + "\n").encode())
        buf = self._read_to_prompt().replace("\r", "")
        if not buf.endswith(PROMPT):
            self._stale = True
        if buf.startswith(command):
            buf = buf[len(command):]
        if buf.endswith(PROMPT):
            buf = buf[: -len(PROMPT)]
        out = buf.strip("\n")
        self.log_lines.extend(l for l in out.splitlines() if l.strip())
        return out

    def _resync(self):
        """Discard everything up to the answer to a unique unknown command.

        The console echoes what is typed, so the marker is matched in the
        REPLY (`unknown command: <tag>`), followed by a prompt. What is
        read on the way -- late replies, boot chatter -- goes to
        log_lines, where klog lines are expected to be.
        """
        self._resyncs += 1
        tag = f"__resync_{self._resyncs}__"
        want = f"unknown command: {tag}"
        # The leading newline ends any line the guest is half-way through
        # holding, so the marker arrives as a line of its own.
        self._s.sendall(("\n" + tag + "\n").encode())
        buf = ""
        deadline = time.time() + 2 * self.timeout
        while time.time() < deadline:
            i = buf.find(want)
            if i >= 0 and buf.find(PROMPT, i) >= 0:
                self._stale = False
                break
            try:
                buf += self._s.recv(65536).decode("utf-8", errors="replace")
            except socket.timeout:
                break
        self.log_lines.extend(l for l in buf.replace("\r", "").splitlines()
                              if l.strip() and tag not in l)

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

    def write_lines(self, path, lines, tries=25):
        """Replace `path` in the guest with `lines`; True once all landed.

        The only writer a test outside the guest has is `tosh -c` with a
        redirection, and it has two traps. UNQUOTED: the kernel shell's
        `spawn` passes a quoted word through WITH its quotes, so tosh
        runs a command literally named "echo panes=2 > ..." and nothing
        is written. And ONE LINE AT A TIME, each confirmed on disk before
        the next: `spawn` returns at once, so a `>` and a `>>` sent back
        to back can land in either order, and the truncating one landing
        last wipes the line before it.
        """
        self.send(f"sh rm {path}")
        for i, line in enumerate(lines):
            self.send(f"sh spawn /bin/tosh -c echo {line} "
                      f"{'>' if i == 0 else '>>'} {path}")
            want = lines[:i + 1]
            for _ in range(tries):
                got = [ln.strip() for ln in (self.send(f"sh cat {path}") or "").splitlines()]
                if all(w in got for w in want):
                    break
                time.sleep(0.2)
            else:
                return False
        return True

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
                st = self.state()
                pending = st.get("pending")
                # A window animation in flight is motion the WM itself
                # asked for; a frame compared while one runs is a frame
                # of a ghost. Absent on an older WM, and then ignored.
                anims = st.get("anims", 0)
            except (ValueError, KeyError):
                pending, anims = None, 0
            if pending is None:      # kernel without the field -- old behaviour
                time.sleep(seconds)
                return
            if pending == 0 and anims == 0:
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

    def widgets(self, title=""):
        """The client's named controls, keyed by name.

        Each value carries the CONTENT-relative rect the client reported
        plus `screen`, the same rect where a click goes -- so a caller
        never does the window-origin arithmetic that is the usual source
        of a test clicking 24 pixels off after a window moved.

        The map comes from the CLIENT (abi/win_proto.h, WIN_REQ_WIDGET):
        the compositor cannot see inside a window, so a client that
        reports nothing -- a kernel-space app, one with no NAMED items --
        answers {} rather than failing. `title` picks a window; the
        default is the frontmost.
        """
        j = self.json(f"gui widgets {title} --json".replace("  ", " "))
        return {w["name"]: w for w in j.get("widgets", [])}

    def widget_at(self, x, y):
        """The name of the control at a screen point, or None."""
        return self.probe(x, y).get("widget")

    def widget_center(self, name, title=""):
        """Screen (x, y) to click for a named control.

        Raises rather than returning a plausible-looking (0, 0): a click
        at the origin lands on the window's corner and the test fails
        somewhere else entirely.
        """
        w = self.widgets(title).get(name)
        if not w:
            have = ", ".join(sorted(self.widgets(title))) or "(none reported)"
            raise KeyError(f"no widget named {name!r}; this window has: {have}")
        return w["screen"]["x"] + w["w"] // 2, w["screen"]["y"] + w["h"] // 2

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

    # WM_CURSOR_* (userland/wm/wm_internal.h): 0 normal, 1 resize-h,
    # 2 resize-v, 3 diagonal, 4 text, 5 wait, 6 the other diagonal.
    # DIAG is the \ axis (top-left/bottom-right), DIAG2 the /.
    (CURSOR_NORMAL, CURSOR_H, CURSOR_V, CURSOR_DIAG,
     CURSOR_TEXT, CURSOR_WAIT, CURSOR_DIAG2) = range(7)

    def cursor_shape(self):
        """The shape the compositor would DRAW under the pointer right
        now -- the frame's edge rules and the client's WIN_REQ_CURSOR
        already resolved against each other. The alternative is
        recognising a 15x21 sprite in a screenshot."""
        return self.state()["cursor"].get("shape", 0)

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
        # `gui warp` IF THE GUEST HAS IT: the kernel puts the pointer
        # there and answers with where it landed, so one round trip
        # replaces the aim-measure-correct loop below -- and it is the
        # only way that works on real hardware, where there is no
        # emulator input layer to inject through. The reply is the
        # DRIVER's position, clamped, not an echo of the request.
        reply = self.send(f"gui warp {x} {y}") or ""
        for line in reply.splitlines():
            if "pointer at (" in line:
                got = line.split("pointer at (", 1)[1].split(")", 1)[0]
                try:
                    cx, cy = (int(v) for v in got.split(","))
                except ValueError:
                    break
                # TELL THE EMULATOR WHERE THE POINTER WENT. QMP moves
                # the cursor in RELATIVE deltas from its own estimate,
                # so a warp the kernel performed leaves that estimate
                # stale and the NEXT click_at() aims from the wrong
                # origin -- measured: osk_test's tray click missed and
                # the panel never opened, while the warp itself was
                # perfect. The old aim-and-correct loop kept the two in
                # step by construction; this has to say so.
                try:
                    qmp.pos[0], qmp.pos[1] = cx, cy
                except (AttributeError, TypeError):
                    pass
                self.settle()   # let the compositor take the raw event
                return (cx, cy)

        # An older guest: aim through the emulator, ask where it got to,
        # re-aim at the remaining error. QMPSession.goto() is open-loop
        # and a large jump lands roughly a third of the way.
        for _ in range(tries):
            qmp.goto(x, y)
            self.settle()
            cx, cy = self.cursor()
            if abs(cx - x) <= tol and abs(cy - y) <= tol:
                return (cx, cy)
            # Correct against the truth, not against the estimate.
            qmp.pos[0], qmp.pos[1] = cx, cy
        return self.cursor()

    def drag_real(self, qmp, x0, y0, x1, y1, steps=4):
        """Press, move, release with the REAL pointer, one confirmed
        warp per step -- the only drag a ring-3 CLIENT actually sees.

        `gui drag` and QMPSession.drag() both fail here, for two
        different reasons, and both fail SILENTLY as "the app ignored
        the drag":

        * `gui drag` queues injected positions the WM consumes one per
          iteration, and between them it reads the real mouse again --
          so the client gets a leave event and no held motion at all.
        * QMPSession.drag() sends its moves faster than the guest draws
          frames under TCG, so the WM sees ONE position change: the
          press and the release land on the same pixel and the client
          is never told anything moved. Measured: a 152px drag showed
          `mx` identical on every frame from press to release.

        A kernel-side app does not notice either problem, because the
        WM calls its on_press every tick with the current position --
        which is why scrollbar_test passes on `gui drag` and a client
        test cannot. Only a CLIENT needs this.

        warp_cursor() is what makes each step land: goto() is open-loop
        and the WM accelerates the delta, so an unconfirmed move ends up
        roughly a third of the way there.
        """
        self.warp_cursor(qmp, x0, y0)
        qmp.mouse_down()
        self.settle()
        for i in range(1, steps + 1):
            self.warp_cursor(qmp, x0 + (x1 - x0) * i // steps,
                             y0 + (y1 - y0) * i // steps)
        qmp.mouse_up()
        self.settle()
        return self.cursor()

    def warp_confirmed(self, qmp, x, y, check, tries=6, settle=0.25):
        """Warp to (x, y) and confirm the APP agrees what is under it.

        warp_cursor() above confirms the cursor's POSITION, which is a
        different claim: it proves the pointer is where you aimed, not
        that where you aimed is what you meant. A list row is one line
        tall, so a y computed from a stale origin, a scrolled view or a
        column header lands on a neighbouring row -- perfectly plausible,
        and the reason the failure reads as "the click did nothing"
        rather than "the click hit the wrong thing".

        `check` is a callable returning truthy when the app REPORTS the
        intended target under the pointer (its hovered row, its hot
        button). Returns True once it does, False if it never did -- and
        a caller that ignores the return value is back to guessing, so
        assert on it.

        Costs one extra layout read per try, which is cheaper than one
        wrong-row failure is to diagnose.
        """
        for _ in range(tries):
            self.warp_cursor(qmp, x, y)
            time.sleep(settle)
            if check():
                return True
        return False

    def hover_frames(self, qmp, tmp, rest_at, hover_at, prefix="hover"):
        """Two SETTLED frames of the same screen: pointer parked away
        from the control, then on it. Returns (rest_png, hover_png).

        THREE THINGS THIS PACKAGES, each of which has cost a run:

        1. **The REAL cursor, warped and confirmed.** `gui move` is one
           WM iteration -- the injected position overrides the mouse for
           the single wm_run() pass that consumes it, and the next pass
           reads the driver again and snaps back. It is exactly right
           for a click (press and release are edges) and useless for
           hover, which has to persist while a capture is taken. A tool
           that hovers with `gui move` photographs the frame AFTER the
           pointer left, and reports a working hover as dead.
        2. **Settled frames.** A capture landing mid-paint fails a
           comparison that has nothing wrong with it -- and with the WM
           in ring 3 the client's repaint is an extra process hop whose
           timing varies with load. See QMPSession.stable_pixels().
        3. **A rest frame with the pointer somewhere real.** The cursor
           sprite is part of the screen, so a "rest" capture taken
           before the pointer moved at all differs from the hover
           capture by the sprite as well as by the wash.

        The CALLER still owns where to sample: park the pointer at one
        end of the control and measure the other, or the sprite's own
        pixels answer the question instead of the hover state. See
        changed_rows() for the band form, and dialog_test.py for the
        two-points form with the neighbouring control as its control.
        """
        import os
        self.warp_cursor(qmp, *rest_at)
        rest = os.path.abspath(os.path.join(tmp, f"{prefix}-rest.png"))
        qmp.stable_pixels(rest)
        self.warp_cursor(qmp, *hover_at)
        hot = os.path.abspath(os.path.join(tmp, f"{prefix}-on.png"))
        qmp.stable_pixels(hot)
        return rest, hot

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

    def drag(self, x0, y0, x1, y1, settle=True, steps=0):
        """`steps` is how many intermediate positions the WM sees.

        It is NOT a duration: the queue is drained one position per WM
        iteration and the loop spins while input is pending, so 48 steps
        take about as long as the default 8 (~150 ms). Ask for more when
        a hit region could be stepped over; nothing here can make a
        scripted drag LAST, which is why a test about what happens during
        one has to ask the WM for a count rather than watch."""
        self.send(f"gui drag {x0} {y0} {x1} {y1}" + (f" {steps}" if steps else ""))
        if not settle:
            return []
        self.settle(SETTLE_S * 2)  # a drag queues ~11 events, not 4
        return self.events()

    def key(self, k, settle=True, mods=""):
        # `mods` is the wm_debug.c word list -- "ctrl", "shift ctrl".
        # It sets the KEY_MOD_* bits the WM delivers ALONGSIDE the key
        # and does not re-encode it, which is what a real keyboard does.
        self.send(f"gui key {k} {mods}".rstrip())
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
        from the WM's own geometry -- the thing gui_flow.py's hardcoded
        MENU_TOP_Y/ITEM_H were an approximation of.

        The menu has TWO COLUMNS now, so the row carries its own centre
        x; a caller that takes the menu's own centre lands on the
        category sidebar for half the rows.

        Only the rows the menu currently DRAWS are here -- the folders,
        the system actions, the open folder's apps. For an app in some
        other folder use menu_app_row(), which opens its folder first.
        """
        m = self.menu()
        for row in m["rows"]:
            if row["label"] == label:
                return (row.get("cx", m["x"] + m["w"] // 2), row["cy"])
        raise KeyError(f"no Start menu row named {label!r}")

    def menu_apps(self):
        """Every app the Start menu can show, as {label: folder label}.

        Independent of which folder is open -- `gui menu --json` reports
        this beside the drawn rows exactly so a test can ask "where does
        this app live" without clicking around to find out.
        """
        return {a["label"]: a["cat"] for a in self.menu().get("apps", [])}

    def menu_select_folder(self, label, settle=True):
        """Clicks the category sidebar row with this label."""
        x, y = self.menu_row(label)
        self.click(x, y)
        if settle:
            self.settle()

    def menu_app_row(self, label, settle=True):
        """Centre point of an app's row, OPENING ITS FOLDER first if the
        app is not currently shown.

        Written because the obvious menu_row(app) silently stopped
        finding most apps the day the menu grew folders: a row that is
        not drawn has no geometry, and a KeyError at that point reads as
        "the app is gone" rather than "it is one click away".
        """
        try:
            return self.menu_row(label)
        except KeyError:
            pass
        folder = self.menu_apps().get(label)
        if folder is None:
            raise KeyError(f"no Start menu app named {label!r}")
        self.menu_select_folder(folder, settle=settle)
        return self.menu_row(label)

    def close(self):
        self._s.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def changed_rows(rest_png, hover_png, box, threshold=1.0):
    """Which pixel ROWS inside `box` differ between two frames.

    For a control whose rows have no reported geometry -- a dropdown's
    popup, a listbox -- where "the row under the pointer lit up" has to
    be measured rather than assumed. `box` is a PIL crop box in SCREEN
    coordinates, (x0, y0, x1, y1); rows are compared as the mean
    brightness across it, so a row's text contributes to both frames
    equally and only a background wash moves the number.

    Returns {"rows": [y, ...], "band": (first, last) or None,
             "rest": {y: mean}, "hover": {y: mean}}.

    THE CONTROL IS THE BAND, not the change. "Something got darker"
    passes on a repaint, a scroll, or a whole list highlighting at once;
    what a working hover looks like is ONE band, containing the pointer,
    no taller than a row. Assert that, not merely that `rows` is
    non-empty.
    """
    from PIL import Image
    x0, y0, x1, y1 = box
    a = Image.open(rest_png).convert("RGB")
    b = Image.open(hover_png).convert("RGB")

    def means(im):
        out = {}
        for y in range(y0, min(y1, im.height)):
            px = list(im.crop((x0, y, x1, y + 1)).getdata())
            out[y] = sum(sum(p) for p in px) / (3.0 * len(px))
        return out

    rest, hover = means(a), means(b)
    rows = [y for y in rest if abs(hover[y] - rest[y]) > threshold]
    return {"rows": sorted(rows),
            "band": (min(rows), max(rows)) if rows else None,
            "rest": rest, "hover": hover}


# **AND PUT IT BACK.** This used to set the setting and walk away, which
# costs nothing on a VM whose image is thrown away -- and permanently
# enables it on the BARE-METAL machine, where `gui_regress.py --host`
# runs these same tools. Found 2026-09-19: the laptop had been logging
# every widget of every frame for an unknown number of boots, which
# turned one boot's persistent log into 183 KiB of layout lines (11x its
# neighbours) and left a mangled line in it that read as a subsystem
# named "f". CLAUDE.md's own rule: a test that applies a setting changes
# the machine for every later tool.
#
# Restored only if this process is what turned it ON -- a machine that
# already had it set was told to, and is not ours to change back.
def _enable_layout_log(sock):
    c = DebugConsole(sock)
    was_on = "on" in c.send("sh config get desktop.layout_log")
    c.send("sh config set desktop.layout_log on")
    if was_on:
        return
    def _restore():
        try:
            DebugConsole(sock).send("sh config set desktop.layout_log off")
        except Exception:
            pass
    atexit.register(_restore)


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

    ASKS FIRST, AND TYPES NOTHING WHEN THE DESKTOP IS ALREADY UP -- which
    it almost always is, since init starts it under the graphical target.
    The keystrokes are not free when they are unnecessary: with no window
    focused they are QUEUED, and the WM delivers them to the first client
    window that opens. The File Manager caught this by being the first
    app here to act on a bare Enter -- it received the "i" and the Enter
    from someone else typing "gui" seconds earlier, treated the Enter as
    "activate the .. row", and opened one directory above where it had
    been told to start. Every other tool was unaffected only because its
    app ignores a stray key.

    Only when the desktop is NOT up does this type "gui" + Enter at the
    shell, then poll readiness via wait_for_desktop() rather than
    sleeping a fixed 2-3s. `sock` must be the SAME serial socket the
    caller's DebugConsole will use (pass args.sock). Returns
    wait_for_desktop()'s result; a caller's own settle()/first assertion
    catches a real failure, so this does not raise on timeout.
    """
    # **TURN THE LAYOUT LOG ON, for every tool at once.** Toykit apps
    # report their widget geometry so a test can drive them by asking
    # rather than by guessing pixels -- and that report is OFF by
    # default (`desktop.layout_log`), because it is written every frame
    # and made `dmesg` unreadable on any machine with a window open.
    #
    # Here rather than in each tool because an app reads the setting
    # ONCE when it starts, so it has to be on before anything is
    # launched -- which is exactly what this function is for. Doing it
    # per tool would mean every new tool rediscovering why its layout
    # poll times out.
    if os.environ.get("TOYOS_REMOTE_HOST"):
        # ON HARDWARE THERE IS NOTHING TO TYPE AT. `gui` is a command at
        # the kernel's serial console, and the laptop has no serial
        # console in use -- which is the whole reason `guictl` exists.
        # The desktop is init's under the graphical target, so this asks
        # and reports rather than trying to start one.
        ready = wait_for_desktop(sock, timeout)
        if not ready:
            print("remote: no desktop is answering on "
                  f"{os.environ['TOYOS_REMOTE_HOST']} -- is it at a shell prompt, "
                  "or mid-boot?", file=sys.stderr)
    else:
        ready = wait_for_desktop(sock, timeout=1.0)
        if not ready:
            qmp.send_text("gui")
            qmp.send_key("ret")
            ready = wait_for_desktop(sock, timeout)
    try:
        _enable_layout_log(sock)
    except Exception:
        # A tool that cannot reach the console has bigger problems than
        # the layout log, and its own first assertion will say so more
        # usefully than an exception from here would.
        pass
    return ready


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
