"""/bin/guictl -- the window manager's diagnostics, asked from ring 3.

WHAT ONLY THIS CAN CHECK. kernel/proc/win_transport_test.c covers the
one-slot refusal and the chunking inside the kernel; neither runs the
PROGRAM, and neither exercises the path that made this hard -- a ring-3
caller whose answer comes from a ring-3 window manager.

THE LOAD-BEARING CHECK IS THE CROSS-PATH ONE. The same subcommand is
asked twice, once through /bin/guictl (ring 3, SYS_WIN_DEBUG, polled)
and once through the serial debug console's own `gui` (ring 0, waits in
place), and the two answers must MATCH. A guictl-only check would pass
just as happily against a program that printed a plausible-looking
answer of its own; the console shares no code with it below the window
server.

The second is that an unknown subcommand is REPORTED. That distinction
was dead for months -- the kernel cleared the incoming flags before
reading them, so WIN_DEBUG_F_UNKNOWN never arrived and `gui
nosuchthing` printed nothing on the serial console too. A check that
only asserted "known commands answer" would not have noticed.

ON DEMAND: it attaches to a running vm.py guest and needs a desktop up.

    python3 tools/vm.py start
    python3 tools/guictl_test.py
"""
import argparse
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok)))
    print(f"  {'ok  ' if ok else 'FAIL'}  {name}" + (f"   -- {detail}" if detail and not ok else ""))


# Lines the kernel writes onto the same console a command's output comes
# back on -- see vm.py's own _NOISE_PREFIXES for why this is needed.
NOISE = re.compile(r"^([a-z][a-z0-9_]*): ")


def clean(text, keep):
    out = []
    for line in text.splitlines():
        if line.startswith(("sh ", "--- ")):
            continue
        m = NOISE.match(line)
        if m and m.group(1) != keep:
            continue
        out.append(line.rstrip())
    return [l for l in out if l]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--instance", default=None)
    args = ap.parse_args()

    def run(cmd, raw=False):
        argv = [sys.executable, os.path.join(REPO, "tools", "vm.py")]
        if args.instance:
            argv += ["--instance", str(args.instance)]
        argv += ["exec"]
        if raw:
            argv += ["--raw"]
        argv += [cmd]
        r = subprocess.run(argv, cwd=REPO, capture_output=True, text=True)
        return r.stdout + r.stderr

    print("guictl_test: the WM's diagnostics from ring 3")

    out = run("guictl state")
    check("state answers with the screen and the cursor",
          "screen " in out and "cursor (" in out, out[:120])

    out = run("guictl compositor")
    check("compositor names a pid", re.search(r"compositor: pid \d+", out), out[:120])

    out = run("guictl taskbar")
    check("taskbar reports geometry", "start" in out and "x=" in out, out[:120])

    # A reply longer than one WIN_DEBUG_CHUNK, so the chunk loop runs.
    out = run("guictl help")
    check("a multi-chunk reply reassembles",
          "gui subcommands" in out and "compositor" in out and len(out) > 700,
          f"{len(out)} bytes")

    out = run("guictl nosuchthing")
    check("an unknown subcommand is reported, not silent",
          "unknown command" in out and "exit 1" in out, out[:160])

    out = run("guictl")
    check("no arguments prints the usage", "usage: guictl" in out, out[:120])

    # --- the cross-path check ------------------------------------------
    # `gui` is a DEBUG CONSOLE command, not a shell one, so it needs
    # --raw; `guictl` is an ordinary program and goes through `sh`.
    mine = clean(run("guictl compositor"), "compositor")
    theirs = clean(run("gui compositor", raw=True), "compositor")
    check("ring 3 and the serial console give the SAME answer",
          mine and mine == theirs, f"guictl={mine} console={theirs}")

    passed = sum(1 for _, ok in results if ok)
    print(f"guictl_test: {passed}/{len(results)} checks passed")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
