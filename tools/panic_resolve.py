#!/usr/bin/env python3
"""Resolve a pasted panic into function names and source lines.

Paste a panic block (from the serial log, or typed off a photograph of
the red box) on stdin or in a file, and this names every kernel address
in it -- the faulting RIP and every candidate in the stack scan -- with
file and line.

WHY IT EXISTS: a maintainer pasted a panic in a chat, and reading it
meant finding the boot log's relocation offset, subtracting it from the
RIP by hand, and running addr2line once per address. That is a
rederive-from-scratch cost paid every time something panics.

The kernel bakes a symbol table in now (tools/gen_syms.py), so a fresh
panic already NAMES its function -- but this stays useful for three
cases the in-kernel table cannot cover:

  * a panic from an OLDER build, or one captured before that landed;
  * the stack scan, where you want file and line for a dozen addresses
    at once rather than a name each;
  * a RING-3 crash, whose RIP belongs to a userland ELF -- pass
    --elf build/userland/gui/apps/notepad.elf and it resolves against
    that instead.

It finds the relocation delta the same way a reader would: the panic
prints one (`kernel relocated +0x...`), and so does the boot banner, so
paste either. With neither, addresses are assumed to be link-time
already and it says so rather than guessing silently.

    python3 tools/panic_resolve.py < panic.txt
    python3 tools/panic_resolve.py --delta 0x55800000 panic.txt
    python3 tools/panic_resolve.py --elf build/userland/tests/crash_test.elf p.txt
"""

import argparse
import re
import subprocess
import sys
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_ELF = os.path.join(REPO, "build", "kernel.bin")

# `kernel relocated +0x5ca00000` -- printed by both the boot banner and
# the panic itself.
RE_DELTA = re.compile(r"relocated \+?0x([0-9a-fA-F]+)")
# Any 0x... that could be an address. Deliberately greedy: the panic
# prints registers too, and a register holding a code address is often
# exactly what you want named.
RE_ADDR = re.compile(r"0x([0-9a-fA-F]{6,16})")
# `0.3.0-dev (0897fd1-dirty)` -- the panic prints its build id now, which
# is the only way to catch the failure mode that makes this tool
# DANGEROUS rather than merely unhelpful: resolving against a different
# build gives confident, plausible, wrong names. (Verified: the same
# address that was try_merge_next in the reported build is
# rtc_read_local a few commits later.)
RE_BUILD = re.compile(r"\(([0-9a-f]{7,12})(-dirty)?\)")
# `in usb_parse_config_interfaces+0xef` beside `RIP=0x2513743f`. The
# kernel resolves the faulting symbol itself (ksyms), so the pair is a
# SECOND source for the relocation delta -- and the only one when the
# panic was photographed off a screen that had already scrolled the
# `kernel relocated` line away, which is how a bare-metal panic usually
# arrives. Derived rather than asked for, because doing it by hand means
# an nm lookup and a subtraction at exactly the moment nobody wants one.
RE_SYMOFF = re.compile(r"\bin ([A-Za-z_][A-Za-z0-9_]*)\+0x([0-9a-fA-F]+)")
RE_RIP = re.compile(r"\bRIP=0x([0-9a-fA-F]+)")


def symbol_addr(elf, name):
    """A symbol's LINK-TIME address, or None. Text symbols only, local
    ('t') as well as global ('T') -- a static function panics too."""
    out = subprocess.run(["nm", elf], capture_output=True, text=True)
    if out.returncode != 0:
        return None
    for line in out.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in ("T", "t") and parts[2] == name:
            return int(parts[0], 16)
    return None


def delta_from_symbol(elf, text):
    """Recover the relocation delta from the panic's own symbol+offset.

    Returns (delta, explanation) or (None, None).
    """
    m_sym = RE_SYMOFF.search(text)
    m_rip = RE_RIP.search(text)
    if not (m_sym and m_rip):
        return None, None
    link = symbol_addr(elf, m_sym.group(1))
    if link is None:
        return None, None
    off = int(m_sym.group(2), 16)
    rip = int(m_rip.group(1), 16)
    delta = rip - (link + off)
    if delta < 0:
        return None, None
    return delta, (f"0x{rip:x} (RIP) - (0x{link:x} {m_sym.group(1)} + "
                   f"0x{off:x}) = 0x{delta:x}")


