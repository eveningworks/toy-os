#!/usr/bin/env python3
"""Screenshot's options: what each one DOES to the next capture, read back
from the disk and the compositor rather than from the app.

1. **The Saving options name the file**: a folder, a template with
   <mode> and <n>, PNG -- `ls` finds snap-screen-1.png, then -2 (the
   counter), and `imginfo` says it is a PNG.
2. **Card off and flash on reach the compositor**: its `wm: notice` line
   says `flash no-card`, and no card is up.
3. **Shift+PrtSc saves the whole screen with no overlay** (`--now
   screen`): a file appears and no Screenshot window ever does.
4. **Alt+PrtSc saves the window with the focus**, at its size, and
   <app> names it -- checked against `gui windows`.
5. **The popover (the pill's "...") toggles an option at once** and it
   is in /etc/screenshot.conf after the overlay closes; Esc closes the
   popover before the overlay.
6. **The Options window moves a Print Screen key** in /etc/shortcuts.conf:
   "Does nothing" takes Print Screen out of `screenshot` and PrtSc then
   launches nothing; Defaults puts it back.

Each capture runs with the overlay's Screen mode, so nothing depends on
where a region lands.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, "tools")
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

CONF = "/etc/screenshot.conf"
SHORTCUTS = "/etc/shortcuts.conf"
DIR = "/home/shot-opt"
PRTSC = "0xf888"
CALC = "/bin/wm/apps/calculator"
HERE = os.path.dirname(os.path.abspath(__file__))


def rect(dbg, prefix, key):
    for line in reversed(dbg.logs(f"{prefix}: layout {key} ", clear=False)):
        try:
            return [int(v) for v in line.split(f"layout {key} ", 1)[1].split()[:4]]
        except ValueError:
            return None
    return None


def window(dbg, part, timeout=12.0, state=None):
    deadline = time.time() + timeout
    while time.time() < deadline:
        for w in dbg.windows():
            if part in w.get("title", "") and (state is None or w.get("state") == state):
                return w
        time.sleep(0.3)
    return None


def gone(dbg, part, timeout=8.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not any(part in w.get("title", "") for w in dbg.windows()):
            return True
        time.sleep(0.3)
    return False


def click_rect(dbg, win, r):
    c = win["content"]
    dbg.send(f"gui click {c['x'] + r[0] + r[2] // 2} {c['y'] + r[1] + r[3] // 2}")
    dbg.settle(0.4)


def popup_drawn(dbg, qmp):
    """Is the open popup's first row PAINTED? The compositor lists a popup
    surface as soon as it is created, presented or not, so the window list
    cannot tell. Its selected first row is the accent; the dialog under an
    unpresented one is panel grey, the same as the control beside it."""
    pop = next((w for w in dbg.windows() if w.get("title") == "Popup"), None)
    if not pop:
        return False, f"no popup: {[w['title'] for w in dbg.windows()]}"
    from PIL import Image
    path = os.path.join(tempfile.mkdtemp(prefix="shotopts-"), "pop.png")
    qmp.screenshot(path)
    im = Image.open(path).convert("RGB")
    y = pop["y"] + 6
    inside = im.getpixel((pop["x"] + pop["w"] - 8, y))
    beside = im.getpixel((pop["x"] + pop["w"] + 40, y))
    return inside != beside, f"popup {pop['x']},{pop['y']} row={inside} beside={beside}"


def files(dbg):
    return sorted(n for n in (dbg.send(f"sh ls {DIR}") or "").split()
                  if n.endswith(".png") or n.endswith(".qoi"))


def wait_file(dbg, name, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if name in files(dbg):
            return True
        time.sleep(0.4)
    return False


def overlay(dbg):
    """PrtSc, and wait for the fullscreen overlay's first report."""
    dbg.logs("screenshot:", clear=True)
    dbg.send(f"gui key {PRTSC}")
    w = window(dbg, "Screenshot", state="fullscreen")
    deadline = time.time() + 8
    while w and time.time() < deadline and not rect(dbg, "screenshot", "pill"):
        time.sleep(0.3)
    return w


def shoot_screen(dbg):
    """The overlay, Screen mode, the shutter; gone after."""
    w = overlay(dbg)
    if not w:
        return False
    dbg.send("gui key s")
    dbg.settle(0.3)
    dbg.send("gui key 0x0d")
    return gone(dbg, "Screenshot")


# THE TWO OPTION FILES, put from the host: a template's <mode> is a
# redirection to any shell that echoes it, so write_lines() stops at that
# line -- which reads as the app ignoring its options.
FIXTURES = {
    "/var/tmp/shotopt1.conf": [f"folder={DIR}", "name=snap-<mode>-<n>", "png=on", "card=off",
                               "flash=on", "start=last", "mode=region"],
    "/var/tmp/shotopt2.conf": [f"folder={DIR}", "name=<app>-<n>", "png=on", "card=off", "flash=off"],
}


