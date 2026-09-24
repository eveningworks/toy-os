#!/usr/bin/env python3
"""Measure what a timer configuration costs and buys, on one booted build.

    python3 tools/timer_bench.py                        # this checkout's toy-os.iso/disk.img
    python3 tools/timer_bench.py --disk D --iso I --label hz250
    python3 tools/timer_bench.py --kvm --label hz1000-kvm
    python3 tools/timer_bench.py --json out.json        # one result, for comparing builds

Boots the build on a COPY of its disk, lets the desktop settle, and reports
three things -- the numbers the `option hz` / `option tick` / `option
highres` choice (drivers.conf) was made on:

  IDLE     timer interrupts and periodic ticks per second across a quiet
           desktop, the share of time the tick was stopped, and the QEMU
           process's host CPU. The first three read QUERY_CLOCK through
           `config get clock.*`; a build without those fields (anything
           before the tickless work) reports host CPU only.
  LATENCY  /tests/timer_bench's sleep overshoot at 1/3/7/16 ms, idle and
           with two busy processes contending.
  WORK     how many work units one busy process completes in 2 s, and two.

WHAT A NUMBER HERE IS NOT: a KVM number when run under TCG, or the other
way round -- say which (`--kvm`). Two runs of ONE build differ by a few
percent in WORK; compare differences larger than that, and run each build
more than once before believing a small one (`--runs`).
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIELDS = ("tick_hz", "tick_mode", "tick_events", "tick_ticks",
          "tick_idle_stops", "tick_stopped_ns", "monotonic_ns")


def vm(args, *rest, timeout=180):
    cmd = [sys.executable, os.path.join(REPO, "tools/vm.py"), "--instance", str(args.instance)]
    if args.kvm:
        cmd.append("--kvm")
    cmd += list(rest)
    r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout + r.stderr


def qemu_cpu_s(args):
    """User+system CPU seconds the QEMU process has used, or None."""
    try:
        pid = int(open(os.path.join(REPO, f".vm.{args.instance}.pid")).read().strip())
        f = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
        return (int(f[11]) + int(f[12])) / os.sysconf("SC_CLK_TCK")
    except (OSError, ValueError, IndexError):
        return None


def clock_facts(args):
    """{field: int} from `config get clock.<field>`; absent fields omitted."""
    _, out = vm(args, "exec", *[f"config get clock.{f}" for f in FIELDS])
    facts, cur = {}, None
    for line in out.splitlines():
        m = re.match(r"--- config get clock\.(\w+) ---", line)
        if m:
            cur = m.group(1)
            continue
        if cur and re.fullmatch(r"\d+", line.strip()):
            facts[cur] = int(line.strip())
            cur = None
    return facts


def one_run(args, disk, iso):
    rc, out = vm(args, "--disk", disk, *(["--iso", iso] if iso else []), "start", timeout=300)
    if rc != 0:
        sys.exit(f"timer_bench: the VM did not start:\n{out[-2000:]}")
    try:
        time.sleep(args.settle)
        res = {}

        a, c0 = clock_facts(args), qemu_cpu_s(args)
        t0 = time.monotonic()
        time.sleep(args.idle)
        b, c1 = clock_facts(args), qemu_cpu_s(args)
        wall = time.monotonic() - t0
        if c0 is not None and c1 is not None:
            res["idle_host_cpu_pct"] = round((c1 - c0) / wall * 100, 1)
        if "monotonic_ns" in a and "tick_events" in a:
            dt = (b["monotonic_ns"] - a["monotonic_ns"]) / 1e9
            res["tick_hz"] = b.get("tick_hz")
            res["tick_mode"] = b.get("tick_mode")
            res["idle_irq_per_s"] = round((b["tick_events"] - a["tick_events"]) / dt)
            res["idle_ticks_per_s"] = round((b["tick_ticks"] - a["tick_ticks"]) / dt)
            res["idle_stopped_pct"] = round(
                (b["tick_stopped_ns"] - a["tick_stopped_ns"]) / 1e9 / dt * 100, 1)

        c0 = qemu_cpu_s(args)
        t0 = time.monotonic()
        vm(args, "exec", "spawn /tests/timer_bench")
        log = ""
        deadline = time.monotonic() + 240
        while time.monotonic() < deadline:
            time.sleep(2)
            _, log = vm(args, "exec", "dmesg -n 120")
            if "timer_bench: done" in log:
                break
        else:
            sys.exit("timer_bench: /tests/timer_bench never finished -- see `dmesg`")
        c1 = qemu_cpu_s(args)
        if c0 is not None and c1 is not None:
            res["bench_host_cpu_pct"] = round((c1 - c0) / (time.monotonic() - t0) * 100, 1)

        hogs = []
        for line in log.splitlines():
            m = re.search(r"timer_bench: (idle|load) (\d+)ms median_us (\d+) p90_us (\d+) max_us (\d+)", line)
            if m:
                k = f"{m.group(1)}_{m.group(2)}ms"
                res[k] = {"median_us": int(m.group(3)), "p90_us": int(m.group(4)),
                          "max_us": int(m.group(5))}
            m = re.search(r"timer_bench: alone units (\d+)", line)
            if m:
                res["alone_units"] = int(m.group(1))
            m = re.search(r"timer_bench: hog units (\d+)", line)
            if m:
                hogs.append(int(m.group(1)))
        if hogs:
            res["load_units"] = sum(hogs)
        return res
    finally:
        vm(args, "stop")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--disk", default=os.path.join(REPO, "disk.img"))
    ap.add_argument("--iso", default=None)
    ap.add_argument("--instance", type=int, default=6)
    ap.add_argument("--kvm", action="store_true")
    ap.add_argument("--label", default="build")
    ap.add_argument("--settle", type=float, default=8.0, help="seconds after boot before measuring")
    ap.add_argument("--idle", type=float, default=10.0, help="idle window, seconds")
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--json", default=None, help="append the result to this JSON-lines file")
    args = ap.parse_args()

    runs = []
    with tempfile.TemporaryDirectory(prefix="timer_bench.") as tmp:
        for i in range(args.runs):
            disk = os.path.join(tmp, "disk.img")
            subprocess.run(["cp", "--reflink=auto", "--sparse=always", args.disk, disk], check=True)
            runs.append(one_run(args, disk, args.iso))
            os.remove(disk)

    result = {"label": args.label, "accel": "kvm" if args.kvm else "tcg", "runs": runs}
    print(json.dumps(result, indent=1))
    if args.json:
        with open(args.json, "a") as f:
            f.write(json.dumps(result) + "\n")


if __name__ == "__main__":
    main()
