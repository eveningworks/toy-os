#!/usr/bin/env python3
"""Drives a network card's ADAPTER SETTINGS end to end on QEMU's e1000.

The path under test: `netctl link` and System Settings' Network >
Adapters page (ui/uui_netadapter.c) -> lib/unetlink.c -> SYS_NET_LINK ->
the net core's merge (kernel/drivers/net/net_link.c) -> the driver's
set_link -- and the save to the card's /etc/net.conf section that netd
applies at boot.

QEMU models no card with every setting, and the e1000 offers exactly one
(interrupt moderation, its ITR register). That is the useful fixture:
the panel must show ONLY the rows a driver offers, so a page that drew
all four for every card would fail here.

Every change is judged through an INDEPENDENT PATH -- the kernel log's
`net: <card>: ... moderation N` line (the core logs what the driver
accepted) and the bytes of /etc/net.conf -- never through what the app
says about itself.

    python3 tools/vm.py start
    python3 tools/netadapter_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
from harness import Results                     # noqa: E402
import port_guard                               # noqa: E402

SETTINGS = "/bin/wm/system/settings"
TITLE = "System Settings"


def card_of(dbg):
    """The first e1000's name and MAC, from `netctl`."""
    out = dbg.send("sh netctl")
    m = re.search(r"^(\S+): e1000\s+([0-9a-f:]{17})", out, re.M)
    return (m.group(1), m.group(2)) if m else (None, None)


def section(dbg, mac):
    """The card's /etc/net.conf section, as {key: value}."""
    out = dbg.send("sh cat /etc/net.conf")
    keys, inside = {}, False
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("["):
            inside = line == f"[{mac}]"
        elif inside and "=" in line and not line.startswith("#"):
            k, v = line.split("=", 1)
            keys[k.strip()] = v.strip()
    return keys


def last_moderation(dbg, card):
    """What the kernel last applied to the card, from its own log line."""
    hits = re.findall(rf"net: {re.escape(card)}: rates \S+, eee \S+, flow \d+, moderation (\d)",
                      dbg.send("sh dmesg"))
    return int(hits[-1]) if hits else None


def wait_for(fn, want, timeout=5.0):
    deadline = time.time() + timeout
    got = fn()
    while got != want and time.time() < deadline:
        time.sleep(0.3)
        got = fn()
    return got


def run(dbg, res):
    card, mac = card_of(dbg)
    if not res.check("an e1000 is there to drive", card is not None, f"card={card} mac={mac}"):
        return
    dbg.send(f"sh netctl link {card} defaults")

    # --- the command line ---------------------------------------------
    out = dbg.send(f"sh netctl link {card} eee on")
    res.check("netctl refuses a setting the driver does not offer (EEE on an e1000)",
              "does not offer eee" in out and "eee" not in section(dbg, mac), out.strip()[-120:])
    out = dbg.send(f"sh netctl link {card} moderation low")
    got = wait_for(lambda: last_moderation(dbg, card), 1)
    res.check("netctl link applies moderation (the kernel log says 1, low)", got == 1, f"log says {got}")
    res.check("...and saves it in the card's own section",
              section(dbg, mac).get("moderation") == "low", f"section={section(dbg, mac)}")
    dbg.send(f"sh netctl link {card} defaults")
    res.check("netctl link defaults takes the key out and the driver's value back",
              "moderation" not in section(dbg, mac) and wait_for(lambda: last_moderation(dbg, card), 0) == 0,
              f"section={section(dbg, mac)} log={last_moderation(dbg, card)}")

    # --- System Settings, Network > Adapters --------------------------
    dbg.send(f"gui spawn {SETTINGS} adapters")
    deadline = time.time() + 10
    win = None
    while time.time() < deadline and win is None:
        time.sleep(0.4)
        win = dbg.window(TITLE)
    if not res.check("System Settings opens on Adapters", win is not None, f"window={bool(win)}"):
        return
    dbg.settle()
    time.sleep(0.8)
    w = dbg.widgets(TITLE)
    res.check("the page shows the card's one setting, Interrupt moderation",
              "netadapter_mod" in w, f"widgets={sorted(w)}")
    absent = [n for n in ("netadapter_speed", "netadapter_eee", "netadapter_flow") if n in w]
    res.check("...and NOT the rows its driver does not offer", not absent, f"shown anyway: {absent}")
    if "netadapter_mod" not in w:
        return

    x, y = dbg.widget_center("netadapter_mod", TITLE)
    dbg.click(x, y)          # opens the list
    dbg.key("h")             # High (fewest interrupts)
    dbg.key("0x0a")          # Enter commits
    got = wait_for(lambda: last_moderation(dbg, card), 3)
    res.check("choosing High in Settings applies it (the kernel log says 3)", got == 3, f"log says {got}")
    res.check("...and saves it for the card", section(dbg, mac).get("moderation") == "high",
              f"section={section(dbg, mac)}")

    w = dbg.widgets(TITLE)
    if res.check("the page has Restore defaults", "netadapter_reset" in w, f"widgets={sorted(w)}"):
        dbg.click(*dbg.widget_center("netadapter_reset", TITLE))
        got = wait_for(lambda: last_moderation(dbg, card), 0)
        res.check("Restore defaults puts the driver's value back (moderation 0, off)",
                  got == 0 and "moderation" not in section(dbg, mac),
                  f"log says {got}, section={section(dbg, mac)}")
    dbg.send("gui close System Settings")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true", help="the VM already shows the desktop")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "netadapter_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    res = Results()
    print("netadapter_test: a network card's adapter settings")
    try:
        run(dbg, res)
    finally:
        dbg.close()
        qmp.close()
    return res.finish("netadapter_test")


if __name__ == "__main__":
    sys.exit(main())
