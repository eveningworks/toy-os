#!/usr/bin/env python3
"""Reusable helpers for driving toy-os in headless QEMU over QMP.

Every GUI-testing session up to this point has re-derived the same few
gotchas from scratch (usually by hitting them first). This module exists
so that doesn't have to keep happening. It's a dev tool, not part of the
OS -- nothing here ships in the kernel or gets compiled by the Makefile.

Gotchas this module already gets right for you:

- The mouse driver in this kernel is PS/2 (relative deltas), not USB
  HID -- there's no USB stack in toy-os at all. Don't reach for
  -device usb-tablet OR -device usb-mouse, and don't try absolute
  positioning -- it's the wrong device class for this guest, and worse,
  adding an explicit USB pointer device makes QEMU route host mouse
  motion to THAT instead of the emulated PS/2 mouse, so the guest
  receives nothing at all (looks exactly like "the mouse doesn't work"
  but is actually a launch-flag mistake -- this bit a real interactive
  session once, see CHANGELOG.md's build-293-adjacent Makefile fix).
  Leave the pointer device unspecified; `click()`/`goto()` below use
  `input-send-event` with `rel` axis events against the default
  emulated PS/2 mouse, tracking the cursor position client-side since
  there's no absolute-position query.
- **Cursor position drifts across separate `QMPSession` instances that
  share an already-open GUI session.** Each new session assumes the
  cursor starts at `cursor_start` (default (640, 360)) without ever
  querying the guest's real position. If a *previous* session already
  entered GUI mode and left it open (didn't press Esc back to the
  shell) and moved the mouse since, the real cursor is somewhere else,
  and every `goto()` in the new session is offset by that error --
  clicks land on the wrong thing. This looks exactly like a UI bug (a
  click "not registering") but isn't. Fix: call `session.recalibrate()`
  right after connecting, in any script that ISN'T entering GUI mode
  itself via a fresh "gui" + Enter this session -- see
  `recalibrate()`'s own docstring for why scripts that DO enter GUI
  mode fresh don't need it (the kernel resets the cursor to screen
  center deterministically on every GUI entry, matching the default
  `cursor_start`) and, importantly, why `recalibrate()` must be called
  AFTER "gui" + Enter if a script does both, never before.
- Do NOT launch QEMU with `-display none` if you need mouse input to
  work. It disables the display head entirely, and `input-send-event`
  then silently no-ops -- it still returns `{"return": {}}` (looks like
  success) but nothing reaches the guest. Use `-vga std -vnc :N`
  instead; a VNC head satisfies input routing even with no client
  ever connecting to it.
- Use `-serial file:/path/to/serial.log`, not `-serial stdio`, for most
  testing (so kernel boot/test output can just be tailed from a file).
  Note some userland tests (e.g. `echotest`) block forever reading
  from the serial port when it's a bare file with nothing on the other
  end -- that's this testing setup's limitation, not a kernel bug, if
  a test hangs at "calling process_run_ring3()" with no further output.
- Background QEMU with `setsid` (see `launch_qemu()` / the shell
  equivalent below), not a plain `&` -- a bare background job tied to
  one shell invocation gets killed when that invocation ends. `setsid`
  detaches it so it survives across separate tool/shell calls.
- **After changing any shared header (`widgets.h`, `gui_apps.h`, etc.),
  `make clean && make all` before testing, not a plain `make all`.**
  The Makefile doesn't track header dependencies (see its `version:`
  target comment), so a stale `.o` compiled against the old struct
  layout can silently desync from other, freshly-rebuilt `.o`s that
  see the new one -- e.g. an array indexed with the wrong stride. This
  produced genuinely bizarre-looking corruption once (Start menu items
  showing raw function-prologue bytes as text) that a clean rebuild
  fixed instantly. If a GUI test shows something inexplicable right
  after a header change, suspect this before suspecting the new code.

Typical usage from a Python REPL or script, once QEMU is already
running (see `launch_qemu_cmd()` for the command to start it with):

    from qmp_test import QMPSession

    qmp = QMPSession()                  # connects to 127.0.0.1:4445
    qmp.send_text("gui")                # type "gui" + Enter at the shell
    qmp.send_key("ret")
    time.sleep(1)                       # let wm_run()/mouse_init() actually start
    # qmp.recalibrate()                 # only needed when REUSING an already-open
                                         # GUI session from a previous script -- see
                                         # the cursor-drift gotcha and recalibrate()'s
                                         # own docstring for the full ordering rule
    qmp.goto(44, 706)                   # move cursor to the Start button
    qmp.click()
    qmp.screenshot("start_menu.png")    # screendump -> ppm -> png in one call

No third-party dependencies beyond Pillow (only needed for
`screenshot()`'s ppm->png conversion -- `pip install pillow
--break-system-packages` if it's missing).
"""

