#!/usr/bin/env python3
"""The kernel debugger: a GDB stub over COM3, driven end to end.

kernel/debug/ is a GDB remote-protocol stub in the kernel (Linux's KGDB,
Windows' KD), armed by `kdebug=ttyS2` on the boot line. This boots a COPY
of disk.img with that word added and a third serial port (`vm.py
--kdebug`), then speaks the protocol to it directly -- a small client
here, so every assertion is about one packet and needs no GDB installed:

  - attaching stops a RUNNING kernel (the first `$` is a break-in);
  - qOffsets reports the KASLR delta the boot log printed, and memory
    read at a symbol plus that delta is the ELF's own bytes there;
  - a software breakpoint in `heap_total_bytes()`, which the debug console's
    `meminfo` calls, stops
    the machine when `meminfo` runs, at exactly that address;
  - a single step moves the PC one instruction on;
  - a hardware WRITE watchpoint on the tick counter fires, and says so
    (`T05watch:<addr>`);
  - a memory write round-trips;
  - an NMI from QEMU's monitor, and a ^C byte, each break into a running
    kernel;
  - detaching resumes it: the stopped `meminfo` finishes, and the machine
    answers a new command.

Then, if `gdb` is on PATH, a real GDB attaches to the same port and must
disassemble a function BY NAME -- which needs its symbols relocated by
qOffsets and the stub's memory read, together.

--positive-control boots the same image WITHOUT `kdebug=`: the attach
must then get no answer, or this is not testing the stub.

ON DEMAND (ondemand_sweep.py): it boots its own VM.

    python3 tools/kdebug_test.py [--instance 6] [--positive-control]
"""

import argparse
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
import install_grub  # noqa: E402
from qmp_test import QMPSession  # noqa: E402

VM = [sys.executable, os.path.join(TOOLS, "vm.py")]
KERNEL = os.path.join(REPO, "build", "kernel.bin")
KDEBUG = os.path.join(REPO, "build", "kernel.debug")
BP_FUNC = "heap_total_bytes"    # called by the debug console's `meminfo`
WATCH_VAR = "g_ticks"           # clockevent.c: written on every tick


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        (self.passes if ok else self.fails).append(name)
        print(f"  {'PASS' if ok else 'FAIL'}  {name}"
              + (f"\n        {detail}" if detail and not ok else ""))
        return ok


# --- the ELF ---------------------------------------------------------------

