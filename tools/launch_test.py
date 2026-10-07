#!/usr/bin/env python3
"""Running programs and scripts from the File Manager, the desktop and
`open` (lib/ulaunch.h, ui/uui_runask.h, /bin/wm/system/runask, Terminal's
`-e`). Each act is proved by its EFFECT -- the script touches a marker
file, the Terminal logs the exit status, the mode is read back with
`stat` -- never by the card having closed.

1. **Enter on an executable script asks**: "Run hello.sh?" with its
   first lines, Run in Terminal / Run / Open in Notepad / Always.
2. **Run** runs it with no Terminal window.
3. **Run in Terminal** runs it in a Terminal that stays open with the
   exit status; **Ctrl+D closes** that window.
4. **"Always do this" + Run** writes application/x-shellscript=run to
   /etc/mimeapps.conf, and the next Enter runs it without asking.
5. **Shift+Enter** runs it in a Terminal without asking.
6. **The context menu's first row is Run in Terminal** for a script --
   it starts a Terminal, where Open would have asked.
7. **A script without an execute bit** gets "Allow running, and run",
   which sets the bit (stat says 0755) and runs it.
8. **An ELF program** is asked about as a program: no Notepad, no
   preview; Esc cancels.
9. **`open` on an app** starts it at once -- no card.
10. **`open` on a script** asks through the runask program's window.
"""
import argparse
import sys
import time

import filemanager_test as fm
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

DIR = "/home/lt"
SCRIPT = f"{DIR}/hello.sh"
NOX = f"{DIR}/nox.sh"
PROG = f"{DIR}/uptime"
MARK = f"{DIR}/ran"
MIME = "/etc/mimeapps.conf"
SAVED = "/var/tmp/lt-mimeapps.conf"
CALC = "/bin/wm/apps/calculator"
K_CTRL_D = "0x04"
K_ESC = "0x1b"


