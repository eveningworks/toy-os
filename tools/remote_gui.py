#!/usr/bin/env python3
"""Driving the BARE-METAL machine with the tools written for a VM.

WHAT THIS IS
------------
Two objects that quack like the two a GUI tool already drives:

    QMPSession   -> RemoteScreen    screenshots, and synthetic input
    DebugConsole -> RemoteConsole   the `gui` vocabulary, and `sh`

A tool constructs those two and asks them for pixels and for the
compositor's own geometry. Nothing it asks for is specific to an
emulator -- `gui menu --json` is the same answer on either machine -- so
the tools do not need porting, only pointing somewhere else. That is
what this module is: the same interface over telnet and TFTP
(tools/remote.py) instead of over QMP and a unix socket.

HOW A TOOL ENDS UP HERE
-----------------------
`TOYOS_REMOTE_HOST=<ip>` in the environment, which `gui_regress.py
--host` sets for its children. `DebugConsole.__new__` and
`QMPSession.__new__` hand back the remote object instead, so a tool that
says `QMPSession(port=args.qmp_port)` gets the laptop with no edit of its
own. The alternative was a `--host` flag and a factory call in each of
fifty-odd tools, i.e. fifty chances to forget; the two constructors are
already the chokepoint every one of them passes through.

Each object prints one line saying where it is pointed, because a test
silently measuring a different machine than the reader thinks is the
worst outcome this file could have.

WHAT IS NOT THE SAME, AND IS REFUSED RATHER THAN FAKED
------------------------------------------------------
`RemoteUnsupported` is raised, with the reason, by anything that needs
QEMU itself:

  * `hmp()` -- the monitor. The one oracle a guest cannot fake, and
    there is no monitor in front of real hardware.
  * `mouse_down()` / `mouse_up()` / `key_down()` / `key_up()` -- a HELD
    button or key. The compositor's injection queue has no press
    without its release (wm_debug.c's cmd_click is one indivisible
    move-press-hold-release), so holding cannot be expressed. Use
    `drag()`, which the WM does implement as one gesture.
  * `move_rel()` -- relative motion with no absolute target. `goto()`
    works, because the kernel can warp.

The refusal names the tool and the call, so a red run says "this check
cannot run on hardware" rather than reporting a healthy machine broken.

WHAT IS SLOWER, SAID PLAINLY
----------------------------
A screenshot is the guest writing a PNG and this machine fetching it
over TFTP -- about a second, against a QMP screendump's tens of
milliseconds -- so a `stable_pixels()` comparison costs two of those.
There is one laptop, so hardware runs are SERIAL: this is a spot-check
tier like `qemu_matrix.py`, never a gate.
"""

import atexit
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import remote  # noqa: E402
from gui_debug import DebugConsole  # noqa: E402
from qmp_test import QMPSession  # noqa: E402

DEFAULT_TELNET_PORT = remote.DEFAULT_TELNET_PORT
DEFAULT_TFTP_PORT = remote.DEFAULT_TFTP_PORT

# Where a capture lands on the machine itself. /var/tmp, never /tmp:
# the latter is a ramfs mount and TFTP reads the file back a moment
# later (remote.py's do_screenshot says the same).
REMOTE_SHOT = "/var/tmp/remote-shot.png"


class RemoteUnsupported(RuntimeError):
    """This call needs QEMU, and there is no QEMU in front of a laptop."""


def close_session():
    """Hand the machine's session back.

    NOT optional housekeeping: `inetd` serves four children at a time
    (userland/bin/inetd.c's CHILDREN_MAX), so a process that keeps one
    open is a quarter of the machine's capacity -- and the next process
    to be refused sees `connection closed` with no hint of why.
    """
    global _SESSION, _SESSION_KEY
    if _SESSION is not None:
        try:
            _SESSION.close()
        except OSError:
            pass
    _SESSION = None
    _SESSION_KEY = None


atexit.register(close_session)


def remote_host():
    """The machine tools should drive, or None for "use the VM"."""
    return os.environ.get("TOYOS_REMOTE_HOST") or None


def _banner(what, host):
    print(f"remote: {what} -> {host} (bare metal, not a VM)", file=sys.stderr)


# ONE TELNET SESSION PER PROCESS, shared by every console and screen in
# it. The first reason is not a preference: the machine serves telnet
# through inetd and has a handful of sessions, so a tool opening one per
# DebugConsole ran out -- and the connection was closed under whatever
# command happened to be in flight, reported as `EOFError: connection
# closed` with the desktop then looking absent. The second is the
# repo's existing rule for the VM side: two consoles on one channel
# steal each other's replies, and a process drives one machine anyway.
_SESSION = None
_SESSION_KEY = None


