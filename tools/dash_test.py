#!/usr/bin/env python3
"""Does the vendored dash actually BEHAVE like a shell?

WHY THIS EXISTS
---------------
`tools/dash_gap.py` proves dash compiles and links. That is a statement
about the C library, not about the shell: a program can build perfectly
and do nothing correct. `dash -c pwd` printing `/` was the whole of the
evidence before this, and it exercises the parser, one builtin and the
exit path -- none of the parts a shell is actually FOR.

This runs real constructs and compares real output: pipelines,
redirection, here-documents, functions, parameter expansion, arithmetic,
`case`, loops, command substitution, `test`, `trap` and exit status.

**THE SCRIPTS ARE FILES ON THE DISK, NOT `dash -c` STRINGS.** A shell
test is made of quotes, and passing them down through this script, then
`vm.py`, then `tosh`, then dash means four lexers each get a turn at
them -- which mangled every early attempt (a pipe arrived at `echo` as a
literal argument). Writing the script into the image with
`tools/tfs3_writer.py` and running `dash /path` removes all four.

**IT WRITES TO A COPY OF disk.img**, made sparsely, so the real image is
untouched and the run is repeatable from the same starting state.

WHAT IT DOES NOT COVER, said plainly: job control (needs a terminal this
harness does not give it) and interactive editing (dash has none -- see
docs/commands/dash.md).

One case here, `pipe-to-missing-command`, was excluded for a while
because it HUNG THE MACHINE -- a write to a pipe whose reader failed to
exec spun the producer at full CPU forever. It is an ordinary case now,
which is what a fix is supposed to look like.
"""
import argparse
import os
import subprocess
import sys
from harness import copy_disk  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

# (name, script, expected lines in order)
#
# **EVERY EXPECTATION IS SPECIFIC.** "some output appeared" is what a
# broken shell also produces; a numbered or computed value is not. The
# loop counts, the arithmetic result and the here-document's body are
# all things only a working implementation can emit.
CASES = [
    ("echo", "echo hello\n", ["hello"]),

    ("exit-status",
     "false\necho rc=$?\ntrue\necho rc=$?\n",
     ["rc=1", "rc=0"]),

    ("variables",
     "x=42\necho x=$x\ny=${x}0\necho y=$y\necho def=${nope:-fallback}\n",
     ["x=42", "y=420", "def=fallback"]),

    ("arithmetic",
     "a=7\nb=$((a * 6))\necho b=$b\necho c=$((10 / 3))\necho d=$((1 << 4))\n",
     ["b=42", "c=3", "d=16"]),

    ("for-loop",
     "for i in a b c; do echo item=$i; done\n",
     ["item=a", "item=b", "item=c"]),

    ("while-loop",
     "n=0\nwhile [ $n -lt 3 ]; do echo n=$n; n=$((n + 1)); done\n",
     ["n=0", "n=1", "n=2"]),

    ("if-else",
     "if [ 1 -eq 1 ]; then echo yes; else echo no; fi\n"
     "if [ 1 -eq 2 ]; then echo bad; else echo else-taken; fi\n",
     ["yes", "else-taken"]),

    ("case",
     "v=beta\ncase $v in\n  alpha) echo A ;;\n  beta) echo B ;;\n"
     "  *) echo other ;;\nesac\n",
     ["B"]),

    ("function",
     "greet() { echo greeting=$1; }\ngreet world\ngreet again\n",
     ["greeting=world", "greeting=again"]),

    # The reader EXISTS here. A pipeline into a command that does not is
    # the case in HANGS_THE_GUEST below.
    ("pipeline",
     "echo piped | cat\necho after-pipe\n",
     ["piped", "after-pipe"]),

    ("redirect-write",
     "echo written > /tmp/dt_a\ncat /tmp/dt_a\n",
     ["written"]),

    ("redirect-append",
     "echo one > /tmp/dt_b\necho two >> /tmp/dt_b\ncat /tmp/dt_b\n",
     ["one", "two"]),

    ("heredoc",
     "cat <<EOF\nline-one\nline-two\nEOF\n",
     ["line-one", "line-two"]),

    ("command-substitution",
     "d=$(echo substituted)\necho got=$d\n",
     ["got=substituted"]),

    ("test-builtin",
     "[ -d / ] && echo root-is-dir\n"
     "[ -f /bin/ls ] && echo ls-is-file\n"
     "[ -f /no/such ] || echo missing-detected\n",
     ["root-is-dir", "ls-is-file", "missing-detected"]),

    ("and-or",
     "true && echo and-ran\nfalse || echo or-ran\n",
     ["and-ran", "or-ran"]),

    ("positional",
     "set -- one two three\necho count=$#\necho first=$1\necho third=$3\n",
     ["count=3", "first=one", "third=three"]),

    ("trap-exit",
     "trap 'echo trapped' EXIT\necho before-exit\n",
     ["before-exit", "trapped"]),

    # **THIS ONE USED TO WEDGE THE MACHINE**, and it is in the table
    # rather than in a comment because that is what a fix looks like: a
    # write to a pipe whose reader failed to exec reported zero bytes,
    # libsys read that as a short write and looped, and the producer
    # spun at full CPU until the guest stopped answering. SIGPIPE and
    # EPIPE end it now, so the script survives its own typo.
    ("pipe-to-missing-command",
     "echo abc | no_such_command\necho survived\n",
     ["survived"]),
]



