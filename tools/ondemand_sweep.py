#!/usr/bin/env python3
"""Run the tools nothing else runs, and report which ones have rotted.

WHY THIS EXISTS
---------------
`preflight.sh` and `gui_regress.py` between them cover most of tools/.
About twenty test tools are covered by NEITHER: they need a particular
boot target, absent hardware, a second QEMU, an IWAD nobody fetched, or
simply take too long for a per-change gate. They run when somebody types
them, which is to say almost never.

**Two of them were found red by accident in one session**, having failed
for an unknown period: `fs_switch_test.py` (10 of its checks, because
`df` and `stat` became /bin programs and it still parsed the ring-0
output) and `init_test.py` (16 of 31, same class). Neither failure was a
regression anybody introduced; both were rot, and the repo had no way to
notice. That is the gap.

WHAT IT IS NOT. Not a gate, and it must never become one -- several of
these need hardware or a fetched IWAD, and a check that cannot pass on a
clean checkout is a check people learn to ignore. It is the thing you
run before a release, or when you want to know whether the on-demand
half of this directory still works.

A SKIP IS A RESULT, NOT A PASS. A tool that cannot run here says so and
is counted separately, because "19 passed" over a table with eight skips
in it is a number that means something different from what it looks
like.

    python3 tools/ondemand_sweep.py               # everything runnable
    python3 tools/ondemand_sweep.py --only init   # one, by name fragment
    python3 tools/ondemand_sweep.py --list        # what it would run
"""
import argparse
import concurrent.futures as cf
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# Each entry: name, script, what it covers, and how it must be run.
#
# `serial` means it needs the one shared VM slot / the physical console
# / its own boot, and cannot share the machine with another run. Those
# are run one at a time even under -j; everything else fans out.
#
# `needs` is a precondition the sweep checks BEFORE running, so a
# missing IWAD reports as a skip with a reason rather than as a failure.
#
# `vm` means the tool ATTACHES to an already-running `vm.py` guest
# rather than launching its own QEMU. Those need one started for them,
# and the sweep does it -- otherwise they fail with "could not connect
# to QMP", which is the sweep's fault and not theirs.
#
# THE HAZARD THIS FIXES IS WORSE THAN A FAILURE. Tools that launch
# their own guest sometimes leave it running, so an attaching tool
# further down the list can find somebody else's VM and PASS on it.
# That is a green line that means nothing, and which one you get
# depends on the order the list happens to be in.
TOOLS = [
    # name          script                     what it covers                        serial  needs                  wants_vm
    # --- storage and boot -------------------------------------------
    ("partition",   "partition_test.py",       "mounting from an MBR/GPT partition", True,  None,                   False),
    ("fs_switch",   "fs_switch_test.py",       "format, remount, reboot persistence", True, None,                   False),
    ("tfs3_v1",     "tfs3_v1_test.py",         "the older TFS3 on-disk version",     True,  None,                   False),
    # FAT32 and the mount table, cross-checked on the HOST with mtools
    # and fsck.fat. It launches its own guest against a COPY of
    # disk.img, so wants_vm is False; it SKIPS cleanly without mtools,
    # which is a skip rather than a pass -- see run_one().
    ("fat32",       "fat32_test.py",           "FAT32 and /boot, against mtools",    True,  None,                   False),
    ("live_boot",   "live_boot_test.py",       "the Live CD's RAM image",            True,
     ("live_iso", "no toy-os-live.iso -- run `make live-iso` first"),                        False),
    ("virtio_boot", "virtio_boot_test.py",     "TFS3 on virtio-blk, no IDE",         True,  None,                   False),
    ("ls",          "ls_test.py",              "/bin/ls flags and the listing cap",  True,  None,                   False),

    # --- shell, console, terminal ------------------------------------
    ("console",     "console_shell_test.py",   "a text boot reaching a ring-3 shell", True, None,                   False),
    ("ctrlc",       "ctrlc_test.py",           "Ctrl-C interrupting a real job",     True,  None,                   False),
    ("jobs",        "jobs_test.py",            "job control: fg, bg, &, Ctrl-Z",     True,  None,                   False),
    # wants_vm: stdin_test ATTACHES to a running guest (its own docstring
    # says to start one first), and without it dies instantly on the
    # serial socket -- the sweep's fault, per the note above.
    ("stdin",       "stdin_test.py",           "blocking fd 0 and /bin/tosh",        True,  None,                   True),
    ("terminal",    "terminal_probe.py",       "the GUI Terminal's keys and paging", True,  None,                   True),
    ("grep",        "grep_test.py",            "/bin/grep through a real shell",     True,  None,                   True),
    ("ansi",        "ansi_cursor_test.py",     "ANSI cursor movement, as pixels",    True,  None,                   False),

    # --- input --------------------------------------------------------
    ("kbd",         "kbd_test.py",             "`kbd`'s four columns, both drivers", True,  None,                   False),
    ("kbd_paths",   "keyboard_paths_test.py",  "PS/2 and virtio-input agree",        True,  None,                   False),
    ("virtio_input", "virtio_input_test.py",   "virtio keyboard, mouse and tablet",  True,  None,                   False),
    # Boots its own guests with -device qemu-xhci, so it is in the sweep
    # rather than the gate: three phases and two reboots per phase.
    ("usb",         "usb_test.py",             "xHCI, and a HID keyboard and mouse", True,  None,                   False),

    # --- display ------------------------------------------------------
    ("virtio_gpu",  "virtio_gpu_test.py",      "the virtio GPU driver",              True,  None,                   False),
    ("hires",       "hires_test.py",           "a desktop above 1280x720",           True,
     ("kcmdline", "needs an ISO built with KCMDLINE=\"video=1920x1080\""),                   False),
    ("taskbar",     "taskbar_test.py",         "taskbar overflow and grouping",      True,  None,                   True),

    # --- init ---------------------------------------------------------
    ("init",        "init_test.py",            "init and service supervision",       True,  None,                   False),

    # --- on demand for their own reasons ------------------------------
    ("doom",        "doom_test.py",            "DOOM runs, draws and takes input",   True,
     ("iwad", "no IWAD fetched -- see tools/fetch_wad.py"),                                   True),
]

