#!/usr/bin/env python3
"""tools/kvm_soak.py -- run the desktop under KVM, across FRESH BOOTS,
and fail on the symptoms that only appear there.

WHY THIS EXISTS
---------------
Every other test in this repo runs under TCG, and on 2026-08-17 that
turned out to hide three real bugs at once -- all of them reported by a
user, none of them reproducible by any tool here:

  * a lost-wakeup race in the ATA driver (the completion flag was
    cleared AFTER the command was issued, so an interrupt landing in
    that window was wiped). The window is a function of how fast the
    transfer completes, so TCG effectively never hit it and KVM, with a
    warm host page cache, hit it constantly. Cost: 5 SECONDS of frozen
    desktop per disk read, then an instant retry.
  * a desktop reload doing 54 whole-file reads, which is 40ms under TCG
    and 2.5s under KVM, where each port-I/O instruction is a VM exit.
  * the filesystem not being re-entrant, so a ring-3 app reading a file
    corrupted the WM's own lookups -- surfacing as the cursor theme
    loading "5 of 6 shapes" on roughly ONE BOOT IN THREE, silently,
    because a shape that fails to load falls back to a built-in one.

The last one is why this boots fresh each round instead of looping
inside one VM: it is intermittent, boot-dependent, and a single clean
run says nothing. A rate is the diagnosis; a verdict is not.

WHAT IT ASSERTS, and each one maps to a bug above:
  * no WM frame exceeds the watchdog threshold (`gui watchdog`)
  * no "READ FAILED" -- a file that exists but will not read
  * every cursor theme load reports the FULL shape count
  * every desktop reload reports the same entry count

USAGE
    python3 tools/kvm_soak.py                 # 3 boots, default workload
    python3 tools/kvm_soak.py -n 10           # hunt an intermittent
    python3 tools/kvm_soak.py --slow-ms 200   # tighten the frame bound
    python3 tools/kvm_soak.py --keep /tmp/soak  # keep each boot's log

NOT in gui_regress.py: it needs /dev/kvm, boots its own VMs, and takes
~15s per round. Run it after touching the disk driver, the filesystem,
the VFS, the scheduler's preemption handling, or anything the WM reads
from disk -- and whenever a user reports something this environment
cannot reproduce.

PROVEN TO GO RED, which is the only thing that makes a clean run mean
anything. Built at the pre-fix commit (dacae1d) in a scratch worktree
and run against it, this failed 3 of 4 rounds:

    round 2: FAIL  ! incomplete cursor theme load(s): bold 5/6
    round 4: FAIL  ! incomplete cursor theme load(s): default 5/6
                   ! desktop entry count varied: [8, 9]

That is the ORIGINAL TREE as the control, and it had to be: reverting
any ONE of the three fixes on the current tree does NOT bring the
symptom back, because the other two independently removed the disk
pressure this workload depends on. (Verified -- with the VFS preemption
guard disarmed entirely, four rounds came back clean.) When a
single-change control will not fire, build the commit that had the bug.

The slow-frame check cannot fire against that old tree, because the
watchdog it reads did not exist yet; the two checks above are what
carried it.

REQUIRES /dev/kvm to be readable. It SKIPS (exit 0) rather than failing
when KVM is unavailable, so CI on a machine without it does not go red
for a reason unrelated to the change -- but it says so loudly, because a
skip that looks like a pass is how this class of bug stayed hidden in
the first place.
"""

import argparse
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole          # noqa: E402
from qmp_test import QMPSession             # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CPANEL = "/bin/wm/system/settings"

# Control Panel is the workload because it is what the reported freeze
# was hit through, and because changing a setting is the cheapest thing
# that makes the desktop do real filesystem work: it writes /etc, which
# bumps fs_generation(), while a ring-3 client is live and busy.
SETTINGS_ROW_H = 20
CHOICE_ROW_H = 22


def kvm_available():
    return os.access("/dev/kvm", os.R_OK | os.W_OK)


def run(cmd, **kw):
    return subprocess.run(cmd, shell=isinstance(cmd, str), cwd=REPO,
                          capture_output=True, text=True, **kw)


