#!/usr/bin/env python3
"""Fail the build on a character COUNT used as a text WIDTH.

`ugfx_char_w()` is the WIDEST advance in the current face. On the
monospace faces this desktop used until the interface face became
Liberation Sans it was also every other advance, so `n * ugfx_char_w()`
measured a string correctly by accident. It does not any more: it is the
width of the widest possible string of that length, and a caller using
it clips its own labels, mis-centres them, or -- worst -- hit-tests a
click against a grid the glyphs are not on.

The fix is always one of the chokepoints in `userland/ui/ugfx.h`:
`ugfx_text_width()`, `ugfx_text_width_n()`, `ugfx_text_fit_chars()`,
`ugfx_text_index_at_x()`, or `ugfx_char_advance('0')` / `('n')` when
what is wanted is a RESERVATION in digits or in ordinary text rather
than a measurement of a string that exists.

WHAT IS FLAGGED, and why this discriminator and not another: an
arithmetic use of the cell width whose other operand is an IDENTIFIER.
A variable paired with the cell width is nearly always a count of
characters in real data -- a `strlen`, a `width_chars` field, a
`MAX_CHARS` budget -- which is exactly the bug. A LITERAL paired with it
is a layout unit ("half a character of padding", "a two-cell gutter"),
which `ugfx.h` explicitly permits and which there are ~76 of in this
tree. Demanding a waiver for each of those would make the check noise
and nobody would read it.

The honest limit of that rule: a literal count used as a text pitch
(`14 * ugfx_char_w()` for a filename column) reads as a layout unit and
is NOT flagged. Two of those existed and both are fixed; if a third
appears it will have to be found by eye.

Waive with a `text-measure-ok: <reason>` comment on the line or the line
above -- the shape `check_dispatch.py` and `check_widget_ops.py` use.
The legitimate reason is a monospace bracket: code inside
`ugfx_set_font(ugfx_font_mono(...))` really is on a fixed grid, and
dividing by the cell is the right answer there.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCAN_DIRS = ["userland"]

CELL_FNS = ("ugfx_char_w", "gfx_char_w")
WAIVER = "text-measure-ok:"

# `int cw = ugfx_char_w();` -- the alias that hides the pattern from a
# naive search for the call itself.
ALIAS_RE = re.compile(
    r"\b(?:int|const\s+int)\s+(\w+)\s*=\s*(?:%s)\s*\(\s*\)" % "|".join(CELL_FNS)
)

IDENT = r"[A-Za-z_]\w*(?:\s*\(\s*\))?"


def cell_terms(text):
    """Every spelling of the cell width in this SCOPE: the calls, plus
    any local aliased to one.

    Scoped per function by the caller, and that is load-bearing rather
    than tidy: a file with an `int w = ugfx_char_w()` in one function
    would otherwise make every `w` in every other function look like a
    cell width, which reported a callback's own parameter as a bug."""
    terms = [fn + r"\s*\(\s*\)" for fn in CELL_FNS]
    for name in sorted(set(ALIAS_RE.findall(text))):
        terms.append(r"\b" + re.escape(name) + r"\b")
    return terms


def functions(lines):
    """(start, end) line indices of each top-level block, using this
    codebase's convention that a function's closing brace is in column
    zero. Approximate on purpose: it only has to keep an alias from
    escaping into a neighbour."""
    spans, start = [], 0
    for i, line in enumerate(lines):
        if line.startswith("}"):
            spans.append((start, i))
            start = i + 1
    if start < len(lines):
        spans.append((start, len(lines) - 1))
    return spans


def patterns(text):
    """Multiplication or division pairing the cell width with a name."""
    out = []
    for term in cell_terms(text):
        # <ident> * cell   /   cell * <ident>   /   <ident> / cell
        out.append(re.compile(r"(?<![\w.])(%s)\s*\*\s*(?:%s)" % (IDENT, term)))
        out.append(re.compile(r"(?:%s)\s*\*\s*(%s)" % (term, IDENT)))
        out.append(re.compile(r"(?<![\w.])(%s)\s*/\s*(?:%s)" % (IDENT, term)))
    return out


def waived(lines, i):
    """A waiver on the line itself, or anywhere in the comment block
    directly above it -- a one-line reason is rarely enough to say WHY
    a grid is a grid, and a waiver that has to fit on one line gets
    written as `text-measure-ok: yes`."""
    if WAIVER in lines[i]:
        return True
    j = i - 1
    while j >= 0 and lines[j].lstrip().startswith("//"):
        if WAIVER in lines[j]:
            return True
        j -= 1
    return False


def is_literal(tok):
    return re.fullmatch(r"\d+|0[xX][0-9a-fA-F]+", tok.strip()) is not None


def scan(path):
    text = path.read_text(errors="replace")
    if not any(fn in text for fn in CELL_FNS):
        return []
    lines = text.splitlines()
    found = []
    for start, end in functions(lines):
        pats = patterns("\n".join(lines[start:end + 1]))
        for i in range(start, end + 1):
            line = lines[i]
            code = line.split("//")[0]
            if not code.strip():
                continue
            if waived(lines, i):
                continue
            hit = None
            for p in pats:
                for m in p.finditer(code):
                    tok = m.group(1).strip()
                    if is_literal(tok):
                        continue
                    # A cell width paired with itself is a ratio, not a count.
                    if any(fn in tok for fn in CELL_FNS):
                        continue
                    hit = tok
                    break
                if hit:
                    break
            if hit:
                found.append((i + 1, line.strip(), hit))
    return found


def main():
    argv = sys.argv[1:]
    control = "--positive-control" in argv

    files = []
    for d in SCAN_DIRS:
        files += sorted((ROOT / d).rglob("*.c"))
        files += sorted((ROOT / d).rglob("*.h"))

    bad = []
    for f in files:
        for line_no, line, tok in scan(f):
            bad.append((f.relative_to(ROOT), line_no, line, tok))

    if control:
        # The control the repo asks for: prove the matcher can fire at
        # all, by running it over a site of the exact shape it hunts.
        probe = "    int w = max_chars * ugfx_char_w();"
        hits = [p for p in patterns(probe) if p.search(probe)]
        if not hits:
            print("positive control FAILED: the matcher did not fire on")
            print("    " + probe)
            return 1
        print("positive control ok: matched `max_chars * ugfx_char_w()`")
        return 0

    if bad:
        print("A CHARACTER COUNT IS NOT A TEXT WIDTH -- %d site(s):\n" % len(bad))
        for path, line_no, line, tok in bad:
            print("  %s:%d" % (path, line_no))
            print("      %s" % line)
            print("      `%s` is a count; the cell width is the WIDEST advance.\n" % tok)
        print("Measure with ugfx_text_width()/_n()/_fit_chars()/_index_at_x(),")
        print("or reserve with ugfx_char_advance('0') for digits / ('n') for text.")
        print("If the code is inside an ugfx_font_mono() bracket it is a real")
        print("grid: waive it with a `%s <reason>` comment." % WAIVER)
        return 1

    print("text measurement ok (%d files scanned)" % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
