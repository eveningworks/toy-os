#!/usr/bin/env python3
"""Ctrl-C at the physical console interrupts the foreground JOB, and
nothing else.

WHAT IS UNDER TEST
------------------
Stages 0-2 of docs/signals-design.md, end to end and through the real
keyboard: a key arrives in the IRQ handler, the kernel recognises it as
INTR, signals the console's foreground process GROUP, and the members
die at their next return to ring 3 -- while the shell that is waiting on
them survives and puts a prompt back.

Five properties, each with a failure the others would not catch:

1. **A running job dies.** `spin_test` writes nothing and makes no
   syscalls while it spins, so the ONLY way it can be interrupted is the
   timer-tick delivery path. A job that made syscalls would be killed by
   the other path and this would say nothing about that one.

2. **The whole PIPELINE dies, not just one stage.** Two spinners joined
   by a pipe are two processes in one group; interrupting only the
   foreground *pid* would leave one running and the shell waiting on it
   forever. This is the check process groups exist for.

3. **The shell survives.** It is in the foreground group's parent, it
   ignores SIGINT, and a Ctrl-C that killed it would end the session --
   which every other check here would still pass, because they assert on
   the job rather than on the shell.

4. **At an EMPTY PROMPT, Ctrl-C is still a keystroke.** With no job
   running the kernel must NOT signal: the byte goes through and the
   line editor abandons the line. Asserted by typing a command, pressing
   Ctrl-C, then Enter -- if the line survived, the command runs and
   leaves its file behind. That file NOT existing is the check.

5. **The signal is SIGINT specifically**, from the kernel's own log, so
   a job that died of something else (a crash, a fault) cannot pass.

WHY NOT THE GUI TERMINAL
------------------------
It reads keys as window events rather than from fd 0, so it owns no
console and has no foreground group -- `Ctrl-C` there is a separate
piece of work (docs/roadmap.md's TTY track). This tool takes the desktop
out of the picture entirely by booting the `text` target, which is also
the only way to get a real ring-3 shell on the physical keyboard.

PRECONDITIONS THIS TOOL ESTABLISHES ITSELF
------------------------------------------
It boots twice, like console_shell_test.py: the first boot sets
`system.default_target text` and the US keyboard layout (a QMP qcode
names a physical key by its US label, and this OS defaults to `se`), the
second is the one under test. Both run against a COPY of disk.img.

Usage:
    python3 tools/ctrlc_test.py
    python3 tools/ctrlc_test.py --instance 2   # alongside another VM

Exits 0 if every check passed, 1 otherwise, 2 if it could not run.
"""

import argparse
import os
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
from gui_debug import DebugConsole      # noqa: E402
from shell_flow import ShellFlow        # noqa: E402
import port_guard                       # noqa: E402

VM = os.path.join(REPO, "tools", "vm.py")

# Long enough that nothing here can pass by the job simply having
# finished -- the failure mode this repo keeps writing rules about.
SPINNER = "spin_test 900000"
# Written by /tests/file_test. Its ABSENCE is what proves a line was
# cancelled rather than run.
MADE = "/filetest.txt"

checks = []


def check(name, ok, detail=""):
    # RETURNS the verdict, so a caller can bail out on a failed
    # precondition rather than running checks that would be vacuous. A
    # version of this that returned None made every `if not check(...)`
    # bail on the FIRST one and report a one-check pass.
    checks.append((name, bool(ok), detail))
    # The detail is the FAILURE's evidence, so it is printed only on one
    # -- a pass carrying "no such line in dmesg" reads as a contradiction
    # of itself.
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + ("" if ok or not detail else f"    [{detail}]"))
    return bool(ok)


def vm_run(disk, instance, *argv):
    cmd = [sys.executable, VM, "--disk", disk]
    if instance:
        cmd += ["--instance", str(instance)]
    r = subprocess.run(cmd + list(argv), cwd=REPO, capture_output=True, text=True)
    return r.stdout + r.stderr


