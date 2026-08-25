#!/usr/bin/env python3
"""tools/mkpart_test.py -- writes a synthetic legacy MBR or GPT
partition table onto the first few sectors of a raw disk image, for
testing kernel/drivers/partition.c's parser (the shell's `parttable`
command) against real, correctly-checksummed data.

toy-os's own disk.img is a GPT that toy-os BOOTS from now
(tools/seed_disk.py builds it: a BIOS boot partition, a FAT32 /boot and
the filesystem), so the synthetic tables below are no longer the only
way to get one -- what they are for is feeding the PARSER shapes the
seeder does not produce.

The writers preserve LBA 0 bytes 0-445 rather than zeroing the sector:
the table lives in bytes 446-511 and the signature in the last two, so
anything a boot sector might hold in front of it survives. Nothing in
this OS puts anything there (TFS3 never touches volume blocks 0-7, and
the format that kept its superblock in bytes 0-4 is gone), but writing
a partition table is not a licence to zero a sector this code does not
own -- kernel/drivers/partition.c's write_mbr() makes the same call.

**QEMU boot order:** a valid 0x55AA MBR signature makes SeaBIOS treat
an image as a bootable hard disk, and it does not check any further. So
a DATA disk carrying a table must be launched with `-boot order=d`
(CD-ROM first) or SeaBIOS jumps into filesystem bytes and hangs with no
serial output at all. toy-os's own disk.img is genuinely bootable now
(tools/install_grub.py puts GRUB on it) and boots with `order=c`; any
image these synthetic writers touch is not, and still needs `order=d`.

Usage:
    python3 tools/mkpart_test.py disk.img --mbr
    python3 tools/mkpart_test.py disk.img --gpt

--mbr writes a legacy 4-entry MBR (two populated entries, two empty
slots) with the standard 0x55AA signature, no GPT involved at all.

--gpt writes a protective MBR (single 0xEE entry spanning the disk) at
LBA 0, a real GPT header (with a correct CRC32) at LBA 1, and a small
partition entry array (two populated entries) starting at LBA 2 --
correct enough for kernel/drivers/partition.c's read-only parser to
validate the header and print both entries.
"""
import argparse
import os
import struct

SECTOR = 512

CRC32_POLY = 0xEDB88320


def crc32(data: bytes) -> int:
    """Same bit-at-a-time CRC-32 as kernel/drivers/partition.c's own
    crc32() -- kept in sync deliberately so a GPT header this script
    writes validates against the kernel's own check, not a
    library-computed one that might use a different convention."""
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            mask = -(crc & 1) & 0xFFFFFFFF
            crc = (crc >> 1) ^ (CRC32_POLY & mask)
    return (~crc) & 0xFFFFFFFF


def write_sector(f, lba: int, data: bytes):
    assert len(data) == SECTOR
    f.seek(lba * SECTOR)
    f.write(data)


def read_sector(f, lba: int) -> bytearray:
    f.seek(lba * SECTOR)
    data = f.read(SECTOR)
    return bytearray(data.ljust(SECTOR, b"\0"))


def mbr_entry(status: int, ptype: int, lba_start: int, num_sectors: int) -> bytes:
    # status(1) chs_start(3, unused/zero) type(1) chs_end(3, unused/zero)
    # lba_start(4 LE) num_sectors(4 LE) -- CHS fields are never read by
    # partition.c (LBA-only parser), zero is fine.
    return struct.pack("<B3sB3sII", status, b"\0\0\0", ptype, b"\0\0\0", lba_start, num_sectors)


def patch_mbr(buf: bytearray, entries: list):
    """Patches the partition-table region (bytes 446-511) of an
    already-read LBA 0 buffer in place -- leaves bytes 0-445 (TFS2's
    magic + whatever boot code area) untouched, see the module
    docstring's mount-preserving explanation."""
    for i in range(4):
        buf[446 + i * 16:446 + (i + 1) * 16] = entries[i] if i < len(entries) else b"\0" * 16
    buf[510] = 0x55
    buf[511] = 0xAA


