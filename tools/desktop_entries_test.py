#!/usr/bin/env python3
"""Drive the .desktop entry system: the ShowIn= key, and live reload.

WHAT IS UNDER TEST
------------------
One directory (/usr/wm/applications) is the application database -- the Start
menu and the Open > submenu -- and two behaviours were added on top of it:

  1. `ShowIn=` puts an entry on one surface, the other, or both.
  2. The window manager notices the directory changing and re-reads it,
     without a restart.

Both are asserted through the KERNEL's own reports rather than by
counting pixels: `gui menu --json` gives the rows the Start menu
actually draws, and `gui probe` says what is at a point on the desktop.
That matters for check 3 below -- an entry hidden from a surface must
also be UNREACHABLE there, not merely undrawn, and "undrawn" is all a
screenshot can tell you.

THE TRAP THIS ENCODES
---------------------
The Start menu's rows are positional: it draws from the visible list and
hit-tests by row index. Drawing from a filtered list while hit-testing
the unfiltered registry lands every click on the WRONG app, and looks
completely correct in a screenshot. So the checks here always pair "the
row is gone" with "the rows that remain still launch what they say" --
the second is the one that would catch that bug.

Usage (the VM must already be up):

    python3 tools/vm.py start
    python3 tools/desktop_entries_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession             # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

DEFAULT_SOCK = ".vm.serial"
ENTRY_DIR = "/usr/wm/applications"

# Named so it sorts last within its category, which keeps it off the end
# of an existing row's coordinates and makes a stray match obvious.
TEST_NAME = "ZZ Probe"
TEST_FILE = f"{ENTRY_DIR}/zzprobe.desktop"



_res = Results()
check = _res.check
checks = _res.rows


def menu_labels(dbg):
    """Every app label the Start menu would show, in row order.

    The menu draws ONE FOLDER at a time now, so the drawn rows answer
    "which folder is open" rather than "did this entry load" -- the
    question this file asks. `gui menu --json` reports the full app list
    beside the drawn rows for exactly that reason, and it is still the
    MENU's own view (it comes from the same per-surface accessor the
    rows do), not the registry's.
    """
    m = dbg.json("gui menu --json")
    return [a["label"] for a in m.get("apps", [])]


def write_entry(dbg, show_in=None):
    """Create the probe entry on the running system, one key per line.

    `write` truncates and `append` adds; both terminate the line, which
    is what makes a multi-line file authorable from the shell at all.
    """
    # Exec names a real ring-3 BINARY, not a builtin. It used to say
    # `builtin:taskmgr`, and that broke the moment Task Manager became a
    # ring-3 process: load_entry() refuses an entry naming a builtin that
    # does not exist (correctly, and loudly), so the probe never loaded
    # and this file reported "live reload did not fire" for a reason that
    # had nothing to do with live reload.
    #
    # A binary rather than the one remaining builtin, so this survives
    # M41's stage 4 deleting the last of them. Nothing here ever launches
    # the entry -- the test is about PARSING and reloading -- so what the
    # path points at only has to be a plausible one.
    dbg.send(f"sh write {TEST_FILE} Name={TEST_NAME}")
    dbg.send(f"sh append {TEST_FILE} Exec=/bin/wm/system/about")
    dbg.send(f"sh append {TEST_FILE} Category=demos")
    if show_in:
        dbg.send(f"sh append {TEST_FILE} ShowIn={show_in}")


def registry_has(dbg, name):
    """Is this entry LOADED, whatever surface shows it?

    `gui apps` lists the whole registry (with each entry's show_in), so
    it is an independent view of parsing -- separate from `gui menu`,
    which reports one surface. Every ShowIn check pairs the two.
    """
    return any(name in line for line in dbg.send("gui apps").splitlines())


def file_size(dbg, path):
    """Bytes on disk, from `stat`. Used to prove the FIXTURE landed
    before asserting anything about what the WM did with it.

    Without this the suite reports vacuous passes: an entry that was
    never created is trivially "not in the menu", so every removal check
    goes green while nothing is being tested. That failure mode showed up
    on this tool's first run -- `append` was concatenating without
    newlines, so the file was one unparseable line and three checks
    passed for the wrong reason.
    """
    out = dbg.send(f"sh stat {path}")
    for line in out.splitlines():
        if "size:" in line:
            for tok in line.split():
                if tok.isdigit():
                    return int(tok)
    return -1


def wait_for_reload(dbg, want, present, timeout=6.0):
    """Poll the menu until `want` is (or isn't) among its labels.

    Polls rather than sleeping a fixed interval for the same reason
    settle() does: the reload is debounced (~500ms) and gated on the
    desktop being idle, so the delay is a range, not a constant.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        if (want in menu_labels(dbg)) == present:
            return True
        time.sleep(0.25)
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "desktop_entries_test")

    if not args.in_gui:
        qmp = QMPSession(port=args.qmp_port)
        enter_gui(qmp, args.sock)

    dbg = DebugConsole(args.sock)
    print("desktop entries (ShowIn + live reload)")

    baseline = menu_labels(dbg)
    check("the Start menu has entries to begin with", len(baseline) > 0,
          f"{len(baseline)} app rows")
    check("the probe entry is not there yet", TEST_NAME not in baseline)

    # --- live reload, both surfaces ----------------------------------
    write_entry(dbg)
    time.sleep(0.5)

    # Prove the FIXTURE before asserting on the behaviour. Four keys, one
    # line each -- anything much smaller means the shell did not write
    # what this test thinks it wrote, and every check below would then
    # pass or fail for a reason that has nothing to do with the WM.
    size = file_size(dbg, TEST_FILE)
    if not check("the entry file was actually written", size > 40,
                 f"{size} bytes"):
        print("\nfixture never landed -- the checks below would be vacuous")
        passed = sum(1 for _, ok, _ in checks if ok)
        print(f"\ndesktop_entries_test: {passed} passed, "
              f"{len(checks) - passed} failed")
        return 1

    appeared = wait_for_reload(dbg, TEST_NAME, present=True)
    if not check("a new entry appears with no restart", appeared):
        print("\nlive reload did not fire -- removal checks would be vacuous")
        passed = sum(1 for _, ok, _ in checks if ok)
        print(f"\ndesktop_entries_test: {passed} passed, "
              f"{len(checks) - passed} failed")
        return 1

    after = menu_labels(dbg)
    check("it did not displace the existing rows",
          all(b in after for b in baseline),
          f"{len(baseline)} -> {len(after)} rows")

    # --- ShowIn: loaded on one surface, filtered off the other --------
    #
    # The assertion is deliberately "LOADED but not shown here", never
    # just "not shown". `gui apps` lists the whole registry with each
    # entry's show_in, so it is an independent view of what was parsed;
    # `gui menu` is what one surface actually draws. Comparing the two
    # is what distinguishes a working filter from an entry that simply
    # failed to load.
    #
    # That distinction is not theoretical: an earlier version of this
    # check asserted only "absent from the menu" and passed with the
    # filter disabled outright. `write` truncates, so mid-rewrite the
    # file is briefly an entry with no Exec -- invalid, dropped from the
    # registry -- and the check was racing that transient state instead
    # of measuring anything.
    write_entry(dbg, show_in="startmenu")
    time.sleep(2.0)
    check("ShowIn=startmenu: loaded", registry_has(dbg, TEST_NAME))
    check("ShowIn=startmenu: shown in the Start menu",
          TEST_NAME in menu_labels(dbg))

    write_entry(dbg, show_in="desktop")
    time.sleep(2.0)
    loaded = registry_has(dbg, TEST_NAME)
    check("ShowIn=desktop: still loaded", loaded,
          "if this is false the check below proves nothing")
    check("ShowIn=desktop: filtered out of the Start menu",
          loaded and TEST_NAME not in menu_labels(dbg))

    # The row is gone -- but the rows that REMAIN must still launch what
    # they name. This is the check that catches a filtered draw paired
    # with an unfiltered hit test, which no screenshot can see.
    survivors = menu_labels(dbg)
    check("the remaining rows are exactly the baseline",
          survivors == baseline,
          f"{survivors[:3]}... vs {baseline[:3]}...")

    m = dbg.json("gui menu --json")
    rows = [r for r in m.get("rows", []) if r.get("kind") == "app"]
    if rows:
        first = rows[0]
        # `gui menu` reports the menu's geometry whether or not it is
        # OPEN, so the rows have to be made real before they can be
        # clicked -- a click at a closed menu's coordinates lands on the
        # desktop and quietly does nothing.
        tb = dbg.json("gui taskbar --json")
        start = tb["start"]
        dbg.click(start["x"] + start["w"] // 2, start["y"] + start["h"] // 2)
        dbg.settle()

        # The ROW's own centre. `m["x"] + 20` was the app column when the
        # menu was one column; it is the folder sidebar now, so that
        # click selected a folder, left the menu open -- and an open menu
        # DEFERS the entry reload every check after this one waits for.
        dbg.click(first["cx"], first["cy"])
        time.sleep(0.8)
        win = dbg.window(first["label"])
        check("clicking row 0 opens the app row 0 names",
              win is not None, f"clicked '{first['label']}'")
        if win is not None:
            dbg.send("gui close 0")
            dbg.settle()
    else:
        check("clicking row 0 opens the app row 0 names", False, "no rows")

    # --- a reload under an OPEN WINDOW is safe now --------------------
    #
    # This used to assert the opposite: that the reload DEFERRED while a
    # kernel-space app was open. struct window::app points into
    # gui_app_registry[], gui_apps_load() rewrites that array in place,
    # and reloading underneath such a window would rebind it to whatever
    # entry landed in its slot. Control Panel was the last app that held
    # one, and it became a ring-3 process in M41 stage 4's prerequisite
    # work -- a client's `app` is 0 (wm_client.c), so no open window
    # holds a registry pointer any more and the deferral cannot fire.
    #
    # The check is INVERTED rather than deleted, because "it defers"
    # becoming untestable is not the same as it not mattering: the guard
    # in userland/wm/wm.c is still there and still correct, and this now
    # pins the property that makes it unreachable. If a kernel-space app
    # ever comes back, this check goes red and points straight at the
    # guard rather than at a mystery rebinding bug months later.
    dbg.send(f"sh rm {TEST_FILE}")
    wait_for_reload(dbg, TEST_NAME, present=False)
    dbg.open_app("System Settings")
    dbg.settle()
    time.sleep(1.2)

    win = dbg.window("System Settings")
    if win is None:
        check("a reload lands while an app window is open", False,
              "System Settings did not open")
    else:
        # It is a RING-3 client, which is the reason the reload is safe.
        # Asserted from client_pid rather than from the title, so a
        # kernel-space app wearing the same name could not pass it.
        check("System Settings is a ring-3 client",
              win.get("client_pid", 0) != 0,
              f"client_pid={win.get('client_pid')}")

        write_entry(dbg)
        check("a reload lands while an app window is open",
              wait_for_reload(dbg, TEST_NAME, present=True))

        dbg.send("gui close 0")
        dbg.settle()

    # --- deletion is noticed too --------------------------------------
    write_entry(dbg)                       # back on both surfaces
    wait_for_reload(dbg, TEST_NAME, present=True)
    dbg.send(f"sh rm {TEST_FILE}")
    removed = wait_for_reload(dbg, TEST_NAME, present=False)
    check("deleting an entry removes it live", removed)

    final = menu_labels(dbg)
    check("the desktop is back exactly as it started", final == baseline,
          f"{len(final)} rows")

    passed = sum(1 for _, ok, _ in checks if ok)
    failed = len(checks) - passed
    print(f"\ndesktop_entries_test: {passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