def slots(dbg):
    """Every live scheduler slot as (pid, name).

    `kstack slots` rather than `ps` -- a deliberate choice, but NOT for
    the reason first written here ("ps is a /bin program, so its output
    goes to the screen"): `sh ps` runs it in the KERNEL's shell, whose
    output does reach this socket, and jobs_test.py relies on that.

    It stays because this tool only needs existence, and `kstack slots`
    is one fewer moving part. **If you need a process's STATE or its CPU
    time, use `DebugConsole.processes()`** (gui_debug.py) rather than
    extending this -- that is the shared parser, and CPU time is the only
    way to tell a suspended process from an idle one.
    """
    out = dbg.send("sh kstack slots") or ""
    rows = []
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 4 and p[0].isdigit() and p[1].isdigit():
            rows.append((int(p[1]), p[2]))
    return rows


def spinners(dbg):
    return [pid for pid, name in slots(dbg) if name.startswith("spin_test")]


def wait_for(fn, want, timeout=8.0):
    """Poll until fn() satisfies `want`, or give up. Returns fn()'s last
    value, so a caller reports what it actually saw rather than only
    that it timed out."""
    end = time.time() + timeout
    last = fn()
    while time.time() < end:
        if want(last):
            return last
        time.sleep(0.25)
        last = fn()
    return last


def dmesg(dbg, tries=6):
    prev = dbg.timeout
    try:
        dbg.timeout = 20.0
        out = ""
        for _ in range(tries):
            out = dbg.send("sh dmesg") or ""
            if out:
                return out
            time.sleep(0.5)
        return out
    finally:
        dbg.timeout = prev


