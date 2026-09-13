#!/usr/bin/env python3
"""tools/loc.py -- how big is this project, honestly.

Counts SOURCE LINES: no generated files, no comments, no blanks. Also
reports the with-comments figure beside it, because the gap between the
two is a real property of this codebase rather than noise -- roughly
two fifths of its lines are commentary, deliberately (see CLAUDE.md on
what earns a comment its length).

WHY THIS IS A TOOL AND NOT A `wc -l`. The answer depends entirely on
knowing which files are GENERATED, and that list is not guessable:
kernel/drivers/font_ttf.c alone is ~16,700 lines of baked glyph tables
from tools/genttf.py, which is a fifth of the whole tree and says
nothing about how much code anyone wrote. The list lives in GENERATED
below; anything tools/gen_*.py writes into the source tree belongs in
it.

The comment stripper is a real one -- it tracks string and character
literals, so a `"//"` inside a string is not mistaken for a comment.
That matters here: the path-joining and format-string code is full of
them.

    python3 tools/loc.py              # the table
    python3 tools/loc.py --by-dir     # one row per top-level directory
    python3 tools/loc.py --files 20   # the twenty largest files

Counts C, headers, NASM, GAS, Python and shell. Skips build/, .git/,
iso/ and data/ (binary artefacts and disk images).
"""
import argparse
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# WRITTEN BY A TOOL, NOT BY A PERSON. Keep this in step with tools/gen_*
# -- a generated file that is missing here inflates the count silently,
# which is the one failure mode this script has.
GENERATED = {
    "kernel/drivers/font_ttf.c",        # tools/genttf.py -- baked glyph tables
    "kernel/include/api/version.h",     # tools/gen_version.sh
    "kernel/include/api/build_date.h",  # tools/gen_version.sh
    "userland/tests/uimg_vectors.h",    # tools/gen_imgdata.py -- decoder vectors
    "userland/lib/usnd_mp3_tables.h",   # tools/gen_mp3_tables.py -- ISO's
                                        # Layer III tables, recovered and
                                        # cross-checked rather than written
}

SKIP_DIRS = {"build", ".git", "iso", "data"}


