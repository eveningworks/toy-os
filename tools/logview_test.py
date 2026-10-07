#!/usr/bin/env python3
"""The Log Viewer, on a fixture log it is opened at (`logview <file>`).

WHAT IT CHECKS, and what a broken version would still pass:

  - it opens the file and REPORTS its layout and the file's counts -- a
    viewer that read nothing reports `lines 0`;
  - the rows are DRAWN in severity colours: the error row's Level cell
    has the theme's error red and the warning row's the warning amber,
    read from a settled screenshot, and an ordinary row has NEITHER (a
    list that tinted every row, or none, fails one side);
  - the Errors segment shows exactly the fixture's errors, and choosing
    the repeated error reports `seen 2` -- the repeat count, not 1;
  - Only this message keeps the two copies, and Mute hides the `test`
    subsystem's lines -- and writes /etc/logview.conf, read back with
    `cat`, so a mute that only lived in memory fails; Unmute takes it out.

Geometry and state are the app's own report (`logview: layout ...`,
`logview: follow ... sel ... seen ...`), never re-derived here.
"""
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

APP = "/bin/wm/apps/logview"
TITLE = "Log Viewer"
FIXTURE = "/var/log/logview_fixture.log"
CONF = "/etc/logview.conf"
ERR_RED = (178, 58, 50)       # utheme_severity(UTHEME_SEV_ERROR)
WARN_AMBER = (160, 92, 0)     # utheme_severity(UTHEME_SEV_WARNING)

# Row 0 an error, row 1 a warning, row 2 ordinary, row 3 the error again,
# row 4 a program line -- in time order, which is the order they show.
LINES = [
    "[kernel] [1.00] <3> test: the disk refused a write",
    "[kernel] [2.00] <4> test: a slow frame",
    "[kernel] [3.00] test: an ordinary line",
    "[kernel] [4.00] <3> test: the disk refused a write",
    "[netd  ] [5.00] netd: eth0 is up",
]

_res = Results()
check = _res.check


def report(dbg, lay):
    for line in dbg.logs("logview:", clear=True):
        m = re.search(r"logview: layout (\S+?)(?: (\d+))? (-?\d+) (-?\d+) (\d+) (\d+)$", line)
        if m:
            lay[m.group(1) + (m.group(2) or "")] = tuple(int(v) for v in m.groups()[2:])
            continue
        m = re.search(r"logview: layout (\S+) (-?\d+)$", line)
        if m:
            lay[m.group(1)] = int(m.group(2))
            continue
        m = re.search(r"logview: follow (\d) timeline (\d) details (\d) mutes (\d+) show (\d) from (\d) "
                      r"only (\d) sel (-?\d+) seen (\d+)", line)
        if m:
            # `only_on`, not `only`: that is the button's layout name.
            for k, v in zip(("follow", "timeline", "details", "mutes", "show", "from", "only_on", "sel", "seen"),
                            m.groups()):
                lay[k] = int(v)
            continue
        m = re.search(r"logview: source (\S+) lines (\d+) shown (\d+)", line)
        if m:
            lay["source"], lay["lines"], lay["shown"] = m.group(1), int(m.group(2)), int(m.group(3))
    return lay


def wait(dbg, lay, ok, timeout=6.0):
    deadline = time.time() + timeout
    report(dbg, lay)
    while not ok(lay) and time.time() < deadline:
        time.sleep(0.15)
        report(dbg, lay)
    return lay


def click(dbg, win, x, y):
    c = win["content"]
    dbg.send("gui click %d %d" % (c["x"] + x, c["y"] + y))
    dbg.settle(0.5)


def near(p, q, tol=40):
    return all(abs(a - b) <= tol for a, b in zip(p, q))