def type_line(flow, text):
    flow.type_command(text)
    flow.session.send_key("ret")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disk", default=None)
    ap.add_argument("--instance", default="auto",
                    help="slot number, or `auto` for the lowest free one")
    ap.add_argument("--keep", action="store_true", help="leave the VM running")
    args = ap.parse_args()

    if args.instance == "auto":
        n = port_guard.find_free_instance()
        if n is None:
            print("ctrlc_test: no free VM slot")
            return 2
        args.instance = n
        print(f"ctrlc_test: slot {args.instance} (QMP {4445 + args.instance})")
    args.instance = int(args.instance)

    tmp = None
    if not args.disk:
        src = os.path.join(REPO, "disk.img")
        if not os.path.exists(src):
            print("ctrlc_test: no disk.img -- run `make iso` first")
            return 2
        tmp = tempfile.NamedTemporaryFile(suffix=".img", delete=False)
        tmp.close()
        subprocess.run(["cp", "--reflink=auto", "--sparse=always", src, tmp.name],
                       check=True)
        args.disk = tmp.name

    sock = ".vm.serial" if not args.instance else f".vm.{args.instance}.serial"
    port = 4445 + args.instance

    try:
        print("first boot -- configuring the target")
        if "ready" not in vm_run(args.disk, args.instance, "start"):
            print("ctrlc_test: could not start the VM")
            return 2
        dbg = DebugConsole(sock)
        dbg.send("sh keyboard us")
        dbg.send("sh config set system.default_target text")
        dbg.send(f"sh rm {MADE}")
        time.sleep(0.5)
        vm_run(args.disk, args.instance, "stop")

        print("second boot -- the one under test")
        if "ready" not in vm_run(args.disk, args.instance, "start"):
            print("ctrlc_test: the text-target boot never became ready")
            return 2
        dbg = DebugConsole(sock)
        flow = ShellFlow(qmp_port=port)
        time.sleep(2.0)   # let init get tosh to a prompt

        before = slots(dbg)
        shell = [pid for pid, name in before if name.startswith("tosh")]
        if not check("a ring-3 shell is on the console", bool(shell),
                     f"slots={before}"):
            return report()
        shell_pid = shell[0]
        check("nothing is spinning yet", not spinners(dbg), f"slots={before}")

        # --- 1. a single running job ----------------------------------
        print("a running job, then Ctrl-C")
        type_line(flow, SPINNER)
        running = wait_for(lambda: spinners(dbg), lambda v: len(v) == 1)
        if not check("the job started", len(running) == 1, f"spinners={running}"):
            return report()

        flow.session.combo(["ctrl", "c"])
        gone = wait_for(lambda: spinners(dbg), lambda v: not v)
        check("Ctrl-C ends the running job", not gone, f"still running: {gone}")

        alive = [pid for pid, name in slots(dbg) if name.startswith("tosh")]
        check("...and the SHELL survives it", shell_pid in alive,
              f"tosh pids now {alive}")

        log = dmesg(dbg)
        check("...by SIGINT specifically, per the kernel's own log",
              "terminated by SIGINT" in log,
              "no 'terminated by SIGINT' line in dmesg")

        # --- 2. a whole pipeline --------------------------------------
        #
        # THE CHECK PROCESS GROUPS EXIST FOR. Two stages, one group: a
        # Ctrl-C that reached only the foreground PID would leave one
        # spinner running and the shell waiting on it forever.
        print("a two-stage pipeline, then Ctrl-C")
        type_line(flow, f"{SPINNER} | {SPINNER}")
        both = wait_for(lambda: spinners(dbg), lambda v: len(v) >= 2)
        if not check("both stages started", len(both) >= 2, f"spinners={both}"):
            return report()

        flow.session.combo(["ctrl", "c"])
        left = wait_for(lambda: spinners(dbg), lambda v: not v)
        check("Ctrl-C ends EVERY stage of the pipeline", not left,
              f"still running: {left}")
        alive = [pid for pid, name in slots(dbg) if name.startswith("tosh")]
        check("...and the shell still survives", shell_pid in alive,
              f"tosh pids now {alive}")

        # --- 3. an empty prompt ---------------------------------------
        #
        # WITH NO JOB RUNNING, Ctrl-C MUST NOT SIGNAL. The byte goes
        # through to the line editor instead, which abandons the line --
        # so the command typed below must NOT run. That file's absence is
        # the whole check, and it is the one that would catch the INTR
        # recognition being made unconditional.
        print("Ctrl-C at an empty prompt cancels the LINE")
        dbg.send(f"sh rm {MADE}")
        time.sleep(0.5)
        flow.type_command("file_test")     # typed, deliberately NOT submitted
        time.sleep(0.5)
        flow.session.combo(["ctrl", "c"])
        time.sleep(0.5)
        flow.session.send_key("ret")       # submit whatever survived
        time.sleep(2.5)
        names = dbg.send("sh ls /") or ""
        check("a cancelled line does not run", "filetest.txt" not in names,
              "filetest.txt exists -- the line was submitted, not cancelled")

        # ...and the same shell is still there to have cancelled it,
        # which is what stops the check above passing because the shell
        # had died.
        alive = [pid for pid, name in slots(dbg) if name.startswith("tosh")]
        check("the shell is still the same one", shell_pid in alive,
              f"tosh pids now {alive}")

        # The POSITIVE CONTROL for that check: the same line, submitted
        # without a Ctrl-C, must leave the file. Without it, "the file is
        # absent" would pass equally well against a shell that could not
        # run anything at all.
        type_line(flow, "file_test")
        time.sleep(2.5)
        names = dbg.send("sh ls /") or ""
        check("control: the SAME line, uncancelled, does run",
              "filetest.txt" in names,
              "the shell could not run it either way -- the check above proves nothing")

    finally:
        if not args.keep:
            vm_run(args.disk, args.instance, "stop")
        if tmp:
            try:
                os.unlink(tmp.name)
            except OSError:
                pass

    return report()


def report():
    failed = [c for c in checks if not c[1]]
    print(f"\nctrlc_test: {'FAIL' if failed else 'PASS'} -- "
          f"{len(checks) - len(failed)} passed, {len(failed)} failed")
    for name, _, detail in failed:
        print(f"  FAILED: {name}" + (f"    [{detail}]" if detail else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
