#!/usr/bin/env python3
"""Check userland/lib/umd.c against every page it will ever render, on the HOST.

WHY A HOST HARNESS BESIDE THE GUEST TEST. `tools/doc_test.py` runs
/bin/doc where it actually lives and proves the pages reach a terminal;
what it cannot afford is BREADTH. A Markdown renderer's bugs hide in the
one page that uses the one construct -- a table with an empty cell, a
numbered list, a paragraph whose first word is longer than the terminal
-- and there are 108 pages, each rendered at three widths. That is a
sweep, not a screenshot.

This compiles the SAME .c file with the host gcc and asserts invariants
that hold for every page:

  - the output is pure ASCII (the console draws no em dash)
  - every line fits the requested width, unless it was copied VERBATIM
    from the source (a code block, or a single word longer than the
    screen -- neither of which a wrapper may break)
  - no `**` markers survive into rendered prose
  - each page's Synopsis block survives verbatim, which is the text
    tools/check_docs.py already pins to the program's own cmd_usage()
  - umd_title / umd_field / umd_section_para answer for every page

  python3 tools/umd_hostcheck.py
  python3 tools/umd_hostcheck.py --cols 40 --file docs/commands/ls.md
  python3 tools/umd_hostcheck.py --positive-control

**A CLEAN RUN PROVES NOTHING UNTIL --positive-control HAS GONE RED.** It
renders every page with the wrap disabled (cols enormous) and checks the
result against 80, so the width assertion must fail. If that comes back
green the harness is measuring nothing.

Exit status is non-zero on any violation. Needs gcc.
"""
import argparse
import glob
import os
import subprocess
import sys
import tempfile
import hostcheck  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DRIVER = r"""
#include "lib/umd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// render <file> <cols> <color> | title <file> | field <file> <name>
//                               | section <file> <name> | plain <file>
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    FILE *f = fopen(argv[2], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = malloc((size_t)n + 1);
    if (!src || fread(src, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);

    // Generous on purpose: the real callers size this for what they
    // will PRINT (doc's summary is 512), and a check that shared
    // that bound would report a truncated paragraph as lost words.
    static char small[8192];
    if (strcmp(argv[1], "title") == 0) {
        umd_title(src, (int)n, small, sizeof small);
        fputs(small, stdout);
        return 0;
    }
    if (strcmp(argv[1], "plain") == 0) {
        umd_plain(src, (int)n, small, sizeof small);
        fputs(small, stdout);
        return 0;
    }
    if (strcmp(argv[1], "field") == 0 && argc > 3) {
        umd_field(src, (int)n, argv[3], small, sizeof small);
        fputs(small, stdout);
        return 0;
    }
    if (strcmp(argv[1], "section") == 0 && argc > 3) {
        umd_section_para(src, (int)n, argv[3], small, sizeof small);
        fputs(small, stdout);
        return 0;
    }
    if (strcmp(argv[1], "render") != 0 || argc < 5) return 2;

    struct umd_opts o;
    o.cols = atoi(argv[3]);
    o.color = atoi(argv[4]);
    o.indent = 3;
    int cap = (int)n * 6 + 8192;
    char *buf = malloc((size_t)cap);
    struct umd_out out = { buf, cap, 0, 0 };
    umd_render(src, (int)n, &o, &out);
    if (out.overflow) { fprintf(stderr, "OVERFLOW\n"); return 3; }
    fwrite(buf, 1, (size_t)out.len, stdout);
    return 0;
}
"""


def build(tmp):
    drv = hostcheck.write(tmp, "umd_host.c", DRIVER)
    return hostcheck.compile(tmp, "umd_host", [drv, os.path.join(ROOT, "userland", "lib", "umd.c")],
                             includes=[os.path.join(ROOT, "userland")], tool="umd_hostcheck")


def run(exe, *args):
    r = subprocess.run([exe, *[str(a) for a in args]],
                       capture_output=True)
    if r.returncode != 0:
        raise RuntimeError("driver failed (%d): %s"
                           % (r.returncode, r.stderr.decode(errors="replace")))
    return r.stdout.decode("utf-8", errors="replace")


def squeeze(s):
    """Only the letters and digits, lower-cased.

    A rendered line is compared against its source this way because the
    renderer legitimately CHANGES the punctuation on the way through: it
    drops backticks and asterisks and transliterates an em dash to `--`.
    Comparing the raw text would report every one of those as a wrapping
    failure, which is the opposite of what this check is for.
    """
    return "".join(c for c in s.lower() if c.isalnum())


def verbatim_source_lines(text):
    """Every source line, squeezed -- what a rendered line may be a copy of."""
    return {squeeze(ln) for ln in text.splitlines() if ln.strip()}


