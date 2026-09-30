#!/usr/bin/env python3
"""/bin/doc: the pages reach the machine, and the command finds them.

WHAT THIS ADDS OVER tools/umd_hostcheck.py
------------------------------------------
The host harness renders every page against the same umd.c and asserts
the wrapping, so the RENDERER is covered far better here than any guest
test could afford. What it cannot see is everything between the
repository and a person at a prompt:

  - that `make iso` actually seeded the pages (staging a file and
    getting it onto the image are two different things, and the second
    one has silently failed before -- see CLAUDE.md on seed/sync/)
  - that a page is found by NAME with no category given
  - that -c narrows, and refuses a category that does not exist
  - that -k and -K answer differently, which is the whole reason there
    are two of them
  - that a name that does not exist SUGGESTS rather than just refusing
  - that the output is plain when it is not going to a terminal

THE LOAD-BEARING CHECK IS THE PAIR OF SEARCHES. `-k` and `-K` would both
pass a test that only asked "did anything come back": the assertion that
distinguishes them is a word that appears in one page's BODY and in no
page's name, title or summary -- `-K` must find it and `-k` must not.
A `doc` with both flags wired to the same code passes everything else.

Everything runs with --no-pager, because at a `#` prompt fd 1 IS a
terminal and the real `doc` would (correctly) sit in its pager waiting
for a keypress.

WHAT THIS TOOL CANNOT SEE, said plainly rather than faked: the
"not a terminal" half -- that `doc ls > out.txt` dumps instead of
paging, and turns its colour off. The kernel debug console has no
redirection (it splits on spaces and hands `>` to the program as an
argument) and quotes do not survive it either, so `tosh -c` cannot be
reached from here; and ANSI escapes never arrive as text anyway, because
kernel/lib/ansi.c parses them before they reach COM1 -- the same reason
tools/ls_test.py cannot assert on colour. Both halves are two
`sys_isatty()` calls shared with /bin/less; reaching them would need a
`text`-target boot, which costs two boots and leaves a persisted setting
behind on the image for every later tool.

RUN IT ON DEMAND. It boots a VM; the renderer is covered far more
cheaply by tools/umd_hostcheck.py.
"""
import argparse
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import port_guard                                  # noqa: E402
import vm as vm_mod                                # noqa: E402 -- strip_terminal_codes()

_fail = 0


def check(name, ok, detail=""):
    global _fail
    print("  %s  %s%s" % ("ok  " if ok else "FAIL", name,
                          ("  -- " + detail) if detail and not ok else ""))
    if not ok:
        _fail += 1


class VM:
    """vm.py, driven as a subprocess -- the shape ls_test.py uses."""

    # A kernel or service log line shares this console; drop everything
    # tagged by somebody else, and keep `doc:`'s own diagnostics, which
    # match the same shape and are half of what is under test.
    NOISE = re.compile(r"^([a-z][a-z0-9_]*): ")

    def __init__(self, disk, instance):
        self.disk, self.instance = disk, instance

    def _cmd(self, *argv):
        cmd = [sys.executable, os.path.join(REPO, "tools", "vm.py")]
        if self.disk:
            cmd += ["--disk", self.disk]
        if self.instance is not None:
            cmd += ["--instance", str(self.instance)]
        return cmd + list(argv)

    def run(self, *argv):
        r = subprocess.run(self._cmd(*argv), cwd=REPO,
                           capture_output=True, text=True)
        return r.stdout + r.stderr

    def sh(self, cmd):
        out = self.run("exec", cmd)
        mine = cmd.split()[0] if cmd.split() else ""
        keep = []
        for l in out.splitlines():
            if l.startswith("sh "):
                continue        # vm.py echoing the line it typed
            m = self.NOISE.match(l)
            if m and m.group(1) != mine:
                continue
            keep.append(l)
        return "\n".join(keep)


