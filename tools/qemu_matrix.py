#!/usr/bin/env python3
"""Run a test suite against SEVERAL QEMU versions, in Docker.

WHY THIS EXISTS
---------------
Every other automated test here runs on whatever QEMU this machine
happens to have, which hides a whole class of bug. Measured on
2026-08-19: a virtio-blk defect -- virtqueue_poll() spending a ~12 ms
budget it believed was 5 s, then letting late completions permanently
desync the used ring -- was invisible on QEMU 11.1 and reproduced every
single time on 8.2.2. Which clocksource the kernel picks depends on the
host, and that decided whether the bug appeared.

It was eventually found through GitHub CI, at six pushes and several
wrong diagnoses, because CI was the only place a second QEMU existed.
That is a bad loop: ~90 s per attempt at best, and the runs are not
always about your code at all (one failed with `apt-get install` timing
out after 8 minutes). This makes the second, third and fourth QEMU
local and fast.

WHAT IT DOES NOT DO
-------------------
It does not build anything in Docker. The ISO and disk image are built
on the HOST by `make iso` and mounted read-only, so what runs here is
byte-for-byte what a normal build produces -- no second toolchain to
drift. The container carries only qemu-system-x86 and python3.

It is also NOT a replacement for `make test`: it is slower (a container
per version) and it is for the question "does this depend on the host's
QEMU?", which is worth asking before a driver change lands and after
any CI failure that will not reproduce locally.

USAGE
    python3 tools/qemu_matrix.py                 # every known version
    python3 tools/qemu_matrix.py --versions 8.2  # just the runner's
    python3 tools/qemu_matrix.py --virtio        # filesystem on virtio-blk
    python3 tools/qemu_matrix.py --versions 8.2 --suite usertest --runs 5
                                                 # the ring-3 suite, as a rate
"""
import argparse
import os
import subprocess
import sys
from harness import copy_disk  # noqa: E402

# base image -> the QEMU it ships. Ubuntu 24.04 is what GitHub's
# ubuntu-latest runner uses, which makes it the one that matters most.
IMAGES = {
    "6.2":  "ubuntu:22.04",
    "7.2":  "debian:12",
    "8.2":  "ubuntu:24.04",   # GitHub's runner
}
DOCKERFILE = "tools/docker/Dockerfile.qemu"


def build(version, base, quiet=True):
    tag = f"toyos-qemu:{version}"
    cmd = ["docker", "build", "-f", DOCKERFILE, "-t", tag,
           "--build-arg", f"BASE={base}", "."]
    if quiet:
        cmd.insert(2, "-q")
    r = subprocess.run(cmd, capture_output=quiet, text=True)
    return tag if r.returncode == 0 else None


# What each suite runs and how it reports. usertest is here because
# ktest never hung on 8.2.2 while the ring-3 suite deadlocked the
# EMULATOR 3 runs in 4 (docs/bugs.md): the two suites drive the disk
# differently, so a clean ktest row says nothing about the other.
SUITES = {
    "ktest":    (["tools/ktest_run.py", "-v"], "ktest: ", ("PASSED", "FAILED")),
    "usertest": (["tools/usertest_run.py"], "usertest_run: ", ("passed",)),
}


