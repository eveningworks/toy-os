#!/usr/bin/env python3
"""Reusable helpers for driving toy-os in headless QEMU over QMP.

Every GUI-testing session up to this point has re-derived the same few
gotchas from scratch (usually by hitting them first). This module exists
so that doesn't have to keep happening. It's a dev tool, not part of the
OS -- nothing here ships in the kernel or gets compiled by the Makefile.

Gotchas this module already gets right for you:

- The mouse driver in this kernel is PS/2 (relative deltas), not USB
  HID. Don't reach for -device usb-tablet or absolute positioning --
  it's the wrong device class for this guest. `click()`/`goto()` below
  use `input-send-event` with `rel` axis events and track the cursor
  position client-side, since there's no absolute-position query.
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

Typical usage from a Python REPL or script, once QEMU is already
running (see `launch_qemu_cmd()` for the command to start it with):

    from qmp_test import QMPSession

    qmp = QMPSession()                  # connects to 127.0.0.1:4445
    qmp.send_text("gui")                # type "gui" + Enter at the shell
    qmp.send_key("ret")
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

    def click(self, button="left", settle=0.1):
        self._cmd({"execute": "input-send-event",
                    "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": button}}]}})
        time.sleep(settle)
        self._cmd({"execute": "input-send-event",
                    "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": button}}]}})

    def click_at(self, x, y, **kw):
        self.goto(x, y)
        self.click(**kw)

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
