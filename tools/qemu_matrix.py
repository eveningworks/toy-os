#!/usr/bin/env python3
"""Run the kernel test suite against SEVERAL QEMU versions, in Docker.

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
"""
import argparse
import os
import subprocess
import sys

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


def run_suite(tag, virtio, timeout):
    """ktest_run.py inside the container, against the host-built ISO."""
    # The disk is COPIED, never mounted read-write: the suite writes to
    # it, and the host's disk.img is re-seeded by `make iso` and may be
    # open in the user's own QEMU.
    work = ".qemu_matrix"
    os.makedirs(work, exist_ok=True)
    # Sparse, always: disk.img is ~4 MB of data in a 9 GB sparse file, and
    # a hole-filling copy costs the 9 GB (CLAUDE.md).
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", "disk.img", f"{work}/disk.img"], check=True)
    args = ["python3", "tools/ktest_run.py", "-v",
            "--disk", f"{work}/disk.img",
            "--qemu-log", f"{work}/qemu.log"]
    if virtio:
        subprocess.run(["cp", "--reflink=auto", "--sparse=always", "disk.img", f"{work}/virtio.img"], check=True)
        args += ["--virtio-disk", f"{work}/virtio.img"]
    cmd = ["docker", "run", "--rm", "-v", f"{os.getcwd()}:/toyos", tag] + args
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, "timed out"
    out = r.stdout + r.stderr
    verdict = next((l for l in out.splitlines() if l.startswith("ktest: ")
                    and ("PASSED" in l or "FAILED" in l)), "")
    device = next((l for l in out.splitlines() if "block device =" in l), "")
    return (r.returncode == 0), f"{verdict.strip()}  [{device.split('=')[-1].strip()}]"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--versions", nargs="*", default=list(IMAGES),
                    help=f"which QEMUs to test (default: all of {list(IMAGES)})")
    ap.add_argument("--virtio", action="store_true",
                    help="put the filesystem on virtio-blk as well as ATA")
    ap.add_argument("--timeout", type=float, default=300.0)
    args = ap.parse_args()

    if not os.path.exists("toy-os.iso"):
        print("qemu_matrix: toy-os.iso not found -- run `make iso` first")
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
        print(f"qemu_matrix: running the suite on QEMU {v} ...", flush=True)
        ok, detail = run_suite(tag, args.virtio, args.timeout)
        rows.append((v, "PASS" if ok else "FAIL", detail))
        if not ok:
            failed += 1

    print()
    for v, status, detail in rows:
        print(f"  {status:4}  QEMU {v:5} {detail}")
    print(f"\nqemu_matrix: {'FAIL' if failed else 'PASS'} -- "
          f"{len(rows) - failed}/{len(rows)} version(s) clean")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
