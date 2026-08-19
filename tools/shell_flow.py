#!/usr/bin/env python3
"""tools/shell_flow.py -- a named, composable QMP flow for the PHYSICAL
(pre-"gui") toy-os shell, mirroring what gui_flow.py's GuiFlow already
does for the GUI: bake in a repeated manual dance once instead of
re-deriving it every testing session.

The friction this fixes: qmp_test.py's QMPSession.send_text() only
covers lowercase letters/digits (see its own docstring) -- typing a
real shell command ("run nx_test", "run stack_smash_test") means
manually interleaving send_text() calls with send_key('spc') for each
space and combo(['shift', 'minus']) for each underscore, character by
character. That's easy to get wrong (a dropped space silently
concatenates the command into one unrecognized token, a plain
send_key('minus') types a hyphen where the command needed an
underscore) -- both mistakes actually happened in the same testing
session that led to this file existing. ShellFlow.run_command() does
that character-by-character mapping once, correctly, so a future
session just calls it with a plain readable command string.

What this does NOT do: parse or return the command's actual text
output. That would need either OCR on a screenshot, or reading the
raw VGA text-mode buffer at physical 0xB8000 -- and this kernel's
console (kernel/drivers/vga.c) auto-selects a FRAMEBUFFER backend
(character glyphs drawn as pixels onto GRUB's linear RGB framebuffer,
see multiboot.c's framebuffer tag) whenever GRUB hands it one, which
is the normal case for this project's QEMU launch flags -- so 0xB8000
stays stale/unwritten and there's no memory-read shortcut to plain
text the way there might be for a kernel that only ever used legacy
VGA text mode. run_command() returns a screenshot path instead; Read
it (or hand it to SendUserFile) to see the actual result, the same way
every other QMP-testing session already does.

Usage:
    import sys; sys.path.insert(0, "tools")
    from shell_flow import ShellFlow

    flow = ShellFlow(qmp_port=4445)
    path = flow.run_command("run nx_test")
    # -> screenshots/<subdir>/run_nx_test.png ready to Read
"""

import sys
import time

sys.path.insert(0, "tools")
from qmp_test import QMPSession  # noqa: E402

# THIS TABLE ASSUMES THE GUEST IS ON THE US KEYBOARD LAYOUT. A QMP
# qcode names a PHYSICAL KEY by its US-layout label, so what the guest
# actually types depends on the layout IT has loaded -- and this OS
# defaults to `se` (Swedish/Finnish), where the key labelled `/` on a US
# board produces `-` and `/` is Shift-7. Sending "slash" there typed a
# HYPHEN into every path: `spawn /bin/tosh` arrived as `spawn -bin-tosh`
# and `touch /probe.txt` created a file called `-probe.txt`, so a test
# asserting on a substring PASSED while nothing it meant had happened.
# Found while building stdin_test.py.
#
# So a tool that types punctuation must put the guest on the US layout
# first -- `keyboard us` at the shell, or `sh keyboard us` over the
# serial debug console, which is what stdin_test.py does. Fixing the
# table per layout was the other option and is worse: the mapping would
# then be silently wrong for anyone who changes the setting.
#
# Characters send_text() can't handle directly (lowercase letters/digits
# only) but that show up constantly in real toy-os shell commands --
# `run <name>` arguments, paths (`cat /etc/toyos.conf`), flags. Extend
# this table (not a hardcoded per-call combo()/send_key() dance) if a
# future command needs a character not covered here -- see
# qmp_test.py's own module docstring for the full qcode-name gotcha
# this table is built from.
_SPECIAL_CHARS = {
    " ": lambda s: s.send_key("spc"),
    "-": lambda s: s.send_key("minus"),
    "_": lambda s: s.combo(["shift", "minus"]),
    "/": lambda s: s.send_key("slash"),
    ".": lambda s: s.send_key("dot"),
    ",": lambda s: s.send_key("comma"),
    ";": lambda s: s.send_key("semicolon"),
    "'": lambda s: s.send_key("apostrophe"),
    "=": lambda s: s.send_key("equal"),
}


class ShellFlow:
    def __init__(self, qmp_port=4445, **session_kwargs):
        self.session = QMPSession(port=qmp_port, **session_kwargs)

    def type_command(self, text, delay=0.05):
        """Types `text` character by character, routing letters/digits
        through send_text() and everything in _SPECIAL_CHARS through
        its own qcode/combo -- the whole point of this class over
        hand-rolling the same dance inline. Does NOT send Enter (see
        run_command(), which does) so a caller building up a line in
        pieces can still call this directly. `delay` between
        characters matches send_text()'s own default and the "rapid
        send_key() calls can silently drop keystrokes" gotcha
        documented in qmp_test.py -- don't lower this without reading
        that first."""
        for ch in text:
            if ch in _SPECIAL_CHARS:
                _SPECIAL_CHARS[ch](self.session)
            elif ch.isalnum() and ch == ch.lower():
                self.session.send_text(ch, delay=0)
            else:
                raise ValueError(
                    f"shell_flow: {ch!r} in {text!r} has no mapping -- "
                    "add it to _SPECIAL_CHARS (see qmp_test.py's send_key() "
                    "qcode-name gotcha for the right qcode to use)"
                )
            time.sleep(delay)

    def run_command(self, command, settle=1.2, screenshot_name=None, subdir=None):
        """Types `command` at the physical shell prompt, presses Enter,
        waits `settle` seconds (a plain sleep, not a real "prompt
        reappeared" detection -- there's no cheap way to detect that
        without OCR, see this module's top comment; scale `settle` up
        for a command that does real work, e.g. a steppable read/write
        or a spawned process), then screenshots. Returns the screenshot
        path. `screenshot_name` defaults to `command` with spaces ->
        underscores (e.g. "run nx_test" -> "run_nx_test.png");
        `subdir` is required the same way gui_flow.py's
        screenshot_named() requires it (this module can't call the
        real clock either)."""
        self.type_command(command)
        self.session.send_key("ret")
        time.sleep(settle)

        if subdir is None:
            raise ValueError("shell_flow: pass subdir explicitly (e.g. today's date) -- "
                              "this module doesn't read the clock itself")
        import os
        name = screenshot_name or command.replace(" ", "_")
        out_dir = os.path.join("screenshots", subdir)
        os.makedirs(out_dir, exist_ok=True)
        return self.session.screenshot(os.path.join(out_dir, f"{name}.png"))


if __name__ == "__main__":
    # Minimal self-check: connect, run `ls` at the physical shell,
    # screenshot. Requires a QEMU instance already running with QMP on
    # the given port (see qmp_test.py's launch_qemu_cmd()) and freshly
    # booted (still at the shell prompt, not already in GUI mode).
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=4445)
    ap.add_argument("--out", default="/tmp/shell_flow_selfcheck.png")
    args = ap.parse_args()

    flow = ShellFlow(qmp_port=args.port)
    flow.type_command("run ls")
    flow.session.send_key("ret")
    time.sleep(1.0)
    flow.session.screenshot(args.out)
    print(f"shell_flow self-check: screenshot written to {args.out}")
