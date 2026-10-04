#!/usr/bin/env python3
"""The Boot Manager and System Settings > Boot menu edit the real grub.cfg.

WHAT IT CHECKS, and what a broken version would still pass:

  - the Boot Manager opens and REPORTS its layout, and its word list is
    DRAWN: the checkbox of a word the entry has is accent-filled and one
    it lacks is white, read from a settled screenshot -- a list that
    reported rows and painted none passes every log check;
  - choosing an entry in the list shows THAT entry (the app's own
    `sel` line), and ticking a word marks the window dirty;
  - Save writes the file: `bootcfg` -- an independent reader -- lists the
    word on that entry, and the old file is grub.cfg.bak;
  - the Text view refuses a broken file: a stray `}` typed at the top is
    reported as a problem, Save opens the refusal and the file on disk
    is unchanged afterwards (checked against `bootcfg`, not the app);
  - System Settings > Boot menu changes the default at once: the
    dropdown, then `bootcfg` shows the `*` moved -- and back.

Geometry is the apps' own report (`bootmgr: layout ...`,
`settings: layout ...`), never re-derived here.
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

BOOTMGR = "/bin/wm/system/bootmgr"
SETTINGS = "/bin/wm/system/settings"
TITLE = "Boot Manager"
CFG = "/boot/boot/grub/grub.cfg"
ACCENT = (70, 110, 160)
K_UP, K_DOWN, K_ENTER = "0xf780", "0xf781", "0x0a"   # api/keyboard.h

_res = Results()
check = _res.check


def layout(dbg, prefix, lay):
    """Merge the app's `<prefix>: layout ...` lines into `lay`."""
    for line in dbg.logs(prefix + ":", clear=True):
        m = re.search(prefix + r": layout (\S+?)(?: (\d+))? (-?\d+) (-?\d+) (\d+) (\d+)$", line)
        if m:
            key = m.group(1) + (m.group(2) or "")
            lay[key] = tuple(int(v) for v in m.groups()[2:])
            continue
        m = re.search(prefix + r": layout (\S+) (-?\d+)$", line)
        if m:
            lay[m.group(1)] = int(m.group(2))
        m = re.search(r"bootmgr: mode (\w+) sel (\d+) dirty (\d) problems (\d+)", line)
        if m:
            lay["mode"], lay["sel"] = m.group(1), int(m.group(2))
            lay["dirty"], lay["problems"] = int(m.group(3)), int(m.group(4))
    return lay


def wait(dbg, prefix, lay, ok, timeout=6.0):
    deadline = time.time() + timeout
    layout(dbg, prefix, lay)
    while not ok(lay) and time.time() < deadline:
        time.sleep(0.15)
        layout(dbg, prefix, lay)
    return lay


