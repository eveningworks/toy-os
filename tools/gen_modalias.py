#!/usr/bin/env python3
"""Write /lib/modules/modules.alias from the built .ko files, and refuse
a module whose imports the kernel does not export.

    python3 tools/gen_modalias.py --exports kernel/core/kexports.c \
        --out build/modules/modules.alias build/modules/*.ko

WHAT IT WRITES -- one line per PCI match in each module's `.pci_drivers`
table, in the shape kernel/core/module.c parses at boot:

    pci 8086 100e * * * e1000
    pci * * 02 00 * some-class-driver

`*` is PCI_ANY. The kernel loads the module named on the first line
whose match takes a device no driver has claimed. This is Linux's
`modules.alias` (what `depmod` writes), derived from the object files
themselves so it cannot disagree with the .ko it ships beside.

WHAT IT CHECKS, and fails the build on:

  - an undefined symbol in a .ko that is not an EXPORT_SYMBOL() in
    kexports.c -- the load would be refused at runtime with the symbol
    named; this names it at build time instead. modules/unexported.c is
    the deliberate exception (its whole point is that refusal).
  - a relocation type in an allocatable section the loader does not
    handle (anything but R_X86_64_64 / PC32 / PLT32) -- the sign of a
    module compiled without -mcmodel=large -fno-pic.

The ELF parse is by hand (struct) so the build needs no pyelftools.
"""
import argparse
import os
import re
import struct
import sys

# Modules that import something unexported ON PURPOSE.
NEGATIVE = {"unexported"}

SHT_SYMTAB, SHT_STRTAB, SHT_RELA = 2, 3, 4
SHF_ALLOC = 2
SHN_UNDEF, SHN_ABS, SHN_COMMON = 0, 0xFFF1, 0xFFF2
STB_WEAK = 2
OK_RELOCS = {0, 1, 2, 4}   # NONE, 64, PC32, PLT32
PCI_ANY = 0xFFFF

# struct pci_driver (kernel/include/kernel/pci_driver.h): name@0,
# matches@8, nmatches@16, probe@24, remove@32, aligned(32) -> 64 bytes.
# struct pci_match: five u16, 10 bytes. Checked below by the field
# offsets the relocations land on, so a layout change fails loudly.
PCI_DRIVER_SIZE = 64
PCI_MATCH_SIZE = 10


