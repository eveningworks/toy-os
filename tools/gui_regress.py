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
# by memory; four keeps a 7-tool run to two rounds while leaving the
# machine usable. `-j1` restores the original serial behaviour exactly.
DEFAULT_JOBS = 4

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
]


def run_one(name, script, disk_src, timeout, keep_logs, slot):
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

    started = time.time()
    try:
        subprocess.run([sys.executable, vm] + inst + ["--disk", img, "start"],
                       cwd=REPO, capture_output=True, timeout=120)
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
        with open(os.path.join(keep_logs, f"{name}.log"), "w") as f:
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
    ap.add_argument("--timeout", type=int, default=600,
                    help="per-tool timeout in seconds (default: 600)")
    ap.add_argument("--logs", metavar="DIR",
                    help="write each tool's full output to DIR/<name>.log")
    ap.add_argument("--list", action="store_true", help="list the tools and exit")
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
                                            args.logs, slot)
        finally:
            free_slots.put(slot)
        return (name, what, slot, status, secs, summary)

    done = {}
    with cf.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(work, *t) for t in picked]
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