def has_colour(im, box, colour):
    x, y, w, h = box
    for yy in range(y, y + h):
        for xx in range(x, x + w):
            if near(im.getpixel((xx, yy)), colour):
                return True
    return False


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "logview_test")

    # FIRST, before this tool holds the serial console vm.py needs. OVER
    # TFTP (`vm.py put`), not DebugConsole.write_lines(): that one
    # types the text through `tosh -c echo`, and a level's `<3>` is an
    # input redirection there -- the file never appeared.
    host = os.path.join(args.tmp, f"logview_fixture_{os.getpid()}.log")
    with open(host, "w") as f:
        f.write("\n".join(LINES) + "\n")
    put = subprocess.run([sys.executable, os.path.join(HERE, "vm.py"), "--instance", str(args.instance),
                          "put", host, FIXTURE], capture_output=True, text=True)
    os.unlink(host)
    if not check("the fixture log reached the guest (vm.py put)", put.returncode == 0,
                 (put.stdout + put.stderr).strip()[-200:]):
        return finish()

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("the Log Viewer on a fixture log (lib/ulogset.h, ui/uui_loglist.h)")

    dbg.send(f"sh rm {CONF}")

    dbg.logs("logview:", clear=True)
    win = dbg.spawn(f"{APP} {FIXTURE}", TITLE)
    lay = wait(dbg, {}, lambda l: "log.rows_area" in l and l.get("lines") == len(LINES))
    if not check("it opens the file and reports it", win and lay.get("lines") == len(LINES),
                 f"lines={lay.get('lines')} source={lay.get('source')} keys={sorted(lay)[:16]}"):
        return finish(dbg)

    # DRAWN, in severity colours, read from a settled frame.
    from PIL import Image
    shot = os.path.join(args.tmp, f"logview_{os.getpid()}.png")
    qmp.screenshot(shot)
    im = Image.open(shot).convert("RGB")
    c = win["content"]
    rx, ry, rw, rh_area = lay["log.rows_area"]
    row_h = lay.get("log.row_h", 20)
    lx, _, lw, _ = lay["log.col_level"]

    def level_cell(row):
        return (c["x"] + lx, c["y"] + ry + row * row_h + 2, lw, row_h - 4)

    check("the error row's level is drawn in the error red", has_colour(im, level_cell(0), ERR_RED))
    check("the warning row's level is drawn in the warning amber", has_colour(im, level_cell(1), WARN_AMBER))
    check("...and an ordinary row in neither",
          not has_colour(im, level_cell(2), ERR_RED) and not has_colour(im, level_cell(2), WARN_AMBER))

    # Errors only, then the repeat count of the error.
    if "show.slot1" in lay:
        x, y, w, h = lay["show.slot1"]
        click(dbg, win, x + w // 2, y + h // 2)
    lay = wait(dbg, lay, lambda l: l.get("show") == 1 and l.get("shown") == 2)
    check("Errors shows exactly the two errors", lay.get("show") == 1 and lay.get("shown") == 2,
          f"show={lay.get('show')} shown={lay.get('shown')}")
    click(dbg, win, rx + rw // 2, ry + row_h // 2)
    lay = wait(dbg, lay, lambda l: l.get("sel", -1) >= 0)
    check("choosing the error reports it was seen twice", lay.get("seen") == 2,
          f"sel={lay.get('sel')} seen={lay.get('seen')}")

    # Only this message, then back to everything.
    for name, want in (("only", 2), ("around", len(LINES))):
        if name in lay:
            x, y, w, h = lay[name]
            click(dbg, win, x + w // 2, y + h // 2)
        lay = wait(dbg, lay, lambda l, want=want: l.get("shown") == want)
    check("Only this message, then Show the lines around it", lay.get("shown") == len(LINES),
          f"shown={lay.get('shown')} show={lay.get('show')}")

    # Mute the selected line's subsystem, kept in /etc.
    if "mute" in lay:
        x, y, w, h = lay["mute"]
        click(dbg, win, x + w // 2, y + h // 2)
    lay = wait(dbg, lay, lambda l: l.get("mutes") == 1)
    check("Mute hides the test lines", lay.get("mutes") == 1 and lay.get("shown") == 1,
          f"mutes={lay.get('mutes')} shown={lay.get('shown')}")
    conf = dbg.send(f"sh cat {CONF}")
    check("...and is kept in /etc/logview.conf", "mute=test" in conf, conf.strip()[-120:])
    lay = wait(dbg, lay, lambda l: "unmute" in l)
    if "unmute" in lay:
        x, y, w, h = lay["unmute"]
        click(dbg, win, x + w // 2, y + h // 2)
    lay = wait(dbg, lay, lambda l: l.get("mutes") == 0)
    conf = dbg.send(f"sh cat {CONF}")
    check("Unmute shows them again and forgets it", lay.get("mutes") == 0 and lay.get("shown") == len(LINES)
          and "mute=" not in conf, f"mutes={lay.get('mutes')} shown={lay.get('shown')} conf={conf.strip()[-80:]}")
    return finish(dbg)


def finish(dbg=None):
    if dbg:
        w = dbg.window(TITLE)
        if w:
            dbg.send(f"gui close {w['z']}")
        dbg.send(f"sh rm {FIXTURE}")
    return _res.finish("logview_test")


if __name__ == "__main__":
    sys.exit(main())
