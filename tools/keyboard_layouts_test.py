#!/usr/bin/env python3
"""keyboard_layouts_test -- several keyboard layouts: the list setting,
the tray indicator, Super+Space, and Settings > Input > Keyboard.

WHAT IT CHECKS
  1. The list (system.keyboard_layouts) refuses a layout with no file,
     and switching the ACTIVE layout is never written to /etc.
  2. The tray item is hidden with one layout and shown with several.
  3. Super+Space with Super HELD walks a pick and Super's release takes
     it; a quick tap is the next layout; Esc cancels the walk. Super is
     held through QMP's input-send-event -- `send-key` releases at once
     and could only ever test the tap.
  4. A click on the item and on a row switches.
  5. The Settings page draws a preview, and selecting another layout
     CHANGES it while a control beside it does not (CLAUDE.md: "it
     responds" is not "it is drawn"); Try it makes the previewed layout
     active while it has focus and gives the old one back after; Move up
     plus Apply reorders the stored list; Add a layout...'s search field
     takes a click, filters, and Enter adds what it found.

Runs against a booted guest (gui_regress gives it a fresh image); it
puts the list back to `us` at the end.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

_res = Results()
check = _res.check
checks = _res.rows

TITLE = "System Settings"


def lay(dbg):
    return dbg.json("gui layout --json")


def sh(dbg, cmd):
    return dbg.send(f"sh {cmd}") or ""


def wait(pred, tries=30, delay=0.2):
    for _ in range(tries):
        v = pred()
        if v:
            return v
        time.sleep(delay)
    return None


def active_code(dbg):
    s = lay(dbg)
    return s["codes"][s["active"]] if 0 <= s["active"] < len(s["codes"]) else ""


def crop(img, r):
    return img.crop((r[0], r[1], r[0] + r[2], r[1] + r[3])).tobytes()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None, help="directory for screenshots")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "keyboard_layouts_test")
    outdir = args.logs or "/tmp"
    os.makedirs(outdir, exist_ok=True)

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("keyboard layouts: list, tray item, Super+Space, Settings page")

    # --- 1. the settings -----------------------------------------------
    sh(dbg, "config set system.keyboard_layouts us")
    sh(dbg, "config set system.keyboard_layout us")
    check("with one layout the tray item is hidden",
          wait(lambda: lay(dbg)["tray_hidden"]), str(lay(dbg)))
    out = sh(dbg, "config set system.keyboard_layouts us,fi,xx")
    check("a layout with no file is refused", "does not accept" in out, out.strip()[:120])
    sh(dbg, "config set system.keyboard_layouts us,fi,de")
    shown = wait(lambda: not lay(dbg)["tray_hidden"] and lay(dbg)["codes"] == ["us", "fi", "de"])
    check("with three the item is shown and lists them in order", shown, str(lay(dbg)))
    sh(dbg, "config set system.keyboard_layout fi")
    conf = sh(dbg, "cat /etc/toyos.conf")
    check("switching the active layout is not written to /etc",
          "keyboard_layouts=us,fi,de" in conf and "keyboard_layout=fi" not in conf, conf[-200:])
    sh(dbg, "config set system.keyboard_layout us")
    wait(lambda: active_code(dbg) == "us")

    # --- 2. Super+Space -----------------------------------------------
    qmp.key_down("meta_l"); time.sleep(0.15)
    qmp.key_down("spc"); qmp.key_up("spc"); time.sleep(0.3); dbg.settle()
    s1 = lay(dbg)
    qmp.key_down("spc"); qmp.key_up("spc"); time.sleep(0.3); dbg.settle()
    s2 = lay(dbg)
    check("Super held + Space opens the switcher with the NEXT layout picked",
          s1["switching"] and s1["pick"] == 1 and s1["active"] == 0, str(s1))
    check("...a second Space moves the pick, and nothing switches yet",
          s2["pick"] == 2 and s2["active"] == 0, str(s2))
    qmp.screenshot(os.path.join(outdir, "kbl_switcher.png"))
    qmp.key_up("meta_l"); time.sleep(0.4); dbg.settle()
    s3 = lay(dbg)
    check("...and releasing Super switches to the pick", not s3["open"] and active_code(dbg) == "de",
          str(s3))
    qmp.combo(["meta_l", "spc"]); time.sleep(0.5); dbg.settle()
    check("a quick Super+Space tap is the next layout, wrapping", active_code(dbg) == "us",
          str(lay(dbg)))
    qmp.key_down("meta_l"); time.sleep(0.15)
    qmp.key_down("spc"); qmp.key_up("spc"); time.sleep(0.2)
    qmp.send_key("esc"); time.sleep(0.2)
    qmp.key_up("meta_l"); time.sleep(0.4); dbg.settle()
    s4 = lay(dbg)
    check("Esc cancels the walk: closed, and the layout unchanged",
          not s4["open"] and active_code(dbg) == "us", str(s4))

    # --- 3. clicks ----------------------------------------------------
    t = lay(dbg)["tray"]
    dbg.click(t["cx"], t["cy"]); dbg.settle()
    s5 = lay(dbg)
    check("a click on the item opens the flyout, not switching", s5["open"] and not s5["switching"],
          str(s5))
    r = s5["rows"][1]
    dbg.click(r["cx"], r["cy"]); dbg.settle()
    check("a click on a row switches to it and closes", active_code(dbg) == "fi" and not lay(dbg)["open"],
          str(lay(dbg)))

    # --- 4. the Settings page ------------------------------------------
    dbg.send("gui spawn /bin/wm/system/settings system.keyboard_layouts")
    w = wait(lambda: dbg.widgets(TITLE).get("kb_list"), tries=40, delay=0.25)
    check("Settings opens on the Keyboard page with the layout list", bool(w), "no kb_list")
    if not w:
        return report(dbg)
    dbg.send("gui move 1270 300"); dbg.settle()
    wd = dbg.widgets(TITLE)
    pv = wd["kb_preview"]; add = wd["kb_add"]; lst = wd["kb_list"]
    pr = (pv["screen"]["x"], pv["screen"]["y"], pv["w"], pv["h"])
    ar = (add["screen"]["x"], add["screen"]["y"], add["w"], add["h"])
    row_h = (lst["h"] - 4) // 3

    def row(i):
        return lst["screen"]["x"] + 40, lst["screen"]["y"] + 2 + i * row_h + row_h // 2

    from PIL import Image
    p0 = os.path.join(outdir, "kbl_page_us.png")
    qmp.screenshot(p0)
    im0 = Image.open(p0).convert("RGB")
    raw = crop(im0, pr)
    dark = sum(1 for i in range(0, len(raw), 3) if raw[i] + raw[i + 1] + raw[i + 2] < 300)
    check("the preview has key labels drawn in it", dark > 200, f"{dark} dark pixels")

    dbg.click(*row(2)); dbg.settle(); time.sleep(0.3)
    p1 = os.path.join(outdir, "kbl_page_de.png")
    qmp.screenshot(p1)
    im1 = Image.open(p1).convert("RGB")
    check("selecting German changes the preview", crop(im0, pr) != crop(im1, pr))
    check("...while the Add button beside it is unchanged", crop(im0, ar) == crop(im1, ar))

    tr = wd["kb_try"]
    dbg.click(tr["screen"]["x"] + 20, tr["screen"]["y"] + tr["h"] // 2); dbg.settle()
    got = wait(lambda: active_code(dbg) == "de", tries=20)
    check("Try it makes the previewed layout active while it has focus", got, str(lay(dbg)))
    dbg.click(*row(0)); dbg.settle()
    back = wait(lambda: active_code(dbg) == "fi", tries=20)
    check("...and leaving it gives the previous layout back", back, str(lay(dbg)))

    dbg.click(*row(2)); dbg.settle()
    up = wd["kb_up"]
    dbg.click(up["screen"]["x"] + up["w"] // 2, up["screen"]["y"] + up["h"] // 2); dbg.settle()
    ap_ = dbg.widgets(TITLE).get("apply")
    dbg.click(ap_["screen"]["x"] + ap_["w"] // 2, ap_["screen"]["y"] + ap_["h"] // 2); dbg.settle()
    stored = wait(lambda: "us,de,fi" in sh(dbg, "config get system.keyboard_layouts"), tries=20)
    check("Move up then Apply stores the new order", stored,
          sh(dbg, "config get system.keyboard_layouts").strip())

    # --- 5. Add a layout...: its search field takes a CLICK -------------
    # Tab first, so focus is on the list and only a working click-to-focus
    # brings it back: a dialog's press used to be recorded as a popup's,
    # which skipped the focus move, and the letters went to the list's
    # type-ahead instead. "switz" filters to German (Switzerland) alone;
    # typed at the list it selects nothing new and Enter adds Albanian.
    add = dbg.widgets(TITLE)["kb_add"]
    dbg.click(add["screen"]["x"] + 20, add["screen"]["y"] + add["h"] // 2); dbg.settle()
    dlg = wait(lambda: dbg.window("Add a layout"), tries=20)
    check("Add a layout... opens its dialog", bool(dlg), "no dialog")
    if dlg:
        c = dlg["content"]
        qmp.send_key("tab"); time.sleep(0.2)
        dbg.click(c["x"] + 40, c["y"] + 25); dbg.settle()
        for k in "switz":
            qmp.send_key(k); time.sleep(0.1)
        time.sleep(0.3); dbg.settle()
        qmp.screenshot(os.path.join(outdir, "kbl_add.png"))
        qmp.send_key("ret"); time.sleep(0.5); dbg.settle()
        check("...Enter closes it", not dbg.window("Add a layout"), "still open")
        ap_ = dbg.widgets(TITLE).get("apply")
        dbg.click(ap_["screen"]["x"] + ap_["w"] // 2, ap_["screen"]["y"] + ap_["h"] // 2); dbg.settle()
        got = wait(lambda: "us,de,fi,ch" in sh(dbg, "config get system.keyboard_layouts"), tries=20)
        check("a click into its search field, \"switz\" and Enter add German (Switzerland)", got,
              sh(dbg, "config get system.keyboard_layouts").strip())
    return report(dbg)


def report(dbg):
    sh(dbg, "config set system.keyboard_layouts us")
    sh(dbg, "config set system.keyboard_layout us")
    passed = sum(1 for _, ok, _ in checks if ok)
    print(f"\nkeyboard_layouts_test: {passed} passed, {len(checks) - passed} failed")
    return 0 if passed == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
