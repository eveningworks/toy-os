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
import shutil
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
    # --- host-side decoder oracles ----------------------------------
    # No guest at all: the same .c compiled with the host gcc, judged by
    # a decoder that shares no code with it.
    ("usnd_host",   "usnd_hostcheck.py",       "the MP3 decoder against ffmpeg",     False,
     ("host_audio", "needs gcc, lame and ffmpeg on PATH"),                                   False),
    # Needs only gcc and the Python standard library -- hashlib and zlib
    # are the oracle -- so it has no `needs` gate at all.
    ("hash_host",   "hash_hostcheck.py",       "crc32/sha256 against hashlib and zlib", False,
     None,                                                                                   False),
    # --- storage and boot -------------------------------------------
    ("partition",   "partition_test.py",       "mounting from an MBR/GPT partition", True,  None,                   False),
    ("fs_switch",   "fs_switch_test.py",       "format, remount, reboot persistence", True, None,                   False),
    # Two guests: one to install from, then the installed disk booted
    # alone. It makes its own images and never touches disk.img, so
    # wants_vm is False.
    ("install",     "install_test.py",         "self-hosted install, then boot it",  False, None,                   False),
    ("tfs3_v1",     "tfs3_v1_test.py",         "the older TFS3 on-disk version",     True,  None,                   False),
    # FAT32 and the mount table, cross-checked on the HOST with mtools
    # and fsck.fat. It launches its own guest against a COPY of
    # disk.img, so wants_vm is False; it SKIPS cleanly without mtools,
    # which is a skip rather than a pass -- see run_one().
    ("fat32",       "fat32_test.py",           "FAT32 and /boot, against mtools",    True,  None,                   False),
    ("live_boot",   "live_boot_test.py",       "the Live CD's RAM image",            True,
     ("live_iso", "no toy-os-live.iso -- run `make live-iso` first"),                        False),
    # Two disks on two drivers -- the one configuration no other tool
    # here boots, and the shape the enumerate-everything bug needed.
    ("multidisk",   "multidisk_test.py",       "two disks, two drivers, root=",      True,  None,                   False),
    ("virtio_boot", "virtio_boot_test.py",     "TFS3 on virtio-blk, no IDE",         True,  None,                   False),
    ("ahci",        "ahci_test.py",            "TFS3 on a SATA drive behind an HBA",  True,  None,                   False),
    ("diskmark",    "diskmark_test.py",        "the Disk Mark GUI benchmark",        True,  None,                   True),
    ("ls",          "ls_test.py",              "/bin/ls flags and the listing cap",  True,  None,                   False),
    ("fileop",      "fileop_test.py",          "lib/ufileop through cp/mv/rm",       True,  None,                   False),
    # ATTACHES to a running guest: it only types at the debug console and
    # compares against digests it computes on the host.
    ("sum",         "sum_test.py",             "/bin/sum and /lib/libhash.so",       False, None,                   True),

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

    # --- networking ---------------------------------------------------
    # The longest tool here by far -- ten guests, each booted against a
    # COPY of disk.img -- and the only coverage of the boot-time address
    # path, since nothing in any gate boots a machine and looks at what
    # it configured itself with.
    ("net",         "net_test.py",             "the stack on both NICs, and DHCP",   True,  None,                   False),
    # The remote-access pair, which is the only way to work on the
    # bare-metal laptop at all. Enables services that ship DISABLED and
    # leaves them enabled on disk.img -- run `make clean-disk && make
    # iso` after it if that matters.
    ("remote",      "remote_test.py",          "telnetd, tftpd and tools/remote.py end to end", True, None,           False),

    # --- sound --------------------------------------------------------
    # Boots its own guests with an AC97 and a wav audiodev, twice. It is
    # the only run in which the ac97 KTESTs do not skip, which is why
    # its own report treats "0 skipped" as an assertion.
    ("audio",       "audio_test.py",           "AC97, the PCM ring and a WAV file",  True,  None,                   False),
    # Three guests, and the only run in which anything reaches an
    # isochronous endpoint. Its middle phase gives each card its own wav
    # recording, which is the only way "which device played" is an
    # assertion rather than a guess.
    ("usb_audio",   "usb_audio_test.py",       "USB audio: isoch OUT, and which card plays", True, None,             False),

    # --- interrupts ---------------------------------------------------
    # The only run in which anything reaches the Local APIC. Its own
    # guest, with USB hardware, because the load-bearing check is that
    # moving a mouse delivers interrupts on a vector -- which nothing
    # else here would notice the absence of, since every control
    # transfer polls.
    ("msi",         "msi_test.py",             "the LAPIC, and the xHCI on an MSI-X vector", True, None,              False),

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

    # ATTACHES to a running vm.py guest (its usage says to start one
    # first), so wants_vm -- without it, it dies on a missing
    # .vm.serial, which is the sweep's fault and not the tool's.
    ("cursor_ibeam", "cursor_ibeam_test.py",   "named pointer shapes and the clamp", True,  None,                  True),

    # --- power --------------------------------------------------------
    # Ends three guests by stopping the machine, and boots one on q35 --
    # the only chipset here with an ACPI reset register.
    ("poweroff",    "poweroff_test.py",        "ACPI poweroff and reset, from the tables", True, None,             False),

    # --- init ---------------------------------------------------------
    ("init",        "init_test.py",            "init and service supervision",       True,  None,                   False),

    # --- the kernel's own output --------------------------------------
    # A COM1 consumer that stops reading must not stop the machine. Its
    # fixture is a reader that goes deaf, so it needs its own boot and
    # its own serial socket.
    ("backpressure", "serial_backpressure_test.py", "a stalled COM1 must not hang the guest", True, None,           True),
    # LAST, and see DIRTIES_IMAGE below.
    ("console_bleed", "console_bleed_test.py",  "the console must not paint over the desktop", True, None,          True),

    # --- on demand for their own reasons ------------------------------
    ("doom",        "doom_test.py",            "DOOM runs, draws and takes input",   True,
     ("iwad", "no IWAD fetched -- see tools/fetch_wad.py"),                                   True),
    # Boots its OWN guests (twice, with an AC97 attached), so wants_vm is
    # False -- handing it one would leave it attached to a machine with
    # no sound hardware, which is the "green line that means nothing"
    # this file's header warns about.
    ("doom_sound",  "doom_sound_test.py",      "DOOM's effects and OPL music",       False,
     ("iwad", "no IWAD fetched -- see tools/fetch_wad.py"),                                   False),
]