def run(dbg, qmp, res):
    def sh(cmd):
        return dbg.send(f"sh {cmd}") or ""

    def rect(key):
        """`key`'s rect in the LAST card's layout block. A block is logged
        only when it differs from the last one, so a card identical to
        the previous one logs nothing -- the old block is still its
        geometry, and a key absent from it is absent from the card."""
        lines = dbg.logs("runask: layout ", clear=False)
        starts = [i for i, ln in enumerate(lines) if "runask: layout icon " in ln]
        for line in lines[starts[-1] if starts else 0:]:
            if f"runask: layout {key} " in line:
                try:
                    return [int(v) for v in line.split(f"layout {key} ", 1)[1].split()[:4]]
                except ValueError:
                    return None
        return None

    def window(part, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            for w in dbg.windows():
                if part in w.get("title", ""):
                    return w
            time.sleep(0.3)
        return None

    def gone(part, timeout=8.0):
        end = time.time() + timeout
        while time.time() < end:
            if not any(part in w.get("title", "") for w in dbg.windows()):
                return True
            time.sleep(0.3)
        return False

    def marked(timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            if "ran" in sh(f"ls {DIR}").split():
                return True
            time.sleep(0.4)
        return False

    def finished(path, timeout=12.0):
        end = time.time() + timeout
        while time.time() < end:
            if any(f"{path} finished, status 0" in ln for ln in dbg.logs("uterm:", clear=False)):
                return True
            time.sleep(0.4)
        return False

    def click(win, r):
        c = win["content"]
        dbg.send(f"gui click {c['x'] + r[0] + r[2] // 2} {c['y'] + r[1] + r[3] // 2}")
        dbg.settle(0.4)

    def fresh():
        """Each step from a clean start: no marker, no Terminal lines, and
        NO CARD LEFT OPEN by a step that failed -- its default button
        would take the next step's Enter and pass it."""
        for w in dbg.windows():
            if w.get("title", "").startswith("Run "):
                dbg.key(K_ESC)
                gone(w["title"])
        sh(f"rm {MARK}")
        dbg.logs("uterm:", clear=True)

    def card(name):
        """Wait for "Run <name>?" and its buttons' report."""
        w = window(f"Run {name}?")
        end = time.time() + 8
        while w and time.time() < end and not rect("cancel"):
            time.sleep(0.3)
        return w

    def close_terminal():
        """False when there was no Terminal to close."""
        if not window("Terminal", timeout=2.0):
            return False
        dbg.key(K_CTRL_D)
        return gone("Terminal")

    # Fixtures: a script, the same without an execute bit, a program.
    sh(f"rm -r {DIR}")
    sh(f"mkdir {DIR}")
    sh(f"cp {MIME} {SAVED}")
    sh(f"rm {MIME}")
    body = ["#!/bin/dash", "# says hello and leaves a mark", "echo hello from hello.sh", f"touch {MARK}"]
    dbg.write_lines(SCRIPT, body)
    dbg.write_lines(NOX, body)
    sh(f"chmod 755 {SCRIPT}")
    sh(f"chmod 644 {NOX}")
    sh(f"cp /bin/uptime {PROG}")
    sh(f"chmod 755 {PROG}")
    dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=0", "left_view=details", "details_pane=0"])

    win = dbg.spawn(f"{fm.SPAWN_PATH} {DIR}", fm.TITLE)
    res.check("the File Manager opens", win is not None, "no window")
    if not win:
        return

    def select(name):
        dbg.key(fm.K_HOME)
        for ch in name:
            dbg.key(ch)
        return fm.wait_layout(dbg, win, lambda l: l.selected == name) or fm.layout_now(dbg, win)

    # 1. Enter asks
    fresh()
    select("hello.sh")
    dbg.key(fm.K_ENTER)
    w = card("hello.sh")
    want = ("what", "head", "terminal", "run", "edit", "always", "cancel")
    have = {k: rect(k) for k in want}
    res.check("Enter on an executable script asks, with its first lines and every choice",
              w is not None and all(have.values()), f"card={w is not None} rects={have}")

    # 2. Run
    if w and have["run"]:
        click(w, have["run"])
    ran = marked()
    res.check("Run runs it, with no Terminal", gone("Run hello.sh?") and ran and
              window("Terminal", timeout=1.5) is None, f"marker={ran}")

    # 3. Run in Terminal, held, then Ctrl+D
    fresh()
    dbg.key(fm.K_ENTER)
    w = card("hello.sh")
    t = rect("terminal")
    if w and t:
        click(w, t)
    term = window("Terminal")
    fin = finished(SCRIPT)
    held = term is not None and window("Terminal", timeout=1.0) is not None
    res.check("Run in Terminal runs it and the window stays with the exit status",
              term is not None and fin and marked() and held,
              f"terminal={term is not None} finished={fin} uterm={dbg.logs('uterm:', clear=False)[-3:]}")
    res.check("Ctrl+D closes the finished Terminal", close_terminal(), "still open")

    # 4. Always + Run
    fresh()
    dbg.key(fm.K_ENTER)
    w = card("hello.sh")
    a, r = rect("always"), rect("run")
    if w and a and r:
        click(w, a)
        click(w, r)
    marked()
    conf = sh(f"cat {MIME}").replace(" ", "")
    fresh()
    dbg.key(fm.K_ENTER)
    asked = window("Run hello.sh?", timeout=3.0) is not None
    ran = marked()
    res.check("'Always do this' + Run is kept, and the next Enter runs without asking",
              "application/x-shellscript=run" in conf and not asked and ran,
              f"conf={conf!r} asked={asked} marker={ran}")
    sh(f"rm {MIME}")

    # 5. Shift+Enter
    fresh()
    dbg.key(fm.K_ENTER, mods="shift")
    asked = window("Run hello.sh?", timeout=2.0) is not None
    fin = finished(SCRIPT)
    res.check("Shift+Enter runs it in a Terminal without asking", not asked and fin,
              f"asked={asked} finished={fin}")
    close_terminal()

    # 6. the context menu's first row
    fresh()
    lay = select("hello.sh")
    names = sorted(n.rstrip("/") for n in fm.listing(dbg, DIR))
    view_row = 1 + names.index("hello.sh") if "hello.sh" in names else None
    m = None
    if lay and view_row is not None and 0 in lay.pane and 0 in lay.rowy:
        px, py, pw, ph = lay.pane[0]
        fm.sure_rclick(dbg, qmp, lay.ox + px + pw // 2,
                       lay.oy + lay.rowy[0] + lay.rowh * view_row + lay.rowh // 2)
        m = fm.wait_layout(dbg, win, lambda l: l.ctx == 1 and 0 in l.popitems)
    if m:
        x, y, w_, h = m.popitems[0]
        fm.sure_click(dbg, qmp, m.ox + x + w_ // 2, m.oy + y + h // 2)
    asked = window("Run hello.sh?", timeout=2.0) is not None
    fin = finished(SCRIPT)
    res.check("a script's context menu starts with Run in Terminal", m is not None and not asked and fin,
              f"menu={m is not None} asked={asked} finished={fin}")
    close_terminal()

    # 7. no execute bit
    fresh()
    select("nox.sh")
    dbg.key(fm.K_ENTER)
    w = card("nox.sh")
    allow, term = rect("allow"), rect("terminal")
    if w and allow:
        click(w, allow)
    fin = finished(NOX)
    mode = sh(f"stat {NOX}")
    res.check("a script without an execute bit is offered 'Allow running, and run', which sets it",
              w is not None and allow and not term and fin and "0755" in mode,
              f"card={w is not None} allow={allow} terminal={term} finished={fin} stat={mode!r}")
    close_terminal()

    # 8. an ELF program
    fresh()
    select("uptime")
    dbg.key(fm.K_ENTER)
    w = card("uptime")
    edit, head, term = rect("edit"), rect("head"), rect("terminal")
    dbg.key(K_ESC)
    res.check("a program is asked about with no Notepad and no preview; Esc cancels",
              w is not None and term and not edit and not head and gone("Run uptime?"),
              f"card={w is not None} terminal={term} edit={edit} head={head}")

    # 9. `open` on an app
    sh(f"open {CALC}")
    calc = window("Calculator")
    asked = window("Run calculator?", timeout=1.0) is not None
    res.check("`open` on an app starts it at once", calc is not None and not asked,
              f"calculator={calc is not None} asked={asked}")
    if calc:
        dbg.key("0xf794", mods="alt")   # Alt+F4
        gone("Calculator")

    # 10. `open` on a script: the runask program. Started by the
    # compositor, as the desktop's double-click starts it -- from `sh`
    # its stderr, and so its layout report, would be the shell's.
    fresh()
    dbg.send(f"gui spawn /bin/open {SCRIPT}")
    w = card("hello.sh")
    dbg.key(K_ESC)
    res.check("`open` on a script asks through runask's own window, and Esc cancels",
              w is not None and rect("terminal") and gone("Run hello.sh?") and not marked(2.0),
              f"card={w is not None}")

    sh(f"rm -r {DIR}")
    sh(f"cp {SAVED} {MIME}")
    sh(f"rm {SAVED}")
    sh(f"rm {fm.FILES_CONF}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "launch_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, res)
    finally:
        dbg.close()
    print(f"\nlaunch_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
