#!/usr/bin/env python3
"""The Help app: contents, links, history and full-text search.

WHAT IT CHECKS, and what a broken version would still pass:

  - Help opens on Home (the Getting started overview) and REPORTS its
    layout -- a window that laid out nothing fails here;
  - the page and the contents are DRAWN: ink in each rect of a settled
    frame, because "it responds" is not "it is drawn";
  - the overview's code spans naming pages are LINKS: reported by the
    page as it draws them, and drawn in the accent -- measured against a
    control rect of body text on the same page, which must have none;
  - clicking a link opens that page; Alt+Left goes back and Alt+Right
    forward, and the toolbar's Home returns to the overview;
  - search looks through page TEXT, not just names: a word found in the
    body of a page whose name and title lack it (checked on the host,
    against the page's own source) narrows the contents, and Enter opens
    a page that really contains it; Esc restores the whole contents;
  - a category heading opens a generated page that links its pages, and
    Right/Left expand and collapse it (the tree is lazy: the app rebuilds
    it, so a dead toggle would leave the row count unchanged).

Geometry is the app's own report (`help: layout ...`, `help: link ...`,
`help: here ...`), never re-derived here.
"""
import glob
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HELP = "/bin/wm/apps/help"
TITLE = "Help"
HOME = "overview"
HOME_LINKS = {"kernel", "processes", "filesystem", "desktop", "shell"}
# In ping's DESCRIPTION, and in neither its name nor its title.
QUERY = "icmp"

checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))
    return bool(ok)


_lay = {"links": {}}