def put_fixtures(instance):
    """Before the console is ours (zip_gui_test.py's rule: a second client
    on the debug socket loses the file without a word)."""
    for guest, lines in FIXTURES.items():
        with tempfile.NamedTemporaryFile("w", suffix=".conf", delete=False) as f:
            f.write("\n".join(lines) + "\n")
            host = f.name
        subprocess.run([sys.executable, os.path.join(HERE, "vm.py"), "--instance", str(instance),
                        "put", host, guest], capture_output=True)
        os.unlink(host)


def use_conf(dbg, fixture):
    dbg.send(f"sh cp {fixture} {CONF}")
    got = [ln.strip() for ln in (dbg.send(f"sh cat {CONF}") or "").splitlines()]
    return all(w in got for w in FIXTURES[fixture])


def run(dbg, res, qmp):
    sh = lambda c: dbg.send(f"sh {c}") or ""   # noqa: E731
    sh(f"rm -r {DIR}")
    sh(f"rm {CONF}")
    wrote = use_conf(dbg, "/var/tmp/shotopt1.conf")
    res.check("the fixture's options are on the guest", wrote, (dbg.send(f"sh cat {CONF}") or "").strip())

    # 1-2. the Saving options, card off, flash on
    dbg.logs("wm: notice", clear=True)
    ok1 = shoot_screen(dbg)
    have1 = wait_file(dbg, "snap-screen-1.png")
    info = sh(f"imginfo {DIR}/snap-screen-1.png")
    res.check("the folder, the template and PNG name the file: snap-screen-1.png, a PNG",
              ok1 and have1 and re.search(r"\bPNG\b", info, re.I) is not None,
              f"overlay={ok1} files={files(dbg)} imginfo={info.strip()!r}")
    time.sleep(0.5)
    lines = dbg.logs("wm: notice", clear=False)
    card = (dbg.state().get("notice") or {})
    res.check("card off and flash on reach the compositor: `flash no-card`, no card up",
              any("flash no-card" in ln and "snap-screen-1.png" in ln for ln in lines) and not card,
              f"notice lines={lines} card={card}")
    ok2 = shoot_screen(dbg)
    res.check("<n> counts on to snap-screen-2.png", ok2 and wait_file(dbg, "snap-screen-2.png"),
              f"files={files(dbg)}")

    # 3. Shift+PrtSc: no overlay
    dbg.logs("wm: shortcut", clear=True)
    dbg.logs("screenshot:", clear=True)
    dbg.send(f"gui key {PRTSC} shift")
    got3 = wait_file(dbg, "snap-screen-3.png")
    saw_window = any(ln for ln in dbg.logs("screenshot: overlay", clear=False))
    launched = dbg.logs("wm: shortcut", clear=False)
    res.check("Shift+PrtSc saves the whole screen with no overlay",
              got3 and not saw_window and any("--now screen" in ln for ln in launched),
              f"files={files(dbg)} overlay={saw_window} launched={launched}")

    # 4. Alt+PrtSc: the focused window, named by its app
    use_conf(dbg, "/var/tmp/shotopt2.conf")
    dbg.send(f"gui spawn {CALC}")
    cw = window(dbg, "Calculator")
    time.sleep(1.0)
    dbg.send(f"gui key {PRTSC} alt")
    got4 = wait_file(dbg, "calculator-1.png")
    info = sh(f"imginfo {DIR}/calculator-1.png")
    m = re.search(r"(\d+)\s*x\s*(\d+)", info)
    size = (int(m.group(1)), int(m.group(2))) if m else None
    want = (cw["w"], cw["h"]) if cw else None
    res.check("Alt+PrtSc saves the focused window at its size, named by <app>",
              got4 and cw is not None and size == want,
              f"files={files(dbg)} imginfo={info.strip()!r} window={want}")
    if cw:
        dbg.send("gui key 0xf794 alt")   # Alt+F4
        gone(dbg, "Calculator")

    # 5. the popover
    w = overlay(dbg)
    more = rect(dbg, "screenshot", "more")
    if w and more:
        click_rect(dbg, w, more)
    deadline = time.time() + 5
    while time.time() < deadline and not rect(dbg, "screenshot", "pop.card"):
        time.sleep(0.3)
    card_row = rect(dbg, "screenshot", "pop.card")
    dbg.logs("screenshot: option", clear=True)
    if w and card_row:
        click_rect(dbg, w, card_row)
    time.sleep(0.5)
    opt = dbg.logs("screenshot: option card", clear=False)
    dbg.logs("screenshot: layout popover", clear=True)
    dbg.send("gui key 0x1b")
    dbg.settle(0.5)
    pop_closed = any(ln.rstrip().endswith("popover 0") for ln in dbg.logs("screenshot: layout popover", clear=False))
    still = window(dbg, "Screenshot", timeout=1.0) is not None
    dbg.send("gui key 0x1b")
    closed = gone(dbg, "Screenshot")
    conf = (sh(f"cat {CONF}")).replace(" ", "")
    res.check("the popover's 'Show the card' turns it on at once, and it is kept",
              card_row is not None and any("card 1" in ln for ln in opt) and "card=on" in conf,
              f"row={card_row} option={opt} conf={conf!r}")
    res.check("Esc closes the popover first, then the overlay", pop_closed and still and closed,
              f"popover closed={pop_closed} overlay after one Esc={still} gone after two={closed}")

    # 6. Options: Print Screen -> Does nothing, then Defaults
    #
    # THE OVERLAY STAYS FULLSCREEN UNDER ITS OWN DIALOG: the part of the
    # taskbar band the dialog does not reach must read the SAME before and
    # after Options opens. The compositor once asked only whether the
    # FOCUSED window was fullscreen, so the dialog taking focus brought
    # the live taskbar back over the frozen frame.
    shots = tempfile.mkdtemp(prefix="shotopts-band-")
    seen = {}

    def options():
        w = overlay(dbg)
        if w:
            # Where the LIVE taskbar would show the overlay's own window
            # button -- the frozen frame was taken before the app ran and
            # has none, so the two differ unmistakably. The bar's dark
            # foot alone did not: a dimmed dark is still dark.
            seen["band"] = (56, w["h"] - 40, 200, w["h"] - 8)
            seen["before"] = qmp.stable_pixels(os.path.join(shots, "before.png"), seen["band"])
        gear = rect(dbg, "screenshot", "gear")
        dbg.logs("options: layout", clear=True)
        if w and gear:
            click_rect(dbg, w, gear)
        ow = window(dbg, "Screenshot Options")
        deadline = time.time() + 6
        while ow and time.time() < deadline and not rect(dbg, "options", "key0"):
            time.sleep(0.3)
        return ow

    ow = options()
    k0 = rect(dbg, "options", "key0")
    res.check("the gear opens Screenshot Options with the Print Screen keys", ow is not None and k0,
              f"windows={[x['title'] for x in dbg.windows()]}")
    if "before" in seen and ow:
        after = qmp.stable_pixels(os.path.join(shots, "after.png"), seen["band"])
        res.check("...and the overlay stays fullscreen under it: no taskbar comes back",
                  after == seen["before"])
    if ow and k0:
        click_rect(dbg, ow, k0)          # opens the list and takes the focus
        drawn, why = popup_drawn(dbg, qmp)
        res.check("the Print Screen dropdown's list is drawn over an idle overlay", drawn, why)
        dbg.send("gui key 0x1b")         # closed again, focused
        dbg.send("gui key D")            # "Does nothing", by its letter
        dbg.settle(0.3)
        ok = rect(dbg, "options", "ok")
        if ok:
            click_rect(dbg, ow, ok)
        gone(dbg, "Screenshot Options")
    dbg.send("gui key 0x1b")
    gone(dbg, "Screenshot")
    sc = sh(f"cat {SHORTCUTS}")
    line = next((ln for ln in sc.splitlines() if ln.startswith("screenshot=")), "")
    dbg.logs("wm: shortcut", clear=True)
    time.sleep(1.5)                      # the compositor polls the file's change counter
    dbg.send(f"gui key {PRTSC}")
    time.sleep(1.5)
    fired = dbg.logs("wm: shortcut", clear=False)
    res.check("'Does nothing' takes Print Screen off `screenshot`, and PrtSc launches nothing",
              line and "Print Screen" not in line and "Shift+Super+S" in line and not fired,
              f"line={line!r} fired={fired}")

    # Defaults puts it back (through Shift+Super+S, PrtSc being unbound).
    dbg.send("gui key S shift super")
    w = window(dbg, "Screenshot", state="fullscreen")
    deadline = time.time() + 8
    while w and time.time() < deadline and not rect(dbg, "screenshot", "gear"):
        time.sleep(0.3)
    gear = rect(dbg, "screenshot", "gear")
    dbg.logs("options: layout", clear=True)
    if w and gear:
        click_rect(dbg, w, gear)
    ow = window(dbg, "Screenshot Options")
    deadline = time.time() + 6
    while ow and time.time() < deadline and not rect(dbg, "options", "defaults"):
        time.sleep(0.3)
    for key in ("defaults", "ok"):
        r = rect(dbg, "options", key)
        if ow and r:
            click_rect(dbg, ow, r)
    gone(dbg, "Screenshot Options")
    dbg.send("gui key 0x1b")
    gone(dbg, "Screenshot")
    sc = sh(f"cat {SHORTCUTS}")
    line = next((ln for ln in sc.splitlines() if ln.startswith("screenshot=")), "")
    res.check("Defaults gives Print Screen back to `screenshot`", "Print Screen" in line, f"line={line!r}")

    sh(f"rm -r {DIR}")
    sh(f"rm {CONF}")
    sh(f"rm {SHORTCUTS}")
    for f in FIXTURES:
        sh(f"rm {f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "screenshot_options_test")
    res = Results()
    put_fixtures(args.instance if args.instance is not None else 0)
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, res, qmp)
    finally:
        dbg.close()
    print(f"\nscreenshot_options_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
