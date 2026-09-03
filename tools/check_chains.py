#!/usr/bin/env python3
"""tools/check_chains.py -- the write_dec/write_hex chains are FROZEN.

`vga_write("x="); vga_write_dec(x); vga_write(" y="); ...` is the idiom
`kfmt.h`'s `vga_printf()`/`klog_printf()` replaced, and it kept being
written beside them: 109 chains across 9 files when this was
added, 62 of them in apps/shell_sys.c. Converting them all is a noisy
diff in code the roadmap already wants moved to ring 3, so the decision
(2026-09-03) was to stop the GROWTH: a file may not gain a chain it did
not have, and a file with none may not start.

The baseline below is a RATCHET, not a fact to keep true: it only ever
goes down. When a file drops below its number this prints the new one
to paste in; going above fails the build. Comments are stripped first,
and the two files that IMPLEMENT the helpers are not counted.

Exits non-zero naming the file and the counts. Run by preflight.sh.
"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

BASELINE = {
    "apps/shell_fs.c": 3,
    "apps/shell_rescue.c": 1,
    "apps/shell_sys.c": 62,
    "kernel/core/kernel.c": 1,
    "kernel/core/multiboot.c": 5,
    "kernel/drivers/ata.c": 11,
    "kernel/drivers/pci.c": 1,
    "kernel/fs/tfs3.c": 14,
    "kernel/test/ktest.c": 11,
}

CALL = re.compile(r"\b(?:vga|klog)_write_(?:dec|hex)\s*\(")
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
IMPLEMENTS = ("vga.c", "klog.c", "kfmt_print.c")


def count(path):
    src = open(path, encoding="utf-8", errors="replace").read()
    return len(CALL.findall(COMMENT.sub("", src)))


def main():
    problems, lower = [], []
    files = sorted(glob.glob(os.path.join(REPO, "kernel", "**", "*.c"), recursive=True)
                   + glob.glob(os.path.join(REPO, "apps", "**", "*.c"), recursive=True))
    for path in files:
        rel = os.path.relpath(path, REPO)
        if rel.endswith("_test.c") or os.path.basename(rel) in IMPLEMENTS:
            continue
        n = count(path)
        b = BASELINE.get(rel, 0)
        if n > b:
            problems.append(f"{rel}: {n} write_dec/write_hex chain call(s), baseline {b}"
                            f" -- new number formatting is vga_printf()/klog_printf()")
        elif n < b:
            lower.append(f"{rel}: {n} (baseline {b}) -- lower the baseline")
    for l in lower:
        print("check_chains: " + l)
    if problems:
        print("check_chains: a write_dec/write_hex chain grew:")
        for p in problems:
            print("  " + p)
        return 1
    print(f"check_chains: ok -- no file gained a write_dec/write_hex chain"
          f" ({sum(BASELINE.values())} frozen across {len(BASELINE)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
