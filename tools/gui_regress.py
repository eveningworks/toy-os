#!/usr/bin/env python3
"""tools/gui_regress.py -- run every GUI test tool, each on a fresh VM.

WHAT THIS IS
------------
The GUI test tools (`uidemo_test.py`, `gfxdemo_test.py`,
`notepad_client_test.py`, ...) each drive one app and assert on its log.
Running them is the standard check after touching `apps/ui/`,
`userland/`, or anything the window manager draws -- and doing that by
hand means retyping the same six commands, each with its own image copy
and VM lifecycle. This is that sequence, once, with a summary table.

    python3 tools/gui_regress.py                 # all of them, 4 at a time
    python3 tools/gui_regress.py -j1             # one at a time (the old behaviour)
    python3 tools/gui_regress.py -k uidemo -k gfxdemo    # a subset
    python3 tools/gui_regress.py --list
    echo $?                                      # 0 = every tool passed

WHY IT RUNS THEM IN PARALLEL
----------------------------
The tools are independent by construction (see below) and each spends
almost all of its wall clock waiting on an emulated machine, so running
them one after another wasted most of the host. Each concurrent tool
gets a VM SLOT -- `vm.py --instance N`, which derives that VM's
pidfile, serial socket, QMP port and VNC display from N, and which the
tool is pointed at with `--sock`/`--qmp-port`. Nothing is shared, so
the isolation the per-tool image and per-tool VM already provided is
unchanged; only the scheduling is.

Tools are STARTED longest-first (`COST_S`/`pick_order` below), because
a parallel run cannot finish before its slowest member does and
`forcequit` (71s of genuinely waiting out ping timeouts) previously sat
eleventh of fourteen and finished alone after everything else had
drained. The summary table is still printed in the declared dependency
order -- only the start order changed.

WHY EACH TOOL GETS ITS OWN IMAGE AND ITS OWN VM
-----------------------------------------------
This is the part that is easy to get wrong and expensive to debug.

  * **A fresh disk copy per tool.** Several of these write files
    (Notepad saves, the Terminal's shell creates things). A tool
    inheriting the previous one's filesystem hits "file already exists"
    paths its assertions never anticipated. The copy also keeps the
    real `disk.img` untouched, which matters if the user has their own
    QEMU open -- see CLAUDE.md on the write lock.
  * **A fresh VM per tool.** Each tool enters GUI mode itself and
    expects an empty desktop. Handing one a desktop with three windows
    already open fails in ways that look exactly like real widget bugs
    -- a click lands on the wrong window, and the log line that proves
    it is in a scrollback nobody reads.

`cp --reflink=auto` makes the copies nearly free on btrfs/XFS, and the
image is sparse besides (~3 MB actually allocated against a 9 GB
apparent size), so the per-tool copy costs far less than it looks.

WHAT THIS DOES NOT COVER
------------------------
`tools/damage_sweep.py` is deliberately NOT in the list: it takes a
different kind of run (`gui damage verify on` makes every frame render
twice, so the whole thing is several times slower) and it has its own
`--positive-control` protocol. Run it separately after touching
anything that draws, damages, focuses or changes window chrome.

Nor does this build anything. Run `make all && make iso` first -- these
tools drive whatever is already on the image.
"""

