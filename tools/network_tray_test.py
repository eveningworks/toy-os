#!/usr/bin/env python3
"""The taskbar's network item: the icon's state, the panel, and the
honest answer about a link nobody can report.

WHAT IS UNDER TEST
------------------
A state-shaped icon left of the speaker (userland/wm/network_popup.c)
opening a READ-ONLY panel with the interface, its address, netmask,
gateway and link speed. It owns no network state: it reads
QUERY_NETDEV once a second and writes nothing at all.

WHAT IT ASSERTS
---------------
1. THE PANEL AGREES WITH `netctl`, which reads the same class through
   a completely different program. That is the check that matters most:
   the compositor reporting its own view back to a test proves only
   that it is self-consistent, and an independent reader is what turns
   it into evidence.
2. IT IS DRAWN, not merely flagged open -- opening and closing are
   paired with a pixel comparison of the panel's rect, and closing
   restores what was underneath.
3. `link_known == 0` IS NOT "DISCONNECTED". The e1000 in a default QEMU
   guest cannot report link state, and the ABI is explicit that this
   differs from a link that is down (query_abi.h). So `connected` must
   follow the ADDRESS, and a build that read the link flag instead
   would show a disconnected icon on a guest that is plainly online.
4. THE ICON FOLLOWS THE STATE -- the name the tray was given must be the
   one the state implies, since a tray icon is tinted to the panel's
   own ink and cannot carry state in colour.
5. THE VISIBILITY SETTING WORKS: `never` hides the item and the strip
   reflows; `auto` brings it back on a guest that has a device.
6. IT IS MUTUALLY EXCLUSIVE with the other tray flyouts, through the
   overlay table's `close` op rather than by naming its peers.

Usage (the VM must already be up):

    python3 tools/vm.py start
    python3 tools/network_tray_test.py
    python3 tools/vm.py stop
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


def net(dbg):
    return dbg.json("gui network --json")


def netctl(dbg):
    """The same facts through a DIFFERENT program -- /bin/netctl walks
    QUERY_NETDEV itself. Run through the console's own `sh`: a second
    tool on .vm.serial would steal this one's replies."""
    return dbg.send("sh netctl") or ""


