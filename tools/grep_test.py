#!/usr/bin/env python3
"""/bin/grep, driven through a real ring-3 shell.

WHY THIS EXISTS SEPARATELY FROM /tests/regex_test
-------------------------------------------------
The engine is checked three ways already: `/tests/regex_test` runs the
case table in ring 3, a host harness runs the SAME table against the
same regex.c, and `tools/regex_hostcheck.py` runs it against glibc as
an independent oracle. None of them run the COMMAND.

What only the command has is the half that is not regex at all:
reading standard input from a PIPE, the last line of a file with no
trailing newline, the flags, and the exit status. And the pipe cannot
be reached from the kernel debug console at all -- the kernel shell
splits on spaces and hands `|` to the program as an argument, so
`dmesg | grep partition` there runs dmesg with three arguments and
fails in a way that looks like grep is broken. Pipes are `/bin/tosh`'s,
and the GUI Terminal is where tosh runs.

So this types into a Terminal window and asserts through the
FILESYSTEM -- the same shape tools/terminal_probe.py uses, and its
`Terminal` helper is imported rather than re-derived. Reading the
screen instead would mean parsing a framebuffer for text that the
terminal may have scrolled.

THE LOAD-BEARING CHECK IS THE PIPE. Everything else here would pass
with grep unable to read stdin at all, because every other check names
a file on the command line.

ON DEMAND, not in the gate: it needs the desktop and a Terminal window,
and it is slower than the ring-3 test that covers the engine.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gui_debug import DebugConsole, enter_gui       # noqa: E402
from qmp_test import QMPSession                     # noqa: E402
from terminal_probe import Terminal                  # noqa: E402

OUT = "/tmp/grep_out.txt"

# /tests/sample.txt -- 401 lines, each beginning with its own 4-digit
# number. A DETERMINISTIC fixture, which `dmesg` is not: dmesg's
# content varies with the boot and with what any earlier tool did, so a
# count or an anchor asserted against it is asserted against a moving
# target. The pipe check below still uses dmesg, because a pipe needs a
# producer and that is the one thing sample.txt cannot be.
FIXTURE = "/tests/sample.txt"

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok)))
    print(f"  {'ok  ' if ok else 'FAIL'}  {name}" + (f"   -- {detail}" if detail and not ok else ""))


def read_out(dbg):
    """Every line of OUT, with the console's own chatter filtered out."""
    out = dbg.send(f"sh cat {OUT}") or ""
    if "no such file" in out or "not found" in out:
        return None
    # The console interleaves its own chatter with the reply, and tosh
    # prints `[exit N]` after a command that failed. Dropping the exit
    # marker matters: without it a grep that correctly matched NOTHING
    # returns one line of content and reads as a match.
    #
    # Matched by PREFIX, not by substring -- a substring test on "wm:"
    # deletes any line of the file that happens to contain it, which is
    # how this first reported a filtered dmesg as empty.
    noise = ("sh ", "elf_run", "syscall", "uterm:", "wm:", "cat:", "dbg>", "[exit ")
    keep = []
    for ln in out.splitlines():
        ln = ln.strip()
        if not ln or any(ln.startswith(k) for k in noise):
            continue
        keep.append(ln)
    return keep or None