class Boot:
    """One fresh VM: its own disk copy, its own slot."""

    def __init__(self, slot, tmpdir):
        self.slot = slot
        self.img = os.path.join(tmpdir, f"kvm_soak{slot}.img")
        self.sock = f".vm.{slot}.serial" if slot else ".vm.serial"
        self.qmp = 4445 + slot

    def start(self):
        run(["python3", "tools/vm.py", "--instance", str(self.slot), "stop"])
        # --sparse=always: disk.img is a few MB of data in a 9 GB sparse
        # file, and a hole-filling copy costs 9 GB of real memory when
        # the destination is a tmpfs.
        run(["cp", "--reflink=auto", "--sparse=always", "disk.img", self.img])
        r = run(["python3", "tools/vm.py", "--kvm", "--instance", str(self.slot),
                 "--disk", self.img, "start"])
        return "ready" in r.stdout

    def stop(self):
        run(["python3", "tools/vm.py", "--instance", str(self.slot), "stop"])


def real_themes(dbg):
    """Theme directories that actually exist on the image.

    Needed because a theme that is genuinely absent SHOULD load 0 of 6 --
    `cursor_theme_test.py` deliberately sets a nonexistent theme named
    `broken`, and `cursor_theme` persists to the disk image, so a soak
    run can inherit it and see a perfectly correct 0-of-6. Checking only
    themes that exist is what makes an incomplete load mean something.
    """
    out = dbg.send("sh ls /usr/share/cursors")
    names = set()
    for line in out.splitlines():
        line = line.strip()
        # `ls` output is interleaved with kernel log lines on this
        # console, so match an entry shape rather than splitting.
        m = re.fullmatch(r"([A-Za-z0-9_-]+)/?", line)
        if m:
            names.add(m.group(1))
    return names