# Scripts run BY NAME rather than through `dash <path>` -- the `#!` line
# is the LOADER's job (kernel/proc/sched_fork.c's shebang_read), so this
# is the only case set here that tests the kernel rather than the shell.
# Each is (name, script, expected lines).
SHEBANG = [
    ("shebang-plain",
     "#!/bin/dash\necho shebang-ran\n",
     ["shebang-ran"]),

    # $0 is the SCRIPT, and the caller's arguments survive the rewrite --
    # the interpreter takes argv[0] and the script path is inserted after
    # it, so a script still sees its own name and its own args.
    ("shebang-args",
     "#!/bin/dash\necho name=$0\necho count=$#\necho first=$1\n",
     ["name=/dt_shebang-args.sh", "count=2", "first=alpha"]),

    # ONE optional argument, not a split word list -- every Unix does
    # exactly this, so `-e -x` would arrive as a single argument.
    ("shebang-interp-arg",
     "#!/bin/dash -u\necho interp-arg-ok\n",
     ["interp-arg-ok"]),
]


def volume(img):
    from mkpart_test import volume_of
    return volume_of(img)


def vm(args, *rest, timeout=90):
    cmd = [sys.executable, os.path.join(HERE, "vm.py"),
           "--instance", str(args.instance)] + list(rest)
    return subprocess.run(cmd, capture_output=True, text=True,
                          timeout=timeout, cwd=REPO)


