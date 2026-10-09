#!/usr/bin/env python3
"""tools/disks_test.py -- Disks (userland/gui/apps/disks.c) on a guest of its own.

The guest boots the ordinary system disk (pinned with `root=ata0p3`) and
a BLANK 256 MiB virtio disk beside it, and Disks is driven through its
whole job on the blank one: a new partition table and partition, mount,
check, unmount, a second partition, a format to FAT32, a delete. Then
the HOST reads the image's GPT itself.

Each step is believed from a path that is not the app's own log:
  * the shell's `lsblk` and `df` -- the kernel's block table and mounts;
  * a file written through the mount and read back;
  * the FAT32 format is believed because the partition then MOUNTS as
    fat32 (a format that wrote nothing would still mount as tfs3);
  * the host's own read of the GPT at the end: the partition Disks kept
    across two table rewrites is still there with its name and LBAs,
    and the deleted one is gone. A rewrite that dropped or moved the
    entries it should keep fails here and nowhere else.
And the refusals: on the SYSTEM disk, Format and Mount are disabled and
do nothing, and the Delete key says why without asking.

On demand (ondemand_sweep.py), not in gui_regress: it boots its own
guest with a second disk.

    python3 tools/disks_test.py [--kvm] [--keep DIR]
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from harness import Results, copy_disk          # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import install_grub                             # noqa: E402
import port_guard                               # noqa: E402

SECTOR = 512
BLANK_BYTES = 256 * 1024 * 1024
LOG = []


def chk(res, ok, name):
    return res.check(name, ok)


class Layout:
    """The last frame's self-reported rects; a frame starts at `menu`."""

    def __init__(self, lines, content):
        self.r, self.state = {}, None
        self.cx, self.cy = content["x"], content["y"]
        last = max([i for i, ln in enumerate(lines) if "disks: layout menu " in ln] or [-1])
        for line in lines[max(last, 0):]:
            if "disks: layout " not in line:
                continue
            parts = line.split("disks: layout ", 1)[1].split()
            try:
                nums = [int(p) for p in parts[1:]]
            except ValueError:
                continue
            if parts[0] == "state":
                self.state = dict(zip(("drive", "seg", "segs", "part"), nums))
            elif len(nums) == 5:
                self.r[f"{parts[0]} {nums[0]}"] = tuple(nums[1:])
            elif len(nums) == 4:
                self.r[parts[0]] = tuple(nums)
            elif len(nums) == 1:
                self.r[parts[0]] = nums[0]

    def centre(self, key):
        x, y, w, h = self.r[key]
        return (self.cx + x + w // 2, self.cy + y + h // 2)


def poll(dbg):
    LOG.extend(dbg.logs("disks:", clear=True))
    return LOG


def wait(dbg, content, want, timeout=20.0):
    deadline = time.time() + timeout
    while True:
        lay = Layout(poll(dbg), content)
        if (lay.state and want(lay)) or time.time() > deadline:
            return lay
        time.sleep(0.3)


def said(text, since):
    return any(text in ln for ln in LOG[since:])


def sh(dbg, cmd):
    return dbg.send(f"sh {cmd}")


def gpt_entries(img):
    """(first, last, name) of each used GPT entry, read by the host."""
    with open(img, "rb") as f:
        f.seek(SECTOR)
        h = f.read(SECTOR)
        if h[:8] != b"EFI PART":
            return None
        entries_lba, = struct.unpack_from("<Q", h, 72)
        count, size = struct.unpack_from("<II", h, 80)
        f.seek(entries_lba * SECTOR)
        raw = f.read(count * size)
    out = []
    for i in range(count):
        e = raw[i * size:(i + 1) * size]
        if e[:16] == bytes(16):
            continue
        first, last = struct.unpack_from("<QQ", e, 32)
        name = e[56:128].decode("utf-16-le").split("\0")[0]
        out.append((first, last, name))
    return out


def run(dbg, qmp, res):
    dbg.send("gui spawn /bin/wm/apps/disks")
    win = None
    for _ in range(50):
        win = dbg.window("Disks")
        if win:
            break
        time.sleep(0.3)
    if not chk(res, win is not None, "the window opens"):
        return
    c = win["content"]
    lay = wait(dbg, c, lambda lay: lay.state["segs"] > 0)
    chk(res, said("disks: 2 drives", 0), "it lists both drives")

    def press(key):
        dbg.click(*lay.centre(key))
        dbg.settle()

    def toolbar(index):
        press(f"toolbar.button {index}")

    TB_FORMAT, TB_CHECK, TB_MOUNT, TB_DELETE = 3, 4, 5, 6

    # 1. The system disk refuses. Its toolbar buttons are DISABLED, so a
    # click must do nothing at all; the Delete key reaches the app, which
    # must say why instead of asking.
    chk(res, lay.r.get("map.count") == 3, f"the system disk's map has its three partitions ({lay.r.get('map.count')})")
    press("map.seg 2")
    lay = wait(dbg, c, lambda lay: lay.state["seg"] == 2)
    mark = len(LOG)
    toolbar(TB_FORMAT)
    toolbar(TB_MOUNT)
    lay = wait(dbg, c, lambda lay: True)
    chk(res, "dialog.button 0" not in lay.r and not said("mkfs", mark) and not said("Mounted", mark)
        and not said("Unmounted", mark), "on the system disk, Format and Mount do nothing")
    dbg.key("0xF788")                        # Delete
    lay = wait(dbg, c, lambda lay: True)
    chk(res, said("cannot be changed while it runs", mark) and "dialog.button 0" not in lay.r,
        "and Delete says why, without asking")

    # 2. The blank disk: one free segment and the form.
    x, y, w, h = lay.r["drives.first_cell"]
    dbg.click(lay.cx + x + w // 2, lay.cy + y + h + h // 2)
    lay = wait(dbg, c, lambda lay: lay.state["drive"] == 1 and "size" in lay.r)
    if not chk(res, "size" in lay.r and lay.r.get("map.count") == 1, "the blank disk is one free segment, with the form"):
        return

    def create(mb, name, fs_slot):
        nonlocal lay
        press("size")
        dbg.key("0x01")                      # select all
        for ch in str(mb):
            dbg.key(ord(ch), settle=False)
        press("name")
        for ch in name:
            dbg.key(ord(ch), settle=False)
        press(f"fs.slot {fs_slot}")
        press("create")
        lay = wait(dbg, c, lambda lay: "dialog.button 0" in lay.r)
        press("dialog.button 0")             # Create

    # 3. A 100 MiB tfs3 partition named "data".
    mark = len(LOG)
    create(100, "data", 0)
    lay = wait(dbg, c, lambda lay: lay.state["part"] == 1)
    out = sh(dbg, "lsblk")
    chk(res, said("mkpart virtio0 1 entries -> 0", mark) and said("mkfs virtio0p1 tfs3 -> 0", mark),
        "Create wrote the table and formatted it")
    chk(res, "virtio0p1" in out and "100.0M" in out, "the kernel names virtio0p1, 100 MiB (lsblk)")

    # 4. Mount it, write through it, check it, unmount it.
    toolbar(TB_MOUNT)
    out = sh(dbg, "df")
    chk(res, "/mnt/virtio0p1" in out and "tfs3" in out, "Mount attaches it at /mnt/virtio0p1 as tfs3 (df)")
    sh(dbg, "write /mnt/virtio0p1/hello.txt disks-was-here")
    out = sh(dbg, "cat /mnt/virtio0p1/hello.txt")
    chk(res, "disks-was-here" in out, "a file written through the mount reads back")
    mark = len(LOG)
    lay = wait(dbg, c, lambda lay: True)
    toolbar(TB_CHECK)
    lay = wait(dbg, c, lambda lay: "dialog.button 0" in lay.r)
    chk(res, said("check /mnt/virtio0p1 read-only -> 0 problems", mark), "Check finds no problems")
    press("dialog.button 0")
    lay = wait(dbg, c, lambda lay: "dialog.button 0" not in lay.r)
    toolbar(TB_MOUNT)
    out = sh(dbg, "df")
    chk(res, "/mnt/virtio0p1" not in out, "Unmount detaches it (df)")
    out = sh(dbg, "ls /mnt")
    chk(res, "virtio0p1" not in out, "and the mount point Disks made is gone")

    # 5. A second partition in what is left, FAT32.
    press("map.seg 1")
    lay = wait(dbg, c, lambda lay: lay.state["seg"] == 1 and "size" in lay.r)
    mark = len(LOG)
    create(64, "spare", 1)
    lay = wait(dbg, c, lambda lay: lay.r.get("map.count", 0) >= 2 and said("mkfs virtio0p2", mark))
    out = sh(dbg, "lsblk")
    chk(res, "virtio0p1" in out and "virtio0p2" in out, "a second partition, the first kept (lsblk)")

    # 6. Format the first as FAT32, and believe it by mounting it.
    press("map.seg 0")
    lay = wait(dbg, c, lambda lay: lay.state["seg"] == 0)
    toolbar(TB_FORMAT)
    lay = wait(dbg, c, lambda lay: "dialog.button 1" in lay.r)
    press("dialog.button 1")                 # Format as FAT32
    lay = wait(dbg, c, lambda lay: "dialog.button 0" not in lay.r)
    toolbar(TB_MOUNT)
    out = sh(dbg, "df")
    chk(res, "fat32" in out and "/mnt/virtio0p1" in out, "Format as FAT32 took: it mounts as fat32 (df)")
    toolbar(TB_MOUNT)

    # 7. Delete the second.
    press("map.seg 1")
    lay = wait(dbg, c, lambda lay: lay.state["seg"] == 1)
    toolbar(TB_DELETE)
    lay = wait(dbg, c, lambda lay: "dialog.button 0" in lay.r)
    press("dialog.button 0")                 # Delete
    time.sleep(1)
    out = sh(dbg, "lsblk")
    chk(res, "virtio0p2" not in out and "virtio0p1" in out, "Delete removes the second and keeps the first (lsblk)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--instance", type=int, default=None)
    ap.add_argument("--kvm", action="store_true")
    ap.add_argument("--keep", metavar="DIR", help="keep the images here")
    args = ap.parse_args()
    if args.instance is None:
        args.instance = port_guard.find_free_instance()
    tmp = args.keep or tempfile.mkdtemp(prefix="disks-")
    os.makedirs(tmp, exist_ok=True)
    root = os.path.join(tmp, "root.img")
    blank = os.path.join(tmp, "blank.img")
    copy_disk("disk.img", root, cwd=REPO)
    res = Results()
    ok, why = install_grub.add_boot_word(root, "root=ata0p3")
    if not chk(res, ok, f"the system disk is pinned as the root {why}"):
        return 1
    with open(blank, "wb") as f:
        f.truncate(BLANK_BYTES)

    def vm(*argv):
        r = subprocess.run([sys.executable, os.path.join(HERE, "vm.py"), "--instance", str(args.instance),
                            "--disk", root, "--extra-disk", blank, "--extra-disk-kind", "virtio",
                            *(["--kvm"] if args.kvm else []), *argv],
                           cwd=REPO, capture_output=True, text=True, check=False, timeout=300)
        return r.stdout + r.stderr

    vm("stop")
    if not chk(res, "ready" in vm("start"), "the guest came up"):
        return 1
    print("disks_test: checks")
    try:
        qmp = QMPSession(port=port_guard.instance_qmp(args.instance))
        sock = port_guard.instance_sock(args.instance)
        enter_gui(qmp, sock)
        dbg = DebugConsole(sock)
        try:
            run(dbg, qmp, res)
        finally:
            dbg.close()
    finally:
        vm("stop")

    parts = gpt_entries(blank)
    chk(res, parts is not None, "the host reads a GPT on the once-blank disk")
    if parts is not None:
        chk(res, len(parts) == 1 and parts[0][2] == "data" and parts[0][0] == 2048 and parts[0][1] == 2048 + 204800 - 1,
            f"one entry is left -- 'data', LBA 2048, 100 MiB -- kept across two rewrites ({parts})")
    print(f"\ndisks_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
