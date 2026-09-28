#!/usr/bin/env python3
"""Build toy-os with UBSAN=1 and list every undefined-behaviour site that fires.

WHAT IT RUNS. A copy of this tree (tracked files plus new untracked
ones, uncommitted edits included) is built with `make UBSAN=1 iso` in a
scratch directory, and then:

  desktop   a vm.py boot to the desktop, left to run, then `dmesg` and
            `log` -- the kernel's reports and the APPLICATION log, where
            a service's or a desktop app's stderr goes (not the serial)
  ktest     the in-kernel suite (ktest_run.py -v)
  usertest  the ring-3 suite (usertest_run.py)

Every "UBSAN: <check> at <file>:<line>:<col>: ..." line from all three is
collected, and each SITE is listed once with where it was seen.

WHY A SCRATCH COPY. `make UBSAN=1 iso` re-seeds disk.img and rewrites
toy-os.iso, and a test runner must not do that to your checkout unasked
(the rule tools/ondemand_sweep.py states for DIRTIES_IMAGE). The copy
has its own build/, iso/, seed/ and disk.img; yours are never touched.

THE POSITIVE CONTROL. kernel/lib/ubsan_test.c and
userland/tests/ubsan_test.c each commit one real signed overflow in a
UBSAN=1 build. Those two sites are expected, not findings -- and if
either is MISSING, the flags never reached that ring and a clean result
means nothing, so the run exits 2.

A SUITE FAILURE IS NOT A UBSAN FINDING. The instrumented build is
slower, so a test with a wall-clock budget can fail here and pass on a
normal build; each suite's verdict is printed, but only sites decide
the exit code.

    python3 tools/ubsan_run.py                   # all three phases
    python3 tools/ubsan_run.py --only ktest      # one phase
    python3 tools/ubsan_run.py --keep DIR        # keep the tree and logs

Exit: 0 no unexpected site; 1 unexpected sites (listed); 2 the build or
a positive control failed.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# The two sites the positive control commits on purpose.
CONTROL_KERNEL = "kernel/lib/ubsan_test.c"
CONTROL_USER = "userland/tests/ubsan_test.c"

LINE_RE = re.compile(r"UBSAN: ([a-z-]+) at (\S+?):(\d+):(\d+): (.*)$")
PC_RE = re.compile(r" \(pc 0x[0-9a-f]+\)\s*$")

PHASES = ("desktop", "ktest", "usertest")


def copy_tree(dst):
    """The sources as the working tree has them, without build products."""
    out = subprocess.run(["git", "ls-files", "-z", "--cached", "--others",
                          "--exclude-standard"], cwd=REPO, check=True,
                         capture_output=True).stdout
    n = 0
    for rel in out.decode().split("\0"):
        src = os.path.join(REPO, rel)
        if not rel or not os.path.lexists(src):   # deleted, not yet committed
            continue
        d = os.path.join(dst, rel)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        if os.path.islink(src):
            os.symlink(os.readlink(src), d)
        else:
            shutil.copy2(src, d)
        n += 1
    return n


def run(cmd, cwd, log, timeout):
    """Runs `cmd`, teeing nothing: the whole transcript goes to `log`."""
    with open(log, "w") as f:
        try:
            p = subprocess.run(cmd, cwd=cwd, stdout=f, stderr=subprocess.STDOUT,
                               timeout=timeout)
            return p.returncode
        except subprocess.TimeoutExpired:
            f.write(f"\nubsan_run: TIMED OUT after {timeout}s\n")
            return None


def phase_desktop(tree, logs, instance, settle, timeout):
    vm = [sys.executable, os.path.join(tree, "tools", "vm.py"), "--instance", str(instance)]
    log = os.path.join(logs, "desktop.log")
    rc = run(vm + ["start"], tree, os.path.join(logs, "desktop-start.log"), timeout)
    if rc != 0:
        return rc, [log]
    try:
        # Long enough for init's services and the desktop's clients to
        # have started and done their first work.
        time.sleep(settle)
        rc = run(vm + ["exec", "dmesg", "log"], tree, log, timeout)
    finally:
        run(vm + ["stop"], tree, os.path.join(logs, "desktop-stop.log"), 60)
    return rc, [log]


def phase_ktest(tree, logs, timeout):
    log = os.path.join(logs, "ktest.log")
    rc = run([sys.executable, os.path.join(tree, "tools", "ktest_run.py"), "-v"],
             tree, log, timeout)
    return rc, [log]


def phase_usertest(tree, logs, instance, timeout):
    log = os.path.join(logs, "usertest.log")
    serial = os.path.join(logs, "usertest-serial.log")
    # The TRANSCRIPT, not only the serial: a /tests program's stderr goes
    # to the console, which the harness reads over the debug console.
    transcript = os.path.join(logs, "usertest-transcript.log")
    rc = run([sys.executable, os.path.join(tree, "tools", "usertest_run.py"),
              "--instance", str(instance), "--serial-log", serial,
              "--transcript", transcript], tree, log, timeout)
    return rc, [log, serial, transcript]


def collect(phase, paths, sites):
    for path in paths:
        if not os.path.exists(path):
            continue
        with open(path, errors="replace") as f:
            for line in f:
                m = LINE_RE.search(line.rstrip("\n"))
                if not m:
                    continue
                check, file, ln, col, msg = m.groups()
                ring = "ring 3" if PC_RE.search(msg) else "kernel"
                msg = PC_RE.sub("", msg)
                s = sites.setdefault((check, file, int(ln), int(col)),
                                     {"msg": msg, "where": set()})
                s["where"].add(f"{phase}/{ring}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", choices=PHASES, action="append",
                    help="run only this phase (repeatable)")
    ap.add_argument("--keep", metavar="DIR",
                    help="build in DIR and keep it, logs in DIR/ubsan-logs")
    ap.add_argument("--instance", type=int, default=5,
                    help="vm.py slot for the desktop and usertest phases (default 5)")
    ap.add_argument("--settle", type=int, default=30,
                    help="seconds the desktop runs before its logs are read")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--timeout", type=int, default=1800,
                    help="per build and per phase, in seconds")
    args = ap.parse_args()
    phases = args.only or list(PHASES)

    tree = args.keep or tempfile.mkdtemp(prefix="toyos-ubsan-")
    os.makedirs(tree, exist_ok=True)
    logs = os.path.join(tree, "ubsan-logs")
    os.makedirs(logs, exist_ok=True)
    print(f"ubsan_run: tree {tree}")
    try:
        n = copy_tree(tree)
        print(f"ubsan_run: {n} files copied; building with UBSAN=1 ...", flush=True)
        build_log = os.path.join(logs, "build.log")
        rc = run(["make", f"-j{args.jobs}", "UBSAN=1", "iso"], tree, build_log, args.timeout)
        if rc != 0:
            print(f"ubsan_run: BUILD FAILED (rc {rc}) -- see {build_log}")
            return 2

        sites, verdicts = {}, {}
        for ph in phases:
            print(f"ubsan_run: {ph} ...", flush=True)
            if ph == "desktop":
                rc, paths = phase_desktop(tree, logs, args.instance, args.settle, args.timeout)
            elif ph == "ktest":
                rc, paths = phase_ktest(tree, logs, args.timeout)
            else:
                rc, paths = phase_usertest(tree, logs, args.instance, args.timeout)
            verdicts[ph] = rc
            collect(ph, paths, sites)

        print()
        for ph in phases:
            rc = verdicts[ph]
            print(f"  {ph:9} {'ok' if rc == 0 else 'TIMED OUT' if rc is None else f'exit {rc}'}")

        seen = {k[1] for k in sites}
        broken = []
        if "ktest" in phases and CONTROL_KERNEL not in seen:
            broken.append(f"no report from {CONTROL_KERNEL}: the kernel is not instrumented")
        if "usertest" in phases and CONTROL_USER not in seen:
            broken.append(f"no report from {CONTROL_USER}: ring 3 is not instrumented")

        found = {k: v for k, v in sites.items() if k[1] not in (CONTROL_KERNEL, CONTROL_USER)}
        print(f"\nubsan_run: {len(found)} site(s) reported undefined behaviour")
        for (check, file, ln, col), s in sorted(found.items(), key=lambda kv: kv[0][1:]):
            print(f"  {file}:{ln}:{col}  {check}  [{', '.join(sorted(s['where']))}]")
            print(f"      {s['msg']}")
        for b in broken:
            print(f"ubsan_run: POSITIVE CONTROL FAILED -- {b}")
        if broken:
            return 2
        return 1 if found else 0
    finally:
        if args.keep:
            print(f"ubsan_run: logs in {logs}")
        else:
            # Keep the logs, drop the tree: a finding is read from them.
            kept = tempfile.mkdtemp(prefix="toyos-ubsan-logs-")
            shutil.copytree(logs, kept, dirs_exist_ok=True)
            shutil.rmtree(tree, ignore_errors=True)
            print(f"ubsan_run: logs kept in {kept}")


if __name__ == "__main__":
    sys.exit(main())
