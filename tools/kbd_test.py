#!/usr/bin/env python3
"""`kbd` reports what the keyboard actually did, on both input drivers.

WHAT IS UNDER TEST
------------------
The four columns of the keyboard tap (kernel/include/kernel/keyboard_tap.h),
asserted against keys whose every stage is known in advance: the PS/2
scancode on the wire, the evdev keycode, the character the layout
produced, and the modifiers held when it was produced.

**THE TAP IS OFF BY DEFAULT, AND THAT IS CHECKED FIRST.** A ring holding
the last hundred-odd keystrokes is a keylogger by any honest
description, and `SYS_QUERY` has no privilege check -- so the state this
system ships in is the one that matters most, and the check that types
keys with the tap OFF and demands an empty log is the one nobody can
afford to have pass vacuously. It is written so it cannot: the same keys
are typed again with the tap on, and must show up.

**THE LOAD-BEARING CHECK OF THE FEATURE ITSELF IS THE SECOND BOOT.** Typing on PS/2 and
reading back `1e 30 'a'` proves the tool can read its own kernel's ring
and nothing more; a tap that simply echoed the wire byte would pass it.
The same keys on `INPUT=virtio` must produce THE SAME keycode and THE
SAME character with the scancode column BLANK -- which is the input
core's whole reason for existing (only the 8042 driver ever sees a
scancode) and cannot be faked by any implementation that is not really
reading the stage it claims to be.

WHY THE ORACLE IS TEXT AND NOT PIXELS. `kbd` prints a table, so there is
nothing here a screenshot would answer better -- and the console is
covered by the desktop on an ordinary graphical boot anyway. The serial
debug console runs it and returns its output, which is a string to
assert on.

RUN IT WITH THE LEGACY LOADER (`sh kbd --last`), NOT `spawn`. A spawned
process's stdout goes to the console framebuffer and never reaches the
serial socket; the legacy `run` loader's does. That is fine for `--last`,
which never sleeps -- and live mode is checked through the process table
instead, for exactly that reason.

WHAT IS NOT COVERED: how the live table LOOKS while it scrolls. The
records it prints are the same ones `--last` prints, and the printing is
one function, so the untested part is the poll loop's pacing.

Usage:
    python3 tools/kbd_test.py
    python3 tools/kbd_test.py --instance 3

Exits 0 if every check passed, 1 otherwise, 2 if it could not run.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
from gui_debug import DebugConsole      # noqa: E402
from qmp_test import QMPSession         # noqa: E402
import port_guard                       # noqa: E402
import vm as vm_mod                     # noqa: E402 -- started_ok()
from harness import copy_disk  # noqa: E402

VM = os.path.join(REPO, "tools", "vm.py")

checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}"
          + ("" if ok or not detail else f"    [{detail}]"))
    return bool(ok)


def serial_sock(instance):
    return ".vm.serial" if not instance else f".vm.{instance}.serial"


def vm_run(disk, instance, virtio, *argv):
    cmd = [sys.executable, VM, "--disk", disk, "--instance", str(instance)]
    if virtio:
        cmd += ["--virtio-input"]
    r = subprocess.run(cmd + list(argv), cwd=REPO, capture_output=True, text=True)
    return r.stdout + r.stderr


# One row of `kbd`'s table. The columns are fixed-width, but split() on
# whitespace would break the one column that legitimately contains a
# space -- an extended scancode prints as "e0 48". Anchored on the shape
# instead: everything before the keycode is the scan column.
ROW = re.compile(
    r"^\s*(?P<seq>\d+)\s+(?P<gap>[-\d]+)\s+(?P<scan>--|[0-9a-f]{2}|e0 [0-9a-f]{2})"
    r"\s+(?P<code>\d+)\s+(?P<prod>\S+(?: \S+)?)\s+(?P<mods>[S-][C-][A-][G-])"
    r"\s+(?P<edge>down|up)\s*$")


def parse(text):
    rows = []
    for line in text.splitlines():
        m = ROW.match(line)
        if m:
            rows.append(m.groupdict())
    return rows


def find(rows, **want):
    """The first row matching every given field, or None."""
    for r in rows:
        if all(r[k] == v for k, v in want.items()):
            return r
    return None


# The keys typed, and what every stage of each must say. Chosen so that
# each one exercises a different part of the path:
#
#   a          the plain case -- one wire byte, a direct keycode, a glyph
#   Shift+a    the SAME keycode producing a DIFFERENT character, and a
#              modifier key that produces no character at all
#   f5         a key with no glyph: a KEY_* code rather than a character
#   up         an EXTENDED scancode (0xE0-prefixed), whose keycode is not
#              its wire byte -- the one shape a naive tap gets wrong
KEYS = ["a", ["shift", "a"], "f5", "up"]

EXPECT = [
    # (name,           scan,    keycode, produced, mods)
    ("a",              "1e",    "30",    "'a'",    "----"),
    ("Shift itself",   "2a",    "42",    "--",     "S---"),
    ("Shift+a",        "1e",    "30",    "'A'",    "S---"),
    ("F5",             "3f",    "63",    "F5",     "----"),
    ("Up arrow",       "e0 48", "103",   "UP",     "----"),
]


def type_keys(qmp):
    for k in KEYS:
        if isinstance(k, list):
            qmp.combo(k)
        else:
            qmp.send_key(k)
        time.sleep(0.15)


def run_path(disk, instance, virtio, label):
    """Boot, type the keys, and return kbd's parsed table (or None)."""
    print(f"{label}: boot")
    # Stopped first, always -- the slot may still be held. The readiness
    # test is vm.started_ok(), NOT `"ready" in out`: vm.py answers a
    # start against a live pidfile with "already running", and "al-ready"
    # contains "ready", so the obvious test passes on the one output that
    # means the opposite.
    vm_run(disk, instance, virtio, "stop")
    out = vm_run(disk, instance, virtio, "start")
    if not vm_mod.started_ok(out):
        print(f"    (start failed: {out.strip()[-300:]})")
        return None
    # The socket is created by QEMU, not by vm.py's readiness check, and
    # the slot's previous guest has only just released the path. Waiting
    # on the ARTIFACT rather than sleeping a fixed amount.
    sock = serial_sock(instance)
    for _ in range(80):
        if os.path.exists(sock):
            break
        time.sleep(0.1)
    dbg = DebugConsole(sock, timeout=20.0)
    qmp = QMPSession(port=4445 + instance)
    time.sleep(1.0)

    # THE OFF STATE FIRST, before anything turns the tap on. Typed keys
    # must leave no record, and `kbd` must say why rather than printing
    # an empty table -- an empty table is what a BROKEN tap looks like
    # too.
    off = check_off_state(dbg, qmp, label)

    dbg.send("sh config set kernel.kbdtap on")
    time.sleep(0.3)
    type_keys(qmp)
    out = dbg.send("sh kbd --last 24") or ""
    return parse(out), dbg, qmp, off


