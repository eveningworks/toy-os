#!/usr/bin/env python3
"""tools/shutdown_test.py -- init stops the services in REVERSE order on reboot.

`reboot` asks init (lib/uinitctl.h's INITCTL_REBOOT) instead of calling
SYS_POWEROFF itself, and init then: stops every running service in the
reverse of its start order -- SIGTERM, up to the service's StopTimeout=,
then SIGKILL -- sends SIGTERM to every other process, waits up to 2 s,
SIGKILLs what is left, and only then reboots. Judged from init's own
lines in the kernel log, captured to a file (`vm.py --serial-log`)
because the machine they describe is gone by the time anyone reads them:

  1. init says it is shutting down for a reboot, and the machine goes:
     QEMU exits (the guest runs with -no-reboot, so a reboot ends it);
  2. the services are stopped in EXACTLY the reverse of the order this
     boot started them, and there are enough of them for that to mean
     something;
  3. a service that ignores SIGTERM (/tests/stubborn, added at runtime
     with StopTimeout=1500) is killed once its timeout runs out, and the
     ones after it are still stopped -- a stuck service delays the
     shutdown, never blocks it;
  4. a process that is no service (`spawn /bin/sleep`) is swept up by the
     final SIGTERM;
  5. init's last word is that it is rebooting, after all of the above.

    python3 tools/shutdown_test.py [--instance N]

THE FIXTURE IS ORDERED `After=toywm`, so it is the first service
stopped and check 3 can see the ones after it still stop.

POSITIVE CONTROL, run when this was written: init's stop loop walked
FORWARD (`for (int oi = 0; ...)`) -> checks 2 and 3 go red (the fixture
is then stopped LAST, so nothing follows it) and the rest stay green.
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
VM = os.path.join(HERE, "vm.py")
sys.path.insert(0, HERE)
from harness import copy_disk  # noqa: E402

STUBBORN = """Name=stubborn
Exec=/tests/stubborn
Restart=no
After=toywm
StopTimeout=1500
"""


class Result:
    def __init__(self):
        self.fails = 0
        self.passes = 0

    def check(self, what, ok, detail=""):
        print(f"  {'PASS' if ok else 'FAIL'}  {what}" + (f"   {detail}" if detail else ""))
        if ok:
            self.passes += 1
        else:
            self.fails += 1
        return ok


def vm(inst, *args, timeout=180):
    p = subprocess.run([sys.executable, VM, "--instance", str(inst), *args], cwd=REPO,
                       capture_output=True, text=True, timeout=timeout)
    return p.stdout + p.stderr


def init_lines(path):
    with open(path, "rb") as f:
        text = f.read().decode("utf-8", "replace")
    return [ln[ln.index("init: "):].strip() for ln in text.splitlines() if "init: " in ln]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--instance", type=int, default=0)
    ap.add_argument("--keep", action="store_true", help="keep the log and disk copy")
    a = ap.parse_args()
    inst = a.instance
    r = Result()
    tmp = tempfile.mkdtemp(prefix="shutdown_test_")
    disk = os.path.join(tmp, "disk.img")
    log = os.path.join(tmp, "serial.log")
    copy_disk(os.path.join(REPO, "disk.img"), disk)
    print("shutdown_test: checks")
    try:
        out = vm(inst, "--disk", disk, "--serial-log", log, "start", timeout=300)
        if "ready" not in out:
            r.check("the guest booted", False, out[-300:])
            return 1

        desc = os.path.join(tmp, "stubborn")
        with open(desc, "w") as f:
            f.write(STUBBORN)
        vm(inst, "put", desc, "/etc/services.d/stubborn")
        started = False
        for _ in range(30):
            if any("init: started stubborn" in ln for ln in init_lines(log)):
                started = True
                break
            time.sleep(0.5)
        if not r.check("the SIGTERM-ignoring fixture service started", started):
            return 1
        vm(inst, "exec", "spawn /bin/sleep 600")
        time.sleep(1.0)

        try:
            vm(inst, "exec", "reboot", timeout=40)
        except subprocess.TimeoutExpired:
            pass
        gone = False
        for _ in range(60):
            if "not running" in vm(inst, "status") or "stopped" in vm(inst, "status"):
                gone = True
                break
            time.sleep(0.5)

        lines = init_lines(log)
        down = [i for i, ln in enumerate(lines) if ln.startswith("init: shutting down for reboot")]
        r.check("init shut down for a reboot, and the machine went", bool(down) and gone,
                f"shutdown line {'found' if down else 'missing'}, qemu {'gone' if gone else 'still up'}")
        if not down:
            print("\n".join(lines[-30:]))
            return 1
        before, after = lines[:down[0]], lines[down[0]:]

        boot = []
        for ln in before:
            m = re.match(r"init: started (\S+) as pid", ln)
            if m and m.group(1) not in boot:
                boot.append(m.group(1))
        stops = [re.match(r"init: stopping (\S+) \(pid", ln).group(1)
                 for ln in after if re.match(r"init: stopping (\S+) \(pid", ln)]
        ours = [n for n in stops if n != "stubborn"]
        want = [n for n in reversed(boot) if n in ours]
        r.check("the services stop in the reverse of the order they started",
                len(ours) >= 4 and ours == want, f"stopped {ours} vs started {boot}")

        killed = [ln for ln in after if ln.startswith("init: stubborn took") and "killed" in ln]
        i_stub = stops.index("stubborn") if "stubborn" in stops else -1
        r.check("a service that ignores SIGTERM is killed at its StopTimeout, and the rest still stop",
                bool(killed) and 0 <= i_stub < len(stops) - 1,
                f"{killed[:1]} position {i_stub} of {len(stops)}")

        others = [ln for ln in after if re.match(r"init: \d+ other process", ln)]
        n_other = int(re.match(r"init: (\d+)", others[0]).group(1)) if others else 0
        r.check("a process that is no service is swept up by the final SIGTERM", n_other >= 1,
                others[0] if others else "no line")

        last = after[-1] if after else ""
        r.check("init's last word is that it is rebooting", last.startswith("init: rebooting"),
                last)
    finally:
        vm(inst, "stop")
        if a.keep:
            print(f"shutdown_test: kept {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    print(f"\nshutdown_test: {r.passes} passed, {r.fails} failed")
    return 1 if r.fails else 0


if __name__ == "__main__":
    sys.exit(main())