def symbol(name):
    """A symbol's LINK-TIME address from the full symbol table (kernel.debug,
    which keeps the locals `nm` needs), or None."""
    elf = KDEBUG if os.path.exists(KDEBUG) else KERNEL
    out = subprocess.run(["nm", elf], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[2] == name:
            return int(parts[0], 16)
    return None


def elf_bytes(vaddr, n):
    """n bytes at a link-time address, from kernel.bin's PT_LOAD segments
    (kernel.debug's sections are NOBITS: it has no code)."""
    with open(KERNEL, "rb") as f:
        data = f.read()
    phoff, = struct.unpack_from("<Q", data, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    for i in range(phnum):
        p_type, _flags, p_off, p_vaddr, _paddr, p_filesz = struct.unpack_from(
            "<IIQQQQ", data, phoff + i * phentsize)
        if p_type == 1 and p_vaddr <= vaddr and vaddr + n <= p_vaddr + p_filesz:
            o = p_off + (vaddr - p_vaddr)
            return data[o:o + n]
    return None


# --- the protocol ------------------------------------------------------------

class Rsp:
    """Just enough of a GDB client: packets with acks, a ^C, and waits."""

    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.buf = b""

    def close(self):
        self.s.close()

    def _byte(self, deadline):
        while not self.buf:
            left = deadline - time.time()
            if left <= 0:
                raise TimeoutError
            self.s.settimeout(left)
            try:
                chunk = self.s.recv(4096)
            except socket.timeout:
                raise TimeoutError
            if not chunk:
                raise ConnectionError("the port closed")
            self.buf += chunk
        b, self.buf = self.buf[:1], self.buf[1:]
        return b

    def send(self, data, timeout=10.0):
        data = data.encode() if isinstance(data, str) else data
        self.s.sendall(b"$" + data + b"#%02x" % (sum(data) & 0xFF))
        deadline = time.time() + timeout
        while True:
            b = self._byte(deadline)
            if b == b"+":
                return
            if b == b"-":
                raise ConnectionError("the stub NAKed a packet")

    def recv(self, timeout=10.0):
        deadline = time.time() + timeout
        while self._byte(deadline) != b"$":
            pass
        body = b""
        while (b := self._byte(deadline)) != b"#":
            body += b
        csum = self._byte(deadline) + self._byte(deadline)
        if int(csum, 16) != sum(body) & 0xFF:
            raise ConnectionError(f"bad checksum on {body[:40]!r}")
        self.s.sendall(b"+")
        return body.decode("latin-1")

    def cmd(self, data, timeout=10.0):
        self.send(data, timeout)
        return self.recv(timeout)

    def interrupt(self):
        self.s.sendall(b"\x03")


def rip_of(g_reply):
    """rip from a `g` reply: 16 GPRs of 8 bytes precede it, little-endian."""
    return int.from_bytes(bytes.fromhex(g_reply[256:272]), "little")


# --- the run -----------------------------------------------------------------

def vm(inst, disk, log, *args, timeout=240):
    cmd = VM + ["--instance", str(inst), "--disk", disk, "--serial-log", log, "--kdebug"] + list(args)
    try:
        r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return ""
    return (r.stdout or "") + (r.stderr or "")


def serial(log):
    try:
        with open(log, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except OSError:
        return ""


def sock_path(inst):
    return os.path.join(REPO, ".vm.kdb" if inst == 0 else f".vm.{inst}.kdb")


def run(inst, disk, log, res):
    boot = vm(inst, disk, log, "start", timeout=300)
    if "ready" not in boot:
        res.check("the guest boots", False, boot[-400:] + serial(log)[-400:])
        return
    armed = "kdebug: GDB stub armed on ttyS2" in serial(log)
    res.check("the boot log says the stub is armed", armed,
              "\n".join(l for l in serial(log).splitlines() if "kdebug" in l)[-300:])

    rsp = Rsp(sock_path(inst))
    try:
        steps(inst, disk, log, res, rsp)
    except (TimeoutError, ConnectionError) as e:
        res.check("the protocol exchange completes", False, f"{type(e).__name__}: {e}")
    finally:
        rsp.close()

    if shutil.which("gdb"):
        real_gdb(inst, res)
    else:
        print("  SKIP  a real GDB attaches -- gdb is not installed")

    out = vm(inst, disk, log, "exec", "uptime", timeout=60)
    res.check("the machine runs on after every detach", re.search(r"up [0-9]", out) is not None,
              out[-200:])


def steps(inst, disk, log, res, rsp):
    sup = rsp.cmd("qSupported:swbreak+;hwbreak+", timeout=15)
    if not res.check("attaching stops a running kernel and answers qSupported",
                     "PacketSize=" in sup, sup):
        return
    res.check("the stop reason is SIGINT", rsp.cmd("?") == "S02")

    off = rsp.cmd("qOffsets")
    m = re.match(r"Text=([0-9a-f]+);Data=\1;Bss=\1$", off)
    logged = re.search(r"kernel relocated \+0x([0-9a-f]+)", serial(log))
    want = int(logged.group(1), 16) if logged else 0
    delta = int(m.group(1), 16) if m else None
    res.check("qOffsets is the KASLR delta the boot log printed", delta == want,
              f"qOffsets {off!r}, boot log +0x{want:x}")
    if delta is None:
        return

    bp_link = symbol(BP_FUNC)
    bp = bp_link + delta
    want_bytes = elf_bytes(bp_link, 16)
    got = rsp.cmd(f"m{bp:x},10")
    res.check(f"memory at {BP_FUNC}+delta is the ELF's bytes there",
              want_bytes is not None and got == want_bytes.hex(),
              f"stub {got!r}\n        elf  {want_bytes.hex() if want_bytes else None}")
    res.check("an unmapped read is refused, not faulted on",
              rsp.cmd("m800000000000,8").startswith("E"))

    g = rsp.cmd("g")
    res.check("`g` carries the 24 core registers (164 bytes)", len(g) == 328, f"{len(g)} hex digits")

    # A software breakpoint, hit from outside.
    res.check(f"Z0 on {BP_FUNC} is accepted", rsp.cmd(f"Z0,{bp:x},1") == "OK")
    rsp.send("c")
    trigger = subprocess.Popen(VM + ["--instance", str(inst), "exec", "meminfo"],
                               cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    stop = rsp.recv(timeout=30)
    res.check("running `meminfo` stops at the breakpoint (S05)", stop == "S05", stop)
    pc = rip_of(rsp.cmd("g"))
    res.check("...with the PC ON the breakpoint, not one past its int3", pc == bp,
              f"pc 0x{pc:x}, breakpoint 0x{bp:x}")

    # One instruction on. The breakpoint is still inserted; a step from it
    # must execute the REAL first instruction, not the int3.
    rsp.send("s")
    stop = rsp.recv(timeout=10)
    pc2 = rip_of(rsp.cmd("g"))
    res.check("a single step stops again one instruction on",
              stop == "S05" and bp < pc2 <= bp + 15, f"{stop}, pc 0x{pc2:x}")
    res.check("z0 removes it", rsp.cmd(f"z0,{bp:x},1") == "OK")

    # A hardware write watchpoint.
    w = symbol(WATCH_VAR) + delta
    res.check("Z2 (write watch, 8 bytes) is accepted", rsp.cmd(f"Z2,{w:x},8") == "OK")
    rsp.send("c")
    stop = rsp.recv(timeout=10)
    res.check("the tick's write to the counter fires it, and names the address",
              stop == f"T05watch:{w:x};", stop)
    res.check("z2 removes it", rsp.cmd(f"z2,{w:x},8") == "OK")
    res.check("a read-only watch is 'not supported' (x86 has none)", rsp.cmd(f"Z3,{w:x},8") == "")

    # A memory write round trip, on the watched counter itself.
    before = rsp.cmd(f"m{w:x},8")
    res.check("M writes memory", rsp.cmd(f"M{w:x},8:efbeadde00000000") == "OK")
    res.check("...and m reads back what it wrote", rsp.cmd(f"m{w:x},8") == "efbeadde00000000", before)

    # Two ways to break into a running kernel.
    rsp.send("c")
    time.sleep(0.5)
    QMPSession(port=4445 + inst).hmp("nmi")
    stop = rsp.recv(timeout=10)
    res.check("an NMI from QEMU's monitor breaks in (S02)", stop == "S02", stop)
    rsp.send("c")
    time.sleep(0.5)
    rsp.interrupt()
    stop = rsp.recv(timeout=10)
    res.check("a ^C byte breaks in (S02)", stop == "S02", stop)

    res.check("D detaches", rsp.cmd("D") == "OK")
    try:
        out, _ = trigger.communicate(timeout=60)
    except subprocess.TimeoutExpired:
        trigger.kill()
        out = "(still stopped)"
    res.check("the `meminfo` that hit the breakpoint completes once resumed",
              trigger.returncode == 0 and "free" in out.lower(), out[-200:])


def real_gdb(inst, res):
    """A real GDB: symbols relocated by qOffsets, a disassembly by NAME."""
    r = subprocess.run(
        ["gdb", "-nx", "-batch", KERNEL,
         "-ex", "set pagination off",
         "-ex", f"target remote {sock_path(inst)}",
         "-ex", f"x/2i {BP_FUNC}",
         "-ex", "info registers rip",
         "-ex", "detach"],
        cwd=REPO, capture_output=True, text=True, timeout=120)
    out = r.stdout + r.stderr
    dis = [l for l in out.splitlines() if f"<{BP_FUNC}" in l]
    res.check("a real GDB disassembles a function by name", len(dis) >= 2
              and "Cannot access memory" not in out, out[-600:])
    res.check("...and detaches cleanly", "Detaching" in out or "detached" in out.lower(), out[-300:])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--instance", type=int, default=6)
    ap.add_argument("--disk", default=os.path.join(REPO, "disk.img"),
                    help="seed image to COPY (never written to directly)")
    ap.add_argument("--positive-control", action="store_true",
                    help="boot WITHOUT kdebug=; the attach must then fail")
    ap.add_argument("--keep", help="keep the serial log at this path")
    args = ap.parse_args()

    for name in (BP_FUNC, WATCH_VAR):
        if symbol(name) is None:
            print(f"kdebug_test: {name} is not in the kernel's symbol table -- build first")
            return 2

    work = tempfile.mkdtemp(prefix="kdebug_test.")
    disk = os.path.join(work, "disk.img")
    log = os.path.join(work, "serial.log")
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", args.disk, disk], check=True)
    if not args.positive_control:
        ok, why = install_grub.add_boot_word(disk, "kdebug=ttyS2")
        if not ok:
            print(f"kdebug_test: cannot arm the copy: {why}")
            return 2

    res = Result()
    try:
        run(args.instance, disk, log, res)
    finally:
        vm(args.instance, disk, log, "stop")
        if args.keep:
            shutil.copy(log, args.keep)
        shutil.rmtree(work, ignore_errors=True)

    if args.positive_control:
        if res.fails:
            print(f"\nkdebug_test: positive control FAILED as it must ({len(res.fails)} finding(s))")
            return 0
        print("\nkdebug_test: positive control PASSED -- something other than the stub answered",
              file=sys.stderr)
        return 1

    print(f"\nkdebug_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