# Deliberately NOT here, each for a stated reason:
#   demo_test.py   -- a showpiece; CLAUDE.md says on demand ONLY, never
#                     in any suite, and putting it in one would be
#                     ignoring an explicit standing instruction.
#   qemu_matrix.py -- needs Docker, and pulls images.
#   kvm_soak.py    -- needs KVM and takes many minutes by design.
#   flake_hunt.py  -- runs something else N times; it has no verdict of
#                     its own to report.
#   damage_*.py    -- analysis passes over a running desktop, not
#                     pass/fail tools.
#   pixel_probe.py -- a library with a CLI, not a test.
#   mkpart_test.py -- despite the name, a WRITER: it takes a disk image
#                     argument and patches a table onto it. Running it
#                     bare is an argparse error, not a result. Its
#                     encoders are covered by the `partition` KTEST
#                     suite and by partition_test.py.


def precondition_met(kind):
    """Can this tool run here at all? Returns (ok, why_not)."""
    if kind is None:
        return True, ""
    key, why = kind
    if key == "iwad":
        for root, _dirs, files in os.walk(os.path.join(REPO, "seed")):
            if any(f.lower().endswith(".wad") for f in files):
                return True, ""
        return False, why
    if key == "live_iso":
        # A separate ISO that `make iso` does not build. Missing it is a
        # precondition, not a failure -- reporting it as red would train
        # the reader to skim past a red line.
        return os.path.exists(os.path.join(REPO, "toy-os-live.iso")), why
    if key == "kcmdline":
        # The BUILT config (iso/boot/grub/grub.cfg), and only its
        # `multiboot2` line. The template at the repo root is the wrong
        # file twice over: it still holds the @KCMDLINE@ placeholder,
        # and it documents `video=` in four comment lines -- so a
        # substring search over it returns TRUE on every checkout and
        # runs hires_test against a default-mode ISO, where CLAUDE.md
        # says every check in it passes VACUOUSLY. A precondition that
        # is wrong in the permissive direction manufactures coverage
        # that does not exist, which is worse than reporting a failure.
        # Reading the ISO's copy is still right even though an ordinary
        # boot reads disk.img's: both are generated from the repo-root
        # template with the same KCMDLINE in one `make iso`, so they
        # cannot disagree about what was baked in.
        cfg = os.path.join(REPO, "iso", "boot", "grub", "grub.cfg")
        if not os.path.exists(cfg):
            return False, "no built ISO tree -- run `make iso` first"
        for line in open(cfg).read().splitlines():
            if line.strip().startswith("multiboot2") and "video=" in line:
                return True, ""
        return False, why
    return True, ""


VERDICT = re.compile(r"^\s*\w[\w /-]*:\s*(PASS|FAIL|FAILED|all checks passed|"
                     r"\d+/\d+ checks?|SKIP)", re.M)


