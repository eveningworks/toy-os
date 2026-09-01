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
import calendar
import re
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
from gui_debug import DebugConsole      # noqa: E402
import vm as vm_mod                   # noqa: E402 -- started_ok(), see its comment
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


def dmesg(dbg, marker=None, tries=8, cmd="sh dmesg"):
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
            out = dbg.send(cmd) or ""
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
        if not vm_mod.started_ok(vm_run(args.disk, args.instance, "start")):
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
        if not vm_mod.started_ok(vm_run(args.disk, args.instance, "start")):
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


        # POLLED, NOT SAMPLED ONCE. A shell reaches its blocking read a
        # moment after init spawns it, so a single sample taken right
        # after boot catches it READY perfectly legitimately -- measured
        # at 2 runs in 4 before this was a poll, which reads as an
        # intermittent kernel bug and is a harness one.
        #
        # It does not weaken the check: an idle shell must REACH
        # SCHED_BLOCKED, and a shell that spins never does, so the bound
        # is what fails rather than the first unlucky sample.
        row = tosh_row(dbg)
        deadline = time.time() + 8
        while time.time() < deadline and not (row and row[1] == 4):
            time.sleep(0.3)
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
        # `rescue touch` is a KERNEL-SHELL-ONLY command -- the kernel's
        # own copies of the file commands live behind that one name
        # (apps/shell_rescue.c), and there is no /bin/rescue -- so a
        # working stand-down leaves nothing behind.
        #
        # It used to be a bare `touch`, on the premise that `touch` was
        # a kernel builtin not on tosh's PATH. That premise went stale
        # when `touch` became a /bin PROGRAM: tosh then found it, ran
        # it, and created the probe file -- so this check failed while
        # the property it names was perfectly intact, and left
        # `kprobe.txt` behind to fail the Ctrl-U check further down as
        # well. A probe has to name something the OTHER shell genuinely
        # cannot reach, and `rescue` is the one name guaranteed to stay
        # that way (CLAUDE.md).
        type_line(flow, "rescue touch /kprobe.txt")
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
        flow.type_command("rescue touch /kprobe.txt")
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
        # THE ENVIRONMENT REACHES A REAL DESCENDANT OF init.
        #
        # This is the only place that can be checked. Inheritance is a
        # LIBRARY convention here -- sys_spawn() passes `environ`, the
        # kernel stores nothing -- so what a process has depends
        # entirely on who started it. A program run from the ring-0
        # shell has no environment at all, correctly, because that
        # shell is not a ring-3 process and has none to pass.
        #
        # On the text target this shell IS init's child, so a program
        # it spawns is init's grandchild, and PATH arriving here means
        # the whole chain works: init seeded it, tosh inherited it, and
        # tosh passed it on. Read back through the debug console's `sh
        # cat`, a completely different path from the one being tested.
        print("the environment")

        dbg.send("sh rm /envprobe.txt")
        time.sleep(0.4)
        type_line(flow, "/tests/env_child PATH > /envprobe.txt")
        time.sleep(2.5)
        out = dbg.send("sh cat /envprobe.txt") or ""
        check("init's PATH reaches a grandchild through the ring-3 shell",
              "/bin" in out, out.strip()[:60])
        # The negative half: a variable nothing set must be absent, or
        # "it found something" would be satisfied by an environment full
        # of noise.
        dbg.send("sh rm /envprobe.txt")
        time.sleep(0.4)
        type_line(flow, "/tests/env_child NOSUCHVAR > /envprobe.txt")
        time.sleep(2.5)
        out = dbg.send("sh cat /envprobe.txt") or ""
        check("and a variable nobody set is reported unset",
              "(unset)" in out, out.strip()[:60])

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

        # A BUILTIN redirects too. A builtin prints through the shell's
        # own sink rather than writing to fd 1, so this is the check that
        # the sink swap works -- without it `pwd > f` would silently
        # print to the screen and leave an empty file.
        #
        # **IT USED TO BE `ls`, WHICH IS NO LONGER A BUILTIN.** tosh's
        # `ls` shadowed /bin/ls and was strictly worse -- no flags, no
        # colour -- so it went the way `cat`'s builtin did. `pwd` is one
        # of the three that remain, and each of those has to be one:
        # `cd` changes the shell's own directory, and `pwd`/`help` have
        # no /bin twin.
        dbg.send("sh rm /redir_ls.txt")
        time.sleep(0.4)
        type_line(flow, "pwd > /redir_ls.txt")
        time.sleep(2.0)
        out = dbg.send("sh cat /redir_ls.txt") or ""
        check("a builtin's output redirects too", "/" in out, out.strip()[:60])

        # --- WHO OWNS THE CONSOLE, AND WHAT IS WAITING FOR IT --------
        #
        # `tty` and the wait reason in `ps`'s STATE column both report
        # state that ONLY EXISTS ON THIS BOOT. Under a desktop the
        # compositor holds the keyboard, nobody owns the console, and
        # every assertion below would pass vacuously or not at all -- a
        # text target is the only way a ring-3 shell owns the physical
        # console, which is why these live here rather than in the gate.
        #
        # Read back through a FILE rather than over this socket, for the
        # reason tosh_row() gives: a /bin program's output goes to the
        # screen tosh owns. That also makes it a round trip -- the file
        # is written by the guest and read by a completely different
        # path (the debug console's `sh cat`).
        print("console ownership, and what a blocked process waits on")
        shell_pid = (tosh_row(dbg) or (0, 0))[0]

        # --- dmesg reaches the kernel log from RING 3 ----------------
        #
        # It was a ring-0 shell builtin, so at a `$` prompt it resolved
        # to a /bin lookup, found nothing, and failed -- which is what a
        # person hits in a Terminal window. What this pins is not that
        # the command exists but that QUERY_KLOG answers a SCHEDULED
        # ring-3 process: the `#` prompt reaches it through the legacy
        # loader, which is a different caller with no scheduler slot, so
        # a check run only there would prove the wrong half.
        dbg.send("sh rm /klog.txt")
        time.sleep(0.4)
        type_line(flow, "dmesg -n 5 > /klog.txt")
        time.sleep(2.5)
        out = dbg.send("sh cat /klog.txt") or ""
        # STRUCTURAL, not a content match, and the first draft of this
        # got it wrong in an instructive way: it looked for a boot line
        # ("toy-os:"), which `-n 5` can never show, because -n tails the
        # NEWEST lines and those are whatever the machine did a moment
        # ago. The check failed against a working dmesg.
        #
        # What is true of every klog line regardless of when it was
        # written is its shape -- `[<seconds>] text`, a timestamp klog.c
        # adds once per logical line. Nothing else the shell could
        # produce here has it: a "command not found", a usage string or
        # an empty file all fail it.
        body = [ln for ln in out.splitlines()
                if ln.strip() and not ln.startswith("sh ")]
        stamped = [ln for ln in body if re.match(r"^\[\s*\d+\.\d+\]", ln)]
        check("`dmesg` reads the kernel log from a ring-3 shell",
              len(stamped) >= 3, f"{len(stamped)} stamped of {len(body)}")
        # -n is a TAIL, so a handful of lines and not seventy. Without
        # this the check above passes against a dmesg that ignores its
        # flags and dumps the whole ring -- which is also how this test
        # would start timing out.
        check("...and -n tails rather than dumping the whole ring",
              0 < len(stamped) <= 8, f"{len(stamped)} lines")

        dbg.send("sh rm /tty.txt")
        time.sleep(0.4)
        type_line(flow, "tty > /tty.txt")
        time.sleep(2.5)
        out = dbg.send("sh cat /tty.txt") or ""
        check("`tty` names the shell as the console's owner",
              f"pid {shell_pid} (" in out, out.strip()[:90])
        # A console with an owner and no foreground group is the state
        # in which Ctrl-C silently does nothing while everything else
        # looks healthy -- kernel/tty.h's invariant, from outside.
        check("...and it has a foreground group, so Ctrl-C means something",
              "foreground group: none" not in out, out.strip()[:90])
        # WHICH suspend reason, not merely that there is one: on a text
        # boot ring 0 stands down because a ring-3 process claimed fd 0,
        # NOT because a compositor took the keyboard. A `tty` that
        # confused the two would print a compositor that is not running.
        check("...because a ring-3 process claimed fd 0, not a compositor",
              "read by a ring-3 process through fd 0" in out
              and "compositor" not in out, out.strip()[:90])

        dbg.send("sh rm /ps.txt")
        time.sleep(0.4)
        type_line(flow, "ps > /ps.txt")
        time.sleep(2.5)
        out = dbg.send("sh cat /ps.txt") or ""
        # The shell is blocked waiting for the very job writing this
        # file, so its row must say so. Asserting on the TOSH row rather
        # than on the whole listing: init is also in waitpid, and a
        # check that accepted any `block(child)` anywhere would pass
        # with the shell's own state reported wrongly.
        tosh_line = next((l for l in out.splitlines() if " tosh" in l), "")
        check("`ps` says the shell is waiting for its CHILD",
              "block(child)" in tosh_line, tosh_line.strip()[:90] or out.strip()[:90])
        # What a broken version would still pass: a state column that
        # printed a bare `block` for everything would satisfy any check
        # looking for the word. Every blocked row must NAME a reason.
        bare = [l.strip() for l in out.splitlines()
                if " block " in l or l.rstrip().endswith(" block")]
        check("...and no blocked process reports a bare `block`",
              not bare, "; ".join(bare)[:90])

        # --- THE EDITOR, AS A /bin PROGRAM -------------------------
        #
        # `edit` was a kernel builtin drawing with vga_putc(). It is a
        # ring-3 binary now that draws with ANSI escapes on fd 1, asks
        # SYS_TCGETWINSZ how big the screen is, and reads a raw fd 0 --
        # which is only possible because a terminal is an object with a
        # size and a termios (docs/tty-design.md).
        #
        # ASSERTED THROUGH THE FILESYSTEM, never through the screen: a
        # full-screen program's display is exactly the thing a screenshot
        # cannot check cheaply, and "the bytes reached the disk" is the
        # claim that matters. Read back by a completely different path
        # (the debug console's `sh cat`), so the editor claiming success
        # proves nothing on its own.
        print("the editor, as a /bin program")
        dbg.send("sh rm /edit_probe.txt")
        time.sleep(0.4)
        type_line(flow, "edit /edit_probe.txt")
        time.sleep(2.5)
        flow.session.send_text("hello")
        time.sleep(0.6)
        flow.session.send_key("f2")   # save
        time.sleep(1.5)
        flow.session.send_key("f3")   # exit
        time.sleep(1.5)
        out = dbg.send("sh cat /edit_probe.txt") or ""
        check("`edit` saves what was typed into it",
              "hello" in out, out.strip()[:60])

        # THE OTHER HALF: an editor that left the terminal in raw mode,
        # or never returned at all, would pass the check above and leave
        # a dead shell. So the shell must run something afterwards --
        # and F3 must have been what ended it, not a crash.
        dbg.send("sh rm /filetest.txt")
        time.sleep(0.4)
        type_line(flow, "file_test")
        time.sleep(2.5)
        check("...and the shell is still usable after it exits",
              "filetest.txt" in root_names(dbg))

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

        # A BUILTIN as the producer -- a builtin prints through the
        # shell's own sink, so this is the check that its output reaches
        # a pipe at all. `pwd` rather than `ls`, which is a /bin program
        # now; see the redirection check above.
        dbg.send("sh rm /pipe_ls.txt")
        time.sleep(0.4)
        type_line(flow, "pwd | catin > /pipe_ls.txt")
        time.sleep(3.0)
        out = dbg.send("sh cat /pipe_ls.txt") or ""
        check("a builtin can feed a pipeline", "/" in out, out.strip()[:60])

        # And a redirect that cannot be opened must NOT run the command.
        # `cat < missing` printing nothing is not enough -- a shell that
        # ran the command anyway would also print nothing.
        dbg.send("sh rm /redir_never.txt")
        time.sleep(0.4)
        type_line(flow, "hello < /no_such_file > /redir_never.txt")
        time.sleep(2.0)
        check("a failed redirect does not run the command",
              "redir_never.txt" not in root_names(dbg))

        # --- dmesg -T ---------------------------------------------------
        #
        # THE SPAN, NOT A PAIRING. Two dumps of a live log cannot be
        # lined up: pairing on message text is unsound (the "syscall:
        # exit()" line repeats dozens of times, so the Nth occurrence in
        # one matches the 1st in the other -- a 3 s error that was the
        # harness's own), pairing only on messages unique in both left 3
        # lines out of 30, and pairing positionally diverges as soon as
        # the log grows between the two reads. The elapsed time BETWEEN
        # the first and last line needs none of that, and it is the
        # quantity `-T` can actually get wrong.
        #
        # SPREAD IS LOAD-BEARING. Nearly every BOOT line lands in second
        # 0, so run at the end of this file, where the log spans the
        # minute of shell testing above -- a deliberate x3 skew passed
        # an earlier version of this check because every offset it saw
        # was 0, which is CLAUDE.md's "the fixture never reached the
        # branch".
        #
        # THROUGH THE HELPER, not a bare send: the log is thousands of
        # characters and DebugConsole's default read returns a truncated
        # buffer, which reads as the command being broken.
        raw = dmesg(dbg)
        abs_ = dmesg(dbg, cmd="sh dmesg -T")

        secs = [int(m.group(1)) for m in
                (re.match(r"\[(\d+)\.\d\d\] ", ln.strip()) for ln in raw.splitlines())
                if m]
        times = [calendar.timegm(tuple(int(m.group(i)) for i in range(1, 7)) + (0, 0, 0))
                 for m in (re.match(r"\[(\d{4})-(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d)\] ",
                                    ln.strip()) for ln in abs_.splitlines())
                 if m]
        check("dmesg -T stamps every line it prints",
              len(times) >= 10 and len(secs) >= 10,
              f"{len(secs)} monotonic, {len(times)} absolute")
        mono_span = (max(secs) - min(secs)) if secs else 0
        abs_span = (max(times) - min(times)) if times else 0
        check("...over a log that spans more than one second",
              mono_span >= 2, f"monotonic span {mono_span}s")
        # +/-1s: time() and the uptime division both truncate.
        check("and the elapsed time it reports matches the monotonic one",
              abs(abs_span - mono_span) <= 1,
              f"monotonic {mono_span}s vs absolute {abs_span}s")


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
