#!/usr/bin/env python3
"""init as pid 1: adoption, reaping, unkillability -- and SYS_SLEEP under it.

Stage 1 of docs/init-design.md, asserted end to end. The exit criterion
it encodes, in the plan's own words: pid 1 is init on every boot, a
spawned program nobody waits on is reaped rather than holding its slot
for the rest of the boot, and `kill 1` is refused.

WHY A SLOT COUNT IS THE ASSERTION. A zombie is only ever cleared by
somebody waiting for it, so before init a process whose parent died
first held its slot forever -- invisible until the table filled up, at
which point the desktop silently stopped launching anything. That
failure has already happened here once (MAX_PROCS was 4). Counting the
table before and after is the only check that sees it, because every
individual process behaves perfectly either way.

THE FIXTURE IS THE LOAD-BEARING PART. /tests/orphan_test spawns children
and exits WITHOUT waiting, so its children outlive it and must be
adopted. It has to be started with `spawn` -- not `run`, whose legacy
loader is not a scheduled process, so its children have ppid 0 already
and there is nothing to orphan. A version of this test written with
`run` would pass against a kernel with adoption removed entirely.

POSITIVE CONTROL, for whoever changes this. In scheduler.c's
reparent_children(), set `heir = 0` unconditionally (i.e. put back the
stage-0 behaviour) and rebuild: the "orphans are reaped" and "the table
returns to its baseline" checks must go red and everything else must
stay green. Verified: 2 of 11 red, which is the right two -- the init
identity, unkillability and SYS_SLEEP checks are not testing adoption
and must not move.

That control already earned its keep once: the reap check originally
asked for `>= 1` reap and stayed GREEN through it, because the shell's
own `spawn` hands the fixture ITSELF to init, so one reap happens even
with adoption removed. It counts against what the fixture says it
abandoned now.

Usage:
    python3 tools/init_test.py
    python3 tools/init_test.py --instance 2     # alongside another VM

Boots a COPY of disk.img. Exits 0 if every check passed, 1 otherwise,
2 if it could not run at all.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VM = os.path.join(REPO, "tools", "vm.py")

# One process row from `ps`: PID PPID STATE CPU MEM NAME.
PS_ROW = re.compile(r"^\s*(\d+)\s+(\d+)\s+(\w+)\s+\S+\s+\S+\s+(\S+)\s*$")

results = []


def check(what, ok, detail=""):
    results.append((what, bool(ok), detail))
    print(f"  {'ok  ' if ok else 'FAIL'}  {what}" + (f"   -- {detail}" if detail else ""))


class VMSession:
    def __init__(self, disk, instance):
        self.disk, self.instance = disk, instance

    def _cmd(self, *argv):
        cmd = [sys.executable, VM, "--disk", self.disk]
        if self.instance:
            cmd += ["--instance", str(self.instance)]
        return cmd + list(argv)

    def run(self, *argv, check_rc=True):
        r = subprocess.run(self._cmd(*argv), cwd=REPO, capture_output=True, text=True)
        if check_rc and r.returncode != 0:
            print(r.stdout + r.stderr, file=sys.stderr)
        return r.stdout + r.stderr

    def sh(self, cmd):
        return self.run("exec", cmd, check_rc=False)

    def ps(self):
        """The process table as {pid: (ppid, state, name)}.

        Parsed from `ps`, which is a real /bin binary -- so this also
        exercises SYS_PROC_INFO's ring-3 path rather than asking the
        kernel directly through a debug command.
        """
        out = self.sh("ps")
        table = {}
        for line in out.splitlines():
            m = PS_ROW.match(line)
            if not m:
                continue
            pid, ppid, state, name = m.groups()
            if name == "NAME":
                continue
            table[int(pid)] = (int(ppid), state, name)
        return table


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disk", default=None)
    ap.add_argument("--instance", type=int, default=0)
    args = ap.parse_args()

    tmp = None
    if not args.disk:
        src = os.path.join(REPO, "disk.img")
        if not os.path.exists(src):
            print("init_test: no disk.img -- run `make iso` first")
            return 2
        tmp = tempfile.NamedTemporaryFile(suffix=".img", delete=False)
        tmp.close()
        # --sparse=always: disk.img is a few MB of data in a 9 GB sparse
        # file, and a hole-filling copy costs the whole 9 GB.
        subprocess.run(["cp", "--reflink=auto", "--sparse=always", src, tmp.name],
                       check=True)
        args.disk = tmp.name

    vm = VMSession(args.disk, args.instance)
    try:
        if "ready" not in vm.run("start"):
            print("init_test: could not start the VM")
            return 2

        # --- init exists, and is what the kernel says it is -----------
        boot = vm.sh("dmesg")
        m = re.search(r"init started as pid (\d+)", boot)
        check("the kernel spawns init at boot", bool(m),
              (m.group(0) if m else "no 'init started as pid' line"))
        init_pid = int(m.group(1)) if m else 0

        check("init holds pid 1", init_pid == 1, f"pid {init_pid}")
        check("init announced itself from ring 3", "init: reaping orphans" in boot)

        table = vm.ps()
        row = table.get(1)
        check("ps sees init in the process table", bool(row), str(row))
        # ppid 0 -- the KERNEL spawned it, which is what makes it a root.
        check("init's parent is the kernel (ppid 0)",
              bool(row) and row[0] == 0 and row[2] == "init", str(row))
        # BLOCKED, not ready: an init that shows as runnable is one
        # spinning in a poll loop, which is the whole thing SYS_SLEEP
        # exists to avoid. This is the check that would catch a
        # regression to a yield-loop.
        check("an idle init is BLOCKED, not spinning",
              bool(row) and row[1] == "block", str(row))

        # --- init cannot be killed -----------------------------------
        out = vm.sh("kill 1")
        check("`kill 1` is refused", "unkillable" in out, out.strip()[:80])
        check("init survives being killed", 1 in vm.ps())

        # --- adoption and reaping ------------------------------------
        baseline = set(vm.ps())
        vm.sh("spawn /tests/orphan_test 4")

        # The children are short-lived and init's reap is driven by the
        # adoption wake, so this settles quickly -- but poll rather than
        # sleeping a fixed interval, and give up loudly.
        deadline = time.time() + 15
        table = {}
        while time.time() < deadline:
            table = vm.ps()
            if set(table) == baseline:
                break
            time.sleep(0.5)

        logs = vm.sh("dmesg")
        reaped = len(re.findall(r"init: reaped orphan pid", logs))
        # Against the number the fixture says it abandoned, not against
        # 1: the shell's own `spawn` hands orphan_test ITSELF to init, so
        # a kernel with adoption removed entirely still produces exactly
        # one reap. `>= 1` here stayed green through the positive
        # control and was measuring the wrong process.
        m = re.search(r"orphan: abandoning (\d+) children", logs)
        want = int(m.group(1)) if m else 0
        check("init reaped every abandoned child", want > 0 and reaped > want,
              f"{reaped} reaped, {want} abandoned")
        # THE SLOT CHECK. Anything left behind is a zombie nothing can
        # ever clear, so name what survived rather than just failing.
        leftover = {p: v for p, v in table.items() if p not in baseline}
        check("the process table returns to its baseline", not leftover,
              str(leftover) if leftover else f"{len(baseline)} slot(s), unchanged")

        # --- SYS_SLEEP, which is how init idles ----------------------
        vm.sh("spawn /tests/sleep_test")
        deadline = time.time() + 20
        out = ""
        while time.time() < deadline:
            out = vm.sh("dmesg")
            if "sleep: all checks passed" in out or "sleep: FAILED" in out:
                break
            time.sleep(0.5)
        bad = [l for l in out.splitlines() if "sleep: FAIL" in l]
        check("SYS_SLEEP passes its own checks",
              "sleep: all checks passed" in out and not bad,
              bad[0].strip()[:90] if bad else "")
    finally:
        vm.run("stop", check_rc=False)
        if tmp:
            os.unlink(tmp.name)

    failed = [w for w, ok, _ in results if not ok]
    print(f"\ninit_test: {len(results) - len(failed)}/{len(results)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