def check_page(exe, path, cols, problems, assert_cols=None):
    name = os.path.relpath(path, ROOT)
    src = open(path, encoding="utf-8").read()
    limit = assert_cols if assert_cols is not None else cols
    out = run(exe, "render", path, cols, 0)

    for b in out:
        if ord(b) > 0x7E and b != "\n":
            problems.append("%s: non-ASCII %r in output" % (name, b))
            break

    srclines = verbatim_source_lines(src)

    def copied(ln):
        """Was this output line carried through verbatim rather than wrapped?"""
        sq = squeeze(ln)
        return sq != "" and any(sq in s for s in srclines)

    for ln in out.splitlines():
        # A line may exceed the width only by being COPIED rather than
        # wrapped: a code block, or one word longer than the screen --
        # neither of which a wrapper is allowed to break.
        if len(ln) > limit and not copied(ln):
            problems.append("%s @%d: line of %d columns: %r"
                            % (name, limit, len(ln), ln[:90]))
        # A code block may legitimately CONTAIN `**` -- about.md shows a
        # warning banner that is literally asterisked -- so this asks
        # only of lines the renderer wrapped.
        if "**" in ln and not copied(ln):
            problems.append("%s: bold markers survived rendering: %r"
                            % (name, ln[:60]))

    # The Synopsis block -- the text check_docs.py pins to cmd_usage().
    syn = []
    inside = False
    for ln in src.splitlines():
        if ln.startswith("## Synopsis"):
            inside = True
            continue
        if inside and ln.startswith("## "):
            break
        if inside and ln.startswith("    "):
            syn.append(ln[4:].strip())
    for s in syn:
        if s and s not in out:
            problems.append("%s: synopsis line missing from output: %r" % (name, s))
            break


def check_fields(exe, path, problems):
    name = os.path.relpath(path, ROOT)
    src = open(path, encoding="utf-8").read()
    title = run(exe, "title", path).strip()
    if not title:
        problems.append("%s: no title" % name)
    cat = run(exe, "field", path, "Category").strip()
    if not cat:
        problems.append("%s: no Category field" % name)
    elif squeeze(cat) not in squeeze(src):
        problems.append("%s: Category %r is not in the page" % (name, cat))
    para = run(exe, "section", path, "Description").strip()
    if not para:
        problems.append("%s: empty Description section" % name)
    else:
        # THE WORD COUNT MUST SURVIVE. A line break between two source
        # lines is whitespace, and a reader that feeds the lines without
        # it joins the last word of one to the first of the next --
        # "for one character: itscoverage map". Squeezed comparison
        # cannot see that (removing spaces hides exactly the missing
        # space), so this counts tokens instead. `doc -k` is where it
        # shows, because a summary is the one place a paragraph is
        # rebuilt from several lines.
        src_para = []
        inside = False
        for ln in src.splitlines():
            if ln.startswith("## Description"):
                inside = True
                continue
            if not inside:
                continue
            if not ln.strip() and src_para:
                break
            if ln.strip():
                src_para.append(ln)
        want = len(" ".join(src_para).split())
        got = len(para.split())
        if want and got < want:
            problems.append("%s: Description lost %d word breaks (%d of %d)"
                            % (name, want - got, got, want))

    # AN ODD NUMBER OF BACKTICKS IN THE LEAD PARAGRAPH means a code span
    # that never closes, so everything after it renders as code and its
    # `**bold**` markers survive. Six pages opened that way -- each the
    # tail of a table cell left behind when the pages were converted out
    # of one big table -- and `doc -k` was what made it visible, because
    # a summary is the one place that paragraph is read alone.
    lead = ""
    inside = False
    for ln in src.splitlines():
        if ln.startswith("## Description"):
            inside = True
            continue
        if inside and ln.strip():
            lead = ln
            break
    if lead.count("`") % 2:
        problems.append("%s: unclosed code span in the lead paragraph: %r"
                        % (name, lead[:60]))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--file", action="append",
                    help="check only these files (default: every command page)")
    ap.add_argument("--cols", type=int, action="append",
                    help="widths to render at (default: 40, 80, 132)")
    ap.add_argument("--positive-control", action="store_true",
                    help="render unwrapped and require the width check to FAIL")
    args = ap.parse_args()

    files = args.file or sorted(
        p for p in glob.glob(os.path.join(ROOT, "docs", "commands", "*.md"))
        if os.path.basename(p) != "README.md")
    if not files:
        sys.exit("umd_hostcheck: no pages found")
    widths = args.cols or [40, 80, 132]

    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        problems = []

        if args.positive_control:
            # Wrapping disabled: every page of prose must now overflow 80.
            for p in files:
                check_page(exe, p, 100000, problems, assert_cols=80)
            if problems:
                print("positive control: %d violations -- the check can fail"
                      % len(problems))
                return 0
            print("POSITIVE CONTROL FAILED: unwrapped output passed the "
                  "width check, so the check is measuring nothing")
            return 1

        for p in files:
            for w in widths:
                check_page(exe, p, w, problems)
            check_fields(exe, p, problems)

    if problems:
        for p in problems[:40]:
            print("FAIL " + p)
        if len(problems) > 40:
            print("... and %d more" % (len(problems) - 40))
        print("umd_hostcheck: %d problems over %d pages" % (len(problems), len(files)))
        return 1
    print("umd_hostcheck: %d pages x %s columns -- clean"
          % (len(files), "/".join(str(w) for w in widths)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