import argparse
import concurrent.futures as cf
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# How many tools run at once by default. Each one is a QEMU with 256 MB
# of guest RAM under TCG, so this is bounded by host cores far more than
# by memory. `-j1` restores the original serial behaviour exactly.
#
# DERIVED from the host rather than fixed at 4: the tool-seconds in a
# full run total roughly 460s, so the ceiling is the slowest single tool
# (notepad, ~41s -- forcequit dropped to ~35s in the 663d63b pass, so it
# is no longer the straggler) and everything between 4 and that is just
# how many cores are free. Half the cores, capped at 8 -- a TCG guest is a busy
# CPU thread plus its I/O, so oversubscribing turns wall-clock into
# settle flakes rather than speed, which is measurable: at five
# concurrent guests both gfxdemo and notepad failed comparisons they
# pass alone. Those two now wait on an OBSERVABLE instead of a sleep,
# which is what makes raising this safe at all -- raise it further only
# after checking that the tools still wait for something the app says.
DEFAULT_JOBS = min(8, max(2, (os.cpu_count() or 4) // 2))

# In rough dependency order: the widget toolkit first, so a toolkit
# regression is reported before the apps built on it start failing for
# what looks like their own reasons.
TOOLS = [
    ("uidemo", "uidemo_test.py", "apps/ui/ widgets, kernel-space"),
    ("uiclient", "uiclient_test.py", "the ported widgets, ring 3"),
    ("winclient", "winclient_test.py", "the ring-3 window protocol"),
    ("gfxdemo", "gfxdemo_test.py", "geometry primitives + the canvas widget"),
    ("calculator", "calculator_client_test.py", "Calculator in ring 3"),
    ("notepad", "notepad_client_test.py", "Notepad in ring 3"),
    ("uterm", "uterm_test.py", "Terminal + the ring-3 shell"),
    ("uapp", "uapp_test.py", "the TWP resize handshake"),
    ("scrollbar", "scrollbar_test.py", "scrollbar behaviour, per the guidelines"),
    ("menubar", "menubar_test.py", "menu bar, submenus and the status bar"),
    ("forcequit", "forcequit_test.py", "not-responding detection and force quit"),
    ("dialog", "dialog_test.py", "the confirm dialog, by pixel value"),
    ("sched", "sched_gui_test.py", "the desktop stays live while a process runs"),
    ("blank", "blank_window_test.py", "no app opens a blank window"),
    ("compositor", "compositor_test.py", "raw input to a ring-3 compositor"),
    ("screen", "screen_surface_test.py", "a ring-3 compositor's screen surface"),
    ("compdeath", "compositor_death_test.py", "the compositor death path (R7)"),
    ("cursor", "cursor_theme_test.py", "cursor themes: shapes as data files, size, fallback"),
    ("crash", "crashtest_test.py", "fault paths: ring-3 crashes, and the gate on kernel panics"),
    ("entries", "desktop_entries_test.py", "ShowIn= and live .desktop reload"),
    ("taskmgr", "taskmgr_test.py", "the table widget, resize reflow, ending a process"),
    ("singleinst", "single_instance_test.py", "one copy of an app, and relaunch raises it"),
    ("settings", "settings_test.py", "the settings registry, in ring 3"),
    ("font", "font_test.py", "runtime TTF faces, live switching, proportional widths"),
    ("idle", "idle_desktop_test.py", "nothing paints over an idle desktop"),
]

# Roughly how long each tool takes, in seconds, used ONLY to decide what
# order to START them in (longest first -- see pick_order below). These
# are measured wall-clock times from one run on one machine, so treat
# them as a hint and nothing more: a stale or wrong number costs some
# scheduling efficiency and can never affect a result, because the tools
# are independent and each gets its own image and VM regardless.
#
# A tool with no entry here is assumed SLOW rather than fast. Being
# wrong in that direction costs nothing (a quick tool started early
# finishes early), while assuming a new tool is quick would risk making
# it the straggler everything else waits behind -- which is the exact
# problem this table exists to fix.
COST_S = {
    # Re-measured 2026-08-18, against the ring-3 desktop (which is the
    # only desktop now). Three of these moved a long way when their
    # tools were reshaped: `compositor` no longer spawns a stand-in
    # compositor and waits for it, `screen` drives its client in `auto`
    # mode instead of one keystroke at a time, and `compdeath` kills the
    # desktop directly. A stale cost is not a correctness problem -- it
    # only makes the start order slightly wrong -- but a tool whose real
    # cost has tripled would quietly become the straggler.
    # Re-measured 2026-08-21 from two full runs: the 663d63b pass cut
    # forcequit from ~71s to ~35s (it waits on observable client death
    # now, not a real ping timeout), so notepad (~41s) is the straggler.
    "notepad": 41,     # the current ceiling -- slowest single tool
    "forcequit": 35,
    "menubar": 32,
    "idle": 10,        # eight captures a third of a second apart
    "font": 19,        # two face switches and a size change, each settled
    "gfxdemo": 24,
    "cursor": 21,
    "uapp": 19,
    "taskmgr": 19,
    "uidemo": 17,
    "blank": 17,
    "uterm": 16,
    "singleinst": 16,  # six launches, each waiting out a client's first frame
    "scrollbar": 15,
    "settings": 20,   # +4 scroll checks, incl. a resize and a wheel
    "calculator": 14,
    "entries": 13,
    "sched": 12,
    "winclient": 12,
    "compdeath": 11,
    "screen": 9,
    "crash": 9,
    "uiclient": 9,
    "dialog": 9,
    "compositor": 8,
}
COST_UNKNOWN_S = 90


def pick_order(picked):
    """Longest job first -- the standard fix for a parallel makespan.

    Order matters here for one reason: with `-j4`, the run cannot finish
    before its slowest tool does, so a slow tool that STARTS late adds
    its whole duration to the tail. `forcequit` (71s) sat eleventh of
    fourteen in dependency order and finished alone, well after the
    other thirteen had drained -- 339s of tool-time took 116s of wall
    clock when the theoretical floor was ~85s.

    This is LPT scheduling (Graham 1969), which is within 4/3 of optimal
    and costs one sort. Deliberately applied to the START order ONLY:
    the summary table is still rebuilt in the declared dependency order,
    so a toolkit regression still reads before the apps built on it.

    Ties keep declaration order (`sorted` is stable), so the ordering is
    deterministic and a run is reproducible.
    """
    return sorted(picked, key=lambda t: -COST_S.get(t[0], COST_UNKNOWN_S))


def run_one(name, script, disk_src, timeout, keep_logs, slot, kvm=False):
    """Run one tool against its own image, in VM slot `slot`.

    Everything that could collide between two concurrently running
    tools is derived from `slot`: vm.py's pidfile/serial socket/QMP
    port/VNC display (`--instance`), and the `--sock`/`--qmp-port` the
    tool itself connects with. The disk copy is already per-tool.

    Note the `stop` below is also slot-scoped -- it used to be a bare
    `vm.py stop`, which under parallelism would have killed a sibling's
    VM rather than a leftover of its own.
    """
    tool = os.path.join(HERE, script)
    if not os.path.exists(tool):
        return ("SKIP", 0.0, f"no such tool: {script}")

    img = os.path.join(tempfile.gettempdir(), f"gui_regress_{name}.img")
    vm = os.path.join(HERE, "vm.py")
    inst = ["--instance", str(slot)]
    sock = ".vm.serial" if slot == 0 else f".vm.{slot}.serial"
    qmp_port = 4445 + slot

    subprocess.run([sys.executable, vm] + inst + ["stop"], cwd=REPO,
                   capture_output=True)
    # --reflink=auto: a copy-on-write clone where the filesystem
    # supports it, a plain copy where it doesn't. Never --reflink=always,
    # which fails outright on ext4. --sparse=always because disk.img is
    # ~4 MB of data in a 9 GB sparse file, and the destination is
    # usually /tmp on a tmpfs -- filling the holes in would cost 9 GB of
    # RAM per tool (`cp` defaults to --sparse=auto and gets this right
    # already; stating it means a future edit cannot quietly lose it,
    # which is exactly how damage_hunt.py's shutil.copyfile did).
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", disk_src, img],
                   cwd=REPO, check=True)

    # KVM is a per-run choice, passed straight to vm.py. Opt-in, never
    # the default -- see launch_qemu_cmd()'s comment on why a gate
    # quietly switched to KVM stops covering the emulated path.
    kvm_args = ["--kvm"] if kvm else []

    started = time.time()
    try:
        # CHECK vm.py's exit code. It used to be ignored, so a guest
        # that never started -- a refused port, a stale ISO, a QEMU
        # that died -- showed up only as the TOOL failing with
        # "could not connect to QMP", which says nothing about why and
        # points at the tool rather than at the launch. Eighteen tools
        # reported that at once before this existed, and the actual
        # reason was one line on vm.py's stderr that nobody read.
        boot = subprocess.run([sys.executable, vm] + inst + kvm_args +
                              ["--disk", img, "start"],
                              cwd=REPO, capture_output=True, text=True,
                              timeout=120)
        if boot.returncode != 0:
            why = (boot.stderr or boot.stdout or "").strip().splitlines()
            return ("FAIL", time.time() - started,
                    "the guest never started: "
                    + (why[0] if why else f"vm.py exited {boot.returncode}"))
        r = subprocess.run([sys.executable, tool,
                            "--sock", sock, "--qmp-port", str(qmp_port)],
                           cwd=REPO, capture_output=True, text=True,
                           timeout=timeout)
        out = r.stdout + r.stderr
        rc = r.returncode
    except subprocess.TimeoutExpired:
        out, rc = f"TIMEOUT after {timeout}s", 124
    finally:
        subprocess.run([sys.executable, vm] + inst + ["stop"], cwd=REPO,
                       capture_output=True)
        # The per-tool image has served its purpose once the VM is
        # stopped. Left behind, a full run leaves thirteen of them in
        # /tmp -- sparse, so cheap on disk, but that is RAM on a tmpfs
        # and they are stale the moment `make iso` reseeds disk.img.
        try:
            os.unlink(img)
        except OSError:
            pass

    if keep_logs:
        # The slot goes in the LOG, not only on the console. An
        # intermittent failure is diagnosed after the fact from these
        # files, and "which slot was it on" is the first question asked
        # of one -- it was unanswerable once already, which is how a
        # slot correlation ended up recorded in docs/roadmap.md on no
        # evidence and had to be withdrawn.
        with open(os.path.join(keep_logs, f"{name}.log"), "w") as f:
            f.write(f"# tool={name} slot={slot} qmp={4445 + slot} "
                    f"serial={'.vm.serial' if slot == 0 else f'.vm.{slot}.serial'}\n")
            f.write(out)

    # The tools all print a "<name>: N passed, M failed" summary line;
    # pull it out for the table, but fall back to the last line rather
    # than to nothing if a tool died before printing one.
    summary = ""
    for line in out.splitlines():
        if "passed," in line and "failed" in line:
            summary = line.strip()
    if not summary:
        tail = [l for l in out.splitlines() if l.strip()]
        summary = tail[-1].strip() if tail else "(no output)"

    return ("PASS" if rc == 0 else "FAIL", time.time() - started, summary)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-k", "--only", action="append", metavar="NAME",
                    help="run only these tools (repeatable); matches on the short name")
    ap.add_argument("--disk", default="disk.img",
                    help="image to copy for each run (default: disk.img)")
    ap.add_argument("--timeout", type=int, default=180,
                    help="per-tool timeout in seconds (default: 180). The slowest "
                         "real tool is ~41s; 180 keeps a wide margin while bounding "
                         "a hung guest to minutes, not the old 10.")
    ap.add_argument("--logs", metavar="DIR",
                    help="write each tool's full output to DIR/<name>.log")
    ap.add_argument("--list", action="store_true", help="list the tools and exit")
    ap.add_argument("--kvm", action="store_true",
                    help="run the guests under KVM instead of TCG -- much "
                         "faster, and deliberately NOT the default: every "
                         "automated test here runs TCG, and the difference is "
                         "load-bearing (see tools/kvm_soak.py). Use it to "
                         "iterate, not to judge a release.")
    ap.add_argument("-j", "--jobs", type=int, default=DEFAULT_JOBS, metavar="N",
                    help=f"run N tools concurrently, each in its own VM slot "
                         f"(default: {DEFAULT_JOBS}). -j1 is the old serial "
                         f"behaviour, one VM at a time in slot 0.")
    args = ap.parse_args()

    if args.list:
        for name, script, what in TOOLS:
            print(f"  {name:<12} {script:<28} {what}")
        return 0

    picked = [t for t in TOOLS
              if not args.only or any(k in t[0] for k in args.only)]
    if not picked:
        print(f"gui_regress: nothing matched {args.only}")
        return 2

    disk = args.disk if os.path.isabs(args.disk) else os.path.join(REPO, args.disk)
    if not os.path.exists(disk):
        print(f"gui_regress: no {disk} -- run `make iso` first")
        return 2
    if args.logs:
        os.makedirs(args.logs, exist_ok=True)

    if shutil.which("qemu-system-x86_64") is None:
        print("gui_regress: qemu-system-x86_64 not on PATH")
        return 2

    jobs = max(1, min(args.jobs, len(picked)))
    print(f"gui_regress: {len(picked)} tool(s), each on its own copy of "
          f"{os.path.basename(disk)}, {jobs} at a time\n")

    # Each concurrent tool gets a VM slot, and the slot is what keeps
    # two of them from sharing a pidfile, a serial socket or a QMP port
    # (see vm.py's _apply_instance and run_one above).
    #
    # Slots are LEASED from a pool, not derived from the tool's position
    # in the list. Position looks equivalent and is not: with `-j4` and
    # seven tools, task 4 also maps to slot 0, but it starts as soon as
    # ANY worker frees up -- which is routinely while task 0 is still
    # running on slot 0. Written that way first, and the symptom was
    # ugly and misleading: the fifth tool's `vm.py --instance 0 stop`
    # killed the first tool's VM out from under it, so the FIRST tool
    # died on a broken pipe and the fifth died on a screenshot that was
    # never written. Neither traceback pointed anywhere near the
    # scheduling. A lease is held for exactly as long as the VM exists.
    free_slots = queue.Queue()
    for s in range(jobs):
        free_slots.put(s)

    def work(name, script, what):
        slot = free_slots.get()
        try:
            status, secs, summary = run_one(name, script, disk, args.timeout,
                                            args.logs, slot, args.kvm)
        finally:
            free_slots.put(slot)
        return (name, what, slot, status, secs, summary)

    done = {}
    with cf.ThreadPoolExecutor(max_workers=jobs) as pool:
        # Submitted longest-first (pick_order), not in declaration
        # order: a ThreadPoolExecutor starts tasks in submission order,
        # so this is what keeps the slowest tool from being the tail.
        futures = [pool.submit(work, *t) for t in pick_order(picked)]
        # as_completed, not map: results print the moment each tool
        # finishes rather than in submission order, so one slow tool
        # doesn't hold up everything behind it. The summary table below
        # is rebuilt in the original (dependency) order regardless.
        for fut in cf.as_completed(futures):
            name, what, slot, status, secs, summary = fut.result()
            print(f"=== {name}: {what}  [slot {slot}]\n"
                  f"    {status}  ({secs:.0f}s)  {summary}\n", end="\n")
            done[name] = (status, secs, summary)

    results = [(name, *done[name]) for name, _s, _w in picked if name in done]

    print("gui_regress: summary")
    for name, status, secs, summary in results:
        print(f"  {status:<5} {name:<12} {secs:5.0f}s  {summary}")

    failed = [r[0] for r in results if r[1] == "FAIL"]
    if failed:
        print(f"\ngui_regress: FAILED -- {', '.join(failed)}")
        if not args.logs:
            print("  re-run with --logs DIR to keep each tool's full output")
        return 1
    print("\ngui_regress: all clear")
    return 0


if __name__ == "__main__":
    sys.exit(main())