def click(dbg, win, rect, dx=None, dy=None):
    c = win["content"]
    x, y, w, h = rect
    dbg.send("gui click %d %d" % (c["x"] + x + (w // 2 if dx is None else dx),
                                  c["y"] + y + (h // 2 if dy is None else dy)))
    dbg.settle(0.5)


def listing(dbg):
    """`bootcfg`'s rows as [(title, default?, words)] -- the independent reader."""
    rows = []
    for line in dbg.send("sh bootcfg").splitlines():
        m = re.match(r"\s+(\d+)\s+(.+?)\s{2,}(\*|next|booted)?\s*(/boot/\S+)\s+(.*)$", line)
        if m:
            rows.append((m.group(2).strip(), m.group(3) == "*", m.group(5).split()))
    return rows


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "bootmgr_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("boot manager and the Boot menu settings page (lib/ubootcfg.h, uui_optlist)")

    # A third entry with words of its own, made by the CLI, so the list
    # has something to select and a word to show ticked.
    dbg.send('sh bootcfg copy 0 "toy-os (debug)" +loglevel=7 --force')
    before = listing(dbg)
    if not check("the fixture entry exists (bootcfg copy)", any(t == "toy-os (debug)" for t, _, _ in before),
                 f"{before}"):
        return finish()
    debug_idx = [t for t, _, _ in before].index("toy-os (debug)")
    known = [ln.split()[0] for ln in dbg.send("sh bootcfg known").splitlines()
             if ln.startswith("  ") and ln.split()]

    dbg.logs("bootmgr:", clear=True)
    win = dbg.spawn(BOOTMGR, TITLE)
    lay = wait(dbg, "bootmgr", {}, lambda l: all(k in l for k in ("entries", "words", "save", "mode"))
               and "words.check0" in l)
    if not check("Boot Manager opens and reports its layout", win and "words" in lay,
                 f"reported {sorted(lay)[:20]}"):
        return finish()

    # Select the fixture entry.
    rh = lay.get("entries.row_h", 0)
    ex, ey, ew, eh = lay["entries"]
    if rh:
        click(dbg, win, (ex, ey + debug_idx * rh, ew, rh))
    lay = wait(dbg, "bootmgr", lay, lambda l: l.get("sel") == debug_idx)
    check("choosing an entry shows that entry", lay.get("sel") == debug_idx,
          f"sel={lay.get('sel')} wanted {debug_idx} (row_h {rh})")

    # DRAWN: row 0 is the entry's first word (on), the row after its
    # words is a known word it lacks (off).
    words = before[debug_idx][2]
    from PIL import Image
    shot = os.path.join(args.tmp, f"bootmgr_{os.getpid()}.png")
    qmp.screenshot(shot)
    im = Image.open(shot).convert("RGB")
    c = win["content"]

    def box_px(row):
        x, y, w, h = lay.get(f"words.check{row}", (0, 0, 0, 0))
        return im.getpixel((c["x"] + x + 2, c["y"] + y + 2)) if w else None

    on_px, off_px = box_px(0), box_px(len(words))
    near = lambda p, q: p and all(abs(a - b) <= 12 for a, b in zip(p, q))
    check("a word the entry has is drawn ticked (accent box)", near(on_px, ACCENT), f"{on_px}")
    check("...and one it lacks is drawn empty (white box)", near(off_px, (255, 255, 255)), f"{off_px}")

    # Tick the first bare known word the entry lacks.
    present = {w.split("=")[0] + ("=" if "=" in w else "") for w in words}
    unused = [k for k in known if k not in present]
    bare = next((k for k in unused if not k.endswith("=")), None)
    row = len(words) + unused.index(bare)
    if f"words.check{row}" in lay:
        click(dbg, win, lay[f"words.check{row}"])
    lay = wait(dbg, "bootmgr", lay, lambda l: l.get("dirty") == 1)
    check(f"ticking {bare} marks the window unsaved", lay.get("dirty") == 1, f"dirty={lay.get('dirty')} row={row}")
    click(dbg, win, lay["save"])
    lay = wait(dbg, "bootmgr", lay, lambda l: l.get("dirty") == 0)
    after = listing(dbg)
    check(f"Save writes it: bootcfg lists {bare} on the entry",
          bare in after[debug_idx][2], f"{after[debug_idx]}")
    bak = dbg.send(f"sh ls {CFG}.bak")
    check("...and the old file is grub.cfg.bak", "no such file" not in bak, bak.strip()[-120:])

    # The Text view refuses a broken file.
    click(dbg, win, lay.get("view.slot1", lay["view"]),
          dx=None if "view.slot1" in lay else lay["view"][2] * 3 // 4)
    lay = wait(dbg, "bootmgr", lay, lambda l: l.get("mode") == "text")
    if check("the Text view opens", lay.get("mode") == "text", f"mode={lay.get('mode')}"):
        base = lay.get("problems", 0)
        dbg.key("0x7d")     # '}' at the caret, the top of the file
        lay = wait(dbg, "bootmgr", lay, lambda l: l.get("problems", 0) > base)
        check("a stray } is reported as a problem", lay.get("problems", 0) > base,
              f"problems {base} -> {lay.get('problems')}")
        disk = listing(dbg)
        dbg.key("0x13")     # Ctrl+S, as the control character the keyboard delivers
        lay = wait(dbg, "bootmgr", lay, lambda l: l.get("ask", (0, 0, 0, 0))[2] > 0)
        check("Save on a broken file opens the refusal", lay.get("ask", (0, 0, 0, 0))[2] > 0,
              f"ask={lay.get('ask')}")
        check("...and the file on disk is unchanged", listing(dbg) == disk)
        dbg.key("0x1b")
        dbg.settle(0.5)
        dbg.key("0x08")     # Backspace takes the } back out
        lay = wait(dbg, "bootmgr", lay, lambda l: l.get("problems", 99) == base)

    w = dbg.window(TITLE)
    if w:
        dbg.send(f"gui close {w['z']}")
        dbg.settle(0.5)

    # System Settings > Boot menu: the default, at once.
    old = dbg.window("System Settings")
    if old:
        dbg.send(f"gui close {old['z']}")
        dbg.settle(1.0)
    dbg.logs("settings:", clear=True)
    sw = dbg.spawn(SETTINGS + " bootmenu", "System Settings")
    slay = wait(dbg, "settings", {}, lambda l: "su_default" in l)
    if not check("System Settings opens on the Boot menu page", sw and "su_default" in slay,
                 f"reported {sorted(k for k in slay if k.startswith('su'))}"):
        return finish()
    click(dbg, sw, slay["su_default"])
    dbg.key(K_DOWN)
    dbg.key(K_ENTER)
    dbg.settle(1.0)
    moved = listing(dbg)
    check("choosing another default writes it (bootcfg's * moved)",
          moved and moved[1][1] and not moved[0][1], f"{[(t, d) for t, d, _ in moved]}")
    click(dbg, sw, slay["su_default"])
    dbg.key(K_UP)
    dbg.key(K_ENTER)
    dbg.settle(1.0)
    back = listing(dbg)
    check("...and back", back and back[0][1], f"{[(t, d) for t, d, _ in back]}")
    w = dbg.window("System Settings")
    if w:
        dbg.send(f"gui close {w['z']}")
    return finish()


def finish():
    return _res.finish("bootmgr_test")


if __name__ == "__main__":
    sys.exit(main())