def _session(host, port, timeout, tries=4):
    """The process's session, opened and PROVEN before it is handed out.

    A connection can be accepted and then go nowhere -- inetd hands it
    to a shell that is still starting, or the previous process's session
    has not been reaped yet -- and the symptom is the FIRST command
    timing out with no output, which reads as "the desktop is gone".
    So the session is tested with an `echo` before it counts as open,
    and a failed one is dropped and retried rather than returned.

    Retrying the HANDSHAKE is safe in a way retrying a command is not:
    nothing has been asked of the machine yet, so nothing can happen
    twice.
    """
    global _SESSION, _SESSION_KEY
    if _SESSION is not None and _SESSION_KEY == (host, port):
        return _SESSION
    if _SESSION is not None:
        try:
            _SESSION.close()
        except OSError:
            pass
        _SESSION = None
    last = None
    for attempt in range(tries):
        try:
            sess = remote.Session(host, port, timeout)
            sess.run("echo __alive__", min(timeout, 8.0))
            _SESSION = sess
            _SESSION_KEY = (host, port)
            return _SESSION
        except (OSError, EOFError, TimeoutError) as e:
            last = e
            try:
                sess.close()
            except Exception:
                pass
            time.sleep(1.0 + attempt)
    raise RuntimeError(f"could not open a working session on {host}:{port}: {last}")


class RemoteConsole(DebugConsole):
    """DebugConsole over telnet, talking to `guictl` instead of `gui`.

    A SUBCLASS on purpose: every helper on DebugConsole -- `json()`,
    `settle()`, `menu_row()`, `hover_frames()`, `open_app()` -- is built
    on `send()`, so overriding the transport carries all of them across
    and none of them can drift into a second implementation. That is the
    same argument `klineedit` makes for being compiled twice.

    The command vocabulary is identical because `guictl` IS the `gui`
    vocabulary reached from a program (docs/commands/guictl.md): the
    compositor registers as the diagnostic provider named `gui`, and
    both front ends ask it the same questions.
    """

    _announced = False

    def __init__(self, sock_path=None, timeout=20.0, host=None,
                 port=DEFAULT_TELNET_PORT, quiet=False):
        # `sock_path` is accepted and ignored: a tool hands over the VM
        # serial socket it would have used, and we are not that machine.
        self.host = host or remote_host()
        if not self.host:
            raise RuntimeError("RemoteConsole needs a host (TOYOS_REMOTE_HOST)")
        self.sock_path = f"{self.host}:{port}"
        self._port = port
        self.timeout = timeout
        self.log_lines = []
        self._klog_last = None
        self._sess = _session(self.host, port, timeout)
        if not quiet and not RemoteConsole._announced:
            RemoteConsole._announced = True
            _banner("console", self.host)

    # -- transport --------------------------------------------------------

    @staticmethod
    def _translate(command):
        """A debug-console line as the machine's own shell takes it.

        `gui X` is `guictl X`; `sh X` is just X, since we are already at
        a shell rather than at the kernel's serial console. Anything
        else goes through untouched -- `dmesg`, `ps` and friends are
        /bin programs on both machines.
        """
        if command.startswith("gui "):
            return "guictl " + command[4:]
        if command == "gui":
            return "guictl help"
        if command.startswith("sh "):
            return command[3:]
        return command

    def send(self, command):
        lines = self._sess.run(self._translate(command), self.timeout)
        out = "\n".join(lines).strip("\n")
        self.log_lines.extend(ln for ln in out.splitlines() if ln.strip())
        return out

    def close(self):
        # DELIBERATELY NOT closing the session: it is the PROCESS's, not
        # this object's (see _session above), and tools open and close
        # consoles freely -- wait_for_desktop() does it once a poll.
        pass

    def reconnect(self, timeout=180.0):
        """Re-attach after the MACHINE went away -- a reboot.

        The VM side of this re-opens a unix socket that never stopped
        existing; here the far end is a machine that has to finish
        POSTing, so the wait is longer and starts with "is anything
        listening at all". The session is dropped first: `inetd`'s child
        for the old connection dies with the machine, and holding a dead
        one costs a quarter of the machine's telnet capacity.
        """
        close_session()
        deadline = time.time() + timeout
        while time.time() < deadline:
            if machine_is_up(self.host, self._port):
                try:
                    self._sess = _session(self.host, self._port, self.timeout)
                    self._klog_last = None   # a new boot, a new ring
                    return True
                except RuntimeError:
                    pass
            time.sleep(3)
        print(f"remote: {self.host} did not come back within {timeout:.0f}s",
              file=sys.stderr)
        return False

    # -- the kernel log, which does not share this wire ------------------
    #
    # ON A VM THE KLOG REACHES THE CONSOLE OBJECT FOR FREE: in replies on
    # a one-port guest, or through DebugConsole's COM1 reader thread when
    # the log has its own port -- so an app's `uidemo: layout ...` lines
    # arrive without asking, and every tool that drives a Toykit app by
    # asking for its widget geometry is built on that. Over telnet there
    # is no such stream -- `guictl`
    # returns one command's reply and nothing else -- so the log is
    # FETCHED instead, from the ring `dmesg` prints, and the delta since
    # the last fetch is what `send()` would have accumulated.
    #
    # Delta by LINE COUNT against the previous fetch: the ring is
    # cumulative and bounded (a few hundred lines), so a run that
    # outpaces it loses the oldest -- the same rate limit CLAUDE.md
    # records for instrumentation, and the reason a probe must stay
    # under a line a second.

    def _klog_delta(self):
        lines = (self._sess.run("dmesg", self.timeout) or [])
        # `dmesg` echoes nothing of its own, but the shell prints the
        # command; Session.run() has already stripped that.
        # ANCHORED ON THE LAST LINE SEEN, not on a count: a FULL ring keeps
        # its length while it scrolls, so a count reported nothing new for
        # the rest of the session. Lines carry a timestamp, so one is unique.
        new = lines
        if self._klog_last is not None and self._klog_last in lines:
            new = lines[len(lines) - lines[::-1].index(self._klog_last):]
        if lines:
            self._klog_last = lines[-1]
        return new

    def events(self, prefix="uidemo:"):
        return [ln for ln in self._klog_delta() if ln.startswith(prefix)]

    def logs(self, match="", clear=True):
        self.log_lines.extend(self._klog_delta())
        hits = [ln for ln in self.log_lines if match in ln]
        if clear:
            self.log_lines = [ln for ln in self.log_lines if match not in ln]
        return hits

    # -- what a serial console can do and telnet cannot ------------------

    def _sync(self):
        pass   # the Session framed its own prompt when it connected