def check_off_state(dbg, qmp, label):
    """With the tap off: nothing recorded, and kbd says so."""
    state = dbg.send("sh config get kernel.kbdtap") or ""
    check(f"{label}: the tap is OFF on a fresh boot",
          "off" in state.lower() and "on" not in state.lower().split(),
          state.replace("\n", " / ")[:80])

    type_keys(qmp)
    out = dbg.send("sh kbd --last 24") or ""
    # NOT "the table is empty" -- a broken tap prints an empty table too.
    # The message naming the switch is what distinguishes "off" from
    # "recording nothing for some other reason".
    check(f"{label}: with the tap off, kbd reports no recording and names the switch",
          "keyboard tap is off" in out and "kernel.kbdtap" in out,
          out.replace("\n", " / ")[:110])
    check(f"{label}: ...and no rows were recorded from the keys just typed",
          len(parse(out)) == 0, f"{len(parse(out))} rows")
    return True


def check_path(rows, label, ps2):
    """The four columns, for one input driver."""
    for name, scan, code, prod, mods in EXPECT:
        # THE SCANCODE COLUMN IS THE ONLY THING THAT MAY DIFFER between
        # the two drivers, and it must differ: a virtio keypress has no
        # scancode, so a blank is the correct report and a number would
        # be an invention.
        want_scan = scan if ps2 else "--"
        row = find(rows, code=code, prod=prod, mods=mods, edge="down")
        if not check(f"{label}: {name} -- keycode {code}, produced {prod}, mods {mods}",
                     row is not None,
                     f"no such row among {len(rows)}"):
            continue
        check(f"{label}: {name} -- scancode column says {want_scan!r}",
              row["scan"] == want_scan, f"row says {row['scan']!r}")

    # A RELEASE IS RECORDED TOO, and reports no character. "Is the key
    # release arriving?" is half of what a stuck key looks like, and a
    # tap that logged only presses could never answer it.
    up = find(rows, code="30", edge="up")
    check(f"{label}: the release of `a` is recorded, producing nothing",
          up is not None and up["prod"] == "--",
          "no up edge for keycode 30" if not up else f"produced {up['prod']!r}")

    seqs = [int(r["seq"]) for r in rows]
    check(f"{label}: sequence numbers are strictly increasing",
          seqs == sorted(seqs) and len(set(seqs)) == len(seqs),
          f"{seqs[:12]}")