class Elf:
    def __init__(self, path):
        self.path = path
        self.data = open(path, "rb").read()
        d = self.data
        if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 1:
            sys.exit(f"gen_modalias: {path}: not an ELF64 little-endian object")
        (e_type, e_machine, _v, _entry, _phoff, e_shoff, _flags, _ehsize,
         _phentsize, _phnum, e_shentsize, e_shnum, e_shstrndx) = \
            struct.unpack_from("<HHIQQQIHHHHHH", d, 16)
        if e_type != 1 or e_machine != 62:
            sys.exit(f"gen_modalias: {path}: not a relocatable x86-64 object")
        if e_shentsize != 64:
            sys.exit(f"gen_modalias: {path}: odd section header size")
        self.sh = []
        for i in range(e_shnum):
            self.sh.append(struct.unpack_from("<IIQQQQIIQQ", d, e_shoff + i * 64))
        names = self.sh[e_shstrndx]
        self.shstr = d[names[4]:names[4] + names[5]]
        self.by_name = {self.sec_name(i): i for i in range(e_shnum)}
        sym_idx = [i for i, s in enumerate(self.sh) if s[1] == SHT_SYMTAB]
        if len(sym_idx) != 1:
            sys.exit(f"gen_modalias: {path}: expected one symbol table")
        self.symtab = sym_idx[0]
        st = self.sh[self.symtab]
        self.strtab = self.sec_bytes(st[6])
        self.syms = []
        for i in range(st[5] // 24):
            (st_name, st_info, _other, st_shndx, st_value, st_size) = \
                struct.unpack_from("<IBBHQQ", d, st[4] + i * 24)
            self.syms.append((self.cstr(self.strtab, st_name), st_info, st_shndx, st_value))

    def sec_name(self, i):
        return self.cstr(self.shstr, self.sh[i][0])

    def sec_bytes(self, i):
        s = self.sh[i]
        return self.data[s[4]:s[4] + s[5]]

    @staticmethod
    def cstr(tab, off):
        end = tab.find(b"\0", off)
        return tab[off:end if end >= 0 else None].decode("utf-8", "replace")

    def relas(self, target_name):
        i = self.by_name.get(".rela" + target_name)
        if i is None:
            return []
        s = self.sh[i]
        out = []
        for k in range(s[5] // 24):
            r_offset, r_info, r_addend = struct.unpack_from("<QQq", self.data, s[4] + k * 24)
            out.append((r_offset, r_info & 0xFFFFFFFF, r_info >> 32, r_addend))
        return out


def undefined_symbols(elf):
    out = []
    for name, info, shndx, _v in elf.syms:
        if shndx == SHN_UNDEF and name and (info >> 4) != STB_WEAK:
            out.append(name)
        if shndx == SHN_COMMON:
            sys.exit(f"gen_modalias: {elf.path}: common symbol {name} -- build with -fno-common")
    return sorted(set(out))


def bad_relocs(elf):
    out = []
    for i, s in enumerate(elf.sh):
        if s[1] != SHT_RELA:
            continue
        target = s[7]
        if not (elf.sh[target][2] & SHF_ALLOC):
            continue
        for _off, rtype, _sym, _add in elf.relas(elf.sec_name(target)):
            if rtype not in OK_RELOCS:
                out.append((elf.sec_name(target), rtype))
    return out


def pci_matches(elf):
    """[(vendor, device, class, subclass, progif)] from .pci_drivers."""
    idx = elf.by_name.get(".pci_drivers")
    if idx is None:
        return []
    sec = elf.sec_bytes(idx)
    if len(sec) % PCI_DRIVER_SIZE:
        sys.exit(f"gen_modalias: {elf.path}: .pci_drivers is not whole entries "
                 f"of {PCI_DRIVER_SIZE} bytes -- struct pci_driver changed; update this tool")
    matches = []
    for r_offset, rtype, symi, addend in elf.relas(".pci_drivers"):
        if r_offset % PCI_DRIVER_SIZE != 8:      # the `matches` pointer
            continue
        if rtype != 1:
            sys.exit(f"gen_modalias: {elf.path}: .pci_drivers pointer is not R_X86_64_64")
        _name, _info, shndx, value = elf.syms[symi]
        entry = r_offset - 8
        (nmatches,) = struct.unpack_from("<i", sec, entry + 16)
        tbl = elf.sec_bytes(shndx)
        base = value + addend
        for k in range(nmatches):
            m = struct.unpack_from("<HHHHH", tbl, base + k * PCI_MATCH_SIZE)
            matches.append(m)
    return matches


def fmt(v):
    return "*" if v == PCI_ANY else f"{v:04x}" if v > 0xFF else f"{v:02x}"


def exports_in(path):
    text = open(path).read()
    return set(re.findall(r"^\s*EXPORT_SYMBOL\((\w+)\)", text, re.M))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exports", required=True, help="kernel/core/kexports.c")
    ap.add_argument("--out", required=True)
    ap.add_argument("kos", nargs="*")
    args = ap.parse_args()

    exports = exports_in(args.exports)
    lines = []
    failed = False
    for ko in sorted(args.kos):
        name = os.path.basename(ko)[:-3] if ko.endswith(".ko") else os.path.basename(ko)
        elf = Elf(ko)
        missing = [s for s in undefined_symbols(elf) if s not in exports]
        if missing and name not in NEGATIVE:
            print(f"gen_modalias: {ko}: imports not in {args.exports}: {', '.join(missing)}")
            failed = True
        for sec, rtype in bad_relocs(elf):
            print(f"gen_modalias: {ko}: relocation type {rtype} in {sec} -- the loader "
                  f"handles 64/PC32/PLT32 only (is it built -mcmodel=large -fno-pic?)")
            failed = True
        for m in pci_matches(elf):
            lines.append("pci " + " ".join(fmt(v) for v in m) + " " + name)
    if failed:
        return 1

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w") as f:
        f.write("# GENERATED by tools/gen_modalias.py from the .ko files beside it.\n")
        f.write("# pci <vendor> <device> <class> <subclass> <progif> <module>\n")
        for l in lines:
            f.write(l + "\n")
    print(f"gen_modalias: {len(args.kos)} module(s), {len(lines)} alias line(s) -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