def summarise(out, rc):
    """One line from the tool's own output, preferring its verdict."""
    lines = [ln.strip() for ln in out.splitlines() if ln.strip()]
    for ln in reversed(lines):
        if re.search(r"\b(PASS|FAIL|FAILED|SKIP|passed|checks)\b", ln):
            return ln[:96]
    return (lines[-1][:96] if lines else f"exit {rc}")


def vm(*args):
    subprocess.run([sys.executable, os.path.join(HERE, "vm.py"), *args],
                   cwd=REPO, capture_output=True, text=True)


def run_one(entry, timeout, logdir):
    name, script, _what, _serial, _needs, wants_vm = entry
    path = os.path.join(HERE, script)

    # ALWAYS stop first, whatever this tool needs. A guest left running
    # by the previous tool is how an attaching tool passes against the
    # wrong machine, and how a launching one hits a busy QMP port.
    vm("stop")
    if wants_vm:
        vm("start")

    t0 = time.time()
    try:
        r = subprocess.run([sys.executable, path], cwd=REPO,
                           capture_output=True, text=True, timeout=timeout)
        out, rc = r.stdout + r.stderr, r.returncode
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode("utf-8", "replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        out += f"\n[timed out after {timeout}s]"
        rc = 124
    dt = time.time() - t0
    vm("stop")
    if logdir:
        os.makedirs(logdir, exist_ok=True)
        with open(os.path.join(logdir, f"{name}.log"), "w") as f:
            f.write(out)
    # A tool that reports its own SKIP is a skip, not a pass -- doom_test
    # exits 0 when there is no IWAD, and counting that as a pass is how a
    # suite reports coverage it does not have.
    skipped = rc == 0 and re.search(r"\bSKIP(PING)?\b", out) is not None
    return name, ("skip" if skipped else "pass" if rc == 0 else "fail"), dt, summarise(out, rc)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--only", help="run tools whose name contains this")
    ap.add_argument("--list", action="store_true", help="print what would run")
    ap.add_argument("--timeout", type=float, default=600.0, help="per tool (default 600s)")
    ap.add_argument("--logs", metavar="DIR", help="write each tool's output here")
    ap.add_argument("-j", "--jobs", type=int, default=1,
                    help="parallel jobs for the tools that allow it. DEFAULT 1: "
                         "nearly all of these take the shared VM slot or the "
                         "physical console, so this is not gui_regress and "
                         "raising it is usually wrong.")
    args = ap.parse_args()

    chosen = [t for t in TOOLS if not args.only or args.only in t[0]]
    if not chosen:
        sys.exit(f"ondemand_sweep: nothing matches --only {args.only!r}")

    if args.list:
        for name, script, what, serial, needs, _vm in chosen:
            ok, why = precondition_met(needs)
            flag = "" if ok else f"   [would skip: {why}]"
            print(f"  {name:<13} {script:<26} {what}{flag}")
        return 0

    results = []
    runnable = []
    for entry in chosen:
        ok, why = precondition_met(entry[4])
        if not ok:
            results.append((entry[0], "skip", 0.0, why))
        else:
            runnable.append(entry)

    print(f"ondemand_sweep: {len(runnable)} tool(s) to run, "
          f"{len(results)} skipped up front\n")

    serial = [e for e in runnable if e[3]]
    parallel = [e for e in runnable if not e[3]]

    if parallel and args.jobs > 1:
        with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            for r in ex.map(lambda e: run_one(e, args.timeout, args.logs), parallel):
                results.append(r)
                print(f"  {r[1].upper():<5} {r[0]:<13} {r[2]:5.0f}s  {r[3]}")
    else:
        serial = parallel + serial

    for entry in serial:
        r = run_one(entry, args.timeout, args.logs)
        results.append(r)
        print(f"  {r[1].upper():<5} {r[0]:<13} {r[2]:5.0f}s  {r[3]}", flush=True)

    npass = sum(1 for r in results if r[1] == "pass")
    nfail = sum(1 for r in results if r[1] == "fail")
    nskip = sum(1 for r in results if r[1] == "skip")
    print(f"\nondemand_sweep: {npass} pass, {nfail} fail, {nskip} skip")
    if nfail:
        print("\nFAILED:")
        for r in results:
            if r[1] == "fail":
                print(f"  {r[0]:<13} {r[3]}")
        print("\nBefore assuming a regression: most of these are ON DEMAND and may")
        print("have rotted rather than broken. `python3 tools/predates.py` will say")
        print("which, and a dirty disk.img explains more failures than it should --")
        print("`make clean-disk && make iso` first.")
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
