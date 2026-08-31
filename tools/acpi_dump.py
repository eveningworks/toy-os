#!/usr/bin/env python3
"""Pull an ACPI table out of a running guest, and prove it arrived intact.

WHY THIS IS NOT `acpi --dump SIG` ON ITS OWN. A real DSDT is tens of
kilobytes, which is thousands of hex bytes through the debug console --
and that console drops output when it is outrun (CLAUDE.md's rule about
a probe destroying the evidence it gathers). Measured: asking for a
whole 8605-byte DSDT in one command returned 8557 bytes, with the
missing lines scattered rather than truncated at the end, which is the
failure mode that looks like success.

So this reads it in RANGES and reassembles, and then checks two things
the guest cannot fake:

  - the length the table declares in its own header, and
  - the ACPI CHECKSUM: every table's bytes sum to zero mod 256.

A table that reassembles to the wrong bytes fails the second one. That
is the oracle -- without it, "I got some hex" and "I got the table" are
the same observation.

    python3 tools/vm.py start
    python3 tools/acpi_dump.py DSDT --out /tmp/dsdt.bin
    python3 tools/acpi_dump.py FACP --print
    python3 tools/vm.py stop

Run it against a guest that is already up; it does not launch one.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole  # noqa: E402

LINE = re.compile(r"^\s*([0-9a-f]{4,8})((?:\s+[0-9a-f]{2})+)\s*$")


def fetch(dbg, sig, at, length):
    """One range, as {offset: byte}. Sparse on purpose: a dropped line
    must show up as a HOLE rather than as a shift, because a shift
    reassembles into plausible-looking wrong bytes."""
    out = {}
    reply = dbg.send(f"sh acpi --dump {sig} --at {at} --len {length}") or ""
    for line in reply.splitlines():
        m = LINE.match(line)
        if not m:
            continue
        off = int(m.group(1), 16)
        for i, b in enumerate(bytes.fromhex("".join(m.group(2).split()))):
            out[off + i] = b
    return out


def declared_length(dbg, sig):
    reply = dbg.send(f"sh acpi --dump {sig} --at 0 --len 8") or ""
    m = re.search(rf"{re.escape(sig)}: (\d+) bytes", reply)
    return int(m.group(1)) if m else None


def pull(dbg, sig, chunk, retries):
    total = declared_length(dbg, sig)
    if total is None:
        return None, None, f"no table with signature {sig}"

    got = {}
    for base in range(0, total, chunk):
        want = min(chunk, total - base)
        for _ in range(retries + 1):
            got.update(fetch(dbg, sig, base, want))
            if all((base + i) in got for i in range(want)):
                break
        # A range that will not come back whole after its retries is left
        # holed; the report below names it rather than papering over it.

    missing = [i for i in range(total) if i not in got]
    if missing:
        return None, total, (f"{len(missing)} byte(s) never arrived, first at "
                             f"0x{missing[0]:x} -- try a smaller --chunk")
    return bytes(got[i] for i in range(total)), total, None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("signature", help="four characters, e.g. DSDT or FACP")
    ap.add_argument("--sock", default=".vm.serial")
    ap.add_argument("--out", default=None, help="write the raw bytes here")
    ap.add_argument("--print", dest="show", action="store_true",
                    help="print it as a C array, ready to paste into a KTEST")
    ap.add_argument("--chunk", type=int, default=512,
                    help="bytes per `acpi --dump` command (default 512)")
    ap.add_argument("--retries", type=int, default=2)
    args = ap.parse_args()

    sig = args.signature.upper()
    dbg = DebugConsole(args.sock)
    data, total, err = pull(dbg, sig, args.chunk, args.retries)
    if err:
        print(f"acpi_dump: {sig}: {err}")
        return 1

    # THE TWO CHECKS. The length is the table's own claim; the checksum
    # is arithmetic over every byte, so it fails on a reassembly that
    # merely looks right.
    print(f"acpi_dump: {sig}: {total} bytes, header says {total}")
    csum = sum(data) & 0xFF
    print(f"acpi_dump: checksum {'OK (sums to 0)' if csum == 0 else f'BAD ({csum})'}")
    if csum != 0:
        print("acpi_dump: the bytes are NOT this table -- do not use them")
        return 1

    if args.out:
        with open(args.out, "wb") as f:
            f.write(data)
        print(f"acpi_dump: wrote {args.out}")
    if args.show:
        print(f"static const uint8_t {sig}_AML[] = {{")
        for i in range(0, len(data), 12):
            print("    " + " ".join(f"0x{b:02x}," for b in data[i:i + 12]))
        print("};")
    return 0


if __name__ == "__main__":
    sys.exit(main())
