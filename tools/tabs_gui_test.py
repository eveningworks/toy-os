#!/usr/bin/env python3
"""Tabs in the File Manager's title bar: drawn by the compositor
(WIN_REQ_TABS), decided by the app (fm_tabs.c). Every click lands on the
rect the compositor exports in `gui windows --json` -- never a formula.

1. **The window opens with one tab, named for its folder.**
2. **Ctrl+T adds a tab and chooses it.**
3. **A tab's label follows its folder**, and the OTHER tab's does not.
4. **Clicking a tab goes back to its folder**, and Back there does not
   wander into the other tab's folders -- each tab has its own history.
5. **A tab's close box closes that tab only.**
6. **The "+" makes a tab; closing the last one closes the window.**
"""
import argparse
import sys
import time

import filemanager_test as fm
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

START = "/usr/share/wallpapers"
K_CTRL_T, K_CTRL_W = "0x14", "0x17"


def run(dbg, qmp, res):
    dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=0", "left_view=details"])
    win = dbg.spawn(f"{fm.SPAWN_PATH} {START}", fm.TITLE)
    res.check("the File Manager opens", win is not None, "no window")
    if not win:
        return

    def me():
        ws = [w for w in dbg.windows() if w["title"] == fm.TITLE]
        return ws[0] if ws else None

    def tabs(pred, timeout=10.0):
        end = time.time() + timeout
        w = me()
        while time.time() < end:
            w = me()
            if w and pred(w):
                return w
            time.sleep(0.3)
        return w

    def labels(w):
        return [t["label"] for t in (w or {}).get("tabs", [])]

    def click(r):
        dbg.send(f"gui click {r['x'] + r['w'] // 2} {r['y'] + r['h'] // 2}")
        dbg.settle()

    # 1.
    w = tabs(lambda w: len(w.get("tabs", [])) == 1)
    res.check("it opens with one tab, named for its folder",
              labels(w) == ["wallpapers"] and w["active_tab"] == 0, f"tabs={labels(w)}")

    def goto(path):
        dbg.key(fm.K_CTRL_L)
        fm.wait_layout(dbg, win, lambda l: l.pathedit == 1)
        for ch in path:
            dbg.key("0x2f" if ch == "/" else ch)
        dbg.key(fm.K_ENTER)
        return fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == path)

    # Tab 0 gets a step of its own to go Back to: wallpapers, then /usr.
    goto("/usr")

    # 2.
    dbg.key(K_CTRL_T)
    w = tabs(lambda w: len(w.get("tabs", [])) == 2)
    res.check("Ctrl+T adds a tab and chooses it",
              len(labels(w)) == 2 and w["active_tab"] == 1, f"tabs={labels(w)} active={w and w['active_tab']}")

    # 3.
    goto("/home")
    w = tabs(lambda w: labels(w) == ["usr", "home"])
    res.check("a tab's label follows its folder, and the other tab's does not",
              labels(w) == ["usr", "home"], f"tabs={labels(w)}")

    # 4. Back in tab 0 must reach ITS wallpapers; a history shared with tab
    #    1 ([/usr, /home]) would step from /usr to /usr and stay.
    click(w["tabs"][0])
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == "/usr") or fm.layout_now(dbg, win)
    w = tabs(lambda w: w["active_tab"] == 0)
    res.check("clicking a tab goes back to its folder",
              lay.dir.get(0) == "/usr" and w["active_tab"] == 0, f"dir={lay.dir.get(0)!r} active={w['active_tab']}")
    dbg.key(fm.K_LEFT, mods="alt")
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == START) or fm.layout_now(dbg, win) or fm.last_layout()
    res.check("...and Back there walks its OWN history (to wallpapers)",
              lay.dir.get(0) == START, f"after Back dir={lay.dir.get(0)!r}")

    # 5.
    w = me()
    c = w["tabs"][1]["close"]
    dbg.send(f"gui click {c['x'] + c['size'] // 2} {c['y'] + c['size'] // 2}")
    dbg.settle()
    w = tabs(lambda w: len(w.get("tabs", [])) == 1)
    res.check("a tab's close box closes that tab only",
              labels(w) == ["wallpapers"], f"tabs={labels(w)}")

    # 6.
    n = w["new_tab"]
    dbg.send(f"gui click {n['x'] + n['size'] // 2} {n['y'] + n['size'] // 2}")
    dbg.settle()
    w = tabs(lambda w: len(w.get("tabs", [])) == 2)
    made = len(labels(w)) == 2
    dbg.key(K_CTRL_W)
    tabs(lambda w: len(w.get("tabs", [])) == 1)
    dbg.key(K_CTRL_W)
    end = time.time() + 10
    while time.time() < end and me():
        time.sleep(0.3)
    res.check("the '+' makes a tab; closing the last tab closes the window",
              made and me() is None, f"made={made} still open={me() is not None}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "tabs_gui_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, res)
    finally:
        dbg.close()
    print(f"\ntabs_gui_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