def run_suite(tag, suite, virtio, timeout, run=1):
    """One suite run inside the container, against the host-built ISO.

    Returns (ok, detail), with ok None when the run HUNG. The container
    is NAMED and killed on a timeout: killing the `docker run` client
    alone leaves the container -- and its QEMU -- running.
    """
    # The disk is COPIED, never mounted read-write: the suite writes to
    # it, and the host's disk.img is re-seeded by `make iso` and may be
    # open in the user's own QEMU.
    work = ".qemu_matrix"
    os.makedirs(work, exist_ok=True)
    # Sparse, always: disk.img is ~4 MB of data in a 9 GB sparse file, and
    # a hole-filling copy costs the 9 GB (CLAUDE.md).
    copy_disk("disk.img", f"{work}/disk.img")
    script, prefix, verdicts = SUITES[suite]
    args = ["python3", "-u"] + script + ["--disk", f"{work}/disk.img"]
    if suite == "ktest":
        args += ["--qemu-log", f"{work}/qemu.log"]
        if virtio:
            copy_disk("disk.img", f"{work}/virtio.img")
            args += ["--virtio-disk", f"{work}/virtio.img"]
    else:
        # The guest's own record, which survives an emulator that hangs.
        args += ["--serial-log", f"{work}/serial-{tag.split(':')[-1]}-{run}.log"]
    name = f"toyos-matrix-{os.getpid()}-{tag.split(':')[-1]}-{run}"
    cmd = ["docker", "run", "--rm", "--name", name,
           "--user", f"{os.getuid()}:{os.getgid()}",
           "-v", f"{os.getcwd()}:/toyos", tag] + args
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        subprocess.run(["docker", "kill", name], capture_output=True)
        return None, f"HUNG after {timeout:.0f}s (container killed)"
    out = r.stdout + r.stderr
    with open(f"{work}/out-{tag.split(':')[-1]}-{run}.log", "w") as f:
        f.write(out)   # which tests failed, after the fact
    verdict = next((l for l in out.splitlines() if l.startswith(prefix)
                    and any(v in l for v in verdicts)), "")
    device = next((l for l in out.splitlines() if "block device =" in l), "")
    # No verdict means the suite never ran (iso_guard refusing a stale
    # build, a VM that would not start): say why rather than print nothing.
    lines = [l.strip() for l in out.splitlines() if l.strip()]
    detail = verdict.strip() or (lines[-1] if lines else f"exit {r.returncode}, no output")
    if device:
        detail += f"  [{device.split('=')[-1].strip()}]"
    return (r.returncode == 0), detail


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--versions", nargs="*", default=list(IMAGES),
                    help=f"which QEMUs to test (default: all of {list(IMAGES)})")
    ap.add_argument("--suite", choices=list(SUITES), default="ktest",
                    help="ktest (the in-kernel suite, the default) or usertest "
                         "(the ring-3 /tests programs)")
    ap.add_argument("--runs", type=int, default=1, metavar="N",
                    help="run the suite N times per version and report a RATE "
                         "-- for an intermittent fault, one run is not a result")
    ap.add_argument("--virtio", action="store_true",
                    help="put the filesystem on virtio-blk as well as ATA (ktest only)")
    ap.add_argument("--timeout", type=float, default=300.0)
    args = ap.parse_args()

    if not os.path.exists("toy-os.iso"):
        print("qemu_matrix: toy-os.iso not found -- run `make iso` first")
        return 1
    if args.virtio and args.suite != "ktest":
        print("qemu_matrix: --virtio applies to the ktest suite only")
        return 1

    rows, failed = [], 0
    for v in args.versions:
        base = IMAGES.get(v)
        if not base:
            print(f"qemu_matrix: unknown version {v} (known: {list(IMAGES)})")
            return 1
        print(f"qemu_matrix: building QEMU {v} image ({base}) ...", flush=True)
        tag = build(v, base)
        if not tag:
            rows.append((v, "BUILD FAILED", ""))
            failed += 1
            continue
        passes = hangs = 0
        for run in range(1, args.runs + 1):
            print(f"qemu_matrix: running {args.suite} on QEMU {v}"
                  f" ({run}/{args.runs}) ...", flush=True)
            ok, detail = run_suite(tag, args.suite, args.virtio, args.timeout, run)
            status = "HUNG" if ok is None else "PASS" if ok else "FAIL"
            passes += ok is True
            hangs += ok is None
            if args.runs > 1:
                print(f"  {status:4}  {detail}", flush=True)
        if args.runs == 1:
            rows.append((v, status, detail))
        else:
            status = "PASS" if passes == args.runs else "FAIL"
            rows.append((v, status, f"{passes}/{args.runs} clean, {hangs} hung"))
        if status != "PASS":
            failed += 1

    print()
    for v, status, detail in rows:
        print(f"  {status:4}  QEMU {v:5} {args.suite:8} {detail}")
    print(f"\nqemu_matrix: {'FAIL' if failed else 'PASS'} -- "
          f"{len(rows) - failed}/{len(rows)} version(s) clean")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
