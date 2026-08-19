#!/usr/bin/env python3
"""tools/mkpart_test.py -- writes a synthetic legacy MBR or GPT
partition table onto the first few sectors of a raw disk image, for
testing kernel/drivers/partition.c's parser (the shell's `parttable`
command) against real, correctly-checksummed data.

toy-os's own disk.img is one raw TFS2 blob starting at LBA 0 (see
docs/tfs2-spec.md) -- there is no real MBR/GPT on it, by design (see
docs/decisions.md's partition-parsing entry), so this exists purely to
let a QMP test session write one on temporarily.

This is deliberately TFS2-mount-preserving, not a blind overwrite:
LBA 0's first 5 bytes (TFS2's "TFS2"+version magic, kernel/drivers/
tfs.c's tfs_init()) are read back and kept as-is before patching in the
MBR partition-table region (bytes 446-511, which TFS2 never uses) --
booting with this in place still mounts the real, unmodified TFS2
filesystem underneath instead of tfs_init() seeing foreign/blank magic
and auto-reformatting the disk (which would silently wipe whatever this
script just wrote before `parttable` ever got a chance to read it).
--gpt's LBA 1 (the GPT header) DOES fully overwrite TFS2's journal
header that would otherwise live there -- confirmed harmless: TFS2's
own read_journal_header() checks its own "JRN1" magic first and
no-ops on a mismatch (GPT headers start "EFI PART"), so this never
corrupts anything replay_journal() would otherwise act on. Still, treat
disk.img as scratch during this test either way and restore a real one
afterward:

    python3 tools/tfs2_writer.py format disk.img --force
    python3 tools/tfs2_writer.py sync disk.img seed

**QEMU boot order:** a valid 0x55AA MBR signature on disk.img makes
SeaBIOS consider it a bootable hard disk -- without an explicit
`-boot order=d` (force CD-ROM first), it may try to boot disk.img's
(nonexistent) boot code instead of the toy-os.iso GRUB CD, hanging with
no serial output at all. Always launch with `-boot order=d` while disk.img
has a partition table on it.

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
    print("(LBA 0 bytes 0-445 preserved -- TFS2 still mounts normally underneath)")


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
    print("(LBA 0 bytes 0-445 preserved -- TFS2 still mounts normally underneath)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("disk", help="path to the raw disk image to patch (e.g. disk.img)")
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--mbr", action="store_true", help="write a legacy MBR")
    group.add_argument("--gpt", action="store_true", help="write a protective-MBR + GPT")
    args = ap.parse_args()

    if args.mbr:
        cmd_mbr(args.disk)
    else:
        cmd_gpt(args.disk)


if __name__ == "__main__":
    main()