def strip_c(text):
    """Remove // and /* */ comments, respecting string and char literals.

    THE LITERAL HANDLING IS NOT OPTIONAL. A naive stripper turns
    `k_snprintf(p, n, "%s/%s", dir, name)` into a comment at the slash
    and eats the rest of the line, which under-counts exactly the files
    that do the most string work.
    """
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c in '"\'':
            q = c
            out.append(c)
            i += 1
            while i < n:
                if text[i] == '\\':
                    out.append(text[i:i + 2])
                    i += 2
                    continue
                out.append(text[i])
                if text[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if c == '/' and i + 1 < n:
            if text[i + 1] == '/':
                while i < n and text[i] != '\n':
                    i += 1
                continue
            if text[i + 1] == '*':
                i += 2
                while i + 1 < n and not (text[i] == '*' and text[i + 1] == '/'):
                    i += 1
                i += 2
                continue
        out.append(c)
        i += 1
    return "".join(out)


def strip_hash(text):
    """# comments, keeping a #! shebang -- it is a real line."""
    out = []
    for line in text.splitlines():
        out.append(line if line.startswith("#!") else line.split("#", 1)[0])
    return "\n".join(out)


def strip_semi(text):
    """NASM's ; comments."""
    return "\n".join(line.split(";", 1)[0] for line in text.splitlines())


KINDS = {
    ".c": ("C", strip_c), ".h": ("C", strip_c),
    ".asm": ("asm", strip_semi), ".S": ("asm", strip_c),
    ".py": ("Python", strip_hash), ".sh": ("shell", strip_hash),
}


def nonblank(text):
    return sum(1 for line in text.splitlines() if line.strip())


def walk():
    """(rel, kind, code, raw) for every counted file."""
    for dirpath, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [d for d in dirnames
                       if d not in SKIP_DIRS and not d.startswith(".")]
        for fn in sorted(filenames):
            rel = os.path.relpath(os.path.join(dirpath, fn), ROOT)
            if rel in GENERATED:
                continue
            kind_strip = KINDS.get(os.path.splitext(fn)[1])
            if not kind_strip:
                continue
            kind, strip = kind_strip
            try:
                text = open(os.path.join(dirpath, fn), errors="replace").read()
            except OSError:
                continue
            yield rel, kind, nonblank(strip(text)), nonblank(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--by-dir", action="store_true",
                    help="one row per top-level directory, not per language")
    ap.add_argument("--by-origin", action="store_true",
                    help="WHO WROTE IT: this repo, a vendored port, or the "
                         "test harness -- the split the other two views hide")
    ap.add_argument("--files", type=int, metavar="N",
                    help="also list the N largest files by code lines")
    args = ap.parse_args()

    rows = list(walk())

    # **VENDORED CODE IS NOT OURS, AND COUNTING IT AS SUCH INFLATES THE
    # NUMBER SILENTLY** -- the same failure the GENERATED list above
    # exists to prevent, from a different direction. userland/ports/
    # holds more lines than the entire OS written here (mbedtls alone is
    # ~116k), so "userland: 242,661" answered the question wrongly and
    # nothing said so.
    if args.by_origin:
        agg = {}
        for rel, _kind, code, raw in rows:
            if rel.startswith("tools" + os.sep):
                who = "tools (never shipped)"
            elif os.sep + "ports" + os.sep in os.sep + rel:
                who = "ports (vendored)"
            else:
                who = "written here"
            a = agg.setdefault(who, [0, 0, 0])
            a[0] += code
            a[1] += raw
            a[2] += 1
        print(f"{'origin':<26}{'code':>9}{'w/comments':>12}{'files':>7}")
        print("-" * 54)
        for who in ("written here", "ports (vendored)", "tools (never shipped)"):
            if who not in agg:
                continue
            c, r, n = agg[who]
            print(f"{who:<26}{c:>9,}{r:>12,}{n:>7}")
        print("-" * 54)
        own = agg.get("written here", [0])[0]
        print(f"\nthe OS as written here -- no ports, no generated files, "
              f"no comments: {own:,} lines")
        return

    groups = {}
    for rel, kind, code, raw in rows:
        top = rel.split(os.sep)[0]
        key = top if args.by_dir else (kind, top)
        g = groups.setdefault(key, [0, 0, 0])
        g[0] += code
        g[1] += raw
        g[2] += 1

    label = "area" if args.by_dir else "lang / area"
    print(f"{label:<26}{'code':>9}{'w/comments':>12}{'files':>7}")
    print("-" * 54)
    tot = [0, 0, 0]
    for key, (code, raw, nf) in sorted(groups.items(), key=lambda x: -x[1][0]):
        name = key if args.by_dir else f"{key[0]:<8} {key[1]}"
        print(f"{name:<26}{code:>9,}{raw:>12,}{nf:>7}")
        tot[0] += code
        tot[1] += raw
        tot[2] += nf
    print("-" * 54)
    print(f"{'TOTAL':<26}{tot[0]:>9,}{tot[1]:>12,}{tot[2]:>7}")

    # THE OS ITSELF, separated from the harness that tests it: tools/ is
    # a fifth of the tree and ships in nothing.
    os_code = sum(c for rel, k, c, _ in rows if not rel.startswith("tools" + os.sep))
    print(f"\nthe OS itself (everything outside tools/): {os_code:,} lines of code")
    if tot[0]:
        print(f"comments and blanks: {100 * (tot[1] - tot[0]) // tot[1]}% of all source lines")

    if args.files:
        print(f"\nlargest {args.files} files by code lines:")
        for rel, _, code, raw in sorted(rows, key=lambda r: -r[2])[:args.files]:
            print(f"  {code:>6,}  ({raw:>6,} w/comments)  {rel}")


if __name__ == "__main__":
    main()