def drive_workload(dbg, qmp, rounds):
    """Enter the GUI, open Control Panel, and change a setting repeatedly."""
    qmp.send_text("gui")
    qmp.send_key("ret")
    time.sleep(2.5)

    dbg.send("gui spawn " + CPANEL)
    deadline = time.time() + 12
    geo = {}
    seen = []
    while time.time() < deadline:
        time.sleep(0.3)
        seen.extend(l.strip() for l in dbg.logs())
        for line in seen:
            m = re.search(r"settings: layout (\w+) (-?\d+) (-?\d+) (-?\d+) (-?\d+)", line)
            if m:
                geo[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
        if "list" in geo and "choices" in geo:
            break
    if "list" not in geo:
        return seen, False

    wins = [w for w in dbg.json("gui windows --json")["windows"]
            if w["title"] == "System Settings"]
    if not wins:
        return seen, False
    c = wins[-1]["content"]
    lx, ly = geo["list"][0], geo["list"][1]
    chx, chy = geo["choices"][0], geo["choices"][1]

    # Walk every settings row, and on each one commit a couple of
    # choices. Which rows exist is deliberately not hardcoded -- the
    # registry is meant to grow.
    #
    # THE DIRECTORY CHURN IS LOAD-BEARING, not decoration. The desktop
    # only re-reads /usr/wm/desktop when that directory actually
    # changed (gui_apps_dir_fingerprint()), and a cached read never
    # reaches the drive -- so without forcing a real reload this
    # workload does almost no disk I/O and cannot exercise the paths
    # this tool exists to watch. Verified the hard way: with the churn
    # absent, disarming the VFS preemption guard entirely still produced
    # four clean rounds. Adding and removing an entry file makes the WM
    # read every .desktop file again, from disk, while a ring-3 client
    # is live and writing /etc -- which is exactly the collision that
    # corrupted kernel-side lookups.
    for row in range(6):
        dbg.send(f"gui click {c['x']+lx+40} {c['y']+ly+int(SETTINGS_ROW_H*(row+0.5))}")
        time.sleep(0.5)
        for which in range(rounds):
            dbg.send(f"gui click {c['x']+chx+30} "
                     f"{c['y']+chy+int(CHOICE_ROW_H*which+CHOICE_ROW_H/2)}")
            time.sleep(0.9)
            # Force a genuine reload of every entry, concurrent with
            # whatever the click just made Control Panel do.
            dbg.send(f"sh write /usr/wm/desktop/zz{row}{which}.desktop x")
            time.sleep(0.9)
            dbg.send(f"sh delete /usr/wm/desktop/zz{row}{which}.desktop")
            time.sleep(0.9)
        seen.extend(l.strip() for l in dbg.logs())
    time.sleep(1.0)
    seen.extend(l.strip() for l in dbg.logs())
    return seen, True


def check(log, slow_ms, themes):
    """Returns (list of problem strings, list of informational counts)."""
    problems, info = [], []

    slow = []
    for line in log:
        m = re.search(r"SLOW FRAME (\d+) ms -- worst phase '(\w+)' (\d+) ms", line)
        if m and int(m.group(1)) >= slow_ms:
            slow.append((int(m.group(1)), m.group(2)))
    if slow:
        worst = max(slow)
        problems.append(f"{len(slow)} slow frame(s), worst {worst[0]}ms in '{worst[1]}'")
    info.append(f"slow frames >= {slow_ms}ms: {len(slow)}")

    failed = [l for l in log if "READ FAILED" in l]
    if failed:
        problems.append(f"{len(failed)} file(s) existed but would not read")

    # "N of M shapes loaded" -- N < M means a read came back wrong or
    # short. The built-in fallback hides this completely on screen, which
    # is exactly why it is asserted here rather than looked at.
    short = []
    for line in log:
        m = re.search(r"cursor: theme \"(\w+)\" -- (\d+) of (\d+) shapes loaded", line)
        if not m:
            continue
        # Only themes that exist: an absent one loading 0 of 6 is the
        # fallback working, not a fault. See real_themes().
        if themes and m.group(1) not in themes:
            continue
        if m.group(2) != m.group(3):
            short.append(f"{m.group(1)} {m.group(2)}/{m.group(3)}")
    if short:
        problems.append(f"incomplete cursor theme load(s): {', '.join(short[:4])}")
    info.append(f"theme loads: {len([l for l in log if 'shapes loaded' in l])}")

    counts = [int(m.group(1)) for l in log
              for m in [re.search(r"wm: (\d+) desktop entries", l)] if m]
    if counts and len(set(counts)) > 1:
        problems.append(f"desktop entry count varied: {sorted(set(counts))}")
    info.append(f"reloads: {len(counts)}" + (f" (all {counts[0]} entries)" if counts else ""))

    return problems, info


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-n", "--rounds", type=int, default=3,
                    help="fresh boots to run (default 3; use more to hunt an intermittent)")
    ap.add_argument("--slot", type=int, default=2, help="VM slot to use")
    ap.add_argument("--slow-ms", type=int, default=300,
                    help="fail on any WM frame at least this long (default 300)")
    ap.add_argument("--changes", type=int, default=2,
                    help="setting changes per row (default 2)")
    ap.add_argument("--tmp", default="/tmp")
    ap.add_argument("--keep", metavar="DIR", help="write each boot's log here")
    args = ap.parse_args()

    if not kvm_available():
        print("kvm_soak: SKIP -- /dev/kvm is not available.")
        print("kvm_soak: this tool exists for bugs that ONLY appear under KVM,")
        print("kvm_soak:   so a skip here is a gap in coverage, not a pass.")
        return 0

    if args.keep:
        os.makedirs(args.keep, exist_ok=True)

    print(f"kvm_soak: {args.rounds} fresh boot(s), slot {args.slot}, "
          f"frame bound {args.slow_ms}ms")
    boot = Boot(args.slot, args.tmp)
    bad_rounds = 0

    for i in range(1, args.rounds + 1):
        if not boot.start():
            print(f"  round {i}: ERROR -- the VM did not come up")
            bad_rounds += 1
            continue
        try:
            dbg = DebugConsole(boot.sock, timeout=30)
            qmp = QMPSession(port=boot.qmp)
            # A tighter threshold than the kernel's default, so the
            # tool's bound is the one that decides.
            dbg.send(f"gui watchdog {max(60, args.slow_ms // 2)}")
            themes = real_themes(dbg)
            # Establish a known-good theme rather than inheriting
            # whatever the last run left on the image -- the same rule
            # cursor_theme_test.py follows, and for the same reason.
            if "default" in themes:
                dbg.send("sh config set cursor_theme default")
                time.sleep(0.5)
                dbg.logs()
            log, ok = drive_workload(dbg, qmp, args.changes)
            if not ok:
                print(f"  round {i}: ERROR -- Control Panel never reported its layout")
                bad_rounds += 1
                continue
            problems, info = check(log, args.slow_ms, themes)
            if args.keep:
                with open(os.path.join(args.keep, f"round{i}.log"), "w") as f:
                    f.write("\n".join(log))
            status = "FAIL" if problems else "pass"
            print(f"  round {i}: {status}  ({'; '.join(info)})")
            for pr in problems:
                print(f"      ! {pr}")
            if problems:
                bad_rounds += 1
        except Exception as e:                       # noqa: BLE001
            # An ERROR is its own outcome, never a pass: a round that
            # measured nothing must not look like a good one.
            print(f"  round {i}: ERROR -- {type(e).__name__}: {e}")
            bad_rounds += 1
        finally:
            boot.stop()

    print()
    if bad_rounds:
        print(f"kvm_soak: FAIL -- {bad_rounds} of {args.rounds} round(s) had problems")
        return 1
    print(f"kvm_soak: PASS -- {args.rounds} round(s) clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
