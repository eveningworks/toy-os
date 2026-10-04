"""tools/notepad_options_test.py -- Notepad Options: saved, applied, remembered.

Drives the Options window (Edit > Options..., over the shared
ui/uui_prefs.h dialog) and then asserts on what the options DO, not on
the window having drawn:

  * OK writes /etc/notepad.conf, only the keys that moved;
  * the status bar going off gives the text its height (the text rect grows);
  * Tab types spaces to the next 8-column stop, and Save trims the
    trailing spaces typed -- read back with `cat`, an independent path;
  * the saved file is kept in /var/lib/notepad/recent;
  * "Reopen the last tabs": closing the window and opening Notepad again
    brings the file back;
  * the Markdown preview set to Always and Below sits UNDER the text.

What a broken version would still pass: none of these is satisfied by
the window merely opening -- each reads the file system or the app's
own layout after the option took effect.

    python3 tools/notepad_options_test.py [--instance N] [--in-gui]
"""
import argparse
import sys
import time

sys.path.insert(0, "tools")
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

NOTEPAD = "/bin/wm/apps/notepad"
CONF = "/etc/notepad.conf"
DOC = "/var/tmp/npo.txt"
F10, RIGHT, TAB, ENTER, CTRL_S = "0xA4", "0x96", "0x09", "0x0d", "0x13"


def rect(dbg, prefix, key):
    for line in reversed(dbg.logs(f"{prefix}: layout {key} ", clear=False)):
        try:
            return [int(v) for v in line.split(f"layout {key} ", 1)[1].split()[:4]]
        except ValueError:
            return None
    return None


def window(dbg, part, timeout=12.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        for w in dbg.windows():
            if part in w.get("title", ""):
                return w
        time.sleep(0.3)
    return None


def click_rect(dbg, win, r):
    c = win["content"]
    dbg.send(f"gui click {c['x'] + r[0] + r[2] // 2} {c['y'] + r[1] + r[3] // 2}")
    dbg.settle(0.4)


def open_options(dbg, res):
    dbg.logs("options: layout", clear=True)
    for k in (F10, RIGHT, "o"):
        dbg.send(f"gui key {k}")
        dbg.settle(0.3)
    ow = window(dbg, "Notepad Options")
    deadline = time.time() + 6
    while ow and time.time() < deadline and not rect(dbg, "options", "pages"):
        time.sleep(0.3)
    res.check("Edit > Options... opens Notepad Options", ow is not None and
              rect(dbg, "options", "pages") is not None,
              f"windows {[w['title'] for w in dbg.windows()]}")
    return ow


def page(dbg, ow, n):
    pg = rect(dbg, "options", "pages")
    rh = None
    for line in reversed(dbg.logs("options: layout pages.row_h ", clear=False)):
        rh = int(line.split("pages.row_h", 1)[1].split()[0])
        break
    rh = rh or 20
    c = ow["content"]
    dbg.send(f"gui click {c['x'] + pg[0] + 30} {c['y'] + pg[1] + n * rh + rh // 2}")
    dbg.settle(0.5)


def run(dbg, res):
    dbg.send(f"sh rm {CONF}")
    dbg.send(f"sh rm {DOC}")
    dbg.send(f"sh cp /etc/shells {DOC}")
    dbg.send(f"gui spawn {NOTEPAD} {DOC}")
    win = window(dbg, "npo.txt")
    if not win:
        res.check("Notepad opens the fixture", False, "no window")
        return
    dbg.settle(1.0)
    t0 = rect(dbg, "notepad", "text")

    ow = open_options(dbg, res)
    if not ow:
        return
    for key in ("tabwidth.slot 2", "tabspaces"):          # Editor: 8, spaces
        r = rect(dbg, "options", key)
        if r:
            click_rect(dbg, ow, r)
    page(dbg, ow, 1)                                       # View
    r = rect(dbg, "options", "statusbar")
    if r:
        click_rect(dbg, ow, r)
    page(dbg, ow, 2)                                       # Files and startup
    for key in ("startup.slot 1", "trim"):
        r = rect(dbg, "options", key)
        if r:
            click_rect(dbg, ow, r)
    ok = rect(dbg, "options", "ok")
    if ok:
        click_rect(dbg, ow, ok)
    time.sleep(1.0)

    conf = (dbg.send(f"sh cat {CONF}") or "").replace(" ", "")
    for want in ("tab_width=8", "tab_spaces=on", "status_bar=off", "reopen=on",
                 "trim_trailing=on"):
        res.check(f"OK writes {want}", want in conf, f"{CONF}: {conf!r}")
    res.check("...and only the keys that moved", "line_numbers" not in conf, f"{conf!r}")

    dbg.settle(0.8)
    t1 = rect(dbg, "notepad", "text")
    res.check("the status bar going off gives the text its height",
              bool(t0 and t1) and t1[3] > t0[3], f"text h {t0 and t0[3]} -> {t1 and t1[3]}")

    for k in ("x", TAB, "y", "0x20", "0x20", ENTER):
        dbg.send(f"gui key {k}")
    dbg.settle(0.5)
    dbg.send(f"gui key {CTRL_S}")
    time.sleep(1.5)
    first = ((dbg.send(f"sh cat {DOC}") or "").splitlines() or [""])
    first = next((ln for ln in first if ln.startswith("x")), "")
    res.check("Tab types spaces to the 8-column stop, and Save trims the trailing ones",
              first == "x       y", f"first line {first!r}")

    recent = dbg.send("sh cat /var/lib/notepad/recent") or ""
    res.check("the saved file is kept in Recent across runs", DOC in recent, f"{recent!r}")
    res.check("...once: opening then saving one file is ONE recent entry",
              recent.count(DOC) == 1 and "(empty)" not in recent, f"{recent!r}")

    # REOPEN: close the window, open Notepad again, and the tab is back.
    w = window(dbg, "npo.txt")
    if w:
        dbg.send(f"gui close {w['z']}")
    deadline = time.time() + 8
    while time.time() < deadline and window(dbg, "npo.txt", timeout=0.3):
        time.sleep(0.3)
    dbg.logs("notepad: reopened", clear=True)
    dbg.open_app("Notepad")
    back = window(dbg, "npo.txt", timeout=10)
    res.check("Reopen the last tabs brings the file back in a new Notepad", back is not None,
              f"windows {[x['title'] for x in dbg.windows()]}")
    if not back:
        return

    # PREVIEW ALWAYS, BELOW: the Markdown pane sits under the text.
    dbg.settle(1.0)
    ow = open_options(dbg, res)
    if not ow:
        return
    page(dbg, ow, 1)
    for key in ("preview.slot 1", "previewpos.slot 1"):
        r = rect(dbg, "options", key)
        if r:
            click_rect(dbg, ow, r)
    ok = rect(dbg, "options", "ok")
    if ok:
        click_rect(dbg, ow, ok)
    time.sleep(1.2)
    dbg.settle(0.8)
    t = rect(dbg, "notepad", "text")
    md = rect(dbg, "notepad", "markdown")
    res.check("Preview Always + Below puts the preview UNDER the text",
              bool(t and md) and md[1] >= t[1] + t[3] and md[0] <= t[0] + 8,
              f"text {t} markdown {md}")
    dbg.send(f"sh rm {CONF}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true",
                    help="the VM already shows the desktop")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "notepad_options_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    res = Results()
    print("notepad_options_test: checks")
    try:
        run(dbg, res)
    finally:
        dbg.close()
        qmp.close()
    print(f"\nnotepad_options_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
