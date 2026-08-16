#!/usr/bin/env python3
"""Build the kernel's own relocation table, for kernel ASLR (Milestone 2).

The kernel is linked at a fixed base (1M, see linker.ld). To let it run
from a different physical address, every ABSOLUTE reference in the image
has to be adjusted by however far it moved. This tool finds those
references and emits them as a table the kernel carries and applies to
itself at boot (kernel/arch/x86_64/reloc.c).

Why this works at all here: the low 4GiB is identity-mapped, VA == PA,
so moving the image keeps it mapped and every PC-RELATIVE reference
survives untouched. Only the absolute ones need help. Measured on this
kernel:

    PC32 + PLT32   ~11,900   relative -- no fixup
    32S             ~5,400   absolute -- fixup
    64              ~1,800   absolute -- fixup
    32                   9   absolute -- fixup

so roughly 7,200 entries, about 29KB of table. This is the shape Linux
uses for CONFIG_RELOCATABLE -- a build-time relocs tool over
`ld --emit-relocs`, not a PIE link.

USAGE
    genrelocs.py <kernel.elf> --out-c <krelocs.c>   generate the table
    genrelocs.py <kernel.elf> --verify <krelocs.c> check a built image

ENTRY FORMAT
    One uint32 per fixup: bit 31 = the patch is 8 bytes wide (an
    R_X86_64_64) rather than 4; bits 0..30 = the location's link-time
    address. 31 bits is not a limitation being papered over -- the
    Makefile passes -mcmodel=kernel, which makes every absolute
    reference a 32-bit sign-extended immediate and therefore pins the
    kernel's base to the low 2GiB regardless. A higher-half kernel is a
    different and much larger change; see docs/roadmap.md.

THE CHECK THAT MATTERS
    --verify re-extracts the relocations from the FINAL image and
    compares them to the table that image is carrying. A table that
    disagrees with its own image is the failure mode that ends in a
    machine that does not boot, with no clue as to why, so it is worth
    a build step rather than a comment. It runs in preflight.sh and CI.
"""

import argparse
import struct
import sys

# ELF constants, kept local rather than depending on pyelftools -- every
# other host-side tool in this repo is pure stdlib for the same reason.
SHT_RELA = 4
SHF_ALLOC = 0x2

R_X86_64_64 = 1
R_X86_64_32 = 10
R_X86_64_32S = 11

# The types that need a fixup, mapped to their patch width in bytes.
# Everything else in this kernel's output is PC-relative (PC32, PLT32)
# and survives a move untouched -- that is the whole reason the table is
# 7k entries rather than 19k.
FIXUP_TYPES = {R_X86_64_64: 8, R_X86_64_32: 4, R_X86_64_32S: 4}

RELOC_WIDE = 1 << 31        # entry bit 31: this patch is 8 bytes wide
RELOC_ADDR_MASK = 0x7FFFFFFF


