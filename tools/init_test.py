#!/usr/bin/env python3
"""init as pid 1: the target, service supervision, adoption and reaping.

Stages 1 and 2 of docs/init-design.md, asserted end to end. The exit
criteria they encode, in the plan's own words: pid 1 is init on every
boot, a spawned program nobody waits on is reaped rather than holding
its slot for the rest of the boot, `kill 1` is refused, the target
setting decides what boots, and the desktop comes back without the shell
being involved.

THE CRASH-LOOP FIXTURE IS A PAIR, and neither half means much alone. A
second service descriptor naming a binary that does not exist is written
onto the disk copy before boot, so this boot has one service that works
(the desktop) and one that cannot possibly start. What is asserted is
BOTH that the broken one is eventually given up on -- an unbounded
`Restart=always` is a machine that spins forever starting a binary that
faults, with no console left to fix it from -- and that the desktop is
untouched by its neighbour failing. A give-up that took the desktop with
it would pass the first check on its own.

WHAT THIS DOES NOT COVER, stated rather than implied: the `target=text`
kernel command-line override, which needs its own ISO. Settle it by hand
with

    make iso KCMDLINE="target=text" && python3 tools/vm.py run "ps"

-- init must report `target text` and the table must hold init alone.
Rebuild the plain ISO afterwards; `make iso` bakes KCMDLINE in.

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

The stage-2 half has its own control: in init.c's service_failed(), drop
the `fast_failures >= SVC_MAX_FAST` give-up. The "given up on" check must
go red while "the desktop survives its neighbour" stays green.

THE ORDERING CHECKS ARE WRITTEN AGAINST THE ORDER THEY CREATE THE FILES
IN, which is what makes them a test rather than a coincidence. Both
groups of three demand the REVERSE of the order their descriptors were
written, and they express it from opposite ends -- one group with
`After=`, one with `Before=` -- so an init that ignored both keys would
have to be handed a perfectly reversed directory listing, twice, to pass.
Verified by making start_due() walk g_svc[] instead of g_order[]:
exactly the two ordering checks go red and nothing else moves.

That control already earned its keep once: the reap check originally
asked for `>= 1` reap and stayed GREEN through it, because the shell's
own `spawn` hands the fixture ITSELF to init, so one reap happens even
with adoption removed. It counts against what the fixture says it
abandoned now.

READINESS IS ASSERTED ON TIMESTAMPS, NOT ON LOG ORDER. `Ready=notify`
means `After=` waits for a service to be USABLE rather than merely
spawned, and the difference is ~300 ms on the desktop -- so the check
compares the [seconds] dmesg stamped on "toywm is ready" against the one
on "started rdyafter". Reading the transcript cannot tell a barrier that
worked from a log that happens to be in that order. The other half is
the timeout: /tests/notready stays alive and announces nothing, so the
barrier can be watched expiring and its dependent starting anyway --
which is the property that stops a hung service leaving the machine with
nothing started.

KNOWN FAILING, AND PRE-EXISTING: twelve of these checks fail, and have
since before the readiness work -- measured by stashing it, rebuilding
HEAD and re-running (15 of 27 there, the same twelve). Partway through
the run init stops reaping and stops starting services. See
`docs/bugs.md` and its repro in `docs/roadmap-details.md`; 19/31 is the
known-good state today, not a regression. Do not read a failure here as
proof that a change broke something without checking that list first.

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
sys.path.insert(0, os.path.join(REPO, "tools"))
import vm as vm_mod                   # noqa: E402 -- started_ok(), see its comment
VM = os.path.join(REPO, "tools", "vm.py")

# One process row from `ps`: PID PPID PGID STATE CPU(s) MEM(K) NAME.
#
# **PGID IS IN IT, AND LEAVING IT OUT MATCHED NOTHING AT ALL.** `ps`
# gained that column and this pattern did not, so every row failed to
# parse and six checks reported a perfectly healthy machine as one where
# init does not appear in the process table. A regex that matches no
# line looks exactly like a system that produced no output -- which is
# why the failure detail prints the row count now.
#
# STATE is \S+ rather than \w+ because it can be `block(child)`, which
# is what an idle init is in.
PS_ROW = re.compile(
    r"^\s*(\d+)\s+(\d+)\s+\d+\s+(\S+)\s+\S+\s+\S+\s+(\S+)\s*$")

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

        # The crash-loop fixture -- see the module docstring. Written
        # from the HOST so the machine boots with it already in place:
        # the give-up happens in the first few seconds and there is
        # nothing to observe if the file arrives afterwards.
        broken = tempfile.NamedTemporaryFile("w", suffix=".svc", delete=False)
        broken.write("Name=broken\nExec=/bin/does-not-exist\n"
                     "Target=graphical\nRestart=always\n")
        broken.close()
        r = subprocess.run([sys.executable, os.path.join(REPO, "tools", "tfs3_writer.py"),
                            "write", args.disk, broken.name, "/etc/services.d/broken"],
                           cwd=REPO, capture_output=True, text=True)
        os.unlink(broken.name)
        if r.returncode != 0:
            print("init_test: could not stage the crash-loop fixture\n"
                  + r.stdout + r.stderr, file=sys.stderr)
            return 2

        # THE READINESS FIXTURES, and they have to be here rather than
        # written at the shell for the same reason the crash-loop one
        # does: both barriers resolve within the first few seconds, and
        # a descriptor that arrives after the boot has nothing left to
        # observe.
        #
        # Two pairs, covering the two ways a barrier can end:
        #
        #   rdyafter  ordered After= the DESKTOP, which announces itself
        #             for real ~300 ms after it is spawned. This is the
        #             one that proves the barrier waits for READINESS
        #             rather than for the spawn -- without it, `rdyafter`
        #             starts in the same pass as toywm.
        #   nrdep     ordered After= a service that never announces
        #             anything (/tests/notready). It must start anyway,
        #             once ReadyTimeout= has run out and not before.
        #
        # Both dependents are /bin/hello with Restart=no, so they print
        # a start line, exit at once and leave nothing behind.
        for name, body in (
            ("rdyafter", "Name=rdyafter\nExec=/bin/hello\nTarget=graphical\n"
                         "Restart=no\nAfter=toywm\n"),
            ("notready", "Name=notready\nExec=/tests/notready\nTarget=graphical\n"
                         "Restart=no\nReady=notify\nReadyTimeout=1500\n"),
            ("nrdep",    "Name=nrdep\nExec=/bin/hello\nTarget=graphical\n"
                         "Restart=no\nAfter=notready\n"),
        ):
            f = tempfile.NamedTemporaryFile("w", suffix=".svc", delete=False)
            f.write(body)
            f.close()
            r = subprocess.run(
                [sys.executable, os.path.join(REPO, "tools", "tfs3_writer.py"),
                 "write", args.disk, f.name, f"/etc/services.d/{name}"],
                cwd=REPO, capture_output=True, text=True)
            os.unlink(f.name)
            if r.returncode != 0:
                print(f"init_test: could not stage the {name} fixture\n"
                      + r.stdout + r.stderr, file=sys.stderr)
                return 2

    vm = VMSession(args.disk, args.instance)
    try:
        if not vm_mod.started_ok(vm.run("start")):
            print("init_test: could not start the VM")
            return 2

        # --- init exists, and is what the kernel says it is -----------
        #
        # POLLED. `vm.py start` returns when the shell prompt appears,
        # and the desktop is coming up right then -- a `dmesg` reply read
        # in that window has come back INCOMPLETE, which showed up as
        # this tool reporting that the kernel never spawned init on
        # roughly one run in five. The line is printed before the prompt
        # exists, so its absence is always the read, never the kernel.
        deadline = time.time() + 20
        boot = ""
        while time.time() < deadline:
            boot = vm.sh("dmesg")
            if "init started as pid" in boot:
                break
            time.sleep(0.5)

        m = re.search(r"init started as pid (\d+)", boot)
        check("the kernel spawns init at boot", bool(m),
              (m.group(0) if m else "no 'init started as pid' line"))
        init_pid = int(m.group(1)) if m else 0

        check("init holds pid 1", init_pid == 1, f"pid {init_pid}")
        check("init announced itself from ring 3", "init: starting" in boot)

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

        # --- the target, and the services it selects ------------------
        m = re.search(r"init: target (\w+)", boot)
        check("init reports the target it read", bool(m),
              (m.group(0) if m else "no 'init: target' line"))
        target = m.group(1) if m else ""

        # Against the REGISTRY rather than against a literal: the whole
        # point of `system.default_target` being a registered setting is
        # that one place answers what it is, and a tool asserting its own
        # copy of the expected value would not notice the two diverging.
        want = vm.sh("config get system.default_target")
        check("init's target is what the settings registry says",
              bool(target) and target in want, f"init said {target!r}")

        m = re.search(r"init: started toywm as pid (\d+)", boot)
        check("init starts the desktop from its service file", bool(m),
              (m.group(0) if m else "no 'started toywm' line"))
        wm_pid = int(m.group(1)) if m else 0

        # The desktop being init's CHILD is the milestone, not merely the
        # desktop running: it is what makes a client window init's
        # grandchild, so killing the desktop reparents its clients here
        # instead of stranding them.
        row = table.get(wm_pid)
        check("the desktop is init's child", bool(row) and row[0] == 1
              and row[2] == "toywm", str(row))

        # --- a service that cannot start is given up on ---------------
        #
        # POLLED, not read from the dmesg captured above: the backoff
        # ladder takes ~4 s to run out and that snapshot is taken within
        # half a second of the prompt appearing. Read once, this check
        # asserted that a service had failed exactly once so far, which
        # is true of a healthy retry and of a broken one.
        deadline = time.time() + 20
        svc = boot
        while time.time() < deadline:
            svc = vm.sh("dmesg")
            if "broken is crash-looping" in svc:
                break
            time.sleep(0.5)

        attempts = len(re.findall(r"init: broken failed to start", svc))
        check("a broken service is retried, not abandoned at once",
              attempts >= 2, f"{attempts} attempt(s)")
        check("a crash-looping service is given up on",
              "broken is crash-looping, giving up" in svc,
              f"after {attempts} attempts")
        # The pair: a neighbour failing must not cost the desktop.
        check("the desktop survives its neighbour crash-looping",
              wm_pid in table and table[wm_pid][2] == "toywm", str(table.get(wm_pid)))

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

        # --- the desktop comes back without the shell -----------------
        #
        # LAST, because it perturbs the table every check above reads.
        # This is stage 2's exit criterion: before it, killing the
        # desktop left the machine at a text console until somebody
        # typed `spawn`.
        if wm_pid:
            # How many desktop exits had ALREADY been logged. Matching
            # "exited with code" anywhere in the log makes this check
            # pass on an unrelated earlier death -- which is not
            # hypothetical: an intermittent one was observed while
            # writing this (see docs/roadmap.md), and the first version
            # of the check reported that exit as proof the kill worked.
            before = len(re.findall(r"init: toywm \(pid \d+\) exited", vm.sh("dmesg")))
            vm.sh(f"kill {wm_pid}")
            deadline = time.time() + 20
            back = None
            while time.time() < deadline:
                for pid, (ppid, _state, name) in vm.ps().items():
                    if name == "toywm" and ppid == 1:
                        back = pid
                        break
                if back:
                    break
                time.sleep(0.5)
            logs = vm.sh("dmesg")
            exits = re.findall(r"init: toywm \(pid \d+\) exited with code [-\d]+ after \d+ ms",
                              logs)
            check("killing the desktop is noticed", len(exits) > before,
                  exits[-1][:90] if exits else "no exit logged at all")
            check("init restarts the desktop with no shell involved",
                  bool(back), f"back as pid {back}" if back else "never came back")
            # A restart after a long run must NOT count toward the crash
            # loop -- otherwise killing the desktop five times gives up
            # on it, which is the opposite of what supervision is for.
            check("a killed-after-running desktop is not treated as a crash loop",
                  "toywm is crash-looping" not in logs)

            # THE OTHER HALF OF THE POLICY, and the one that regressed: a
            # CLEAN exit is a request to stop. The Start menu's "Exit to
            # shell" makes the desktop return 0, and under an
            # unconditional restart init put it straight back, so the menu
            # item silently did nothing. `gui close`-style paths cannot be
            # driven from here, so this asserts it the way the code
            # decides it -- by exit CODE -- using a service of its own.
            #
            # /bin/hello exits 0 immediately, which is exactly the shape
            # under test. It is written as a service with the default
            # policy, so what is checked is the DEFAULT rather than a
            # value this test chose.
            vm.sh("write /etc/services.d/oneshot Name=oneshot")
            vm.sh("append /etc/services.d/oneshot Exec=/bin/hello")

            # AND THEN WAKE INIT. It rescans /etc/services.d on every
            # pass of its loop, but with a child running it BLOCKS in
            # waitpid -- so a descriptor added while the desktop is up is
            # not noticed until something makes init loop. That is by
            # design (a periodic rescan would mean init polling, which is
            # the whole thing SYS_SLEEP exists to avoid) and it is why
            # this needs a nudge: `spawn` reparents to init, so a
            # short-lived program exiting is a wake-up.
            vm.sh("spawn /bin/hello")

            deadline = time.time() + 20
            logs = ""
            while time.time() < deadline:
                logs = vm.sh("dmesg")
                if "oneshot exited cleanly" in logs:
                    break
                time.sleep(0.5)
            starts = len(re.findall(r"init: started oneshot as pid", logs))
            check("a service that exits cleanly is NOT restarted",
                  "oneshot exited cleanly -- not restarting it" in logs
                  and starts == 1,
                  f"{starts} start(s)")

            # --- ORDERING: After=/Before= decide the spawn order -------
            #
            # THE CONTROL IS BUILT IN: both groups demand the REVERSE of
            # the order their files are created in, expressed from
            # opposite ends of the relation. An init that ignored the
            # keys entirely starts them in whatever order listdir hands
            # back, which would have to be perfectly reversed -- twice --
            # to pass. Asserting an order that happens to match creation
            # order would pass against a version that does nothing.
            #
            # /bin/hello exits 0 at once and Restart=no keeps it down, so
            # these six leave nothing running behind them.
            groups = [
                # Written x, y, z; constrained to start z, y, x.
                [("ordx", "After=ordy"),
                 ("ordy", "After=ordz"),
                 ("ordz", "")],
                # Written p, q, r; constrained to start r, q, p --
                # again the reverse of write order, so listdir order
                # cannot satisfy it either. The key is carried by the
                # LATER file each time, which is what expresses the same
                # relation from its other end.
                [("ordp", ""),
                 ("ordq", "Before=ordp"),
                 ("ordr", "Before=ordq")],
            ]
            for grp in groups:
                for name, order_key in grp:
                    vm.sh(f"write /etc/services.d/{name} Name={name}")
                    vm.sh(f"append /etc/services.d/{name} Exec=/bin/hello")
                    vm.sh(f"append /etc/services.d/{name} Restart=no")
                    if order_key:
                        vm.sh(f"append /etc/services.d/{name} {order_key}")

            vm.sh("spawn /bin/hello")   # wake init, as above

            wanted = ["ordz", "ordy", "ordx", "ordr", "ordq", "ordp"]
            deadline = time.time() + 25
            logs = ""
            while time.time() < deadline:
                logs = vm.sh("dmesg")
                if all(f"init: started {n} as pid" in logs for n in wanted):
                    break
                time.sleep(0.5)

            started = re.findall(r"init: started (ord\w+) as pid", logs)
            def seq(names):
                return [n for n in started if n in names]

            g1 = seq({"ordx", "ordy", "ordz"})
            g2 = seq({"ordp", "ordq", "ordr"})
            check("After= starts the named service first",
                  g1 == ["ordz", "ordy", "ordx"], " ".join(g1) or "none started")
            check("Before= orders from the other end of the same relation",
                  g2 == ["ordr", "ordq", "ordp"], " ".join(g2) or "none started")

            # A cycle must cost one edge and nothing else. Both halves
            # matter: an init that refused the cycle by refusing to start
            # anything would satisfy "it said cycle" on its own, and a
            # machine that starts nothing has no console left to fix
            # itself from.
            for a, b in (("ordm", "ordn"), ("ordn", "ordm")):
                vm.sh(f"write /etc/services.d/{a} Name={a}")
                vm.sh(f"append /etc/services.d/{a} Exec=/bin/hello")
                vm.sh(f"append /etc/services.d/{a} Restart=no")
                vm.sh(f"append /etc/services.d/{a} After={b}")
            vm.sh("spawn /bin/hello")

            deadline = time.time() + 25
            while time.time() < deadline:
                logs = vm.sh("dmesg")
                if "ordering cycle" in logs:
                    break
                time.sleep(0.5)
            cyc = re.findall(r"init: started (ordm|ordn) as pid", logs)
            check("a cycle is reported rather than hung on",
                  "init: ordering cycle" in logs)
            check("...and both services in it still start",
                  set(cyc) == {"ordm", "ordn"}, " ".join(cyc) or "none started")

            # An unresolvable name is normal, not fatal: naming a service
            # on the other boot target reaches here identically.
            vm.sh("write /etc/services.d/ordu Name=ordu")
            vm.sh("append /etc/services.d/ordu Exec=/bin/hello")
            vm.sh("append /etc/services.d/ordu Restart=no")
            vm.sh("append /etc/services.d/ordu After=nosuchservice")
            vm.sh("spawn /bin/hello")

            deadline = time.time() + 25
            while time.time() < deadline:
                logs = vm.sh("dmesg")
                if "init: started ordu as pid" in logs:
                    break
                time.sleep(0.5)
            check("an After= naming nothing loaded is reported and ignored",
                  "names no loaded service" in logs
                  and "init: started ordu as pid" in logs)

            # --- READINESS: After= means "usable", not "spawned" -------
            #
            # THE ASSERTION IS ON TIMESTAMPS, not on log order, and that
            # is what makes it a test. dmesg stamps every line, so the
            # question "did the dependent start before or after the
            # thing it is ordered after became ready" has a number
            # behind it rather than a reading of the transcript.
            #
            # POSITIVE CONTROL for whoever changes this: in init.c's
            # start_due(), drop the svc_waiting_on_deps() line. The two
            # "waits for" checks must go red and the two "is ready" /
            # "timeout is reported" checks must stay green -- they are
            # about the announcement reaching init, not about the
            # barrier. Verified.
            deadline = time.time() + 30
            logs = ""
            while time.time() < deadline:
                logs = vm.sh("dmesg")
                if ("init: started rdyafter as pid" in logs
                        and "init: started nrdep as pid" in logs):
                    break
                time.sleep(0.5)

            def stamp(pattern):
                """The [seconds] dmesg stamped on the first matching line."""
                m = re.search(r"\[\s*(\d+\.\d+)\]\s*" + pattern, logs)
                return float(m.group(1)) if m else None

            wm_started = stamp(r"init: started toywm as pid")
            wm_ready = stamp(r"init: toywm is ready after (\d+) ms")
            dep_started = stamp(r"init: started rdyafter as pid")

            # The desktop's own announcement, on an ordinary boot. It is
            # the first real caller, and the gap it reports is the whole
            # feature: everything between claiming the compositor role
            # and compositing a frame used to be invisible.
            check("the desktop announces itself, later than it was spawned",
                  wm_ready is not None and wm_started is not None
                  and wm_ready > wm_started,
                  f"spawned {wm_started}, ready {wm_ready}")

            # A dependent must not start in the same pass as the service
            # it is ordered after. Without the barrier these two stamps
            # are equal to the millisecond.
            check("After= a notify service waits for the ANNOUNCEMENT",
                  dep_started is not None and wm_ready is not None
                  and dep_started >= wm_ready,
                  f"toywm ready {wm_ready}, rdyafter started {dep_started}")

            nr_started = stamp(r"init: started notready as pid")
            nr_timeout = stamp(r"init: notready did not report ready")
            nrdep_started = stamp(r"init: started nrdep as pid")

            # ...and the barrier ALWAYS expires. A machine that starts
            # nothing has no console left to fix itself from, so this is
            # the check that says a hung service degrades the boot
            # instead of ending it.
            check("a service that never announces is reported, not waited on forever",
                  nr_timeout is not None and nr_started is not None
                  and nr_timeout >= nr_started + 1.0,
                  f"started {nr_started}, timed out {nr_timeout}")
            check("...and its dependent then starts anyway",
                  nrdep_started is not None and nr_timeout is not None
                  and nrdep_started >= nr_timeout,
                  f"timeout {nr_timeout}, nrdep started {nrdep_started}")

    finally:
        vm.run("stop", check_rc=False)
        if tmp:
            os.unlink(tmp.name)

    failed = [w for w, ok, _ in results if not ok]
    print(f"\ninit_test: {len(results) - len(failed)}/{len(results)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
