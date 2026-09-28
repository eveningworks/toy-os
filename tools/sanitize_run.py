#!/usr/bin/env python3
"""Build toy-os with UBSAN=1 and/or KASAN=1 and list every site that fires.

WHAT IT RUNS. A copy of this tree (tracked files plus new untracked
ones, uncommitted edits included) is built with `make UBSAN=1 KASAN=1
iso` -- or one of them, with --ubsan / --kasan -- in a scratch
directory, and then:

  desktop   a vm.py boot to the desktop, left to run, then `dmesg` and
            `log` -- the kernel's reports and the APPLICATION log, where
            a service's or a desktop app's stderr goes (not the serial)
  ktest     the in-kernel suite (ktest_run.py -v)
  usertest  the ring-3 suite (usertest_run.py, with its transcript)

Every report from all three is collected, and each SITE is listed once
with where it was seen:

  UBSAN: <check> at <file>:<line>:<col>: ...     (both rings)
  KASAN: <kind> in <func>+0x..: <read|write> ... (kernel), whose
         link-time pc is resolved to file:line with addr2line

WHY A SCRATCH COPY. `make ... iso` re-seeds disk.img and rewrites
toy-os.iso, and a test runner must not do that to your checkout unasked
(the rule tools/ondemand_sweep.py states for DIRTIES_IMAGE). The copy
has its own build/, iso/, seed/ and disk.img; yours are never touched.

THE POSITIVE CONTROLS. kernel/lib/ubsan_test.c and
userland/tests/ubsan_test.c each commit one real signed overflow, and
kernel/lib/kasan_test.c commits one of every KASAN class (heap overflow,
use-after-free, double free, freed page, stack, global). Those sites are
expected, not findings -- and if any is MISSING, the build was not
instrumented the way it claims and a clean result means nothing, so the
run exits 2.

A SUITE FAILURE IS NOT A FINDING. An instrumented kernel is several
times slower, and KASAN's quarantine delays every kfree's merge, so a
test with a wall-clock budget or an exact heap-layout expectation can
fail here and pass on a normal build. Each suite's verdict is printed;
only reported sites decide the exit code.

    python3 tools/sanitize_run.py                 # both, all three phases
    python3 tools/sanitize_run.py --kasan         # KASAN only
    python3 tools/sanitize_run.py --only ktest    # one phase
    python3 tools/sanitize_run.py --keep DIR      # keep the tree and logs

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

UBSAN_CONTROLS = ("kernel/lib/ubsan_test.c", "userland/tests/ubsan_test.c")
KASAN_CONTROL = "kernel/lib/kasan_test.c"
# One per class kasan_test.c commits, matched against the report's kind.
KASAN_CONTROL_KINDS = ("heap-out-of-bounds", "use-after-free", "double-free",
                       "a freed page", "stack-out-of-bounds", "global-out-of-bounds")

UBSAN_RE = re.compile(r"UBSAN: ([a-z-]+) at (\S+?):(\d+):(\d+): (.*)$")
PC_RE = re.compile(r" \(pc 0x[0-9a-f]+\)\s*$")
KASAN_RE = re.compile(r"KASAN: (.+?) in (\S+): (read|write) of size (\d+) at (0x[0-9a-f]+)")
KASAN_PC_RE = re.compile(r"link-time pc (0x[0-9a-f]+)")

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
    """Runs `cmd` with the whole transcript going to `log`."""
    with open(log, "w") as f:
        try:
            p = subprocess.run(cmd, cwd=cwd, stdout=f, stderr=subprocess.STDOUT,
                               timeout=timeout)
            return p.returncode
        except subprocess.TimeoutExpired:
            f.write(f"\nsanitize_run: TIMED OUT after {timeout}s\n")
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
    rc = run([sys.executable, os.path.join(tree, "tools", "ktest_run.py"), "-v",
              "--timeout", str(timeout // 2)], tree, log, timeout)
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
    """sites: key -> {"tool", "kind", "where", "msg", "pc"}."""
    for path in paths:
        if not os.path.exists(path):
            continue
        with open(path, errors="replace") as f:
            lines = f.read().splitlines()
        for i, line in enumerate(lines):
            m = UBSAN_RE.search(line)
            if m:
                check, file, ln, col, msg = m.groups()
                ring = "ring 3" if PC_RE.search(msg) else "kernel"
                s = sites.setdefault(("UBSAN", f"{file}:{ln}:{col}"),
                                     {"kind": check, "msg": PC_RE.sub("", msg),
                                      "file": file, "where": set()})
                s["where"].add(f"{phase}/{ring}")
                continue
            m = KASAN_RE.search(line)
            if m:
                kind, func, rw, size, addr = m.groups()
                pc = None
                if i + 1 < len(lines):
                    p = KASAN_PC_RE.search(lines[i + 1])
                    pc = p.group(1) if p else None
                s = sites.setdefault(("KASAN", pc or func),
                                     {"kind": kind, "msg": f"{rw} of size {size} in {func}",
                                      "pc": pc, "file": None, "where": set()})
                s["where"].add(phase)


def resolve(tree, sites):
    """KASAN sites carry a link-time pc; name them by file:line."""
    elf = next((p for p in (os.path.join(tree, "build", "kernel.debug"),
                            os.path.join(tree, "build", "kernel.bin")) if os.path.exists(p)), None)
    pcs = [s["pc"] for (tool, _), s in sites.items() if tool == "KASAN" and s["pc"]]
    if not elf or not pcs or not shutil.which("addr2line"):
        return
    # The pc is a RETURN address -- the byte after the call -- so ask for
    # the one before it, or the line reported is the next statement.
    out = subprocess.run(["addr2line", "-e", elf] + [hex(int(p, 16) - 1) for p in pcs],
                         capture_output=True, text=True).stdout.splitlines()
    by_pc = dict(zip(pcs, out))
    for (tool, _), s in sites.items():
        if tool == "KASAN" and s["pc"] in by_pc:
            loc = by_pc[s["pc"]]
            loc = loc[loc.find("/kernel/") + 1:] if "/kernel/" in loc else loc
            loc = loc[loc.find("apps/"):] if loc.startswith("/") and "/apps/" in loc else loc
            s["loc"] = loc.split(" (")[0]
            s["file"] = s["loc"].rsplit(":", 1)[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ubsan", action="store_true", help="UBSAN only (default: both)")
    ap.add_argument("--kasan", action="store_true", help="KASAN only (default: both)")
    ap.add_argument("--only", choices=PHASES, action="append",
                    help="run only this phase (repeatable)")
    ap.add_argument("--keep", metavar="DIR",
                    help="build in DIR and keep it, logs in DIR/sanitize-logs")
    ap.add_argument("--instance", type=int, default=5,
                    help="vm.py slot for the desktop and usertest phases (default 5)")
    ap.add_argument("--settle", type=int, default=45,
                    help="seconds the desktop runs before its logs are read")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--timeout", type=int, default=2400,
                    help="per build and per phase, in seconds")
    args = ap.parse_args()
    ubsan = args.ubsan or not args.kasan
    kasan = args.kasan or not args.ubsan
    phases = args.only or list(PHASES)

    tree = args.keep or tempfile.mkdtemp(prefix="toyos-sanitize-")
    os.makedirs(tree, exist_ok=True)
    logs = os.path.join(tree, "sanitize-logs")
    os.makedirs(logs, exist_ok=True)
    flags = (["UBSAN=1"] if ubsan else []) + (["KASAN=1"] if kasan else [])
    print(f"sanitize_run: tree {tree}")
    try:
        n = copy_tree(tree)
        print(f"sanitize_run: {n} files copied; building with {' '.join(flags)} ...", flush=True)
        build_log = os.path.join(logs, "build.log")
        rc = run(["make", f"-j{args.jobs}"] + flags + ["iso"], tree, build_log, args.timeout)
        if rc != 0:
            print(f"sanitize_run: BUILD FAILED (rc {rc}) -- see {build_log}")
            return 2

        sites, verdicts = {}, {}
        for ph in phases:
            print(f"sanitize_run: {ph} ...", flush=True)
            if ph == "desktop":
                rc, paths = phase_desktop(tree, logs, args.instance, args.settle, args.timeout)
            elif ph == "ktest":
                rc, paths = phase_ktest(tree, logs, args.timeout)
            else:
                rc, paths = phase_usertest(tree, logs, args.instance, args.timeout)
            verdicts[ph] = rc
            collect(ph, paths, sites)
        resolve(tree, sites)

        print()
        for ph in phases:
            rc = verdicts[ph]
            print(f"  {ph:9} {'ok' if rc == 0 else 'TIMED OUT' if rc is None else f'exit {rc}'}")

        broken = []
        files = {s["file"] for s in sites.values() if s["file"]}
        if ubsan and "ktest" in phases and UBSAN_CONTROLS[0] not in files:
            broken.append(f"no UBSAN report from {UBSAN_CONTROLS[0]}: the kernel is not instrumented")
        if ubsan and "usertest" in phases and UBSAN_CONTROLS[1] not in files:
            broken.append(f"no UBSAN report from {UBSAN_CONTROLS[1]}: ring 3 is not instrumented")
        if kasan and "ktest" in phases:
            seen = {s["kind"] for s in sites.values() if s["file"] == KASAN_CONTROL}
            for k in KASAN_CONTROL_KINDS:
                if not any(k in kind for kind in seen):
                    broken.append(f"no KASAN '{k}' report from {KASAN_CONTROL}")

        control = set(UBSAN_CONTROLS) | {KASAN_CONTROL}
        found = {k: v for k, v in sites.items() if v["file"] not in control}
        print(f"\nsanitize_run: {len(found)} site(s) reported")
        for (tool, key), s in sorted(found.items(), key=lambda kv: (kv[0][0], str(kv[1].get("loc") or kv[0][1]))):
            where = ", ".join(sorted(s["where"]))
            print(f"  {tool} {s.get('loc') or key}  {s['kind']}  [{where}]")
            print(f"      {s['msg']}")
        for b in broken:
            print(f"sanitize_run: POSITIVE CONTROL FAILED -- {b}")
        if broken:
            return 2
        return 1 if found else 0
    finally:
        if args.keep:
            print(f"sanitize_run: logs in {logs}")
        else:
            # Keep the logs, drop the tree: a finding is read from them.
            kept = tempfile.mkdtemp(prefix="toyos-sanitize-logs-")
            shutil.copytree(logs, kept, dirs_exist_ok=True)
            shutil.rmtree(tree, ignore_errors=True)
            print(f"sanitize_run: logs kept in {kept}")


if __name__ == "__main__":
    sys.exit(main())
