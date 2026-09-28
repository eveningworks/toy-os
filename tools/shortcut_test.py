#!/usr/bin/env python3
"""Global keyboard shortcuts: the bindings, the Super gesture, and rebinding.

**THE COMPOSITOR OWNS THESE, so every check drives the compositor and
asserts on what it LAUNCHED**, not on a window appearing -- a window is
also what you get from clicking the Start menu, and this is about the
key path. `wm: shortcut -> <command>` is the app's own report and names
the command, so a binding that fires the wrong program fails loudly.

Four things here have bitten and are each pinned by a check:

- **THE COMMAND PATHS DRIFT FROM THE .desktop ENTRIES.** Two of the four
  were wrong the first time this ran (Terminal is `uterm`, Task Manager
  lives under wm/system) and the symptom was `pid -1` in a log nobody was
  reading. `check_commands_exist` compares the table against the disk.
- **SUPER IS A MODIFIER AND A KEY.** Tapping it opens the Start menu;
  holding it over another key must NOT, or every Super shortcut opens the
  menu on the way out.
- **THE CAPTURE CONTROL NEEDS THE COMPOSITOR TO STAND DOWN.** Binding
  Super+E means receiving Super+E, which the compositor would otherwise
  eat. Checked by capturing a combination that IS already bound.
- **usetting_get() RETURNS 1 FOR SUCCESS.** Reading it as a syscall
  result made every binding fall through to its compiled-in fallback, so
  a rebinding was stored, reloaded and ignored. The rebinding check is
  end to end for that reason: Apply, then press the NEW key.
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402

SETTINGS_EXEC = "/bin/wm/system/settings"
CONF = "/etc/shortcuts.conf"

# What the defaults should launch. Kept here as the OTHER copy on
# purpose: this file and kernel/lib/shortcut_actions.c disagreeing is
# exactly the drift the first check exists to catch.
DEFAULTS = [
    ("file_manager", "Super+E",       "gui key e super",        "/bin/wm/apps/files"),
    ("terminal",     "Ctrl+Alt+T",    "gui key 0x14 ctrl alt",  "/bin/wm/apps/uterm"),
    ("screenshot",   "Shift+Super+S", "gui key S shift super",  "/bin/wm/apps/screenshot"),
    ("screenshot",   "Print Screen",  "gui key 0xB9",           "/bin/wm/apps/screenshot"),
    ("task_manager", "Ctrl+Alt+Del",  "gui key 0x99 ctrl alt",  "/bin/wm/system/taskmgr"),
]


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, what, ok, detail=""):
        (self.passes if ok else self.fails).append(what)
        print(f"  {'PASS' if ok else 'FAIL'}  {what}")
        if not ok and detail:
            print(f"        {detail}")


# WHERE SETTINGS SAYS ITS CONTROLS ARE, content-relative: one `control`
# line per slot when a page is drawn, and one `layout button` line per
# button. Clicked from these rather than from fixed pixels -- the fixed
# ones missed every control once the pages were reorganised, which read
# as "capture does not suppress the shortcut" and "Apply does not
# persist" (docs/bugs.md) while the clicks landed on nothing.
CONTROL_RE = r"settings: control \d+ (\S+) (-?\d+) (-?\d+) (-?\d+) (-?\d+) "
BUTTON_RE = r"settings: layout button (\w+) (-?\d+) (-?\d+) (-?\d+) (-?\d+)"


def settings_rects(dbg):
    """{name: (x, y, w, h)} as the app last reported them.

    Off the console WIRE (logs()), never `sh dmesg`: the log ring holds
    ~14 KB, and every window poll's JSON reply pushes an app's lines out
    of it within seconds.
    """
    rects = {}
    for line in dbg.logs("settings: ", clear=False):
        m = re.search(CONTROL_RE, line) or re.search(BUTTON_RE, line)
        if m:
            rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    return rects


def click_named(dbg, res, content, rects, suffix):
    """Click the centre of the control whose name ends in `suffix`."""
    hit = [r for name, r in rects.items() if name == suffix or name.endswith("." + suffix)]
    if not hit:
        res.check(f"Settings reports where {suffix} is", False,
                  f"reported: {sorted(rects)}")
        return False
    x, y, w, h = hit[-1]
    dbg.click(content["x"] + x + w // 2, content["y"] + y + h // 2)
    return True


def fired(dbg, wait=2.5):
    """The commands the compositor launched since the last call."""
    time.sleep(wait)
    out = []
    for line in dbg.logs("wm: shortcut ->", clear=True):
        m = re.search(r"shortcut -> (\S+)", line)
        if m:
            out.append(m.group(1))
    return out


def menu_open(dbg):
    for line in (dbg.send("gui state") or "").splitlines():
        if "start_menu=" in line:
            return line.split("start_menu=")[1].split()[0] in ("1", "true")
    return None


def check_commands_exist(dbg, res):
    """Every command a shortcut can launch is actually on the disk.

    THE CHECK THAT WOULD HAVE CAUGHT THE ORIGINAL BUG. A binding naming a
    path that is not there fails as `pid -1` in a log, which is to say it
    fails as nothing at all.
    """
    missing = []
    for _, _, _, cmd in DEFAULTS:
        out = dbg.send(f"sh ls {cmd}") or ""
        if "no such file" in out.lower() or "cannot access" in out.lower():
            missing.append(cmd)
    res.check("every bound command exists on the disk", not missing,
              f"missing: {missing} -- a shortcut naming one of these fires "
              "and launches nothing, with only a `pid -1` in the log")


def run(dbg, qmp, res):
    dbg.send(f"sh rm {CONF}")          # start from the compiled-in defaults
    check_commands_exist(dbg, res)

    # --- 1. each default binding launches its own program -------------
    for name, spelling, keycmd, cmd in DEFAULTS:
        dbg.logs("wm: shortcut ->", clear=True)
        dbg.send(keycmd)
        got = fired(dbg)
        res.check(f"{spelling} launches {os.path.basename(cmd)}",
                  cmd in got,
                  f"{name}: launched {got or 'nothing'}")

    # --- 2. an UNBOUND combination launches nothing -------------------
    # The control for every check above: if the matcher fired on anything
    # at all, they would all pass without meaning it.
    dbg.logs("wm: shortcut ->", clear=True)
    dbg.send("gui key q super")
    res.check("control: an unbound combination launches nothing",
              not fired(dbg), "Super+Q is bound to nothing")

    # --- 3. Super alone is the Start menu; Super HELD is not ----------
    dbg.send("gui key 0xA6"); dbg.send("gui key 0xA6 up"); time.sleep(1.5)
    res.check("tapping Super opens the Start menu", menu_open(dbg) is True,
              f"start_menu={menu_open(dbg)}")
    dbg.send("gui key 0xA6"); dbg.send("gui key 0xA6 up"); time.sleep(1.5)
    res.check("...and tapping it again closes it", menu_open(dbg) is False,
              f"start_menu={menu_open(dbg)}")

    # **THE ONE THAT MAKES SUPER+E POSSIBLE AT ALL.** Super used to act
    # on the PRESS, so every Super shortcut would open the menu too.
    dbg.logs("wm: shortcut ->", clear=True)
    dbg.send("gui key 0xA6")
    dbg.send("gui key e super")
    dbg.send("gui key 0xA6 up")
    got = fired(dbg)
    res.check("Super HELD over another key does not open the Start menu",
              menu_open(dbg) is False and "/bin/wm/apps/files" in got,
              f"start_menu={menu_open(dbg)} launched={got}")


def run_rebinding(dbg, qmp, res):
    """Rebind through System Settings, and press the new key."""
    dbg.send(f"gui spawn {SETTINGS_EXEC}")
    deadline = time.time() + 25
    win = None
    while time.time() < deadline and win is None:
        time.sleep(0.5)
        win = dbg.window("System Settings")
    if win is None:
        res.check("System Settings opened", False, "no window")
        return
    content = win["content"]

    # Reach the Shortcuts page BY SEARCHING, which needs no pixel: typing
    # the category's name filters the sidebar to it, and the first match
    # opens (settings.c's search_changed). This clicked a fixed (150, 211)
    # after a blind scroll, which stopped landing on the row.
    search = None
    for line in dbg.logs("settings: layout search", clear=False):
        m = re.search(r"settings: layout search (-?\d+) (-?\d+) (\d+) (\d+)", line)
        if m:
            search = [int(v) for v in m.groups()]
    row = None
    if search:
        dbg.send(f"gui click {content['x'] + search[0] + search[2] // 2} "
                 f"{content['y'] + search[1] + search[3] // 2}")
        time.sleep(0.5)
        for ch in "shortcuts":
            dbg.send(f"gui key {ord(ch)}")
        time.sleep(2)
    page = [l for l in (dbg.send("sh dmesg") or "").splitlines()
            if "settings: page Shortcuts" in l]
    res.check("the Shortcuts page lists every action",
              any("slots 4" in l for l in page),
              f"page line: {page[-1] if page else row or 'none'}")

    # **CAPTURE A COMBINATION THAT IS ALREADY BOUND.** Without the
    # compositor standing down (WIN_REQ_INHIBIT_SHORTCUTS) this launches
    # a file manager instead of recording, which is the whole reason the
    # inhibitor exists.
    rects = settings_rects(dbg)
    before = len([l for l in (dbg.send("gui windows") or "").splitlines()
                  if "File Manager" in l])
    dbg.logs("wm: shortcut ->", clear=True)
    # THE SCREENSHOT ROW, NOT THE TERMINAL ONE: the fourth row sits below
    # the page's scroll viewport at the default window size, so a press
    # there is clipped away and nothing arms -- and the File Manager it
    # then launched covered the window for every click after it.
    if not click_named(dbg, res, content, rects, "screenshot"):
        return
    time.sleep(1.5)
    dbg.send("gui key e super")
    got = fired(dbg)
    after = len([l for l in (dbg.send("gui windows") or "").splitlines()
                 if "File Manager" in l])
    res.check("capturing an ALREADY BOUND combination records it, "
              "and does not fire it",
              not got and after == before,
              f"launched {got}, file managers {before} -> {after} -- the "
              "compositor did not stand down for the capture")

    # Esc cancels, so the page is left as it was found.
    dbg.send("gui key 0x1B")
    time.sleep(1)

    # --- the end-to-end rebinding -------------------------------------
    if not click_named(dbg, res, content, rects, "task_manager"):
        return
    time.sleep(1.2)
    dbg.send("gui key 0x0D ctrl alt")   # Ctrl+Alt+M
    time.sleep(1.5)
    if not click_named(dbg, res, content, rects, "apply"):
        return
    time.sleep(3)
    conf = dbg.send(f"sh cat {CONF}") or ""
    res.check("Apply writes the new binding to /etc/shortcuts.conf",
              "task_manager=Ctrl+Alt+M" in conf.replace(" ", ""),
              f"{CONF}: {conf.strip()[:120]}")

    # **PRESS IT.** Storing a binding the compositor never reloads is the
    # exact shape of the usetting_get() bug -- everything above passed
    # while the key did nothing.
    dbg.logs("wm: shortcut ->", clear=True)
    dbg.send("gui key 0x0D ctrl alt")
    res.check("...and the compositor reloads, so the NEW key works",
              "/bin/wm/system/taskmgr" in fired(dbg),
              "stored and reloaded but the key does nothing")

    dbg.logs("wm: shortcut ->", clear=True)
    dbg.send("gui key 0x99 ctrl alt")
    res.check("...and the OLD key no longer does", not fired(dbg),
              "Ctrl+Alt+Delete still launches the task manager")

    dbg.send(f"sh rm {CONF}")        # leave the machine as it was found


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true",
                    help="the VM already shows the desktop")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "shortcut_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    res = Result()
    print("shortcut_test: checks")
    try:
        run(dbg, qmp, res)
        run_rebinding(dbg, qmp, res)
    finally:
        dbg.close()
    print(f"\nshortcut_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
