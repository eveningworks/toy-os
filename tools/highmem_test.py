#!/usr/bin/env python3
"""The frame allocator on a guest with more than 4 GiB: managed, idle, zoned.

Runs the `mm` KTEST suite on an 8 GiB guest and requires that its
above-4-GiB check RAN. On the ordinary 256 MiB boot that check skips,
and `ktest_run.py` reports a skip as a pass of everything else -- so
`make test` alone can never say whether frames above 4 GiB are
accounted for. This tool exists so that question has a runner.

What a PASS says: the firmware map's memory above 4 GiB is managed by
pmm and zoned, the identity map reaches it (a pattern written through
it reads back), the kernel heap grows into it, user pages and page
tables and window buffers come from it, and virtio's BAR -- which
SeaBIOS moves to 768 GiB on a machine this size -- is reachable through
paging_map_device(). It runs the WHOLE suite rather than `mm`, because
the BAR move is what an 8 GiB machine changes for everything else.

Then it asks the same question from OUTSIDE the kernel: a real ring-3
process is spawned and the high zone's free count must actually drop.
The KTESTs name the frame behind one page they mapped themselves; this
is the end-to-end version, and it is the half that would survive a
KTEST fixture drifting away from the path a process really takes.

What it does NOT say: that memory past 4 GiB can be CONSUMED at scale.
That is tools/highmem_consume.py, which is heavier and on demand.

    python3 tools/highmem_test.py
    python3 tools/highmem_test.py --mem 6144    # any size past 4 GiB
"""
import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--mem", type=int, default=8192, metavar="MIB",
                    help="guest RAM in MiB (default 8192; must exceed 4096)")
    ap.add_argument("--port", type=int, default=None, help="serial port for ktest_run.py")
    args = ap.parse_args()
    if args.mem <= 4096:
        print("highmem_test: FAIL -- --mem must exceed 4096 or the check under test skips")
        return 1

    cmd = [sys.executable, os.path.join(HERE, "ktest_run.py"), "--mem", str(args.mem)]
    if args.port:
        cmd += ["--port", str(args.port)]
    r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    out = r.stdout + r.stderr
    tail = [ln for ln in out.splitlines() if ln.strip()][-8:]
    if r.returncode != 0:
        print(f"highmem_test: FAIL -- the mm suite failed on a {args.mem} MiB guest")
        for ln in tail:
            print(f"    {ln}")
        return 1
    # The suite passing is not enough: the above-4-GiB checks must have
    # RUN. A guest that somehow came up with less memory than asked
    # would skip them, and a skip here is the whole hazard this tool is
    # for. Other suites skip for their own reasons (no AHCI, no
    # virtio-gpu), so the test is that none of OUR skip lines appear.
    if "guest has no memory above 4 GiB" in out:
        print("highmem_test: FAIL -- an above-4-GiB check skipped instead of running")
        for ln in tail:
            print(f"    {ln}")
        return 1
    m = re.search(r"(\d+) passed, (\d+) failed, (\d+) skipped", out)
    print(f"highmem_test: {m.group(1) if m else '?'} checks ran on a {args.mem} MiB "
          f"guest, the above-4-GiB ones among them")

    rc = ring3_phase(args.mem)
    if rc:
        return rc
    print("highmem_test: PASS")
    return 0


def ring3_phase(mem):
    """A real process must move the high zone's free count.

    Deliberately its own guest rather than a reuse of the suite's: the
    KTEST boot ends with the machine shut down, and starting a fresh one
    is a few seconds against a check nothing else makes.
    """
    subprocess.run([sys.executable, "tools/vm.py", "stop"], capture_output=True)
    cmd = [sys.executable, "tools/vm.py", "--mem", str(mem), "start"]
    if subprocess.run(cmd, cwd=REPO, capture_output=True).returncode != 0:
        print("highmem_test: FAIL -- could not start a guest for the ring-3 check")
        return 1
    sys.path.insert(0, HERE)
    from gui_debug import DebugConsole
    try:
        con = DebugConsole(".vm.serial", timeout=60.0)

        def free_high():
            out = con.send("sh config get mem.frame_free_high")
            for ln in out.splitlines():
                if re.fullmatch(r"\d+", ln.strip()):
                    return int(ln.strip())
            return None

        before = free_high()
        if not before:
            print("highmem_test: FAIL -- the guest reports no free frames above 4 GiB")
            return 1
        # 256 MiB takes a couple of seconds to fault in and write, so a
        # poll twice a second cannot miss it. The threshold is half of
        # that: the process also frees as it exits, and the point is
        # that the drop is UNMISTAKABLE, not that it is exact.
        con.send("sh spawn /tests/memtest 256")
        low = before
        deadline = time.time() + 60
        while time.time() < deadline:
            now = free_high()
            if now is not None and now < low:
                low = now
            if (before - low) * 4096 >= 128 * 1024 * 1024:
                break
            time.sleep(0.5)
        dropped = (before - low) * 4096 / (1024 * 1024)
        if dropped < 128:
            print(f"highmem_test: FAIL -- a ring-3 process moved the high zone by "
                  f"only {dropped:.0f} MiB; user pages are not coming from it")
            return 1
        print(f"highmem_test: a ring-3 process took {dropped:.0f} MiB from above 4 GiB")
        return 0
    finally:
        subprocess.run([sys.executable, "tools/vm.py", "stop"], capture_output=True)


if __name__ == "__main__":
    sys.exit(main())
