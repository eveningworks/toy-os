#!/usr/bin/env python3
"""The same keys do the same thing on PS/2 and on virtio-input.

WHAT IS UNDER TEST
------------------
The property the input core exists for, asserted end to end: which
DRIVER a key arrived through must not be observable. A keyboard is a
`struct input_source`, the canonical event is a Linux evdev keycode, and
`/etc/kbs` is keyed on those -- so nothing between the wire and the
character should differ between the two paths.

It was not true, and nothing noticed. The layout used to be keyed on AT
set-1 SCANCODES, which forced the input core to translate evdev DOWN into
that legacy encoding for every non-PS/2 device -- a hand-kept table, which
had a hole exactly where a hand-kept table gets one: KEY_102ND, the extra
key an ISO keyboard has between Left Shift and Z. On every Nordic layout
that key carries `|`, so **a pipeline could be typed on a PS/2 boot and
not on an `INPUT=virtio` one.** Both work; only one was ever tried.

THE ORACLE IS THE FILESYSTEM, NOT THE SCREEN, and that is the second
thing this tool learned. Comparing two screenshots of the typed line
seems obvious and is a trap: the two boots log different things above
the prompt, the caret blinks, and `_` DRAWS NOTHING on the ring-0
console (its ink lies below `line_h`, which the console deliberately
does not paint -- see gfx.c). So the screen cannot tell "the key never
arrived" from "the glyph is invisible", which is precisely the
distinction that matters here. A file either exists or does not.

WHAT EACH LINE PROVES, and neither covers the other's character:

- `touch /kb_probe.txt` -- `_` reached the shell. Invisible on screen,
  unambiguous in a directory listing.
- `echo x | touch /kb_pipe.txt` -- `|` reached the shell AND a two-stage
  pipeline ran its SECOND stage. The discrimination is the point: with
  the pipe, `touch` runs and the file appears; without it the line is
  `echo x touch /kb_pipe.txt`, which prints three words and creates
  NOTHING. So the file's existence separates the two on its own.

  Deliberately no `>` in it. Combining a pipe with a redirect is a
  SHELL question, and mixing one into a KEYBOARD check means a failure
  here could be either -- which is exactly what the first version did,
  leaving an empty file and no way to tell which half was at fault.

PRECONDITIONS THIS TOOL ESTABLISHES ITSELF
------------------------------------------
Four boots: for each input path, one to set the `text` target and the
`se` layout, one to type. `se` rather than `us` because the ISO key only
carries `|` on a layout that has one -- on `us` this would pass
vacuously. Both run against a COPY of disk.img.

Usage:
    python3 tools/keyboard_paths_test.py
    python3 tools/keyboard_paths_test.py --instance 3

Exits 0 if every check passed, 1 otherwise, 2 if it could not run.
"""

import argparse
import os
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
from gui_debug import DebugConsole      # noqa: E402
import vm as vm_mod                   # noqa: E402 -- started_ok(), see its comment
from qmp_test import QMPSession         # noqa: E402
import port_guard                       # noqa: E402
from harness import copy_disk  # noqa: E402

VM = os.path.join(REPO, "tools", "vm.py")

PROBE = "/kb_probe.txt"     # its own name carries the underscore
PIPED = "/kb_pipe.txt"      # created ONLY if `|` arrived -- see the docstring

checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}"
          + ("" if ok or not detail else f"    [{detail}]"))
    return bool(ok)


def serial_sock(instance):
    # Slot 0 keeps the original, unsuffixed name -- see vm.py's
    # _apply_instance(), which every other tool here relies on too.
    return ".vm.serial" if not instance else f".vm.{instance}.serial"


def vm_run(disk, instance, virtio, *argv):
    cmd = [sys.executable, VM, "--disk", disk, "--instance", str(instance)]
    if virtio:
        cmd += ["--virtio-input"]
    r = subprocess.run(cmd + list(argv), cwd=REPO, capture_output=True, text=True)
    return r.stdout + r.stderr