# A word that is in the BODY of at least one page and in no page's name,
# title, category or first sentence. Found on the host so the assertion
# below cannot quietly stop discriminating when the pages change.
def body_only_word(candidates):
    import glob
    pages = {}
    for p in sorted(glob.glob(os.path.join(REPO, "docs", "commands", "*.md"))):
        if os.path.basename(p) == "README.md":
            continue
        pages[os.path.basename(p)[:-3]] = open(p, encoding="utf-8").read()
    for w in candidates:
        lw = w.lower()
        in_body = [n for n, t in pages.items() if lw in t.lower()]
        if not in_body:
            continue
        # The four fields -k looks at, approximated the same way.
        shallow = False
        for n, t in pages.items():
            head = "\n".join(t.splitlines()[:2])
            desc = ""
            inside = False
            for ln in t.splitlines():
                if ln.startswith("## Description"):
                    inside = True
                    continue
                if inside and ln.strip():
                    desc = ln
                    break
            if lw in (n + head + desc).lower():
                shallow = True
                break
        if not shallow:
            return w, in_body
    return None, []


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disk", help="disk image (default: vm.py's)")
    port_guard.add_instance_args(ap)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "doc_test")   # refuses a mixed pair

    vm = VM(args.disk, args.instance)
    print("doc_test: starting the guest")
    started = vm.run("start")
    if "ready" not in started:
        print(started)
        sys.exit("doc_test: the guest did not boot")

    try:
        # 1. The pages reached the image at all.
        listing = vm.sh("ls /usr/share/doc/cmd")
        check("pages are on the image", "ls.md" in listing and "doc.md" in listing,
              listing[:200])

        # 2. A page is found by NAME, with no category given.
        out = vm.sh("doc --no-pager --color=never ls")
        check("doc ls renders the ls page",
              "SYNOPSIS" in out and "ls [flags] [dir]" in out, out[:200])
        check("the heading is the page's own name", out.lstrip().startswith("LS"),
              out[:60])
        check("markup does not survive", "**" not in out and "`" not in out,
              out[:200])
        check("the output is ASCII", all(ord(c) < 128 for c in out))

        # 3. -c narrows, and an unknown category is refused BY NAME rather
        #    than by silently finding nothing.
        out = vm.sh("doc --no-pager --color=never -c cmd sync")
        check("-c cmd finds the page", "SYNOPSIS" in out, out[:120])
        out = vm.sh("doc --no-pager -c nope ls")
        check("an unknown category is refused", "no such category" in out, out[:120])
        check("...and the real ones are named", "cmd" in out, out[:120])

        # 4. THE TWO SEARCHES ANSWER DIFFERENTLY. See the module docstring.
        word, in_body = body_only_word(
            ["preemption", "rubber-band", "watchdog", "hysteresis",
             "quiescent", "backoff", "endianness"])
        if not word:
            check("a body-only word exists to test with", False,
                  "no candidate word is body-only any more")
        else:
            k = vm.sh("doc --no-pager -k %s" % word)
            K = vm.sh("doc --no-pager -K %s" % word)
            check("-k does not match on body text (%s)" % word,
                  "nothing matched" in k, k[:160])
            check("-K matches on body text (%s)" % word,
                  any(("cmd/%s:" % n) in K for n in in_body), K[:200])

        # -k still matches what it is for. The name is taken from the
        # pages this build ships, not written here: `timezone` was one
        # until the command went away (94a006bf) and the check went red.
        name = next(n[:-3] for n in sorted(os.listdir(os.path.join(REPO, "docs", "commands")))
                    if n.endswith(".md") and n != "README.md" and len(n) > 7)
        k = vm.sh("doc --no-pager -k %s" % name)
        check("-k matches a name", ("cmd/%s" % name) in k, k[:160])
        check("-k prints ONE line per page",
              all(len(l) < 200 for l in k.splitlines() if l.startswith("cmd/")),
              k[:200])

        # 5. A wrong name suggests, rather than only refusing.
        out = vm.sh("doc --no-pager lsdisk")
        check("a wrong name is reported", "no page for" in out, out[:120])
        check("...and near names are suggested",
              "cmd/lsdisplay" in out or "cmd/lsblk" in out, out[:200])

        # 6. Colour never arrives as TEXT. The debug console is a terminal,
        #    so the escapes arrive on the wire (read with --escapes; vm.py
        #    strips them otherwise) -- what this catches is a malformed
        #    one, which a real terminal would print as `[1;36m` beside
        #    every code span. The same check tools/ls_test.py makes.
        raw = vm.run("exec", "--escapes", "doc --no-pager --color=always ls")
        plain = vm_mod.strip_terminal_codes(raw)
        check("styling arrives as escape sequences", "\x1b[" in raw, raw[:120])
        check("styling never prints as literal text",
              "[36m" not in plain and "[1m" not in plain and "\x1b" not in plain,
              plain[:200])
        out = vm.sh("doc --no-pager --color=always ls")
        check("...and the page is still all there", "SYNOPSIS" in out, out[:120])

        # 7. -l lists every category's pages.
        out = vm.sh("doc --no-pager -l")
        n = len([l for l in out.splitlines() if l.startswith("cmd/")])
        check("-l lists the pages", n > 90, "%d lines" % n)
    finally:
        vm.run("stop")

    print("doc_test: %s" % ("PASS" if _fail == 0 else "%d FAILED" % _fail))
    return 1 if _fail else 0


if __name__ == "__main__":
    sys.exit(main())