def clean(text):
    """Drop the guest's own chatter, keeping the script's output."""
    out = []
    for ln in text.splitlines():
        s = ln.strip()
        if not s:
            continue
        # Kernel and service lines all carry a `prefix:` and are never
        # what a test script prints.
        if s.startswith(("syscall:", "wm:", "init:", "fs:", "ramfs:",
                         "vm:", "cursor:", "ugfx:", "win_surface:",
                         "--- ", "mouse:", "usb:", "net:", "block:")):
            continue
        if s.startswith("/$") or s.startswith("#"):
            continue
        out.append(s)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--instance", default="0",
                    help="VM slot (see tools/port_guard.py)")
    ap.add_argument("--keep", action="store_true",
                    help="leave the guest running afterwards")
    ap.add_argument("--only", help="run one case by name")
    ap.add_argument("--positive-control", action="store_true",
                    help="expect a WRONG line from one case and require "
                         "it to fail -- proves the comparison is live")
    args = ap.parse_args()

    img = os.path.join(REPO, "build", "dash_test.img")
    src = os.path.join(REPO, "disk.img")
    if not os.path.exists(src):
        print("dash_test: no disk.img -- run `make iso` first")
        return 2
    os.makedirs(os.path.dirname(img), exist_ok=True)
    # SPARSE, and the flag matters: disk.img is ~130 MB of data in a
    # 9 GB file, so a hole-filling copy costs 9 GB (CLAUDE.md).
    copy_disk(src, img)

    lba, sectors = volume(img)
    cases = [c for c in CASES if not args.only or c[0] == args.only]
    if not cases:
        print(f"dash_test: no case named {args.only!r}")
        return 2

    scratch = os.path.join(REPO, "build", "dash_scripts")
    os.makedirs(scratch, exist_ok=True)
    shebang = [c for c in SHEBANG if not args.only or c[0] == args.only]
    for name, body, _ in cases + shebang:
        p = os.path.join(scratch, name + ".sh")
        open(p, "w").write(body)
        r = subprocess.run([sys.executable, os.path.join(HERE, "tfs3_writer.py"),
                            "write", img, p, f"/dt_{name}.sh",
                            "--at-lba", str(lba), "--sectors", str(sectors)],
                           capture_output=True, text=True, cwd=REPO)
        if r.returncode != 0:
            print(f"dash_test: could not stage {name}: {r.stderr.strip()[:200]}")
            return 2

    r = subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                        "--instance", str(args.instance), "--disk", img, "start"],
                       capture_output=True, text=True, cwd=REPO)
    if r.returncode != 0:
        print("dash_test: guest did not start\n" + r.stdout[-800:] + r.stderr[-800:])
        return 2

    # **A WARM-UP COMMAND, AND IT IS NOT SUPERSTITION.** The FIRST exec
    # after boot came back empty every run while the same case passed
    # alone -- the guest is still emitting boot lines when it arrives,
    # and its output is not yet being collected. Without this the first
    # case in the table fails whatever it is, which reads as a bug in
    # that case rather than in the harness. Discarded on purpose.
    try:
        vm(args, "exec", "true", timeout=60)
    except subprocess.TimeoutExpired:
        pass

    passed = failed = 0
    try:
        for name, _body, want in cases:
            if args.positive_control and name == cases[0][0]:
                want = want + ["a line no shell would ever print"]
            try:
                out = vm(args, "exec", f"dash /dt_{name}.sh")
                got = clean(out.stdout + out.stderr)
            except subprocess.TimeoutExpired:
                print(f"  FAIL  {name:24s} guest stopped answering -- see "
                      f"HANGS_THE_GUEST in this file")
                failed += 1
                continue
            # SUBSEQUENCE, not equality: the guest interleaves its own
            # lines, and pinning the exact surrounding output would make
            # this fail on an unrelated kernel message.
            it = iter(got)
            ok = all(any(w == g for g in it) for w in want)
            if ok:
                print(f"  ok    {name}")
                passed += 1
            else:
                print(f"  FAIL  {name:24s} wanted {want}")
                print(f"        got {got[:12]}")
                failed += 1

        # BY NAME, with no interpreter on the command line: if the
        # loader does not read the `#!` line these fail as "not an ELF".
        # The BARE path, never `spawn <path>` -- the physical shell's
        # bare-name spawn WAITS, and `spawn` does not, so the child's
        # output is not collected and every case reads as empty.
        for name, _body, want in shebang:
            try:
                out = vm(args, "exec", f"/dt_{name}.sh alpha beta")
                got = clean(out.stdout + out.stderr)
            except subprocess.TimeoutExpired:
                print(f"  FAIL  {name:24s} guest stopped answering")
                failed += 1
                continue
            it = iter(got)
            if all(any(w == g for g in it) for w in want):
                print(f"  ok    {name}")
                passed += 1
            else:
                print(f"  FAIL  {name:24s} wanted {want}")
                print(f"        got {got[:12]}")
                failed += 1
    finally:
        if not args.keep:
            subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                            "--instance", str(args.instance), "stop"],
                           capture_output=True, text=True, cwd=REPO)

    print(f"\ndash_test: {passed}/{passed + failed} passed")
    if args.positive_control:
        if failed:
            print("positive control: ok -- the impossible line was not found")
            return 0
        print("positive control: FAILED -- a line no shell prints was "
              "reported as present, so this comparison proves nothing")
        return 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