def tray_mode(dbg, mode, want_hidden, tries=25):
    dbg.send(f"sh config set desktop.tray_network {mode}")
    for _ in range(tries):
        if net(dbg)["tray_hidden"] == want_hidden:
            return True
        time.sleep(0.4)
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None, help="directory for screenshots")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "network_tray_test")

    outdir = args.logs or "/tmp"
    os.makedirs(outdir, exist_ok=True)
    shot = lambda n: os.path.join(outdir, n)  # noqa: E731

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("network tray item (icon -> card, its switch, and netctl)")

    if net(dbg)["open"]:
        dbg.send("gui click 640 300")
        dbg.settle(); time.sleep(0.3)

    # --- 1. visible, and agreeing with an INDEPENDENT reader ----------
    if not check("`auto` shows the item on a guest that has a device",
                 tray_mode(dbg, "auto", False), str(net(dbg)["tray_hidden"])):
        return report()

    n = net(dbg)
    check("the tray reports an item to click", n["tray"]["w"] > 0, str(n["tray"]))
    check("it found the guest's network device", n["have_device"] and n["devices"] >= 1,
          f"devices={n['devices']}")

    # AN ADDRESS IS NOT A PRECONDITION THIS TEST MAY ASSUME. /bin/netd
    # is still doing DHCP for the first seconds of a boot, and under the
    # parallel suite it can still be unfinished when this runs -- which
    # failed here as "the address does not match" on a guest that was
    # simply not addressed YET. Wait for one, then assert against
    # whichever state we actually got; the cross-check against netctl
    # is real in both.
    for _ in range(30):
        n = net(dbg)
        if n["ip"] != "none":
            break
        time.sleep(1)

    ifc = netctl(dbg)
    check("the interface name matches /bin/netctl", n["name"] and n["name"] in ifc,
          f"{n['name']!r} in netctl output")
    if n["ip"] != "none":
        check("...and so does the address", n["ip"] in ifc,
              f"{n['ip']!r} in netctl output")
    else:
        # netctl prints exactly this when query_netdev's ip is 0, so
        # the two still have to agree -- this is not a skip.
        check("...and with no address yet, netctl says so too",
              "(unconfigured)" in ifc, "both report no address")

    # --- 2. the link the driver cannot answer for ---------------------
    #
    # A guest whose e1000 reports no link state is the normal case here.
    # `connected` must come from the address; if a future guest DOES
    # report link, the assertion still holds -- it is about the rule,
    # not about this adapter.
    expect_connected = n["ip"] != "none" and not n["link_local"]
    check("`connected` follows the ADDRESS, not a link flag",
          n["connected"] == expect_connected,
          f"connected={n['connected']} ip={n['ip']} link_known={n['link_known']}")
    if not n["link_known"] and n["ip"] != "none" and not n["link_local"]:
        check("...and an unreportable link is not called disconnected",
              n["connected"] is True, "link_known=false, ip present")

    # --- 3. it opens, and it is PAINTED -------------------------------
    box = (n["x"], n["y"], n["x"] + n["w"], n["y"] + n["h"])
    before = qmp.stable_pixels(shot("net_before.png"), box)
    dbg.send(f"gui click {n['tray']['cx']} {n['tray']['cy']}")
    dbg.settle(); time.sleep(0.4)
    check("clicking the tray icon opens it", net(dbg)["open"])
    opened = qmp.stable_pixels(shot("net_open.png"), box)
    check("...and the panel is actually PAINTED", opened != before, "pixels changed")

    # --- 4. a second click on the icon closes it ----------------------
    dbg.send(f"gui click {n['tray']['cx']} {n['tray']['cy']}")
    dbg.settle(); time.sleep(0.4)
    check("a second click on the icon closes it", not net(dbg)["open"])
    closed = qmp.stable_pixels(shot("net_closed.png"), box)
    check("...and what was underneath is repainted", closed == before, "restored")

    # --- 5. dismissed by a click outside ------------------------------
    dbg.send(f"gui click {n['tray']['cx']} {n['tray']['cy']}")
    dbg.settle(); time.sleep(0.4)
    if check("reopened for the dismiss check", net(dbg)["open"]):
        dbg.send("gui click 400 300")
        dbg.settle(); time.sleep(0.4)
        check("a click on the desktop dismisses it", not net(dbg)["open"])

    # --- 6. mutual exclusion, through the overlay table ---------------
    dbg.send(f"gui click {n['tray']['cx']} {n['tray']['cy']}")
    dbg.settle(); time.sleep(0.4)
    v = dbg.json("gui volume --json")
    dbg.send(f"gui click {v['tray']['cx']} {v['tray']['cy']}")
    dbg.settle(); time.sleep(0.4)
    st = dbg.json("gui state --json")["overlays"]
    check("opening the volume flyout closes the network panel",
          st.get("volume") is True and st.get("network") is False, str(st))
    dbg.send("gui click 400 300")
    dbg.settle(); time.sleep(0.3)

    if check("reopened for the Start-menu check", (
            dbg.send(f"gui click {n['tray']['cx']} {n['tray']['cy']}"),
            dbg.settle(), time.sleep(0.4), net(dbg)["open"])[-1]):
        # SUPER ACTS ON THE RELEASE, so both edges: pressing it only
        # ARMS the gesture (wm.c), and anything pressed while it is
        # held disarms it. A lone down-edge did nothing at all.
        dbg.send("gui key 0xf795")
        dbg.send("gui key 0xf795 up")
        dbg.settle(); time.sleep(0.4)
        st = dbg.json("gui state --json")["overlays"]
        check("the Super key opens the Start menu and closes the network panel",
              st.get("start_menu") is True and st.get("network") is False, str(st))
        dbg.send("gui key 0xf795")
        dbg.send("gui key 0xf795 up")
        dbg.settle(); time.sleep(0.3)

    # --- 8. the card's actions: the switch, and netctl behind it -------
    #
    # The switch runs `netctl down|up` as a child; netd answers at once
    # and the kernel flag and the address are the outcome. Read back
    # through QUERY_NETDEV (`gui network`) AND /bin/netctl's listing.
    def wait_for(pred, secs=15):
        for _ in range(int(secs * 4)):
            m = net(dbg)
            if pred(m):
                return m
            time.sleep(0.25)
        return net(dbg)

    dev = n["name"]
    dbg.send(f"gui click {n['tray']['cx']} {n['tray']['cy']}")
    dbg.settle(); time.sleep(0.4)
    c = net(dbg)
    check("the card shows a traffic graph while the card is on", c["graph"]["h"] > 0,
          str(c["graph"]))
    check("...and has a switch for the adapter", c["switch"]["w"] > 0, str(c["switch"]))
    dbg.send(f"gui click {c['switch']['cx']} {c['switch']['cy']}")
    dbg.settle()
    off = wait_for(lambda m: m["admin_down"])
    # THE ADDRESS STAYS, unused (a card addressed by hand would have
    # nobody to give it back); what proves "off" is that nothing goes out.
    check("the switch takes the card down, keeping its address",
          off["admin_down"] and off["ip"] != "none", f"admin_down={off['admin_down']} ip={off['ip']}")
    check("...and /bin/netctl says it is switched off", "switched off" in netctl(dbg))
    dbg.timeout = 15
    ping = dbg.send("sh ping -c 1 10.0.2.2") or ""
    dbg.timeout = 6.0
    check("...and nothing is routed through it: a ping gets no reply",
          "bytes from" not in ping, ping.strip()[:160])
    qmp.screenshot(shot("net_switched_off.png"))
    c = net(dbg)
    dbg.send(f"gui click {c['switch']['cx']} {c['switch']['cy']}")
    dbg.settle()
    on = wait_for(lambda m: not m["admin_down"] and m["ip"] != "none", 20)
    check("on again, netd leases it back", not on["admin_down"] and on["ip"] != "none",
          f"admin_down={on['admin_down']} ip={on['ip']}")
    dbg.send("gui click 400 300")
    dbg.settle(); time.sleep(0.3)

    # `up` and `renew` wait up to 15 s for the address (netctl.c's
    # WAIT_MS), longer than the console's default reply timeout.
    dbg.timeout = 25
    out = dbg.send(f"sh netctl down {dev}") or ""
    check("`netctl down` answers through netd", f"{dev}: down" in out, out.strip()[:120])
    out = dbg.send(f"sh netctl up {dev}") or ""
    check("`netctl up` waits for and prints the address",
          on["ip"] in out, out.strip()[:120])
    out = dbg.send(f"sh netctl renew {dev}") or ""
    check("`netctl renew` gets the address again", on["ip"] in out, out.strip()[:120])
    dbg.timeout = 6.0

    # --- 7. the visibility setting, and the strip reflowing -----------
    tray_x_shown = dbg.json("gui taskbar --json")["tray_x"]
    hid = tray_mode(dbg, "never", True)
    check("`never` hides the item", hid, str(net(dbg)["tray_hidden"]))
    tray_x_hidden = dbg.json("gui taskbar --json")["tray_x"]
    check("...and the strip reflows -- the tray starts further right",
          hid and tray_x_hidden > tray_x_shown,
          f"tray_x {tray_x_shown} -> {tray_x_hidden}")

    back = tray_mode(dbg, "auto", False)
    check("`auto` brings it back on a guest that has a device", back,
          f"tray_x {dbg.json('gui taskbar --json')['tray_x']}")
    return report()


def report():
    passed = sum(1 for _, ok, _ in checks if ok)
    print(f"\nnetwork_tray_test: {passed} passed, {len(checks) - passed} failed")
    return 0 if passed == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
