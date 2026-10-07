#!/usr/bin/env python3
"""tools/live_boot_test.py -- the ISO boots and is USABLE with no disk.

WHAT THIS COVERS
----------------
A Live CD boot: GRUB hands the kernel a TFS3 image as a module, the
kernel mounts it through a RAM block device, and the system comes up
with a real /bin and a real /usr (docs/live-cd-design.md). QEMU is
launched with NO -drive at all, which is the point -- an empty disk
image would let the ordinary disk path run and prove nothing.

**Ask what a broken version would still pass.** A live boot that mounted
NOTHING still reaches a shell and still shows a desktop, because the
kernel degrades to an empty RAM filesystem and the WM is compiled in. So
"it booted" and "there is a prompt" are worthless as evidence here. The
assertion has to be that a file the ISO shipped can be read, and that
the volume reports itself honestly.

The four checks:

  1. The RAM block device registered and the filesystem mounted on it.
  2. A file the image SHIPPED is listable -- /bin/wm/apps exists with
     the apps in it. This is the check that separates "mounted the live
     image" from "came up with an empty RAM filesystem".
  3. A ring-3 binary from that image RUNS. Reading a directory only
     proves metadata; executing proves the data blocks are really there
     and really reachable through the RAM device.
  4. It reports itself NOT PERSISTENT. A live volume that claimed
     otherwise would be the most misleading thing this feature could
     do -- a user would expect their files to survive the power going
     off.

PHASE 2: THE POLICY, WHICH IS THE HALF THAT SILENTLY BROKE
----------------------------------------------------------
"Live only when asked for, or when there is no disk" is the rule that
keeps a live session from displacing somebody's installed system. It was
enforced by asking `ata_present()` -- the LEGACY IDE probe -- so on a
machine whose only disk is AHCI or virtio the kernel concluded "no disk"
and took over anyway. Nothing saw it, because every automated live test
here boots with NO DISK AT ALL, which is the one configuration where
both the right and the wrong predicate agree.

So phase 2 boots the SAME scratch ISO twice, with the `live` word left
out, and requires OPPOSITE outcomes: with a disk attached the image must
be declined, and with no disk it must be used. Each run is the other's
control -- a kernel that always takes the image, or never does, fails
one of them whatever it does to the other.

    python3 tools/live_boot_test.py                 # both phases
    python3 tools/live_boot_test.py --phase nodisk  # just the original
    echo $?
"""

import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_test import launch_qemu_cmd          # noqa: E402
from harness import Results  # noqa: E402

BOOT_TIMEOUT_S = 40.0
PROMPT = "dbg> "


class Shell:
    """One command at a time over the serial debug console.

    Deliberately its own few lines rather than importing vm.py's: that
    module drives a MANAGED vm (its own pidfile, its own socket path,
    its own disk), and this test's entire point is a QEMU launched
    differently -- no disk at all. Sharing the driver would have meant
    teaching vm.py about a VM it does not own.
    """

    def __init__(self, sock_path, timeout=15.0):
        self.path = sock_path
        self.timeout = timeout

    def run(self, command):
        """`sh <cmd>` is how the debug console reaches the shell -- see
        tools/vm.py, which drives the same wire."""
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(self.timeout)
            s.connect(self.path)
            # A bare newline first: synchronise on a fresh prompt rather
            # than trying to match whatever the boot printed.
            s.sendall(b"\n")
            buf = ""
            deadline = time.time() + self.timeout
            while time.time() < deadline and not buf.endswith(PROMPT):
                buf += s.recv(65536).decode("utf-8", errors="replace")
            s.sendall((command + "\n").encode())
            out = ""
            deadline = time.time() + self.timeout
            while time.time() < deadline and not out.endswith(PROMPT):
                out += s.recv(65536).decode("utf-8", errors="replace")
            if out.startswith(command):
                out = out[len(command):]
            return out[:-len(PROMPT)] if out.endswith(PROMPT) else out

    def close(self):
        pass


Result = Results