def cmd_mbr(path):
    entries = [
        mbr_entry(0x80, 0x83, 2048, 204800),      # bootable, Linux, 100MB
        mbr_entry(0x00, 0x07, 206848, 1048576),   # NTFS/exFAT, 512MB
    ]
    with open(path, "r+b") as f:
        buf = read_sector(f, 0)
        patch_mbr(buf, entries)
        write_sector(f, 0, bytes(buf))
    print(f"wrote legacy MBR to {path} (2 entries: type 0x83 lba=2048, type 0x07 lba=206848)")
    print("(LBA 0 bytes 0-445 preserved -- see the module docstring)")


GPT_ENTRY_SIZE = 128
GPT_ENTRIES_LBA = 2
GPT_NUM_ENTRIES = 128  # spec-typical count -- most slots stay zeroed/unused

# Well-known GUIDs (Microsoft mixed-endian encoding, see partition.c's
# top comment) -- Linux filesystem and EFI System Partition.
def guid_bytes(text: str) -> bytes:
    parts = text.split("-")
    d1 = struct.pack("<I", int(parts[0], 16))
    d2 = struct.pack("<H", int(parts[1], 16))
    d3 = struct.pack("<H", int(parts[2], 16))
    d4 = bytes.fromhex(parts[3] + parts[4])
    return d1 + d2 + d3 + d4


LINUX_FS_GUID = guid_bytes("0FC63DAF-8483-4772-8E79-3D69D8477DE4")
ESP_GUID = guid_bytes("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")
DISK_GUID = guid_bytes("11111111-2222-3333-4444-555555555555")
PART1_GUID = guid_bytes("AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE")
PART2_GUID = guid_bytes("12345678-9ABC-DEF0-1122-334455667788")


def gpt_name(text: str) -> bytes:
    encoded = text.encode("utf-16-le")
    return encoded + b"\0" * (72 - len(encoded))


def gpt_entry(type_guid: bytes, unique_guid: bytes, lba_start: int, lba_end: int, name: str) -> bytes:
    return type_guid + unique_guid + struct.pack("<QQQ", lba_start, lba_end, 0) + gpt_name(name)


def cmd_gpt(path):
    entries = [
        gpt_entry(ESP_GUID, PART1_GUID, 34, 2047, "EFI System"),
        gpt_entry(LINUX_FS_GUID, PART2_GUID, 2048, 206847, "toy-os root"),
    ]
    entries_buf = b"".join(entries) + b"\0" * (GPT_ENTRY_SIZE * (GPT_NUM_ENTRIES - len(entries)))
    entries_crc = crc32(entries_buf)

    header_size = 92
    header = bytearray(SECTOR)
    header[0:8] = b"EFI PART"
    struct.pack_into("<I", header, 8, 0x00010000)   # revision 1.0
    struct.pack_into("<I", header, 12, header_size)
    struct.pack_into("<I", header, 16, 0)            # header_crc32 -- filled in below
    struct.pack_into("<Q", header, 24, 1)             # my_lba
    struct.pack_into("<Q", header, 32, 0)             # alternate_lba (no backup written -- test-only)
    struct.pack_into("<Q", header, 40, 34)            # first_usable_lba
    struct.pack_into("<Q", header, 48, 206814)        # last_usable_lba
    header[56:72] = DISK_GUID
    struct.pack_into("<Q", header, 72, GPT_ENTRIES_LBA)
    struct.pack_into("<I", header, 80, GPT_NUM_ENTRIES)
    struct.pack_into("<I", header, 84, GPT_ENTRY_SIZE)
    struct.pack_into("<I", header, 88, entries_crc)

    header_crc = crc32(bytes(header[:header_size]))
    struct.pack_into("<I", header, 16, header_crc)

    with open(path, "r+b") as f:
        mbr_buf = read_sector(f, 0)
        patch_mbr(mbr_buf, [mbr_entry(0x00, 0xEE, 1, 0xFFFFFFFF)])
        write_sector(f, 0, bytes(mbr_buf))
        # LBA 1 (GPT header) fully overwrites TFS2's journal header --
        # confirmed harmless, see the module docstring.
        write_sector(f, 1, bytes(header))
        for i in range(0, len(entries_buf), SECTOR):
            f.seek(GPT_ENTRIES_LBA * SECTOR + i)
            f.write(entries_buf[i:i + SECTOR])

    print(f"wrote GPT (protective MBR + header + {len(entries)} entries) to {path}")
    print(f"disk GUID {DISK_GUID.hex()} (mixed-endian raw bytes, not display order)")
    print("(LBA 0 bytes 0-445 preserved -- see the module docstring)")