class Elf64:
    """Just enough ELF64 to walk section headers and .rela sections."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        if self.data[:4] != b"\x7fELF":
            raise SystemExit(f"{path}: not an ELF file")
        if self.data[4] != 2:
            raise SystemExit(f"{path}: not ELF64")

        e_shoff, = struct.unpack_from("<Q", self.data, 0x28)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", self.data, 0x3A)

        self.sections = []
        for i in range(e_shnum):
            off = e_shoff + i * e_shentsize
            (name, stype, flags, addr, offset, size,
             link, info, align, entsize) = struct.unpack_from("<IIQQQQIIQQ", self.data, off)
            self.sections.append({
                "name_off": name, "type": stype, "flags": flags, "addr": addr,
                "offset": offset, "size": size, "link": link, "info": info,
                "entsize": entsize,
            })

        strtab = self.sections[e_shstrndx]
        for s in self.sections:
            start = strtab["offset"] + s["name_off"]
            end = self.data.index(b"\0", start)
            s["name"] = self.data[start:end].decode()

    def section(self, name):
        for s in self.sections:
            if s["name"] == name:
                return s
        return None


def extract(path):
    """Every absolute reference in the image's allocated sections.

    Returns a sorted list of (link_address, width). Skipping relocations
    whose TARGET section is not SHF_ALLOC is what keeps debug info out:
    .debug_* carries thousands of R_X86_64_64s that are never in memory,
    and patching them at boot would mean writing to addresses the image
    does not occupy.
    """
    elf = Elf64(path)
    out = []
    skipped_nonalloc = 0

    for s in elf.sections:
        if s["type"] != SHT_RELA:
            continue
        target = elf.sections[s["info"]] if s["info"] < len(elf.sections) else None
        if target is None:
            continue
        if not (target["flags"] & SHF_ALLOC):
            skipped_nonalloc += s["size"] // 24
            continue

        for off in range(s["offset"], s["offset"] + s["size"], 24):
            r_offset, r_info = struct.unpack_from("<QQ", elf.data, off)
            rtype = r_info & 0xFFFFFFFF
            width = FIXUP_TYPES.get(rtype)
            if width is None:
                continue
            if r_offset > RELOC_ADDR_MASK:
                raise SystemExit(
                    f"{path}: fixup at 0x{r_offset:x} is above 2GiB, which "
                    f"-mcmodel=kernel cannot address -- see this tool's header")
            out.append((r_offset, width))

    out.sort()
    return out, skipped_nonalloc


def encode(entries):
    return [addr | (RELOC_WIDE if width == 8 else 0) for addr, width in entries]


def write_c(path, words, src):
    with open(path, "w") as f:
        f.write("// GENERATED by tools/genrelocs.py -- do not edit.\n")
        f.write(f"// Source image: {src}\n")
        f.write("//\n")
        f.write("// One uint32 per absolute reference in the kernel image: bit 31\n")
        f.write("// means an 8-byte patch, the rest is the link-time address.\n")
        f.write("// kernel/arch/x86_64/reloc.c walks this; linker.ld places it\n")
        f.write("// after .data on purpose, so its presence cannot move any\n")
        f.write("// address it records.\n")
        f.write("#include <stdint.h>\n\n")
        f.write('__attribute__((used, section(".krelocs")))\n')
        f.write("const uint32_t kernel_relocs[] = {\n")
        for i in range(0, len(words), 8):
            row = ", ".join(f"0x{w:08x}" for w in words[i:i + 8])
            f.write(f"    {row},\n")
        f.write("};\n")


def read_c(path):
    """Reads back a generated table. Deliberately parses the emitted
    literals rather than the built object, so --verify compares the
    table's CONTENT and not merely its size."""
    words = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith("0x"):
                continue
            for tok in line.rstrip(",").split(","):
                tok = tok.strip()
                if tok.startswith("0x"):
                    words.append(int(tok, 16))
    return words


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf", help="a kernel image linked with --emit-relocs")
    ap.add_argument("--out-c", help="write the table as a C file")
    ap.add_argument("--verify", metavar="TABLE_C",
                    help="check that TABLE_C matches the relocations in ELF")
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args()

    entries, skipped = extract(args.elf)
    words = encode(entries)
    wide = sum(1 for _, w in entries if w == 8)

    if args.out_c:
        write_c(args.out_c, words, args.elf)
        if not args.quiet:
            print(f"genrelocs: {len(words)} fixups "
                  f"({wide} 64-bit, {len(words) - wide} 32-bit), "
                  f"{len(words) * 4} bytes of table -> {args.out_c}")
            print(f"genrelocs: skipped {skipped} relocations in non-allocated sections")

    if args.verify:
        have = read_c(args.verify)
        if have == words:
            if not args.quiet:
                print(f"genrelocs: OK -- {args.verify} matches {args.elf} "
                      f"({len(words)} fixups)")
            return 0
        # Report the shape of the disagreement, not just that there is
        # one: a size mismatch and a content mismatch have completely
        # different causes (a section moved vs. the table was stale).
        print(f"genrelocs: MISMATCH between {args.verify} and {args.elf}",
              file=sys.stderr)
        print(f"  table has {len(have)} entries, image needs {len(words)}",
              file=sys.stderr)
        if len(have) == len(words):
            bad = [i for i, (a, b) in enumerate(zip(have, words)) if a != b]
            print(f"  {len(bad)} entries differ; first at index {bad[0]}: "
                  f"table 0x{have[bad[0]]:08x} vs image 0x{words[bad[0]]:08x}",
                  file=sys.stderr)
            print("  a same-size, different-content mismatch means a section "
                  "MOVED -- check linker.ld's .krelocs placement", file=sys.stderr)
        return 1

    if not args.out_c and not args.verify:
        print(f"{len(words)} fixups ({wide} 64-bit, {len(words) - wide} 32-bit), "
              f"{len(words) * 4} bytes of table")
    return 0


if __name__ == "__main__":
    sys.exit(main())