def layout(dbg):
    for line in dbg.logs("help:", clear=True):
        m = re.search(r"help: layout (\w+) (-?\d+) (-?\d+) (\d+) (\d+)$", line)
        if m:
            _lay[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
        m = re.search(r'help: here "([^"]*)" sel "([^"]*)" rows (\d+) row_h (\d+) '
                      r'top (\d+) back (\d) fwd (\d)', line)
        if m:
            if m.group(1) != _lay.get("here"):
                _lay["links"] = {}      # a new page's links replace the old
            _lay["here"], _lay["sel"] = m.group(1), m.group(2)
            for k, v in zip(("rows", "row_h", "top", "back", "fwd"), m.groups()[2:]):
                _lay[k] = int(v)
        m = re.search(r"help: link (\S+) (-?\d+) (-?\d+) (\d+) (\d+)", line)
        if m:
            _lay["links"][m.group(1)] = tuple(int(v) for v in m.groups()[1:])
        m = re.search(r'help: search "([^"]*)" matches (\d+)', line)
        if m:
            _lay["query"], _lay["matches"] = m.group(1), int(m.group(2))
    return _lay


def wait(dbg, ok, timeout=6.0):
    deadline = time.time() + timeout
    lay = layout(dbg)
    while not ok(lay) and time.time() < deadline:
        time.sleep(0.1)
        lay = layout(dbg)
    return lay


def window(dbg):
    wins = [w for w in dbg.windows() if w["title"] == TITLE]
    return wins[-1] if wins else None


def click(dbg, win, x, y):
    c = win["content"]
    dbg.send("gui click %d %d" % (c["x"] + x, c["y"] + y))
    dbg.settle(0.5)


def centre(r):
    return r[0] + r[2] // 2, r[1] + r[3] // 2


def ink(im, rect, pred):
    x, y, w, h = rect
    crop = im.crop((x, y, x + w, y + h)).convert("RGB")
    return sum(1 for p in crop.getdata() if pred(p))


def page_source(name):
    for d in ("docs/commands", "data/usr/share/doc/guide"):
        p = os.path.join(REPO, d, name + ".md")
        if os.path.exists(p):
            return open(p, encoding="utf-8").read()
    return ""


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "help_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("help (contents tree, uui_markdown links, history, full-text search)")

    old = window(dbg)
    if old:
        dbg.send(f"gui close {old['z']}")
        dbg.settle(1.0)
    dbg.logs("help:", clear=True)
    win = dbg.spawn(HELP, TITLE)
    lay = wait(dbg, lambda l: all(k in l for k in ("tree", "doc", "search", "tool2"))
               and l.get("here") == HOME and HOME_LINKS <= set(l["links"]))
    if not check("Help opens on Home and reports its layout",
                 win and lay.get("here") == HOME and "doc" in lay,
                 f"here={lay.get('here')} reported {sorted(k for k in lay if k != 'links')}"):
        return finish()
    check("...with nothing to go back or forward to", not lay["back"] and not lay["fwd"],
          f"back={lay['back']} fwd={lay['fwd']}")

    from PIL import Image
    shot = os.path.join(args.tmp, f"help_{os.getpid()}.png")
    qmp.screenshot(shot)
    im = Image.open(shot).convert("RGB")
    c = win["content"]
    at = lambda r: (c["x"] + r[0], c["y"] + r[1], r[2], r[3])
    dark = lambda p: sum(p) < 300
    check("the page is drawn", ink(im, at(lay["doc"]), dark) > 500)
    check("the contents are drawn", ink(im, at(lay["tree"]), dark) > 200)

    # Links: every one the overview names, reported AND drawn blue.
    links = lay["links"]
    check("the overview's page names are links", HOME_LINKS <= set(links),
          f"links {sorted(links)}")
    blue = lambda p: p[2] > p[0] + 60 and p[2] > 120
    lk = links.get("kernel", (0, 0, 0, 0))
    on_link = ink(im, at(lk), blue)
    # Control: the same height of body text, from the page's top-left
    # (the title), which has no link and no accent.
    d = lay["doc"]
    ctl = ink(im, at((d[0] + 8, d[1] + 8, lk[2], lk[3])), blue)
    check("a link is drawn in the accent, body text is not",
          on_link > 10 and ctl == 0, f"link {on_link} px, control {ctl} px")

    # Follow it, then history.
    click(dbg, win, *centre(lk))
    lay = wait(dbg, lambda l: l.get("here") == "kernel")
    check("clicking a link opens its page", lay.get("here") == "kernel",
          f"here={lay.get('here')}")
    check("...and Back is now possible", lay.get("back") == 1)
    dbg.key("0x95", mods="alt")
    lay = wait(dbg, lambda l: l.get("here") == HOME)
    check("Alt+Left goes back", lay.get("here") == HOME and lay.get("fwd") == 1,
          f"here={lay.get('here')} fwd={lay.get('fwd')}")
    dbg.key("0x96", mods="alt")
    lay = wait(dbg, lambda l: l.get("here") == "kernel")
    check("Alt+Right goes forward", lay.get("here") == "kernel", f"here={lay.get('here')}")
    click(dbg, win, *centre(lay["tool2"]))
    lay = wait(dbg, lambda l: l.get("here") == HOME)
    check("the toolbar's Home returns to the overview", lay.get("here") == HOME,
          f"here={lay.get('here')}")

    # Full-text search.
    src = page_source("ping")
    first = src.split("\n", 1)[0].lower()
    if not check("fixture: the query is in ping's text but not its name or title",
                 QUERY in src.lower() and QUERY not in first, first):
        return finish()
    all_rows = lay["rows"]
    click(dbg, win, *centre(lay["search"]))
    for ch in QUERY:
        dbg.key(ch)
    lay = wait(dbg, lambda l: l.get("query") == QUERY)
    check("typing searches, and the contents narrow",
          lay.get("query") == QUERY and 0 < lay.get("matches", 0) and lay["rows"] < all_rows,
          f"query {lay.get('query')!r} matches {lay.get('matches')} rows {lay['rows']} of {all_rows}")
    dbg.key("0x0a")
    lay = wait(dbg, lambda l: l.get("here") not in (HOME, None))
    here = lay.get("here", "-")
    check("Enter opens a match that really contains the word",
          QUERY in page_source(here).lower(), f"opened {here}")
    dbg.key("0x1b")
    lay = wait(dbg, lambda l: l.get("query") == "")
    check("Esc clears the search", lay.get("query") == "", f"query={lay.get('query')!r}")

    # A category: its own page, and a lazy expand/collapse.
    click(dbg, win, *centre(lay["tool2"]))
    lay = wait(dbg, lambda l: l.get("here") == HOME)
    tx, ty, tw, _ = lay["tree"]
    rh = lay["row_h"]

    # Collapsing the branch the open page is in moves the selection to
    # its heading, so the arrows still have a row to move from: Down
    # opens the next category. Left at no selection, Down did nothing.
    rows_open = lay["rows"]
    expander = (tx + 4 + 5, ty + rh // 2)     # uui_tree's PAD_X, EXP_W / 2
    click(dbg, win, *expander)
    lay = wait(dbg, lambda l: l["rows"] < rows_open and l.get("sel") == "Getting started")
    check("collapsing the open page's branch selects its heading",
          lay["rows"] < rows_open and lay.get("sel") == "Getting started"
          and lay.get("here") == HOME,
          f"rows {rows_open} -> {lay['rows']} sel={lay.get('sel')} here={lay.get('here')}")
    dbg.key("0x92")
    lay = wait(dbg, lambda l: l.get("here") not in (HOME, None))
    check("...and Down moves on from it", lay.get("here") not in (HOME, None),
          f"here={lay.get('here')}")
    click(dbg, win, *expander)                # Getting started open again
    lay = wait(dbg, lambda l: l["rows"] == rows_open)
    click(dbg, win, *centre(lay["tool2"]))    # and Home, so the click below changes page
    lay = wait(dbg, lambda l: l.get("here") == HOME)
    # Getting started is open with its pages below it; the next heading
    # is the first row after them -- counted from the pages themselves.
    guide = len(glob.glob(os.path.join(REPO, "data/usr/share/doc/guide/*.md")))
    click(dbg, win, tx + tw // 2, ty + (guide + 1) * rh + rh // 2)
    lay = wait(dbg, lambda l: l.get("here") not in (HOME, None) and len(l["links"]) >= 3)
    cat = lay.get("here")
    check("a category heading opens its own page of links",
          len(lay["links"]) >= 3, f"here={cat} links {len(lay['links'])}")
    rows1 = lay["rows"]
    dbg.key("0x96")
    lay = wait(dbg, lambda l: l["rows"] > rows1)
    check("Right expands it", lay["rows"] > rows1, f"rows {rows1} -> {lay['rows']}")
    dbg.key("0x95")
    lay = wait(dbg, lambda l: l["rows"] == rows1)
    check("Left collapses it", lay["rows"] == rows1, f"rows -> {lay['rows']}")
    return finish()


def finish():
    passed = sum(1 for _, ok, _ in checks if ok)
    print(f"\nhelp_test: {passed} passed, {len(checks) - passed} failed")
    return 0 if checks and passed == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