# ---- real, usable tables ------------------------------------------------
#
# Everything above writes a SYNTHETIC table: fixed sizes, no backup GPT,
# enough for the parser to chew on and not enough to put a filesystem
# in. What follows writes a table you can actually boot toy-os from --
# aligned, sized to the image, and with GPT's backup structures present.
#
# Kept in this file rather than in a new one because it is the same
# encoder either way, and two partition-table writers in one tree is
# exactly the drift this repo keeps deleting.

ALIGN_LBA = 2048          # 1 MiB, what every modern tool aligns to
GPT_TAIL_SECTORS = 33     # backup entry array (32) + backup header (1)

# Microsoft Basic Data -- the same GUID kernel/drivers/partition.c
# writes, and for the same reason: it is the one "generic filesystem
# data" type that Windows, Linux and macOS all recognise.
BASIC_DATA_GUID = guid_bytes("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7")

# The two partition types that belong to the FIRMWARE rather than to
# this OS, and which a layout can therefore ask for by name:
#
#   bios  a BIOS boot partition -- where GRUB's core.img is EMBEDDED,
#         with no filesystem in it at all. GPT has no equivalent of
#         MBR's post-table gap, so this type is how a BIOS+GPT machine
#         gets one; tools/install_grub.py writes core.img into it.
#   esp   an EFI System Partition -- FAT32, and where /boot lives here.
#         Typed as an ESP even though this is a BIOS boot: it IS one in
#         shape (FAT32, /boot/kernel.bin) and naming it so is what lets
#         the kernel leave it alone (kernel/fs/vfs.c refuses to mount or
#         format a firmware partition), and what makes the roadmap's
#         UEFI track a matter of adding a second loader rather than a
#         second disk layout.
BIOS_BOOT_GUID = guid_bytes("21686148-6449-6E6F-744E-656564454649")

# MBR type bytes for the same two, for the --mbr path. There is no
# legacy equivalent of a BIOS boot partition and there does not need to
# be: an MBR leaves a GAP between the boot sector and the first
# partition, which is where core.img has always been embedded. GPT has
# no such gap, which is the whole reason its own type exists.
MBR_TYPE_DATA = 0x83
MBR_TYPE_ESP = 0xEF

PART_KINDS = {
    "data": (BASIC_DATA_GUID, MBR_TYPE_DATA, "toyos"),
    "bios": (BIOS_BOOT_GUID, MBR_TYPE_ESP, "BIOS boot"),
    "esp": (ESP_GUID, MBR_TYPE_ESP, "boot"),
}


def disk_sectors(path):
    return os.path.getsize(path) // SECTOR


