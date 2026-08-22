#!/usr/bin/env python3
"""Ctrl-Z suspends a job, `jobs` lists it, and `fg` brings it back.

WHAT IS UNDER TEST
------------------
Job control end to end, through the real keyboard: SUSP arrives in the
keyboard IRQ, the line discipline recognises it, the console's
foreground GROUP is stopped, `/bin/tosh` hears about it through
SYS_WAITPID's SYS_WUNTRACED and puts it in its job table, and `fg`
hands the terminal back and resumes it.

The sibling of ctrlc_test.py, and deliberately the same shape -- the two
keys go down the same path and differ only in what they do at the end of
it.

Seven properties, each with a failure the others would not catch:

1. **The job SURVIVES.** This is the whole difference from Ctrl-C: the
   process is still there, still holding its memory. A Ctrl-Z that
   killed the job would pass every check about the shell.

2. **And it STOPS.** Surviving is not enough -- a suspended process that
   keeps running is what a missing check in the scheduler's picker looks
   like, and it is invisible from the process list alone. Measured from
   the CPU column, which cannot advance for something nothing schedules.

3. **The shell gets its prompt back**, which is what says the terminal
   came back to it rather than staying with a job nobody is running.

4. **`jobs` lists it**, with an id -- because a suspended job nobody can
   name is one nobody can resume.

5. **`fg` resumes it**: running again, accruing CPU again, and holding
   the terminal again. That last part is asserted by Ctrl-C'ing it
   afterwards: if `fg` had not handed the terminal over, the key would
   go to the line editor and the job would survive.

6. **A PIPELINE suspends as a unit and resumes as a unit.** Two stages
   are one group and one job; suspending only the reported stage would
   leave the other running, and resuming only one would leave the shell
   waiting on a stage nothing will continue.

7. **Ctrl-Z at an empty prompt does nothing.** With no job in front, the
   shell must not suspend itself -- there would be nobody left to resume
   it, and the session would be over.

PRECONDITIONS THIS TOOL ESTABLISHES ITSELF
------------------------------------------
Boots twice, as ctrlc_test.py does: the first boot sets the `text`
target and the US keyboard layout (a QMP qcode names a physical key by
its US label, and this OS defaults to `se`), the second is the one under
test. Both run against a COPY of disk.img.

Usage:
    python3 tools/jobs_test.py
    python3 tools/jobs_test.py --instance 2   # alongside another VM

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
# Where `jobs` output is redirected to. A builtin prints through the
# shell's sink, which goes to the physical screen -- unreadable from
# this socket -- so it is redirected to a file and read back through the
# kernel's own `cat`, which is an INDEPENDENT path to the same bytes.
JOBS_OUT = "/jobs.txt"

checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + ("" if ok or not detail else f"    [{detail}]"))
    return bool(ok)


def vm_run(disk, instance, *argv):
    cmd = [sys.executable, VM, "--disk", disk]
    if instance:
        cmd += ["--instance", str(instance)]
    r = subprocess.run(cmd + list(argv), cwd=REPO, capture_output=True, text=True)
    return r.stdout + r.stderr


def ps_rows(dbg):
    """Every process as (pid, state, cpu, name), from `/bin/ps`.

    Through the debug console's `sh`, which runs it in the KERNEL's
    shell -- a different reader from the ring-3 one under test, so a
    broken tosh cannot make this agree with it.
    """
    out = dbg.send("sh ps") or ""
    rows = []
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 7 and p[0].isdigit() and p[1].isdigit():
            try:
                rows.append((int(p[0]), p[3], float(p[4]), p[-1]))
            except ValueError:
                continue
    return rows


def spinners(dbg):
    return [(pid, state, cpu) for pid, state, cpu, name in ps_rows(dbg)
            if name.startswith("spin_test")]


def shell_pids(dbg):
    return [pid for pid, _s, _c, name in ps_rows(dbg) if name.startswith("tosh")]


def wait_for(fn, want, timeout=10.0):
    end = time.time() + timeout
    last = fn()
    while time.time() < end:
        if want(last):
            return last
        time.sleep(0.25)
        last = fn()
    return last


def type_line(flow, text):
    flow.type_command(text)
    flow.session.send_key("ret")


def read_file(dbg, path):
    return dbg.send(f"sh cat {path}") or ""


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
            print("jobs_test: no free VM slot")
            return 2
        args.instance = n
        print(f"jobs_test: slot {args.instance} (QMP {4445 + args.instance})")
    args.instance = int(args.instance)

    tmp = None
    if not args.disk:
        src = os.path.join(REPO, "disk.img")
        if not os.path.exists(src):
            print("jobs_test: no disk.img -- run `make iso` first")
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
            print("jobs_test: could not start the VM")
            return 2
        dbg = DebugConsole(sock)
        dbg.send("sh keyboard us")
        dbg.send("sh config set system.default_target text")
        dbg.send(f"sh rm {JOBS_OUT}")
        time.sleep(0.5)
        vm_run(args.disk, args.instance, "stop")

        print("second boot -- the one under test")
        if "ready" not in vm_run(args.disk, args.instance, "start"):
            print("jobs_test: the text-target boot never became ready")
            return 2
        dbg = DebugConsole(sock)
        flow = ShellFlow(qmp_port=port)
        time.sleep(2.0)   # let init get tosh to a prompt

        shell = shell_pids(dbg)
        if not check("a ring-3 shell is on the console", bool(shell),
                     f"processes={ps_rows(dbg)}"):
            return report()
        shell_pid = shell[0]

        # --- 1-3. suspend one job -------------------------------------
        print("a running job, then Ctrl-Z")
        type_line(flow, SPINNER)
        running = wait_for(lambda: spinners(dbg), lambda v: len(v) == 1)
        if not check("the job started", len(running) == 1, f"spinners={running}"):
            return report()

        flow.session.combo(["ctrl", "z"])
        stopped = wait_for(lambda: spinners(dbg),
                           lambda v: len(v) == 1 and v[0][1] == "stopped")
        check("Ctrl-Z leaves the job ALIVE", len(stopped) == 1,
              f"spinners={stopped}")
        check("...and the kernel reports it as stopped",
              len(stopped) == 1 and stopped[0][1] == "stopped",
              f"spinners={stopped}")

        # THE CHECK A PROCESS LIST CANNOT MAKE. "stopped" is a label; not
        # being scheduled is the behaviour, and only the CPU column can
        # see the difference.
        cpu_a = stopped[0][2] if stopped else -1.0
        time.sleep(2.0)
        now = spinners(dbg)
        cpu_b = now[0][2] if now else -1.0
        check("...and it stops accruing CPU time", cpu_a >= 0 and cpu_b == cpu_a,
              f"cpu {cpu_a} -> {cpu_b}")

        # --- 4. `jobs` ------------------------------------------------
        print("`jobs` lists it")
        type_line(flow, f"jobs > {JOBS_OUT}")
        time.sleep(1.5)
        listing = read_file(dbg, JOBS_OUT)
        check("`jobs` lists the suspended job", "Stopped" in listing and "[1]" in listing,
              f"jobs output: {listing!r}")
        check("...naming the command it suspended", "spin_test" in listing,
              f"jobs output: {listing!r}")

        # --- 5. `fg` --------------------------------------------------
        print("`fg` brings it back")
        type_line(flow, "fg")
        resumed = wait_for(lambda: spinners(dbg),
                           lambda v: len(v) == 1 and v[0][1] != "stopped")
        check("`fg` resumes the job", len(resumed) == 1 and resumed[0][1] != "stopped",
              f"spinners={resumed}")
        cpu_c = resumed[0][2] if resumed else -1.0
        time.sleep(2.0)
        now = spinners(dbg)
        check("...and it accrues CPU again", now and now[0][2] > cpu_c,
              f"cpu {cpu_c} -> {now[0][2] if now else None}")

        # THE TERMINAL WENT WITH IT. If `fg` had resumed the job without
        # handing the console over, this Ctrl-C would reach the line
        # editor instead and the job would survive.
        flow.session.combo(["ctrl", "c"])
        gone = wait_for(lambda: spinners(dbg), lambda v: not v)
        check("...and it holds the TERMINAL -- Ctrl-C reaches it", not gone,
              f"still running: {gone}")

        # --- 6. a pipeline --------------------------------------------
        print("a two-stage pipeline, then Ctrl-Z")
        type_line(flow, f"{SPINNER} | {SPINNER}")
        both = wait_for(lambda: spinners(dbg), lambda v: len(v) >= 2)
        if not check("both stages started", len(both) >= 2, f"spinners={both}"):
            return report()

        flow.session.combo(["ctrl", "z"])
        susp = wait_for(lambda: spinners(dbg),
                        lambda v: len(v) >= 2 and all(s[1] == "stopped" for s in v))
        check("Ctrl-Z suspends EVERY stage of the pipeline",
              len(susp) >= 2 and all(s[1] == "stopped" for s in susp),
              f"spinners={susp}")

        type_line(flow, f"jobs > {JOBS_OUT}")
        time.sleep(1.5)
        listing = read_file(dbg, JOBS_OUT)
        check("...listed as ONE job, not two",
              listing.count("Stopped") == 1, f"jobs output: {listing!r}")

        type_line(flow, "fg")
        back = wait_for(lambda: spinners(dbg),
                        lambda v: len(v) >= 2 and all(s[1] != "stopped" for s in v))
        check("`fg` resumes every stage",
              len(back) >= 2 and all(s[1] != "stopped" for s in back),
              f"spinners={back}")

        flow.session.combo(["ctrl", "c"])
        wait_for(lambda: spinners(dbg), lambda v: not v)

        # --- 7. an empty prompt ---------------------------------------
        #
        # WITH NO JOB IN FRONT, Ctrl-Z MUST NOT SUSPEND ANYTHING -- least
        # of all the shell, which nobody would be left to resume. The
        # byte goes to the line editor, which has no meaning for it.
        print("Ctrl-Z at an empty prompt suspends nothing")
        flow.session.combo(["ctrl", "z"])
        time.sleep(1.0)
        rows = ps_rows(dbg)
        alive = [pid for pid, _s, _c, name in rows if name.startswith("tosh")]
        check("the shell survives Ctrl-Z at a prompt", shell_pid in alive,
              f"tosh pids now {alive}")
        check("...and is not itself stopped",
              all(state != "stopped" for pid, state, _c, _n in rows if pid == shell_pid),
              f"processes={rows}")

        # The shell still WORKS, which "it exists and is not stopped"
        # does not prove on its own.
        type_line(flow, f"jobs > {JOBS_OUT}")
        time.sleep(1.5)
        listing = read_file(dbg, JOBS_OUT)
        check("control: the shell still runs commands",
              "Stopped" not in listing,
              f"jobs output: {listing!r} -- expected an empty table")

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
    print(f"\njobs_test: {'FAIL' if failed else 'PASS'} -- "
          f"{len(checks) - len(failed)} passed, {len(failed)} failed")
    for name, _, detail in failed:
        print(f"  FAILED: {name}" + (f"    [{detail}]" if detail else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