class RemoteScreen(QMPSession):
    """The QMPSession surface, backed by the machine's own screenshot
    program and by the compositor's input injection.

    A SUBCLASS for two reasons: `isinstance(x, QMPSession)` stays true,
    and Python only runs `__init__` on what `__new__` hands back when it
    is an instance of the class that was called -- so the dispatch in
    qmp_test.py needs this relationship to exist.

    Not a QMP connection at all: `screenshot` is a ring-3 program on the
    machine asking the compositor for pixels (remote.py's do_screenshot
    says why that is the only way), fetched over TFTP; input goes in
    through `guictl`, which is where a VM tool's clicks go too -- QMP is
    the odd one out, entering below the driver.
    """

    _announced = False

    def __init__(self, host=None, port=None, telnet_port=DEFAULT_TELNET_PORT,
                 tftp_port=DEFAULT_TFTP_PORT, timeout=20.0, console=None,
                 quiet=False, machine=None, **_ignored):
        # `host`/`port` are QMPSession's -- 127.0.0.1 and a TCP port on
        # THIS machine -- and mean nothing here, so they are accepted and
        # ignored the way RemoteConsole ignores a serial socket path. The
        # machine is named by the environment, or explicitly as
        # `machine=` when something constructs this directly.
        self.host = machine or remote_host()
        if not self.host:
            raise RuntimeError("RemoteScreen needs a host (TOYOS_REMOTE_HOST)")
        self.telnet_port = telnet_port
        self.tftp_port = tftp_port
        self.timeout = timeout
        # ONE console, shared. Two telnet sessions against one machine
        # steal each other's replies exactly as two DebugConsoles on one
        # serial socket do, and the failure looks like a dropped command
        # rather than like contention.
        self._console = console or RemoteConsole(host=self.host, timeout=timeout,
                                                 port=telnet_port, quiet=True)
        self.pos = [0, 0]
        self._tb_h = None
        if not quiet and not RemoteScreen._announced:
            RemoteScreen._announced = True
            _banner("screen", self.host)

    # -- pixels -----------------------------------------------------------

    def screenshot(self, png_path, ppm_path=None, settle=0.3, stable=True,
                   tries=4, interval=0.15, pointer=True, box=None):
        """A frame, fetched from the machine.

        `pointer=True` by default because the cursor is part of what a
        VM capture shows and a hover check compares captures: the
        laptop's sprite is on a hardware plane the screenshot program
        does not otherwise composite, so a capture without it differs
        from QEMU's in exactly the place those tests look.

        SETTLED the same way QMPSession does it -- fetch until two agree
        -- with one difference that matters: A WHOLE FRAME IS NEVER
        STILL HERE. A fetch is about a second, the taskbar clock ticks
        once a second, so two full frames essentially never match and
        the comparison would burn every retry and then return anyway.
        So the comparison is over `box` when the caller named one, and
        otherwise over the screen ABOVE THE TASKBAR, whose height the
        compositor reports. A test whose own box is in the taskbar
        passes it and gets the strict thing it asked for.
        """
        png_path = os.path.abspath(png_path)
        if settle:
            time.sleep(settle)
        self._fetch(png_path, pointer)
        if not stable:
            return png_path
        last = self._region(png_path, box)
        for _ in range(max(0, tries - 1)):
            time.sleep(interval)
            self._fetch(png_path, pointer)
            cur = self._region(png_path, box)
            if cur == last:
                return png_path
            last = cur
        return png_path

    def _region(self, png_path, box):
        """The pixels a settle compares: `box`, or everything above the
        taskbar -- the one strip guaranteed to change every second."""
        from PIL import Image
        with Image.open(png_path) as im:
            im = im.convert("RGB")
            if box:
                return im.crop(box).tobytes()
            return im.crop((0, 0, im.width, max(1, im.height - self._taskbar_h()))).tobytes()

    def _taskbar_h(self):
        if self._tb_h is None:
            self._tb_h = 0
            for line in (self._console.send("gui state") or "").splitlines():
                # `screen 1920x1080, taskbar 27px`
                if "taskbar " in line and line.strip().startswith("screen "):
                    try:
                        self._tb_h = int(line.split("taskbar ", 1)[1].split("px", 1)[0])
                    except ValueError:
                        self._tb_h = 0
        return self._tb_h

    def _fetch(self, local, pointer):
        cmd = "screenshot -f png" + (" -p" if pointer else "") + f" {REMOTE_SHOT}"
        out = self._console.send(cmd)
        for line in out.splitlines():
            if line.startswith("screenshot:"):
                raise RuntimeError(f"remote screenshot failed: {line}")
        self._console.send("sync")
        rc = remote.do_get(self.host, self.tftp_port, REMOTE_SHOT, local, self.timeout)
        if rc != 0:
            raise RuntimeError(f"could not fetch {REMOTE_SHOT} from {self.host}")
        with open(local, "rb") as f:
            return f.read()

    def stable_pixels(self, png_path, box=None, tries=12, settle=0.15):
        """Raw RGB bytes for `box`, once the frame has stopped changing --
        QMPSession.stable_pixels()'s contract, byte for byte."""
        from PIL import Image
        # The box goes to the SETTLE as well as to the crop: what the
        # caller is about to compare is exactly what has to hold still.
        self.screenshot(png_path, stable=True, tries=min(tries, 4),
                        interval=settle, box=box)
        with Image.open(png_path) as im:
            im = im.convert("RGB")
            return (im.crop(box) if box else im).tobytes()

    # -- input ------------------------------------------------------------
    #
    # Everything here goes through the compositor's injection queue,
    # which is where a VM tool's clicks land too. `settle()` after each
    # one for the same reason the console's helpers do it: injection is
    # asynchronous, one event per frame.

    def click_at(self, x, y, **_kw):
        self._console.click(x, y)
        self.pos = [x, y]

    def click(self, button="left"):
        x, y = self.pos
        if button == "right":
            self._console.rclick(x, y)
        else:
            self._console.click(x, y)

    def goto(self, x, y, settle=0.2, step_settle=0.02):
        """The REAL pointer, warped by the kernel (`gui warp`).

        QMPSession moves it by feeding relative deltas to the emulator's
        input layer, which hardware has no equivalent of -- so the
        capability was put where it belongs instead, in the mouse driver
        (api/mouse.h's mouse_set_position). One round trip, and the
        answer is where the pointer actually landed.
        """
        reply = self._console.send(f"gui warp {x} {y}") or ""
        for line in reply.splitlines():
            if "pointer at (" in line:
                got = line.split("pointer at (", 1)[1].split(")", 1)[0]
                cx, cy = (int(v) for v in got.split(","))
                self.pos = [cx, cy]
                self._console.settle()
                return (cx, cy)
        # NOT a fall back to relative motion: DebugConsole.warp_cursor()
        # would then aim through this very method and recurse. A machine
        # whose WM predates `gui warp` simply cannot park a pointer, and
        # saying so beats a RecursionError.
        self._no("goto()", "this machine's compositor has no `gui warp` -- "
                           "flash it with a build that has one")

    def recalibrate(self, **_kw):
        return self.goto(*self.pos) if self.pos != [0, 0] else None

    def send_key(self, qcode):
        self._console.key(_qcode_to_guictl(qcode))

    def send_text(self, text, delay=0.03):
        for ch in text:
            self._console.key(ch)
            if delay:
                time.sleep(delay)

    def combo(self, qcodes):
        """One key with modifiers -- `guictl key <k> shift ctrl`."""
        mods = [q for q in qcodes if q in ("shift", "ctrl", "alt")]
        keys = [q for q in qcodes if q not in ("shift", "ctrl", "alt")]
        if len(keys) != 1:
            raise RemoteUnsupported(
                f"combo{tuple(qcodes)}: the injection queue takes one key plus "
                "modifiers, not a chord of several keys")
        self._console.key(_qcode_to_guictl(keys[0]), mods=" ".join(mods))

    def wheel(self, direction, notches=1, delay=0.08):
        n = notches if direction in ("up", 1) else -notches
        self._console.wheel(n)

    def drag(self, from_x, from_y, to_x, to_y, *, steps=8, **_kw):
        self._console.drag(from_x, from_y, to_x, to_y, steps=steps)
        self.pos = [to_x, to_y]

    # -- what hardware cannot do ------------------------------------------

    def _no(self, what, why):
        raise RemoteUnsupported(f"{what} on bare metal: {why}")

    def _cmd(self, obj, **_kw):
        """EVERY inherited QMP call lands here, and is refused by name.

        The overrides above cover what hardware can do; anything else on
        QMPSession reaches QEMU directly, and a silent AttributeError
        about a missing socket would send a reader looking at the wrong
        thing entirely.
        """
        what = obj.get("execute", "?") if isinstance(obj, dict) else str(obj)
        self._no(f"QMP `{what}`",
                 "this call has no bare-metal equivalent; see tools/remote_gui.py")

    def hmp(self, command):
        self._no("hmp()", "there is no QEMU monitor in front of a real machine")

    def mouse_down(self, button="left"):
        self._no("mouse_down()", "the compositor's injection queue has no press "
                                 "without its release -- use drag()")

    def mouse_up(self, button="left"):
        self._no("mouse_up()", "see mouse_down()")

    def key_down(self, qcode):
        self._no("key_down()", "a HELD key cannot be expressed through `guictl key`; "
                               "it sends a press and a release together")

    def key_up(self, qcode):
        self._no("key_up()", "see key_down()")

    def move_rel(self, dx, dy):
        self._no("move_rel()", "relative motion has no target to confirm; use goto()")

    # -- housekeeping ------------------------------------------------------

    def close(self):
        self._console.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