def addr2line(elf, addrs):
    """One addr2line call for every address -- it accepts a list."""
    if not addrs:
        return {}
    out = subprocess.run(
        ["addr2line", "-f", "-e", elf] + [f"0x{a:x}" for a in addrs],
        capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"panic_resolve: addr2line failed on {elf}:\n{out.stderr.strip()}")
    lines = out.stdout.splitlines()
    resolved = {}
    for i, a in enumerate(addrs):
        name = lines[2 * i] if 2 * i < len(lines) else "??"
        where = lines[2 * i + 1] if 2 * i + 1 < len(lines) else "??"
        resolved[a] = (name, where)
    return resolved


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("file", nargs="?", help="panic text; stdin if omitted")
    ap.add_argument("--elf", default=DEFAULT_ELF,
                    help=f"image to resolve against (default {DEFAULT_ELF})")
    ap.add_argument("--delta", help="relocation offset, if the text lacks one")
    args = ap.parse_args()

    text = open(args.file).read() if args.file else sys.stdin.read()
    if not text.strip():
        sys.exit("panic_resolve: nothing to read")

    if not os.path.exists(args.elf):
        sys.exit(f"panic_resolve: no such image: {args.elf}\n"
                  "Build it first, and make sure it is the SAME build the "
                  "panic came from -- a mismatched image resolves to "
                  "confidently wrong names.")

    # Build check first: a mismatch makes everything below wrong.
    want = RE_BUILD.search(text)
    if want:
        have = None
        vh = os.path.join(REPO, "kernel", "include", "api", "version.h")
        if os.path.exists(vh):
            m = re.search(r'TOYOS_BUILD_ID\s+"([0-9a-f]+)', open(vh).read())
            if m:
                have = m.group(1)
        if have and not have.startswith(want.group(1)) \
                and not want.group(1).startswith(have):
            print(f"panic_resolve: WARNING -- this panic is from build "
                  f"{want.group(1)}, but {os.path.relpath(args.elf, REPO)} "
                  f"is build {have}.")
            print("  Names below will be CONFIDENTLY WRONG. Check out that "
                  "commit and rebuild, or pass --elf pointing at its image.\n")
        elif have:
            print(f"panic_resolve: build {have} matches the panic.\n")

    delta = None
    if args.delta:
        delta = int(args.delta, 16 if args.delta.startswith("0x") else 10)
    else:
        m = RE_DELTA.search(text)
        if m:
            delta = int(m.group(1), 16)

    derived = None
    if delta is None:
        delta, derived = delta_from_symbol(args.elf, text)

    if delta is None:
        print("panic_resolve: no relocation offset found -- treating "
              "addresses as link-time.")
        print("  If this panic came from a relocated kernel (the normal "
              "case), pass --delta or include the boot line that says "
              "`kernel relocated +0x...`.\n")
        delta = 0
    elif derived:
        # Say HOW, so a reader can check the arithmetic rather than
        # trust it -- the wrong delta resolves to confident nonsense.
        print(f"panic_resolve: relocation delta 0x{delta:x}, derived from "
              f"the faulting symbol\n  {derived}\n")
    else:
        print(f"panic_resolve: relocation delta 0x{delta:x}\n")

    seen = []
    for m in RE_ADDR.finditer(text):
        a = int(m.group(1), 16)
        if a >= delta and (a - delta) not in seen:
            seen.append(a - delta)

    resolved = addr2line(args.elf, seen)

    # Report in the order the addresses appeared, annotating the original
    # lines -- reading a panic is easier when the answer sits next to the
    # question rather than in a separate table.
    printed = set()
    for line in text.splitlines():
        stripped = line.rstrip()
        if not stripped:
            continue
        print(stripped)
        for m in RE_ADDR.finditer(line):
            a = int(m.group(1), 16)
            link = a - delta
            if a < delta or link not in resolved:
                continue
            name, where = resolved[link]
            # Skip anything addr2line could not place in a source file:
            # a stack scan sweeps up data addresses and section bounds
            # too, and naming those adds noise to the one list a reader
            # is trying to read.
            if name in ("??", "") or where.startswith("??"):
                continue
            key = (stripped, link)
            if key in printed:
                continue
            printed.add(key)
            where = where.replace(REPO + "/", "")
            print(f"        -> {name}  ({where})")

    if not printed:
        print("\npanic_resolve: nothing resolved. Either the addresses are "
              "not from this image, or the delta is wrong.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