# `df` PRINTS A TABLE, and this tool parsed `used:`/`total:` LINES --
# the shape it had before df became a /bin program. Every number came
# back 0 and three checks failed against a live boot that was working
# perfectly. That rot was invisible because the live image could not be
# built at all (its size was a hardcoded 24 MiB the seed tree had
# outgrown), so nothing ran this tool to notice.
#
#   filesystem on           size     used     free     use%  persists
#   tfs3       /            23.4M    4.2M     19.2M    17%   no
#
# AND IT GREW AN `on` COLUMN when Real mount points landed, which shifted
# every index by one and is the same rot again -- a tool reading columns
# positionally has to be updated with the command. The ROOT row is now
# picked BY ITS MOUNT POINT rather than by being the only row, since a
# machine with a /boot has more than one.
def parse_df(df):
    """(size_kb, used_kb, persists) from df's ROOT row."""
    def kb(cell):
        mult = {"K": 1, "M": 1024, "G": 1024 * 1024}.get(cell[-1:].upper(), 0)
        if not mult:
            return int("".join(c for c in cell if c.isdigit()) or 0) // 1024
        try:
            return int(float(cell[:-1]) * mult)
        except ValueError:
            return 0

    for line in df.splitlines():
        cols = line.split()
        if len(cols) >= 7 and cols[0] != "filesystem" and cols[1] == "/" and "%" in cols[5]:
            return kb(cols[2]), kb(cols[3]), cols[6].lower()
    return 0, 0, ""


# --- phase 2: the mount POLICY ----------------------------------------

# `live` is deliberately absent from this entry: phase 2 is about what
# the kernel decides when nobody asked. The shipped live ISO's default
# entry forces it, so this cannot be tested with that image.
UNASKED_CFG = """insmod all_video
set gfxpayload=auto
set timeout=0
menuentry "toy-os (unasked)" {
    multiboot2 /boot/kernel.bin
    module2 /boot/live.img live.img
    boot
}
"""


