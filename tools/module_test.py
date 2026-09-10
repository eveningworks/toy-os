#!/usr/bin/env python3
"""Loadable kernel modules, end to end, in a running guest.

WHAT THIS COVERS. kernel/core/module.c's KTESTs load hello.ko from
inside the kernel and prove the loader; this drives the same thing the
way a person does -- /bin/modload, /bin/modunload, /bin/lsmod -- and
then the one check no KTEST can make: unloading the e1000 MODULE takes
the network away, loading it again brings the network back, with
/bin/netd re-leasing an address on its own.

THE PHASES, and what a broken build would still pass

  1. Boot-time autoload: e1000 is a module (drivers.conf) and QEMU's
     default machine has the card, so `lsmod` must list it holding one
     device and the log must say it was loaded FOR 8086:100e. A kernel
     with e1000 built in would pass the network checks and fail here.
  2. hello.ko: loads, says so in the log, is listed, refuses a second
     load, unloads and says so. Every refusal below is asserted to FAIL
     -- a loader that accepts everything passes the load half alone.
  3. unexported.ko is refused with the symbol NAMED in the log.
  4. A truncated copy of hello.ko is refused as not loadable.
  5. modunload e1000: the interface is gone from lsdrv and ping cannot
     send. modload e1000: the address is back within the netd poll
     interval and ping answers. Asserting the DOWN half is what makes
     the UP half evidence: without it a reload that silently did
     nothing would pass on the boot-time lease.

It writes only under /var/tmp on the guest.

    python3 tools/vm.py start
    python3 tools/module_test.py
"""
import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok)))
    print(f"  {'ok  ' if ok else 'FAIL'}  {name}" + (f"   -- {detail}" if detail and not ok else ""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--instance", default=None)
    args = ap.parse_args()

    def run(*cmds):
        argv = [sys.executable, os.path.join(REPO, "tools", "vm.py")]
        if args.instance:
            argv += ["--instance", str(args.instance)]
        argv += ["exec", *cmds]
        r = subprocess.run(argv, cwd=REPO, capture_output=True, text=True)
        return r.stdout + r.stderr

    def dmesg():
        return run("dmesg")

    # A previous run, or a person, may have left hello loaded.
    run("modunload hello")

    # --- 1. boot-time autoload -------------------------------------------
    out = run("lsmod")
    check("lsmod lists e1000 holding a device", "e1000" in out and "1 device" in out, out[-300:])
    log = dmesg()
    check("log says e1000 was loaded for 8086:100e", "module: e1000 for 8086:100e" in log)
    check("lsdrv shows e1000 bound to the card", "e1000" in run("lsdrv") and "net-" in run("lsdrv"))

    # --- 2. hello ---------------------------------------------------------
    out = run("modload hello", "lsmod")
    check("modload hello succeeds and is listed", "hello" in out and "exit 1" not in out, out[-300:])
    check("hello logged its load", "hello: loaded" in dmesg())
    out = run("modload hello")
    check("a second modload hello is refused", "exit 1" in out and "exists" in out, out[-300:])
    out = run("modunload hello", "lsmod")
    check("modunload hello succeeds and it is gone", "exit 1" not in out and "hello" not in out.split("--- lsmod ---")[-1], out[-300:])
    check("hello logged its unload", "hello: unloaded" in dmesg())
    out = run("modunload hello")
    check("unloading an absent module is refused", "exit 1" in out, out[-200:])

    # --- 3. unexported ----------------------------------------------------
    out = run("modload unexported", "lsmod")
    check("unexported.ko is refused", "exit 1" in out and "unexported" not in out.split("--- lsmod ---")[-1], out[-300:])
    check("the log names the unexported symbol", "unknown symbol scheduler_kill" in dmesg())

    # --- 4. truncated ------------------------------------------------------
    out = run("cp /lib/modules/hello.ko /var/tmp/trunc.ko", "truncate /var/tmp/trunc.ko 900",
              "modload /var/tmp/trunc.ko", "lsmod")
    check("a truncated .ko is refused", "exit 1" in out and "trunc" not in out.split("--- lsmod ---")[-1], out[-300:])
    check("the log says why", "outside the file" in dmesg() or "truncated" in dmesg())
    run("rm /var/tmp/trunc.ko")

    # --- 5. the e1000 round trip ---------------------------------------------
    out = run("modunload e1000", "lsdrv", "ping -c 1 10.0.2.2")
    check("modunload e1000 succeeds", "modunload: exit 1" not in out, out[-300:])
    check("the card is gone from lsdrv", "e1000" not in out.split("--- lsdrv ---")[-1].split("--- ping")[0])
    check("ping cannot send with the driver gone", "0 packets transmitted" in out or "send failed" in out, out[-300:])

    out = run("modload e1000", "lsmod")
    check("modload e1000 binds the card again", "1 device" in out, out[-300:])
    deadline = time.time() + 40
    inet = ""
    while time.time() < deadline:
        inet = run("ifconfig")
        if "inet 10." in inet:
            break
        time.sleep(2)
    check("netd leased an address again", "inet 10." in inet, inet[-300:])
    out = run("ping -c 1 10.0.2.2")
    check("ping answers after the reload", "1 received" in out, out[-300:])

    failed = [n for n, ok in results if not ok]
    print(f"\nmodule_test: {len(results) - len(failed)}/{len(results)} passed"
          + (f" -- FAILED: {', '.join(failed)}" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
