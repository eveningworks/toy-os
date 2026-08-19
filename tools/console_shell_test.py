#!/usr/bin/env python3
"""A `text` boot reaches a RING-3 shell prompt, and the kernel shell is
not involved.

WHAT IS UNDER TEST
------------------
Stage 4 of docs/init-design.md. On the `text` target init starts
/bin/tosh from /etc/services.d, and apps/apps.c's kernel shell stands
down instead of racing it for the keyboard. Four properties, each with a
failure the others would not catch:

1. **init starts it, not a person.** The serial log must say
   `init: started tosh`, and tosh must be init's child. Spawning tosh by
   hand -- which tools/stdin_test.py already does -- proves the shell
   works and says nothing about the boot.

2. **The kernel shell stood down.** Asserted through BEHAVIOUR, not only
   through its log line: `touch` is a kernel-shell builtin and is not on
   tosh's PATH, so typing `touch /kprobe.txt` at the console must create
   NOTHING. If the ring-0 REPL were still reading, the file appears.
   That is stdin_test.py's claim probe, pointed at the boot path.

3. **A typed line runs a program.** `file_test` (a /tests ELF on tosh's
   PATH) writes /filetest.txt, checked over the debug console -- a round
   trip from a keystroke through the key ring, a parked process's
   trapframe, a parsed line and a spawn. Nothing shorter satisfies it.

4. **Ctrl-D gets you a new prompt, not a dead console.** The descriptor
   says `Restart=always` for the reason getty does: a console with
   nobody on it must not be reachable by pressing a key. The check is
   a SECOND `init: started tosh` in the log, not a changed pid -- a pid
   is a slot index plus one and slots are reused, so the replacement
   lands in the slot the dead one just left and reports the same number.
   A line typed at the new prompt must still run, and the kernel shell
   must still not have taken the console back in the gap.

PRECONDITIONS THIS TOOL ESTABLISHES ITSELF
------------------------------------------
It boots twice. The first boot sets `system.default_target text` (and
the US keyboard layout, since a QMP qcode names a physical key by its US
label and this OS defaults to `se` -- every `/` typed arrives as `-`
otherwise, and a substring assertion passes against the wrong file).
The second boot is the one under test. Both run against a COPY of
disk.img, which this tool writes to /etc on.

WHICH CHECKS ARE LOAD-BEARING, AND WHAT THIS TOOL CANNOT SEE
------------------------------------------------------------
Measured with a positive control, and the result is worth stating
plainly because it is not the one that was expected. Disabling
apps/apps.c's console_belongs_to_init() gate reddens ONLY the
"stood down" log check -- the /kprobe.txt probe stays green.

That is correct rather than a weak test: keyboard_claim_console() takes
the console on tosh's FIRST READ, about ten milliseconds into the boot,
and from then on the ring-0 REPL is suspended whether or not it started.
What the gate removes is the WINDOW BEFORE that claim exists -- a banner
and a prompt drawn onto a console that is about to belong to somebody
else, and any key typed in the meantime. This tool types seconds later,
so it cannot reach that window; racing a 10 ms gap would be a flake, not
a check. The probe is still worth keeping: it is what would catch the
claim regressing, which is the other half of the same property.

Usage:
    python3 tools/console_shell_test.py
    python3 tools/console_shell_test.py --instance 2   # alongside another VM

Exits 0 if every check passed, 1 otherwise, 2 if it could not run.
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
from gui_debug import DebugConsole      # noqa: E402
from shell_flow import ShellFlow        # noqa: E402
import port_guard                       # noqa: E402

VM = os.path.join(REPO, "tools", "vm.py")
PROBE = "/kprobe.txt"
MADE = "/filetest.txt"

checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"    [{detail}]" if detail else ""))


def vm_cmd(disk, instance, *argv):
    cmd = [sys.executable, VM, "--disk", disk]
    if instance:
        cmd += ["--instance", str(instance)]
    return cmd + list(argv)


def vm_run(disk, instance, *argv):
    r = subprocess.run(vm_cmd(disk, instance, *argv), cwd=REPO,
                       capture_output=True, text=True)
    return r.stdout + r.stderr


def root_names(dbg):
    """The names in `/` as a SET of exact entries -- never a substring
    search over the raw reply, which cannot tell a file called
    `-kprobe.txt` (the harness typing the wrong key) from the property
    being asserted. stdin_test.py paid for that one."""
    out = dbg.send("sh ls /") or ""
    names = set()
    for line in out.splitlines():
        for tok in line.split():
            if tok.endswith("/") or "." in tok:
                names.add(tok.rstrip("/"))
    return names


def dmesg(dbg, marker=None, tries=8):
    """The kernel log, read with a generous timeout and RETRIED until a
    marker appears.

    Both halves are load-bearing. dmesg is thousands of characters over
    a serial socket, and DebugConsole's default 6 s read returns a
    TRUNCATED buffer that ends mid-line -- which looks exactly like the
    kernel never printing the line being looked for. And init's own
    lines arrive as it gets to them, so a read taken too early is
    missing them rather than contradicting them.
    """
    prev = dbg.timeout
    try:
        dbg.timeout = 20.0
        out = ""
        for _ in range(tries):
            out = dbg.send("sh dmesg") or ""
            if marker is None or marker in out:
                return out
            time.sleep(1.0)
        return out
    finally:
        dbg.timeout = prev


def tosh_row(dbg):
    """(pid, state) of the running tosh, or None. `kstack slots` is the
    one kernel command reporting a scheduler state over the serial
    console; `ps` is a /bin program, so its output goes to the screen
    tosh owns rather than to this socket."""
    out = dbg.send("sh kstack slots") or ""
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 4 and p[0].isdigit() and p[1].isdigit() and p[3].isdigit():
            if p[2].startswith("tosh"):
                return int(p[1]), int(p[3])
    return None


def type_line(flow, text):
    flow.type_command(text)
    flow.session.send_key("ret")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disk", default=None)
    # `auto` by default: this tool owns its guest end to end and nothing
    # else needs to attach to it, so there is no reason to fight a
    # gui_regress.py (slots 0-3) that may be running in another
    # terminal. It prints the slot it took -- see
    # port_guard.find_free_instance() on why that reporting is the
    # point, and why it is a narrowing rather than a lock.
    ap.add_argument("--instance", default="auto",
                    help="slot number, or `auto` for the lowest free one")
    ap.add_argument("--keep", action="store_true", help="leave the VM running")
    args = ap.parse_args()

    if args.instance == "auto":
        n = port_guard.find_free_instance()
        if n is None:
            print("console_shell_test: no free VM slot")
            return 2
        args.instance = n
        print(f"console_shell_test: slot {args.instance} (QMP {4445 + args.instance})")
    args.instance = int(args.instance)

    tmp = None
    if not args.disk:
        src = os.path.join(REPO, "disk.img")
        if not os.path.exists(src):
            print("console_shell_test: no disk.img -- run `make iso` first")
            return 2
        tmp = tempfile.NamedTemporaryFile(suffix=".img", delete=False)
        tmp.close()
        # --sparse=always: disk.img is a few MB of data in a 9 GB sparse
        # file, and a hole-filling copy costs the whole 9 GB.
        subprocess.run(["cp", "--reflink=auto", "--sparse=always", src, tmp.name],
                       check=True)
        args.disk = tmp.name

    sock = ".vm.serial" if not args.instance else f".vm.{args.instance}.serial"
    port = 4445 + args.instance

    try:
        print("first boot -- configuring the target")
        if "ready" not in vm_run(args.disk, args.instance, "start"):
            print("console_shell_test: could not start the VM")
            return 2
        dbg = DebugConsole(sock)
        dbg.send("sh keyboard us")
        dbg.send("sh config set system.default_target text")
        dbg.send(f"sh rm {PROBE}")
        dbg.send(f"sh rm {MADE}")
        time.sleep(0.5)
        vm_run(args.disk, args.instance, "stop")

        print("second boot -- the one under test")
        if "ready" not in vm_run(args.disk, args.instance, "start"):
            print("console_shell_test: the text-target boot never became ready")
            return 2
        dbg = DebugConsole(sock)
        flow = ShellFlow(qmp_port=port)

        boot = dmesg(dbg, "init: started tosh")
        check("the machine booted the text target", "boot target 'text'" in boot)
        m = re.search(r"init: started tosh as pid (\d+)", boot)
        check("init started the console shell", bool(m),
              m.group(0) if m else "no 'init: started tosh' line")
        check("the kernel shell stood down",
              "kernel shell standing down" in boot)

        row = tosh_row(dbg)
        # 4 == SCHED_BLOCKED (kernel/proc/scheduler.c's enum sched_state).
        check("an idle tosh is BLOCKED, not spinning",
              bool(row) and row[1] == 4,
              f"pid {row[0]} state {row[1]}" if row else "no tosh in kstack slots")

        before = root_names(dbg)
        check("fixture is clean -- neither probe file exists",
              "kprobe.txt" not in before and "filetest.txt" not in before,
              " ".join(sorted(before)))

        print("the console has one reader, and it is not the kernel shell")
        # `touch` is a kernel-shell builtin and is NOT on tosh's PATH,
        # so a working stand-down leaves nothing behind.
        type_line(flow, "touch /kprobe.txt")
        time.sleep(1.5)
        check("a kernel-shell builtin typed at the console creates nothing",
              "kprobe.txt" not in root_names(dbg))

        print("a typed line reaches the ring-3 shell")
        type_line(flow, "file_test")
        time.sleep(2.5)
        check("a program spawned from the typed line ran",
              "filetest.txt" in root_names(dbg))

        print("Ctrl-D, and init puts a new prompt back")
        dbg.send(f"sh rm {MADE}")
        time.sleep(0.4)
        flow.session.combo(["ctrl", "d"])
        # init's first restart has a zero backoff, but the exit, the
        # reap and the spawn are three scheduler passes.
        time.sleep(3.0)
        # NOT "a new pid": a pid is a slot index plus one and slots are
        # reused, so the restarted shell lands in the slot the dead one
        # just left and reports the SAME pid. The log is what
        # distinguishes a restart from a shell that never exited.
        after = dmesg(dbg, "init: tosh")
        exited = "init: tosh (pid" in after and "exited with code" in after
        starts = after.count("init: started tosh")
        row2 = tosh_row(dbg)
        # The EXIT line is the unambiguous half. A raw start count is
        # not: the debug console interleaves live klog output with the
        # dump it is replying to, so a line printed during the read is
        # counted twice.
        check("init restarted the console shell after Ctrl-D",
              exited and starts >= 2,
              f"exit line {'seen' if exited else 'MISSING'}, {starts} start(s)")
        check("a shell is on the console again", bool(row2) and row2[1] == 4,
              f"pid {row2[0]} state {row2[1]}" if row2 else "no tosh row")

        type_line(flow, "file_test")
        time.sleep(2.5)
        names = root_names(dbg)
        check("the restarted shell runs a typed line", "filetest.txt" in names)
        check("the kernel shell did not take the console back in the gap",
              "kprobe.txt" not in names)

        # --- the SHARED line editor, at the real prompt ---------------
        #
        # /bin/tosh edits with kernel/lib/klineedit.c, compiled a second
        # time into libuapp.a. userland/tests/klineedit_test.c already
        # proves that build produces the right BUFFER; what it cannot
        # reach is the console front end around it -- the raw fd-0 key
        # stream, the '\r' redraw, and history. These three assert that
        # end, and each asserts through the FILESYSTEM, so a redraw that
        # merely looks plausible cannot satisfy them.
        print("the shared line editor at the console")

        # 1. Cursor movement. `zfile_test` is not a program; Home then
        #    Delete removes the leading z and leaves `file_test`, which
        #    is. An append-only editor cannot pass this: Home and Delete
        #    would be ignored and `zfile_test: not found` is all that
        #    happens.
        dbg.send(f"sh rm {MADE}")
        time.sleep(0.4)
        flow.type_command("zfile_test")
        flow.session.send_key("home")
        time.sleep(0.2)
        flow.session.send_key("delete")
        time.sleep(0.2)
        flow.session.send_key("ret")
        time.sleep(2.5)
        check("Home + Delete edit mid-line, and the edited line runs",
              "filetest.txt" in root_names(dbg))

        # 2. Ctrl-U kills the line. Typing a command that WOULD leave a
        #    trace, killing it, then running one that does: the probe
        #    file must be absent and the other present, so "the kill
        #    worked" is distinguishable from "nothing ran at all".
        dbg.send(f"sh rm {MADE}")
        time.sleep(0.4)
        flow.type_command("touch /kprobe.txt")
        flow.session.combo(["ctrl", "u"])
        time.sleep(0.3)
        type_line(flow, "file_test")
        time.sleep(2.5)
        names = root_names(dbg)
        check("Ctrl-U killed the line before it ran",
              "kprobe.txt" not in names and "filetest.txt" in names,
              " ".join(sorted(names)))

        # 3. History. Up recalls the previous line and Enter runs it --
        #    with nothing typed in between, so the only way the file can
        #    come back is the editor having refilled the buffer.
        dbg.send(f"sh rm {MADE}")
        time.sleep(0.4)
        flow.session.send_key("up")
        time.sleep(0.3)
        flow.session.send_key("ret")
        time.sleep(2.5)
        check("Up recalls the previous command and it runs again",
              "filetest.txt" in root_names(dbg))

        # --- REDIRECTION -------------------------------------------
        #
        # `>` and `<` are the payoff of the descriptor table: the shell
        # points its OWN fd 1 (or 0) at a file around the spawn and the
        # child inherits it, which is the dance fork() normally exists
        # to allow. Each is asserted by reading the file back through a
        # completely different path (the debug console's `sh cat`), not
        # by anything the shell reports about itself.
        print("redirection")

        dbg.send("sh rm /redir.txt")
        time.sleep(0.4)
        # An EXTERNAL program: /bin/hello writes to fd 1 and exits, and
        # is told nothing about the file.
        type_line(flow, "hello > /redir.txt")
        time.sleep(2.5)
        out = dbg.send("sh cat /redir.txt") or ""
        check("`>` sends an external program's output to a file",
              "Hello" in out, out.strip()[:60])

        # `>>` appends where `>` truncated. Running the same command
        # twice must leave TWO copies -- a `>>` that silently truncated
        # would leave one and look identical to a working `>`.
        type_line(flow, "hello >> /redir.txt")
        time.sleep(2.5)
        out = dbg.send("sh cat /redir.txt") or ""
        check("`>>` appends rather than truncating",
              out.count("Hello") >= 2, f"{out.count('Hello')} copies")

        # `<` points fd 0 at a file. /tests/catin reads stdin and writes
        # it out, so this is a round trip through both redirections at
        # once -- and it can only work if the CHILD inherited fd 0.
        dbg.send("sh rm /redir_in.txt")
        time.sleep(0.4)
        type_line(flow, "catin < /redir.txt > /redir_in.txt")
        time.sleep(2.5)
        out = dbg.send("sh cat /redir_in.txt") or ""
        check("`<` feeds a file to a program's stdin",
              "Hello" in out, out.strip()[:60])

        # A BUILTIN redirects too. tosh's `ls` prints through the
        # shell's own sink rather than writing to fd 1, so this is the
        # check that the sink swap works -- without it `ls > f` would
        # silently print to the screen and leave an empty file.
        dbg.send("sh rm /redir_ls.txt")
        time.sleep(0.4)
        type_line(flow, "ls / > /redir_ls.txt")
        time.sleep(2.0)
        out = dbg.send("sh cat /redir_ls.txt") or ""
        check("a builtin's output redirects too", "bin" in out, out.strip()[:60])

        # --- PIPELINES ---------------------------------------------
        #
        # `a | b` is the descriptor table's other payoff: the shell
        # points stage A's fd 1 and stage B's fd 0 at the two ends of a
        # pipe, spawns both, and closes its own copies -- the last of
        # which is what lets B ever see EOF.
        print("pipelines")

        dbg.send("sh rm /pipe_out.txt")
        time.sleep(0.4)
        # hello writes one line; catin copies stdin to stdout. Asserted
        # through a file, so nothing depends on what the shell echoes.
        type_line(flow, "hello | catin > /pipe_out.txt")
        time.sleep(3.0)
        out = dbg.send("sh cat /pipe_out.txt") or ""
        check("a two-stage pipeline carries data between processes",
              "Hello" in out, out.strip()[:60])

        # THREE stages, so the loop is exercised rather than a special
        # case for two. Each catin is a real process copying the stream
        # on, so the text has to survive two pipes.
        dbg.send("sh rm /pipe3.txt")
        time.sleep(0.4)
        type_line(flow, "hello | catin | catin > /pipe3.txt")
        time.sleep(3.5)
        out = dbg.send("sh cat /pipe3.txt") or ""
        check("a three-stage pipeline works, not just two",
              "Hello" in out, out.strip()[:60])

        # A BUILTIN as the producer. tosh's `ls` prints through the
        # shell's own sink, so this is the check that a builtin's output
        # reaches a pipe at all.
        dbg.send("sh rm /pipe_ls.txt")
        time.sleep(0.4)
        type_line(flow, "ls / | catin > /pipe_ls.txt")
        time.sleep(3.0)
        out = dbg.send("sh cat /pipe_ls.txt") or ""
        check("a builtin can feed a pipeline", "bin" in out, out.strip()[:60])

        # And a redirect that cannot be opened must NOT run the command.
        # `cat < missing` printing nothing is not enough -- a shell that
        # ran the command anyway would also print nothing.
        dbg.send("sh rm /redir_never.txt")
        time.sleep(0.4)
        type_line(flow, "hello < /no_such_file > /redir_never.txt")
        time.sleep(2.0)
        check("a failed redirect does not run the command",
              "redir_never.txt" not in root_names(dbg))


    finally:
        if not args.keep:
            vm_run(args.disk, args.instance, "stop")
        if tmp and not args.keep:
            try:
                os.unlink(tmp.name)
            except OSError:
                pass

    passed = sum(1 for _, ok, _ in checks if ok)
    print(f"\nconsole_shell_test: {passed}/{len(checks)} checks passed")
    return 0 if passed == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