def build_unasked_iso(tmp, kernel, image):
    """A one-entry ISO carrying the live module and NOT asking for it."""
    tree = os.path.join(tmp, "live_policy_iso")
    subprocess.run(f"rm -rf {tree}", shell=True, check=True)
    os.makedirs(os.path.join(tree, "boot", "grub"))
    subprocess.run(f"cp {kernel} {tree}/boot/kernel.bin", shell=True, check=True)
    subprocess.run(f"cp {image} {tree}/boot/live.img", shell=True, check=True)
    with open(os.path.join(tree, "boot", "grub", "grub.cfg"), "w") as f:
        f.write(UNASKED_CFG)
    iso = os.path.join(tmp, "live_policy.iso")
    mkrescue = shutil.which("grub-mkrescue") or shutil.which("grub2-mkrescue")
    if not mkrescue:
        return None
    subprocess.run(f"{mkrescue} -o {iso} {tree}", shell=True, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return iso


def foreign_disk(tmp):
    """A disk with a real GPT and nothing toy-os can claim.

    Not a blank image: a kernel that ignored partition tables would treat
    a blank one as "no disk" and pass phase 2 for the wrong reason.
    """
    path = os.path.join(tmp, "live_policy_disk.img")
    subprocess.run(f"rm -f {path} && truncate -s 256M {path}", shell=True, check=True)
    here = os.path.dirname(os.path.abspath(__file__))
    subprocess.run(f"python3 {here}/mkpart_test.py {path} --gpt --layout 200M",
                   shell=True, check=True, stdout=subprocess.DEVNULL)
    return path


def boot_and_read_fs_lines(iso, disk, tmp, tag, timeout_s=25):
    """Boot once and return the kernel's own `fs:` lines."""
    log = os.path.abspath(os.path.join(tmp, f"live_policy_{tag}.log"))
    if os.path.exists(log):
        os.remove(log)
    drive = ""
    if disk:
        # AHCI on purpose -- an IDE disk is the one kind the old
        # predicate could see, so it would pass with the bug present.
        drive = (f" -device ahci,id=ahci -drive if=none,id=d0,file={disk},format=raw"
                 f" -device ide-hd,drive=d0,bus=ahci.0")
    cmd = (f"timeout {timeout_s} qemu-system-x86_64 -cdrom {iso} -boot order=d"
           f" -m 512{drive} -serial file:{log} -display none -no-reboot")
    subprocess.run(cmd, shell=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    if not os.path.exists(log):
        return []
    with open(log, "rb") as f:
        text = f.read().decode("utf-8", "replace")
    return [ln for ln in text.splitlines() if ln.startswith("fs:")]


def run_policy_phase(res, tmp):
    kernel, image = "build/kernel.bin", "build/live.img"
    for f in (kernel, image):
        if not os.path.exists(f):
            res.check("phase 2 could run", False, f"{f} not found -- run `make live-iso`")
            return
    iso = build_unasked_iso(tmp, kernel, image)
    if iso is None:
        print("live_boot_test: no grub-mkrescue -- skipping the policy phase")
        return
    disk = foreign_disk(tmp)

    # A disk is present and nobody asked for a live session: the image
    # must be DECLINED. The disk holds no toy-os filesystem, so the
    # honest outcome is ramfs -- and saying so is itself the evidence
    # that the disk was the thing consulted.
    lines = boot_and_read_fs_lines(iso, disk, tmp, "disk")
    used_image = any("live image" in ln for ln in lines)
    res.check("an unasked-for live image does NOT displace a real disk",
              bool(lines) and not used_image,
              " | ".join(lines[:4]) or "no fs: lines at all")

    # The same ISO with no disk: now it MUST use the image. Without this
    # half, a kernel that never touched the module would pass the check
    # above and this tool would call a dead feature healthy.
    lines = boot_and_read_fs_lines(iso, None, tmp, "nodisk")
    res.check("with no disk at all, the image IS used (the control)",
              any("live image" in ln for ln in lines),
              " | ".join(lines[:4]) or "no fs: lines at all")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    # The LIVE iso, which is its own artifact (`make live-iso`) -- the
    # ordinary one carries no module, deliberately, because GRUB reads
    # the whole thing off the CD before the kernel starts and that cost
    # belongs to the image that needs it. See the Makefile.
    ap.add_argument("--iso", default="toy-os-live.iso")
    ap.add_argument("--tmp", default="/tmp")
    ap.add_argument("--phase", choices=("both", "nodisk", "policy"),
                    default="both", help="which phase to run")
    args = ap.parse_args()

    if not os.path.exists(args.iso):
        print(f"live_boot_test: {args.iso} not found -- run `make live-iso` first")
        return 1

    res = Result()

    if args.phase == "policy":
        run_policy_phase(res, args.tmp)
        return report(res)

    serial = os.path.abspath(os.path.join(args.tmp, "live_serial.log"))
    sock = os.path.abspath(os.path.join(args.tmp, "live.serial"))
    pidfile = os.path.abspath(os.path.join(args.tmp, "live_qemu.pid"))
    for f in (serial, pidfile):
        if os.path.exists(f):
            os.remove(f)

    # NO DISK. launch_qemu_cmd(disk=None) omits -drive entirely.
    cmd = launch_qemu_cmd(iso=args.iso, disk=None, serial_log=serial,
                          qmp_port=4499, vnc_display=9, pidfile=pidfile)
    # A unix SOCKET rather than the helper's log file: this test drives
    # the console interactively, and `-serial file:` is write-only.
    cmd = cmd.replace(f"-serial file:{serial}", f"-serial unix:{sock},server,nowait")
    subprocess.run(cmd, shell=True, check=True)

    sh = None
    try:
        deadline = time.time() + BOOT_TIMEOUT_S
        while time.time() < deadline:
            try:
                # Short timeout for the PROBE only -- it is asked
                # repeatedly while the guest boots. The real shell gets
                # the full one, or a slow command comes back truncated
                # and reads as a wrong answer rather than a slow one.
                Shell(sock, timeout=3.0).run("")
                sh = Shell(sock)
                break
            except Exception:
                time.sleep(0.5)
        if sh is None:
            res.check("the ISO booted with no disk", False,
                      f"no serial console within {BOOT_TIMEOUT_S}s")
            raise SystemExit(1)

        # 1. mounted from RAM
        df = sh.run("sh df")
        # Mounted AND with content: an empty RAM filesystem reports the
        # same backend name, so the name alone proves nothing.
        size_kb, used_kb, persists = parse_df(df)
        res.check("the filesystem mounted from the live image",
                  "tfs3" in df.lower() and used_kb > 1000,
                  f"{used_kb} KB used -- an empty RAM filesystem, not the image")
        total_kb = size_kb

        # The SIZE has to be the image's, and this is the check that
        # catches a partial last block group being mis-measured. The live
        # image is the only small volume anything boots, so it is the
        # only place the bug is visible: a KTEST on the 9 GB dev disk
        # cannot see one group over-reported (1.4% of the total), and
        # `df` on this volume said 127 MB for a 16 MiB filesystem.
        # Against the image's OWN partition, as the kernel logged it at
        # mount -- not a fixed 128 MiB: the image outgrew one block group
        # once the seed tree did, and the bug this guards (a partial last
        # group counted whole) would report a whole extra group past it.
        log = sh.run("sh dmesg") or ""
        m = re.search(r"mounting tfs3 from partition \d+ \(LBA \d+, (\d+) sectors\)", log)
        part_kb = int(m.group(1)) * 512 // 1024 if m else 0
        res.check("the reported size is the image's, not a whole block group",
                  0 < total_kb <= part_kb,
                  f"df says {total_kb} KB for an image partition of {part_kb} KB "
                  f"-- the last group's real span is being ignored")

        # 2. shipped content is there -- the check that separates a live
        #    mount from an empty RAM filesystem
        ls = sh.run("sh ls /bin/wm/apps")
        res.check("a directory the image shipped is readable",
                  "calculator" in ls and "notepad" in ls, ls.strip()[:200])

        # 2b. THE DIRECTORIES A BAD TRIM ATE. `tfs3_writer.py trim`
        #     punched holes at a VOLUME-relative offset through the raw
        #     fd, so on a partitioned image every punch landed one
        #     partition-start early -- on blocks that were in use. It
        #     emptied /usr/wm/applications, /etc/services.d and
        #     /usr/wm/startup in every live image, and the live CD then
        #     booted with a full /bin and a desktop with NO APPS.
        #
        #     Check 2 above could not see it: it reads /bin/wm/apps,
        #     which sat far enough from a free run to survive. So this
        #     names the three that did not -- a corruption that lands
        #     somewhere specific needs a check that looks THERE, and
        #     "some directory is readable" is not that check.
        for d, want in (("/usr/wm/applications", ".desktop"),
                        ("/etc/services.d", "toywm"),
                        ("/usr/wm/startup", "README.md")):
            got = sh.run(f"sh ls {d}")
            ok = want in got if want else len(got.split()) > 2
            res.check(f"{d} survived the image build", ok, got.strip()[:200])

        # 3. and its data blocks really are reachable
        out = sh.run("sh run libc_test")
        res.check("a binary from the live image runs",
                  "PASS" in out or "pass" in out, out.strip()[:200])

        # 4. and it says so honestly
        # In df's own words: "RAM-only -- won't survive reboot". A live
        # volume claiming persistence would tell a user their files are
        # safe when the next power cycle erases them.
        res.check("the volume reports itself NOT persistent",
                  persists == "no", df.strip()[:160])
    finally:
        if sh:
            sh.close()
        if os.path.exists(pidfile):
            with open(pidfile) as f:
                pid = f.read().strip()
            if pid:
                subprocess.run(f"kill {pid}", shell=True)

    if args.phase == "both":
        run_policy_phase(res, args.tmp)

    return report(res)


def report(res):
    print(f"\nlive_boot_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