# TOOLS THAT LEAVE disk.img CHANGED, run LAST for that reason.
#
# `console_bleed` deletes /etc/services.d/toywm and cannot put it back
# -- there is nothing in the guest to copy it from -- so the image has
# no autostarted desktop until the next `make iso`. Any tool running
# after it boots into a machine it did not configure, which is this
# file's own "green line that means nothing" hazard pointed at itself.
# The sweep says so at the end rather than running `make iso` on
# somebody's behalf: re-seeding the image is not a thing a test runner
# should do unasked.
DIRTIES_IMAGE = {"console_bleed"}

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
    if key == "host_audio":
        # A HOST-side check rather than a guest one: it compiles the
        # decoder with the host gcc and compares against ffmpeg, so what
        # it needs is tools, not a VM. Missing them is a skip -- the gate
        # must not start requiring lame and ffmpeg on every checkout,
        # the same rule that keeps Docker out of preflight.
        return all(shutil.which(t) for t in ("gcc", "lame", "ffmpeg")), why
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

    # A tool that changes disk.img runs after everything else, or the
    # tools behind it boot a machine it reconfigured -- see
    # DIRTIES_IMAGE.
    runnable.sort(key=lambda e: e[0] in DIRTIES_IMAGE)
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
    dirtied = sorted(r[0] for r in results if r[0] in DIRTIES_IMAGE and r[1] != "skip")
    if dirtied:
        print(f"\ndisk.img was changed by: {', '.join(dirtied)}"
              "\n  Run `make iso` before anything else boots it.")
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