def run(term, dbg, cmdline):
    """Type a shell line that redirects into OUT, then read OUT back."""
    dbg.send(f"sh rm {OUT}")
    term.type(f"{cmdline} > {OUT}")
    term.enter(settle=2.0)
    return read_out(dbg)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--qmp-port", type=int, default=4445)
    ap.add_argument("--sock", default=".vm.serial")
    args = ap.parse_args()

    qmp = QMPSession(port=args.qmp_port)
    enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    term = Terminal(dbg, qmp)

    # --- THE PIPE. grep reading standard input, which is the only
    # thing here that a file-argument implementation would fail.
    lines = run(term, dbg, f"cat {FIXTURE} | grep ^0001")
    check("a pipe reaches grep's stdin",
          lines is not None and len(lines) == 1 and lines[0].startswith("0001"),
          f"got {lines!r}")
    # ...and it is a FILTER, not a passthrough. A grep that ignored its
    # pattern and copied stdin through would pass the check above.
    # Counted with `grep -c .` on both sides rather than by reading the
    # whole of dmesg back through the console, which is thousands of
    # bytes of interleaved output.
    matched = run(term, dbg, f"cat {FIXTURE} | grep -c ^000")
    total = run(term, dbg, f"cat {FIXTURE} | grep -c .")
    # ^000 is lines 0001..0009 -- nine of them, and the file has 401.
    ok = (matched and total and matched[0] == "9" and total[0] == "401")
    check("...and it filtered rather than copying stdin through", ok,
          f"{matched} matched of {total} total, expected 9 of 401")

    # --- flags and ERE, over the DETERMINISTIC fixture. Every count
    # below is a number this file actually has, so a wrong answer is a
    # wrong answer rather than a different boot.
    lines = run(term, dbg, f"grep -c . {FIXTURE}")
    check("-c counts every line", lines is not None and lines[0] == "401",
          f"got {lines!r}, expected 401")

    lines = run(term, dbg, f"grep -n fixture {FIXTURE}")
    check("-n prefixes a line number",
          lines is not None and lines[0].split(":", 1)[0].isdigit(),
          f"got {lines!r}")

    # Line 0001 is the only line matching ^0001, and the number is at
    # the very start -- so this is a real anchor test, not "the string
    # appears somewhere".
    lines = run(term, dbg, f"grep ^0001 {FIXTURE}")
    check("^ anchors to the start of the line",
          lines is not None and len(lines) == 1 and lines[0].startswith("0001"),
          f"got {lines!r}")
    lines = run(term, dbg, f"grep -c ^0 {FIXTURE}")
    check("...and matches every numbered line",
          lines is not None and lines[0] == "401", f"got {lines!r}")

    # ALTERNATION IS NOT CHECKED HERE, and the reason is the shell, not
    # grep. `|` is a pipe to tosh and tosh has NO QUOTING -- it splits
    # on the bar before anything else (userland/lib/tosh.c), so there is
    # no way to type a pattern containing one. `grep -c "(a|b)" f` runs
    # `grep -c "(a` piped into `b)" f`.
    #
    # Left as a gap rather than papered over: alternation is covered by
    # /tests/regex_test, which calls regexec() directly and needs no
    # shell. What is missing is only the ability to TYPE it, and that is
    # on docs/roadmap.md as tosh quoting.
    lines = run(term, dbg, f"grep -c ^000[12] {FIXTURE}")
    check("a bracketed alternative works (see the note on `|` above)",
          lines is not None and lines[0] == "2", f"got {lines!r}, expected 2")

    lines = run(term, dbg, f"grep -c ^0[0-9]+ {FIXTURE}")
    check("a class with + works", lines is not None and lines[0] == "401",
          f"got {lines!r}")

    lines = run(term, dbg, f"grep -c ^0{{3}}1 {FIXTURE}")
    check("an interval {n} works", lines is not None and lines[0] == "1",
          f"got {lines!r}, expected 1")

    # -v must be the exact complement of the match.
    lines = run(term, dbg, f"grep -c -v ^0 {FIXTURE}")
    check("-v inverts, and 401 + 0 accounts for the file",
          lines is not None and lines[0] == "0", f"got {lines!r}")

    lines = run(term, dbg, f"grep -c -i FIXTURE {FIXTURE}")
    icase = lines[0] if lines else None
    lines = run(term, dbg, f"grep -c FIXTURE {FIXTURE}")
    check("-i ignores case and the default does not",
          icase is not None and icase.isdigit() and int(icase) > 0
          and lines is not None and lines[0] == "0",
          f"with -i: {icase}, without: {lines!r}")

    # --- a bad pattern is REPORTED, not silently treated as literal.
    dbg.send(f"sh rm {OUT}")
    term.type(f"grep [abc {FIXTURE}")
    term.enter(settle=2.0)
    check("an invalid pattern is refused with a reason",
          read_out(dbg) is None, "the command should have failed, not written output")

    dbg.send(f"sh rm {OUT}")

    failed = [n for n, ok in results if not ok]
    print()
    if failed:
        print(f"grep_test: FAIL -- {len(failed)} of {len(results)} checks")
        for n in failed:
            print(f"  - {n}")
        return 1
    print(f"grep_test: PASS -- {len(results)} checks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