# QMP names a few keys the WM's console spells differently. Everything
# else is a single character or an 0xNN code, which both sides agree on.
_QCODE = {
    "ret": "0x0a", "kp_enter": "0x0a", "esc": "0x1b", "backspace": "0x08",
    "tab": "0x09", "spc": " ", "up": "0xf791", "down": "0xf792",
    "left": "0xf795", "right": "0xf796", "pgup": "0xf793", "pgdn": "0xf794",
    "home": "0xf797", "end": "0xf798", "delete": "0xf799", "insert": "0xf7b3",
    "f1": "0xf7ab", "f2": "0xf79a", "f3": "0xf79b", "f4": "0xf7a5", "f10": "0xf7a4",
}


def _qcode_to_guictl(qcode):
    if qcode in _QCODE:
        return _QCODE[qcode]
    if len(qcode) == 1:
        return qcode
    raise RemoteUnsupported(f"key {qcode!r} has no `guictl key` spelling")


def machine_is_up(host, port=DEFAULT_TELNET_PORT, timeout=4.0):
    """Can this machine be reached at all? Asked BEFORE a suite starts.

    Silence from the Lenovo means it is switched off, not faulty
    (local_info.txt) -- so a runner asks first and says which machine did
    not answer, rather than reporting fifty tools as broken.
    """
    try:
        s = socket.create_connection((host, port), timeout=timeout)
        s.close()
        return True
    except OSError:
        return False


def desktop_is_up(console, tries=3):
    """Is a compositor running? `gui state` answers only when one is.

    Tolerates a transport error rather than raising: this is asked
    BEFORE a suite starts, precisely to turn "nothing is answering" into
    one sentence instead of into fifty red tools.
    """
    for _ in range(tries):
        try:
            if "screen " in (console.send("gui state") or ""):
                return True
        except (OSError, EOFError, TimeoutError):
            pass
        time.sleep(1.0)
    return False