# --- typing, key by key ----------------------------------------------
#
# NOT send_text() for the punctuation. A QMP qcode names a PHYSICAL key
# by its US label, and this guest is on `se` -- so the key US calls `/`
# types `-`, and `_` is Shift over it. Spelling each one out is what
# makes the typed line mean what it says.
#
# `alt_r`, NOT `altgr`: QEMU's qcode for the right Alt. An invalid qcode
# is refused by QMP and sends NOTHING, which reads exactly like the guest
# dropping the key -- an earlier version of this tool blamed the OS for
# its own typo.
SE_KEYS = {
    "/": ["shift", "7"],
    "_": ["shift", "slash"],
    "|": ["alt_r", "less"],
    ">": ["shift", "less"],
    ".": ["dot"],
    " ": ["spc"],
}


def type_line(qmp, text):
    for ch in text:
        if ch in SE_KEYS:
            qmp.combo(SE_KEYS[ch])
        else:
            qmp.send_key(ch)
        time.sleep(0.06)
    qmp.send_key("ret")
    time.sleep(1.8)


def run_path(disk, instance, virtio, label):
    """Boot twice on one input path; return (root listing, piped content)."""
    print(f"{label}: boot 1 -- target and layout")
    if not vm_mod.started_ok(vm_run(disk, instance, virtio, "start")):
        return None, None
    dbg = DebugConsole(serial_sock(instance))
    dbg.send("sh keyboard se")
    dbg.send("sh config set system.default_target text")
    dbg.send(f"sh rm {PROBE}")
    dbg.send(f"sh rm {PIPED}")
    time.sleep(0.5)
    vm_run(disk, instance, virtio, "stop")

    print(f"{label}: boot 2 -- type")
    if not vm_mod.started_ok(vm_run(disk, instance, virtio, "start")):
        return None, None
    dbg = DebugConsole(serial_sock(instance))
    qmp = QMPSession(port=4445 + instance)
    time.sleep(2.5)

    type_line(qmp, "touch /kb_probe.txt")
    type_line(qmp, "echo x | touch /kb_pipe.txt")

    # POLLED: the pipeline is two ELF loads, and under TCG it can land
    # seconds after Enter -- one read at a fixed delay failed a `|` that
    # had arrived.
    deadline = time.monotonic() + 15
    while True:
        names = dbg.send("sh ls /") or ""
        if ("kb_probe.txt" in names and "kb_pipe.txt" in names) \
                or time.monotonic() > deadline:
            break
        time.sleep(0.5)
    vm_run(disk, instance, virtio, "stop")
    return names, None


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--instance", default="auto")
    args = ap.parse_args()

    if args.instance == "auto":
        n = port_guard.find_free_instance()
        if n is None:
            print("keyboard_paths_test: no free VM slot")
            return 2
        args.instance = n
        print(f"keyboard_paths_test: slot {args.instance} (QMP {4445 + args.instance})")
    inst = int(args.instance)

    src = os.path.join(REPO, "disk.img")
    if not os.path.exists(src):
        print("keyboard_paths_test: no disk.img -- run `make iso` first")
        return 2

    results = {}
    for label, virtio in (("PS/2", False), ("virtio-input", True)):
        t = tempfile.NamedTemporaryFile(suffix=".img", delete=False)
        t.close()
        copy_disk(src, t.name)
        try:
            results[label] = run_path(t.name, inst, virtio, label)
        finally:
            vm_run(t.name, inst, virtio, "stop")
            os.unlink(t.name)

    for label in ("PS/2", "virtio-input"):
        names, _ = results.get(label, (None, None))
        if not check(f"{label}: the boot reached a prompt", names is not None):
            continue

        # `_` ARRIVED, even though the console draws nothing for it.
        check(f"{label}: `_` reaches the shell (invisible on screen, real in a name)",
              "kb_probe.txt" in names,
              f"`ls /` said: {names.strip()[:120]}")

        # `|` ARRIVED AND PIPED. Without the pipe the same keystrokes
        # are `echo x touch /kb_pipe.txt`, which creates nothing -- so
        # the file's existence is the whole discrimination.
        check(f"{label}: `|` reaches the shell AND pipes",
              "kb_pipe.txt" in names,
              f"`ls /` said: {names.strip()[:120]}")

    return report()


def report():
    failed = [c for c in checks if not c[1]]
    print(f"\nkeyboard_paths_test: {'FAIL' if failed else 'PASS'} -- "
          f"{len(checks) - len(failed)} passed, {len(failed)} failed")
    for name, _, detail in failed:
        print(f"  FAILED: {name}" + (f"    [{detail}]" if detail else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