def check_live(dbg, qmp, label):
    """Live mode: it arms the tap, runs, quits, and disarms again."""
    # FROM AN OFF TAP, because arming is the behaviour under test and a
    # tap left on by the checks above would let a broken auto-arm pass.
    dbg.send("sh config set kernel.kbdtap off")
    time.sleep(0.3)

    # A LONG TIMEOUT ON PURPOSE. If the idle timeout could fire during
    # this check, "it quit" would prove nothing about Esc.
    dbg.send("sh spawn /bin/kbd --timeout 120")
    time.sleep(1.2)
    check(f"{label}: live mode ARMS the tap it needs",
          "on" in (dbg.send("sh config get kernel.kbdtap") or "").lower())

    def state_of_newest_kbd():
        # THE STATE, NOT THE NAME: asking whether the string "kbd" appears
        # would pass with live mode wholly broken if any earlier copy were
        # still in the table. `gone` when `ps` answered and listed no kbd
        # -- a `spawn`ed program is init's child, and init reaps it.
        out = dbg.send("sh ps") or ""
        newest = "gone" if "PID" in out else None
        for line in out.splitlines():
            f = line.split()
            if len(f) >= 7 and f[-1] == "kbd":
                newest = (int(f[0]), f[3])
        return newest

    # POLLED FOR THE PARKED STATE, not sampled once. It sleeps 20ms
    # between polls, so it is blocked the overwhelming majority of the
    # time -- but a single sample can still land in the awake sliver, and
    # `ready` there means "running", not "broken". Kept as "it PARKS"
    # rather than weakened to "it is alive": a version that spun instead
    # of sleeping is exactly what this should catch.
    st = None
    for _ in range(15):
        st = state_of_newest_kbd()
        if isinstance(st, tuple) and st[1].startswith("block"):
            break
        time.sleep(0.3)
    if not check(f"{label}: live mode starts and parks between polls",
                 isinstance(st, tuple) and st[1].startswith("block"),
                 f"ps says {st}"):
        return
    pid = st[0]

    qmp.send_key("esc")
    time.sleep(0.3)
    qmp.send_key("esc")
    # POLLED, NOT SLEPT. The exit is the observable; a fixed wait long
    # enough on an idle host is a flake on a busy one, and this tool runs
    # beside others.
    st2 = None
    for _ in range(20):
        time.sleep(0.4)
        st2 = state_of_newest_kbd()
        if st2 == "gone" or (st2 and st2[0] == pid and st2[1] == "zombie"):
            break
    check(f"{label}: Esc twice quits it -- from the LOG, with no read of fd 0",
          st2 == "gone" or (st2 is not None and st2[0] == pid and st2[1] == "zombie"),
          f"ps says {st2}")

    # AND DISARMS ON THE WAY OUT. A tool that leaves a keystroke recorder
    # running after it exits is the whole thing the default is there to
    # prevent, so this is asserted rather than assumed.
    after = dbg.send("sh config get kernel.kbdtap") or ""
    check(f"{label}: ...and DISARMS the tap it armed",
          "off" in after.lower(), after.replace("\n", " / ")[:80])

    # THE GUARD, and the reason it exists: through the legacy `run`
    # loader there is no scheduler slot, so the sleep is refused and the
    # monotonic clock stands still -- a poll loop there spins forever and
    # takes the machine with it. The assertion is not the message; it is
    # that the machine still answers afterwards.
    #
    # `run`, NOT the bare name: a bare name at the kernel's prompt is
    # spawned with a slot now (apps/shell_path.c). And a DYNAMIC kbd is
    # refused by that loader before its own guard runs (PT_INTERP), so
    # either refusal counts; live mode starting is the failure.
    out = dbg.send("sh run /bin/kbd") or ""
    check(f"{label}: live mode refuses the legacy loader instead of hanging",
          ("scheduler slot" in out or "isn't a valid ELF64 executable" in out)
          and "Press keys" not in out, out.replace("\n", " / ")[:90])
    check(f"{label}: ...and the machine still answers after that refusal",
          "PID" in (dbg.send("sh ps") or ""))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--instance", default="auto")
    args = ap.parse_args()

    src = os.path.join(REPO, "disk.img")
    if not os.path.exists(src):
        print("kbd_test: no disk.img -- run `make iso` first")
        return 2

    tables = {}
    for label, virtio in (("PS/2", False), ("virtio-input", True)):
        # A FRESH SLOT PER BOOT, not one slot reused. Stopping a guest
        # does not free its QMP port straight away: this tool's own
        # QMPSession leaves the connection in TIME_WAIT on the server
        # side, so port 4445+N stays unbindable for around a minute and
        # port_guard -- correctly -- refuses the next boot on it. Waiting
        # that out would add a minute to the run for nothing, and pinning
        # both boots to one slot is what made this look like a virtio
        # failure when it was a socket-lifetime one.
        inst = (port_guard.find_free_instance() if args.instance == "auto"
                else int(args.instance))
        if inst is None:
            print("kbd_test: no free VM slot")
            return 2
        print(f"kbd_test: {label} on slot {inst} (QMP {4445 + inst})")
        t = tempfile.NamedTemporaryFile(suffix=".img", delete=False)
        t.close()
        copy_disk(src, t.name)
        try:
            got = run_path(t.name, inst, virtio, label)
            if got is None:
                tables[label] = None
            else:
                rows, dbg, qmp, _off = got
                tables[label] = rows
                if not virtio:
                    check_live(dbg, qmp, label)
        finally:
            vm_run(t.name, inst, virtio, "stop")
            os.unlink(t.name)

    print()
    for label, ps2 in (("PS/2", True), ("virtio-input", False)):
        rows = tables.get(label)
        if not check(f"{label}: the boot reached a prompt and kbd printed rows",
                     rows, "no rows parsed"):
            continue
        check_path(rows, label, ps2)

    # THE PARITY CHECK, stated as its own assertion rather than left
    # implicit in two passing lists: which driver a key arrived through
    # must not be observable ANYWHERE except the scancode column.
    a, b = tables.get("PS/2"), tables.get("virtio-input")
    if a and b:
        def shape(rows):
            return [(r["code"], r["prod"], r["mods"], r["edge"]) for r in rows]
        check("the two drivers agree on every column except the scancode",
              shape(a) == shape(b),
              f"PS/2 {shape(a)[:4]} vs virtio {shape(b)[:4]}")
        check("...and only the PS/2 boot reports scancodes at all",
              all(r["scan"] != "--" for r in a) and all(r["scan"] == "--" for r in b),
              f"PS/2 {[r['scan'] for r in a][:4]} virtio {[r['scan'] for r in b][:4]}")

    failed = [n for n, ok, _ in checks if not ok]
    print(f"\nkbd_test: {len(checks) - len(failed)}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
