#!/usr/bin/env python3
"""Drive Disk Mark (userland/gui/apps/diskmark.c) and assert on it.

THE APP DOES NO I/O ITSELF -- it spawns /bin/diskbench and polls what
that prints, so this also covers the spawn, the report file and the
reap. The version that ran the passes in its own event loop sat at
"Disk Mark (Not Responding)" for the length of a pass, which is why the
title-bar control below is not optional.

THE LOAD-BEARING CHECK IS THE NUMBERS, NOT THE RUN FINISHING. A build
whose throughput arithmetic truncated to zero logged "all four passes
complete" and drew four tiles reading `0.0 MB/s`, with correct IOPS
beside them -- everything an "it ran" check looks at was green. So every
result is parsed and required to be positive.

AND THE NUMBERS MUST BE DRAWN, not merely logged. That is the
Calculator-with-invisible-buttons failure (docs/gui-guidelines.md): the
ring-3 Calculator shipped with no visible buttons at all because every
check asserted that clicking one changed the display, which it did. So
the tiles are compared as PIXELS before and after the run, with the
window CHROME sampled the same way as a control -- a frame that changed
everywhere is a repaint, not a result.

GEOMETRY COMES FROM THE CLIENT (`diskmark: layout ...`), never
re-derived here. Hand-computed coordinates cost two build-and-test
cycles while this app was being written -- the click landed inside the
button and nothing happened, because the app had declared its widgets
for DRAWING and not for INPUT, and a wrong-looking coordinate was the
first thing suspected.

Usage (the VM must already be up):

    python3 tools/vm.py start
    python3 tools/diskmark_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402

DEFAULT_SOCK = ".vm.serial"
TITLE = "Disk Mark"
# Four passes over 16 MiB on an emulated disk. Generous: the tool waits
# on the app's OWN completion line, so this only bounds a hang.
RUN_TIMEOUT_S = 240.0

PROFILES = ["SEQ Q1T1 READ", "SEQ Q1T1 WRITE",
            "RND4K Q1T1 READ", "RND4K Q1T1 WRITE"]


def layout_of(lines):
    """The rects the client reported, content-relative."""
    run, tiles = None, {}
    for line in lines:
        if "diskmark: layout " not in line:
            continue
        p = line.split("diskmark: layout ", 1)[1].split()
        if p[0] == "run" and len(p) >= 5:
            run = tuple(int(v) for v in p[1:5])
        elif p[0] == "tile" and len(p) >= 6:
            tiles[int(p[1])] = tuple(int(v) for v in p[2:6])
    return run, tiles


def this_run(text):
    """The log from THIS run's spawn onward.

    `dmesg` returns the whole ring, so an earlier run's "all four passes
    complete" satisfies a naive grep -- which is exactly what happened:
    the tool declared success on a stale line, screenshotted mid-run and
    then failed three cleanup checks because the child was still going.
    Every assertion below therefore reads only from the last spawn.
    """
    marker = "diskmark: spawned"
    i = text.rfind(marker)
    return text[i:] if i >= 0 else ""


def results_of(text):
    """{profile: (mbps, detail)} from the client's own result lines."""
    out = {}
    for m in re.finditer(r"diskmark: result (.+?) = ([\d.]+) MB/s (.*)", text):
        out[m.group(1).strip()] = (float(m.group(2)), m.group(3).strip())
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sock", default=DEFAULT_SOCK)
    ap.add_argument("--qmp-port", type=int, default=4445)
    ap.add_argument("--logs", default=None, help="directory for screenshots")
    args = ap.parse_args()

    tmp = args.logs or "."
    os.makedirs(tmp, exist_ok=True)

    checks = []

    def check(name, ok, detail=""):
        checks.append((name, ok))
        print(f"  {'ok  ' if ok else 'FAIL'}  {name}" + (f"  -- {detail}" if detail and not ok else ""))

    qmp = QMPSession(port=args.qmp_port)
    # enter_gui() takes the SESSION, not the port -- and it is what turns
    # `desktop.layout_log` on for every tool, before any app starts. It
    # opens its own console on the socket, so it goes BEFORE this tool's:
    # two consoles on one socket steal each other's replies, and the
    # setting write was lost that way while ulogf() hid it.
    enter_gui(qmp, sock=args.sock)
    con = DebugConsole(args.sock)

    con.open_app(TITLE)
    con.settle()

    wins = con.send("gui windows")
    check("the window opened", TITLE in wins)

    # Content origin, so the client's content-relative rects can be
    # turned into screen coordinates.
    content = None
    for line in wins.splitlines():
        if TITLE in line:
            nums = [int(v) for v in re.findall(r"-?\d+", line)]
            # z, x, y, w, h, then content x/y/w/h
            if len(nums) >= 9:
                content = (nums[5], nums[6], nums[7], nums[8])
    check("the window reported its content rect", content is not None)
    if not content:
        return 1
    ox, oy, cw, ch = content

    log = con.send("sh dmesg")
    run_rect, tiles = layout_of(log.splitlines())
    check("the client reported its Run button's geometry", run_rect is not None)
    check("the client reported four tile rects", len(tiles) == 4,
          f"{len(tiles)} reported")
    if not run_rect or len(tiles) != 4:
        return 1

    # A tile's interior, in screen coordinates, inset past its border.
    def tile_box(i):
        x, y, w, h = tiles[i]
        return (ox + x + 2, oy + y + 2, ox + x + w - 2, oy + y + h - 2)

    before = os.path.join(tmp, "diskmark_before.png")
    after = os.path.join(tmp, "diskmark_after.png")
    qmp.screenshot(before)

    # Click Run, at the rect the CLIENT reported.
    rx, ry, rw, rh = run_rect
    con.click(ox + rx + rw // 2, oy + ry + rh // 2)

    deadline = time.time() + RUN_TIMEOUT_S
    done = False
    log = ""
    while time.time() < deadline:
        time.sleep(4)
        log = this_run(con.send("sh dmesg"))
        if "diskmark: all four passes complete" in log:
            done = True
            break
        if "diskbench failed" in log or "could not start" in log:
            break
    check("the run completed", done)

    results = results_of(log)
    check("all four profiles reported a result", len(results) == 4,
          f"{sorted(results)}")

    # THE ONE THAT CATCHES A ZERO. Every result must be positive; a
    # build with broken arithmetic passes every check above.
    zeros = [n for n, (mbps, _) in results.items() if mbps <= 0.0]
    check("no profile reported 0 MB/s", not zeros, f"zero: {zeros}")

    # The random profiles carry IOPS and a latency; the sequential ones
    # deliberately do not -- MB/s at 1 MiB is the figure that means
    # something there.
    rnd_ok = all("IOPS" in results.get(p, (0, ""))[1] for p in PROFILES[2:])
    seq_ok = all("IOPS" not in results.get(p, (0, ""))[1] for p in PROFILES[:2])
    check("the random profiles report IOPS and latency", rnd_ok)
    check("the sequential ones do not", seq_ok)

    qmp.screenshot(after)

    # DRAWN, not just logged. Every tile must differ from its own
    # before-image; the TITLE BAR must not, which is the control that
    # separates "the results were painted" from "the window repainted".
    try:
        from PIL import Image
    except ImportError:
        print("  skip  the results are actually drawn -- Pillow not installed")
    else:
        b = Image.open(before).convert("RGB")
        a = Image.open(after).convert("RGB")
        changed = []
        for i in range(4):
            box = tile_box(i)
            changed.append(list(b.crop(box).getdata()) != list(a.crop(box).getdata()))
        check("every tile was repainted with its result", all(changed),
              f"unchanged: {[i for i, c in enumerate(changed) if not c]}")

        chrome = (ox, oy - 18, ox + cw, oy - 4)   # the title bar, above the content
        check("the title bar did NOT change (the control)",
              list(b.crop(chrome).getdata()) == list(a.crop(chrome).getdata()))

    # SELF-CLEANING. The temp file is the only litter this app can leave,
    # and the size picker exists to bound it.
    ls = con.send("sh ls /tmp")
    # BOTH of them: the worker's scratch file and the report the GUI
    # polls. An interrupted run leaving either behind is the litter the
    # size picker exists to bound.
    check("the worker's temp file was removed", "diskmark.tmp" not in ls)
    check("the report file was removed", "diskmark.out" not in ls)

    # The child must be REAPED, not left a zombie -- the GUI polls with
    # sys_waitpid_nohang() and a missed reap is a slot that never returns.
    ps = con.send("sh ps")
    check("the diskbench child was reaped", "diskbench" not in ps)

    # And each result logged EXACTLY ONCE. The drain re-reads the whole
    # report every tick and is idempotent by design, so a show_result()
    # that did not check for "unchanged" reprinted every result on every
    # tick and buried dmesg.
    once = all(log.count(f"diskmark: result {p} ") <= 1 for p in PROFILES)
    check("each result was logged once, not every tick", once)

    failures = [n for n, ok in checks if not ok]
    print()
    print(f"diskmark_test: {len(checks) - len(failures)}/{len(checks)} checks passed")
    if failures:
        print("diskmark_test: FAILED -- " + "; ".join(failures))
        return 1
    print("diskmark_test: PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