def parse_layout(spec, usable):
    """'64M:esp,rest' -> [(sectors, kind), ...] laid out end to end.

    Sizes are bytes with a K/M/G suffix, or bare SECTORS -- the same
    rule /bin/mkpart follows, so a layout typed at one is valid at the
    other. Exactly one entry may be `rest`.

    A `:kind` suffix names the partition TYPE (see PART_KINDS); without
    one an entry is ordinary filesystem data, which is what /bin/mkpart
    writes and what every layout meant before the boot partition
    existed.
    """
    out, rest_at = [], None
    for item in spec.split(","):
        item = item.strip()
        kind = "data"
        if ":" in item:
            item, kind = item.rsplit(":", 1)
            item, kind = item.strip(), kind.strip()
            if kind not in PART_KINDS:
                raise SystemExit(f"mkpart_test: unknown partition kind {kind!r} "
                                 f"(known: {', '.join(sorted(PART_KINDS))})")
        if item == "rest":
            if rest_at is not None:
                raise SystemExit("mkpart_test: only one partition can be `rest`")
            rest_at = len(out)
            out.append((0, kind))
            continue
        mult = 1
        if item[-1:] in "KkMmGg":
            mult = {"k": 1024, "m": 1024 ** 2, "g": 1024 ** 3}[item[-1].lower()]
            item = item[:-1]
        if not item.isdigit():
            raise SystemExit(f"mkpart_test: cannot parse size {item!r}")
        out.append((int(item) if mult == 1 else int(item) * mult // SECTOR, kind))

    fixed = sum(n for i, (n, _k) in enumerate(out) if i != rest_at)
    if fixed > usable:
        raise SystemExit("mkpart_test: the layout does not fit on this image")
    if rest_at is not None:
        out[rest_at] = (usable - fixed, out[rest_at][1])
        if out[rest_at][0] == 0:
            raise SystemExit("mkpart_test: `rest` has nothing left over")
    return out


def plan(path, spec, gpt):
    """[(start_lba, sectors, kind), ...] for a layout on this image.

    The KIND is internal to the encoders below -- everything they RETURN
    is the (start, sectors) pair every caller has always taken, so a
    layout that names no kinds behaves exactly as it did.
    """
    total = disk_sectors(path)
    tail = GPT_TAIL_SECTORS if gpt else 0
    usable = total - ALIGN_LBA - tail
    if usable <= 0:
        raise SystemExit("mkpart_test: image too small to partition")
    at = ALIGN_LBA
    parts = []
    for n, kind in parse_layout(spec, usable):
        parts.append((at, n, kind))
        at += n
    return parts


def real_mbr(path, spec):
    planned = plan(path, spec, gpt=False)
    if len(planned) > 4:
        raise SystemExit("mkpart_test: an MBR holds at most 4 partitions -- use --gpt")
    entries = [mbr_entry(0x00, PART_KINDS[kind][1], start, n)
               for start, n, kind in planned]
    with open(path, "r+b") as f:
        buf = read_sector(f, 0)
        patch_mbr(buf, entries)
        write_sector(f, 0, bytes(buf))
    return [(start, n) for start, n, _kind in planned]


def real_gpt(path, spec):
    """A complete GPT: protective MBR, primary header + entries, and the
    BACKUP header + entries in the last 33 sectors.

    The backup is what makes this different from cmd_gpt() above, and it
    matters for more than tidiness: a real partition editor on another
    system reads the backup to cross-check the primary, and refuses or
    "repairs" a disk whose backup is missing.
    """
    total = disk_sectors(path)
    planned = plan(path, spec, gpt=True)
    parts = [(start, n) for start, n, _kind in planned]

    # Per-partition unique GUIDs derived from the index, not random:
    # `make iso` should produce a byte-identical image from the same
    # inputs, and a random GUID would make every rebuild differ. The
    # KERNEL randomises them (partition.c's guid_generate) because a
    # disk written on a running machine has no such reproducibility
    # requirement and collision-avoidance is the point there.
    entries = [gpt_entry(PART_KINDS[kind][0],
                         guid_bytes("70796F73-0000-4000-8000-%012X" % (i + 1)),
                         start, start + n - 1,
                         PART_KINDS[kind][2] if kind != "data" else "toyos%d" % (i + 1))
               for i, (start, n, kind) in enumerate(planned)]
    entries_buf = b"".join(entries) + b"\0" * (GPT_ENTRY_SIZE * (GPT_NUM_ENTRIES - len(entries)))
    entries_crc = crc32(entries_buf)

    entry_sectors = len(entries_buf) // SECTOR      # 32
    backup_hdr = total - 1
    backup_entries = backup_hdr - entry_sectors

    def header(self_lba, other_lba, entry_lba):
        h = bytearray(SECTOR)
        h[0:8] = b"EFI PART"
        struct.pack_into("<I", h, 8, 0x00010000)
        struct.pack_into("<I", h, 12, 92)
        struct.pack_into("<Q", h, 24, self_lba)
        struct.pack_into("<Q", h, 32, other_lba)
        struct.pack_into("<Q", h, 40, GPT_ENTRIES_LBA + entry_sectors)   # first usable = 34
        struct.pack_into("<Q", h, 48, backup_entries - 1)                # last usable
        h[56:72] = DISK_GUID
        struct.pack_into("<Q", h, 72, entry_lba)
        struct.pack_into("<I", h, 80, GPT_NUM_ENTRIES)
        struct.pack_into("<I", h, 84, GPT_ENTRY_SIZE)
        struct.pack_into("<I", h, 88, entries_crc)
        struct.pack_into("<I", h, 16, crc32(bytes(h[:92])))
        return bytes(h)

    with open(path, "r+b") as f:
        # Backup first, protective MBR LAST -- the same publish-last
        # order kernel/drivers/partition.c writes in, so a half-written
        # image reads as unpartitioned rather than as a broken table.
        for i in range(0, len(entries_buf), SECTOR):
            f.seek((backup_entries * SECTOR) + i)
            f.write(entries_buf[i:i + SECTOR])
        write_sector(f, backup_hdr, header(backup_hdr, 1, backup_entries))

        for i in range(0, len(entries_buf), SECTOR):
            f.seek(GPT_ENTRIES_LBA * SECTOR + i)
            f.write(entries_buf[i:i + SECTOR])
        write_sector(f, 1, header(1, backup_hdr, GPT_ENTRIES_LBA))

        mbr_buf = read_sector(f, 0)
        patch_mbr(mbr_buf, [mbr_entry(0x00, 0xEE, 1, min(total - 1, 0xFFFFFFFF))])
        write_sector(f, 0, bytes(mbr_buf))
    return parts


# ---- reading a table -----------------------------------------------
#
# The DECODER, beside the encoders above for the same reason the kernel
# keeps partition_read_table() beside partition_write_table(): the field
# offsets, the CRC and GPT's inclusive end LBA get one implementation
# each, and a round trip through both is a real check.
#
# WHY THE HOST NEEDS THIS AT ALL. Once disk.img carries a partition
# table, every host tool that reaches into the filesystem -- the seeder,
# check_layout, the fixture stagers -- has to know where the volume
# STARTS. Hardcoding 2048 in each of them is the pointer-somebody-must-
# maintain shape this repo keeps deleting, so they all ask volume_of().


def kind_of(type_guid: bytes, mbr_type: int) -> str:
    """'bios', 'esp' or 'data' for one partition's type.

    The only three this OS's own tools write, and the only distinction
    a host tool needs: a firmware partition (the first two) is NEVER
    where the filesystem is, which is what volume_of() below is asking.
    Anything unrecognised reads as 'data' -- a disk from elsewhere is
    not bound by what we would have written.
    """
    if type_guid is not None:
        for name, (guid, _mbr, _label) in PART_KINDS.items():
            if type_guid == guid:
                return name
        return "data"
    return "esp" if mbr_type == MBR_TYPE_ESP else "data"


def read_table(path, with_kind=False):
    """[(start_lba, sectors), ...] for the image's partitions.

    With `with_kind`, [(start_lba, sectors, kind)] instead -- see
    kind_of(). Empty when there is no table. Deliberately small: this
    answers "where are the partitions", not "what are their GUIDs" --
    the guest's `parttable` is what prints a table, and duplicating that
    here would be a second thing to keep true.
    """
    total = disk_sectors(path)
    with open(path, "rb") as f:
        mbr = read_sector(f, 0)
        if mbr[510] != 0x55 or mbr[511] != 0xAA:
            return []

        protective = any(mbr[446 + i * 16 + 4] == 0xEE for i in range(4))
        if not protective:
            out = []
            for i in range(4):
                e = mbr[446 + i * 16:446 + (i + 1) * 16]
                if e[4] == 0:
                    continue
                start, count = struct.unpack("<II", e[8:16])
                out.append((start, count, kind_of(None, e[4])) if with_kind
                           else (start, count))
            return out

        hdr = read_sector(f, 1)
        if bytes(hdr[0:8]) != b"EFI PART":
            return []
        header_size = struct.unpack_from("<I", hdr, 12)[0]
        stored = struct.unpack_from("<I", hdr, 16)[0]
        check = bytearray(hdr[:header_size])
        check[16:20] = b"\0\0\0\0"
        if crc32(bytes(check)) != stored:
            return []
        entry_lba, = struct.unpack_from("<Q", hdr, 72)
        n_entries, entry_size = struct.unpack_from("<II", hdr, 80)

        out = []
        per_sector = SECTOR // entry_size
        for s in range((n_entries + per_sector - 1) // per_sector):
            if entry_lba + s >= total:
                break
            sec = read_sector(f, entry_lba + s)
            for i in range(per_sector):
                e = sec[i * entry_size:(i + 1) * entry_size]
                if e[:16] == b"\0" * 16:
                    continue
                first, last = struct.unpack_from("<QQ", e, 32)
                if last < first:
                    continue
                # GPT's end is INCLUSIVE
                out.append((first, last - first + 1, kind_of(bytes(e[:16]), 0))
                           if with_kind else (first, last - first + 1))
        return out


def is_tfs3(path, base_lba):
    """Does a TFS3 superblock sit at this volume's block 8?

    The one implementation of that check on the host -- seed_disk.py
    asks this too. Any format version claims the image: picking one
    here would make a newer image look BLANK, and a blank image is the
    one that gets FORMATTED.
    """
    with open(path, "rb") as f:
        f.seek(base_lba * SECTOR + 8 * 4096)
        blk = f.read(5)
    return len(blk) == 5 and blk[:4] == b"TFS3" and 1 <= blk[4] <= 2


def volume_of(path):
    """(base_lba, sectors) for the FILESYSTEM volume on this image.

    The whole image when it is not partitioned -- so a caller passes the
    result straight to `tfs3_writer.py --at-lba N --sectors M` and works
    on either shape without asking which it has. That is the point: the
    flat image is still a supported layout (the live ISO's RAM image is
    one), and no tool should have to branch on it.

    **IT IS NOT "PARTITION 1" ANY MORE.** A bootable disk.img puts a
    BIOS boot partition and a FAT32 /boot in front of the filesystem
    (tools/install_grub.py), so the volume is found by LOOKING -- the
    partition carrying a TFS3 superblock wins, and failing that the
    first one that is not the firmware's. Answering "partition 1" here
    would hand every host tool the 1 MiB partition holding GRUB's
    core.img, and the seeder would format over the bootloader.
    """
    parts = read_table(path, with_kind=True)
    if not parts:
        return (0, disk_sectors(path))
    for start, count, _kind in parts:
        if is_tfs3(path, start):
            return (start, count)
    for start, count, kind in parts:
        if kind == "data":
            return (start, count)
    return parts[0][:2]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("disk", help="path to the raw disk image to patch (e.g. disk.img)")
    group = ap.add_mutually_exclusive_group(required=False)
    group.add_argument("--mbr", action="store_true", help="write a legacy MBR")
    group.add_argument("--gpt", action="store_true", help="write a protective-MBR + GPT")
    ap.add_argument("--print-volume", action="store_true",
                    help="print `<base_lba> <sectors>` for the filesystem "
                         "volume (partition 1, or the whole image) and exit -- "
                         "the shell-callable form of volume_of()")
    ap.add_argument("--layout", metavar="SIZE[,SIZE...]",
                    help="write a REAL, usable table with these partitions "
                         "(e.g. '64M,rest') instead of the synthetic fixed one. "
                         "Aligned to LBA 2048; --gpt also writes the backup "
                         "structures. This is what seed_disk.py --partition uses.")
    args = ap.parse_args()

    if args.print_volume:
        base, n = volume_of(args.disk)
        print(f"{base} {n}")
        return
    if not (args.mbr or args.gpt):
        ap.error("one of --mbr, --gpt or --print-volume is required")

    if args.layout:
        parts = real_gpt(args.disk, args.layout) if args.gpt else real_mbr(args.disk, args.layout)
        kind = "GPT" if args.gpt else "MBR"
        print(f"wrote a real {kind} to {args.disk}:")
        for i, (start, n) in enumerate(parts):
            print(f"  {i + 1}  LBA {start}  {n} sectors  {n * SECTOR / (1 << 20):.1f} MiB")
        return

    if args.mbr:
        cmd_mbr(args.disk)
    else:
        cmd_gpt(args.disk)


if __name__ == "__main__":
    main()