import json
import socket
import time


def launch_qemu_cmd(iso="toy-os.iso", disk="disk.img", serial_log="serial.log",
                     qmp_port=4445, vnc_display=5):
    """Return the shell command to launch toy-os headlessly with QMP + a
    working input head. Run this with setsid so it survives across
    separate tool calls, e.g.:

        (setsid nohup <this string> > qemu.log 2>&1 < /dev/null &) ; sleep 1

    Deliberately no -display none (see module docstring) -- -vnc gives
    a display head without needing an actual VNC client to connect.
    """
    return (
        f"qemu-system-x86_64 -cdrom {iso} -drive file={disk},format=raw,if=ide "
        f"-vga std -m 256 -serial file:{serial_log} "
        f"-qmp tcp:127.0.0.1:{qmp_port},server,nowait -vnc :{vnc_display}"
    )


class QMPSession:
    """A connected, capabilities-negotiated QMP socket with mouse/keyboard/
    screenshot helpers layered on top. One instance per QEMU process.
    """

    def __init__(self, host="127.0.0.1", port=4445, cursor_start=(640, 360),
                 connect_timeout=5.0, connect_retries=10, retry_delay=0.5):
        last_err = None
        for _ in range(connect_retries):
            try:
                self._sock = socket.create_connection((host, port), timeout=connect_timeout)
                break
            except OSError as e:
                last_err = e
                time.sleep(retry_delay)
        else:
            raise RuntimeError(f"could not connect to QMP at {host}:{port}: {last_err}")

        self._buf = b""
        self._recv_json()  # greeting
        self._cmd({"execute": "qmp_capabilities"})
        self.pos = list(cursor_start)  # tracked client-side -- see module docstring

    # -- low-level ------------------------------------------------------

    def _recv_json(self):
        while b"\n" not in self._buf:
            chunk = self._sock.recv(4096)
            if not chunk:
                break
            self._buf += chunk
        line, _, self._buf = self._buf.partition(b"\n")
        return json.loads(line)

    def _cmd(self, obj):
        self._sock.sendall((json.dumps(obj) + "\n").encode())
        return self._recv_json()

    # -- keyboard ---------------------------------------------------------

    def send_key(self, qcode):
        """Send one key by QMP qcode name, e.g. 'ret', 'esc', 'backspace',
        or a single lowercase letter/digit."""
        return self._cmd({
            "execute": "send-key",
            "arguments": {"keys": [{"type": "qcode", "data": qcode}]},
        })

    def send_text(self, text, delay=0.03):
        """Type each character of `text` as its own key (letters/digits
        only -- for space use send_key('spc'), for punctuation use the
        matching qcode name). Does not send Enter; call send_key('ret')
        after if needed."""
        for ch in text:
            self.send_key(ch)
            time.sleep(delay)

    # -- mouse ------------------------------------------------------------

    def move_rel(self, dx, dy):
        events = []
        if dx:
            events.append({"type": "rel", "data": {"axis": "x", "value": dx}})
        if dy:
            events.append({"type": "rel", "data": {"axis": "y", "value": dy}})
        if events:
            self._cmd({"execute": "input-send-event", "arguments": {"events": events}})

    # Max |dx|/|dy| per single move_rel() call. The PS/2 relative-mouse
    # protocol this kernel's driver speaks encodes each packet's delta as
    # a signed byte (-128..127); a single input-send-event with a bigger
    # value than that gets silently truncated/wrapped by QEMU's PS/2
    # emulation, so a big one-shot goto() (e.g. from the default (640,360)
    # start position to a taskbar button) can land far from the intended
    # target. goto() below chunks into steps this size to stay safe.
    _MAX_STEP = 100

    def goto(self, x, y, settle=0.2, step_settle=0.02):
        """Move the cursor to an absolute (x, y) by sending the relative
        delta from the last known position, broken into <= _MAX_STEP
        chunks (see its comment for why -- a single large delta is not
        reliable). Position is tracked client side (see class docstring)
        -- if the guest ever moves the cursor on its own (it doesn't, in
        this WM), self.pos would drift."""
        total_dx, total_dy = x - self.pos[0], y - self.pos[1]
        steps = max(1, (max(abs(total_dx), abs(total_dy)) + self._MAX_STEP - 1) // self._MAX_STEP)
        cur_x, cur_y = self.pos[0], self.pos[1]
        for i in range(1, steps + 1):
            nx = x if i == steps else self.pos[0] + total_dx * i // steps
            ny = y if i == steps else self.pos[1] + total_dy * i // steps
            self.move_rel(nx - cur_x, ny - cur_y)
            cur_x, cur_y = nx, ny
            time.sleep(step_settle)
        self.pos[0], self.pos[1] = x, y
        time.sleep(settle)

    def recalibrate(self, steps=30, step_settle=0.01, settle=0.1):
        """Fixes the cross-session cursor-drift gotcha (see module
        docstring): drives the REAL cursor to the top-left screen
        corner with enough chunked negative movement to guarantee it
        clamps there regardless of where a previous session left it,
        then tells this session's client-side tracker the truth (pos =
        (0, 0)). Call this once, in any script that isn't provably the
        first QMPSession against a freshly launched QEMU process.
        `steps` * 100px of guaranteed travel (the default 30 -> 3000px)
        should clamp from anywhere on any screen resolution this
        project uses; raise it if you ever use a bigger one.

        ORDERING MATTERS: call this AFTER entering GUI mode (after
        sending "gui" + Enter and a short settle), never before. The
        kernel's PS/2 aux mouse device isn't enabled until
        mouse_init() runs (see mouse.c), which happens inside
        wm_run() when GUI mode starts -- relative motion sent earlier
        than that has nothing listening on the other end and is lost.
        mouse_init() also unconditionally resets the cursor to screen
        center (bound_w/2, bound_h/2) every time GUI mode is
        (re-)entered, which is *why* QMPSession's default
        `cursor_start` is (640, 360) -- that's the exact 1280x720
        center this kernel boots into. So: if your script enters GUI
        mode itself via a fresh "gui" + Enter, you don't strictly need
        recalibrate() at all (the default already matches). You DO
        need it when connecting to a GUI session a previous script
        already left open (no fresh mouse_init() to reset anything) --
        call it right after connecting, before any goto()/click().
        """
        for _ in range(steps):
            self.move_rel(-100, -100)
            time.sleep(step_settle)
        self.pos[0], self.pos[1] = 0, 0
        time.sleep(settle)

    def mouse_down(self, button="left"):
        self._cmd({"execute": "input-send-event",
                    "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": button}}]}})

    def mouse_up(self, button="left"):
        self._cmd({"execute": "input-send-event",
                    "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": button}}]}})

    def click(self, button="left", settle=0.1):
        self.mouse_down(button)
        time.sleep(settle)
        self.mouse_up(button)

    def click_at(self, x, y, **kw):
        self.goto(x, y)
        self.click(**kw)

    def drag(self, x, y, hold=0.2, settle=0.2, step_settle=0.02):
        """Press the left button at the current position, move (via
        the same chunked goto() every other move uses -- see its
        docstring for why a raw move_rel() isn't safe here) to (x, y)
        while held, then release. For dragging a scrollbar thumb, a
        window titlebar, etc. Call goto() first if the press needs to
        start somewhere other than the current tracked position.
        """
        self.mouse_down()
        time.sleep(hold)
        self.goto(x, y, settle=step_settle, step_settle=step_settle)
        time.sleep(hold)
        self.mouse_up()
        time.sleep(settle)

    def wheel(self, direction, notches=1, delay=0.08):
        """Scrolls the mouse wheel `notches` times. `direction` is
        'up' or 'down'. QEMU's PS/2 IntelliMouse emulation reports
        wheel movement as synthetic 'wheel-up'/'wheel-down' button
        press+release pairs over QMP -- there's no separate scroll
        event type, this is genuinely how it's done.
        """
        button = "wheel-up" if direction == "up" else "wheel-down"
        for _ in range(notches):
            self.mouse_down(button)
            self.mouse_up(button)
            time.sleep(delay)

    # -- screenshots --------------------------------------------------------

    def screenshot(self, png_path, ppm_path=None, settle=0.3):
        """screendump -> .ppm (native QMP format) -> .png (via Pillow),
        so the result can go straight to SendUserFile / an image viewer.
        """
        ppm_path = ppm_path or (png_path.rsplit(".", 1)[0] + ".ppm")
        self._cmd({"execute": "screendump", "arguments": {"filename": ppm_path}})
        time.sleep(settle)
        try:
            from PIL import Image
        except ImportError as e:
            raise RuntimeError(
                "Pillow is required for screenshot() -- "
                "pip install pillow --break-system-packages"
            ) from e
        Image.open(ppm_path).save(png_path)
        return png_path

    def close(self):
        self._sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


if __name__ == "__main__":
    print(__doc__)
    print("\nLaunch command (run with setsid, see module docstring):")
    print("  " + launch_qemu_cmd())
