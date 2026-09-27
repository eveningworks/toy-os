#!/usr/bin/env python3
"""The kernel debugger: a GDB stub over COM3 or the network, end to end.

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
    (`T05thread:<tid>;watch:<addr>;`);
  - every process is a thread: `qfThreadInfo` lists the kernel context
    (0x3e8) and pid 1, `qThreadExtraInfo` names it `init`, and `g` on a
    PARKED thread marks the registers nobody saved as unavailable, and
    memory is read in the SELECTED thread's address space -- init's and
    tosh's first bytes at 0x8000000000 are each their own ELF's;
  - `toy-symbols` loads a kernel module (e1000_transmit disassembles by
    name at its real address) and a dynamic program with its /lib
    libraries: tosh's bt runs from the scheduler into tosh.c's main;
  - a memory write round-trips;
  - an NMI from QEMU's monitor, and a ^C byte, each break into a running
    kernel;
  - detaching resumes it: the stopped `meminfo` finishes, and the machine
    answers a new command.

Then, if `gdb` is on PATH, a real GDB attaches to the same port and must
disassemble a function BY NAME -- which needs its symbols relocated by
qOffsets and the stub's memory read, together.

--net runs every check above over the NETWORK transport instead: a second
e1000 the debugger owns (`vm.py --kdebug-net`), `kdebug=net,...` with a
fresh random key, and the keyed datagrams of tools/kdebug_bridge.py. It
adds that a datagram under the WRONG key, and an exact REPLAY of an
accepted one, both get silence -- and runs the real GDB through the
bridge.

--positive-control boots the same image WITHOUT `kdebug=`: the attach
must then get no answer, or this is not testing the stub.

ON DEMAND (ondemand_sweep.py): it boots its own VM.

    python3 tools/kdebug_test.py [--instance 6] [--net] [--positive-control]
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
import kdebug_bridge  # noqa: E402
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

    def __init__(self, link):
        self.s = link          # a connected socket, or kdebug_bridge.UdpLink
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
            except (socket.timeout, BlockingIOError):
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


def is_stop(reply, sig):
    """`T<sig>thread:<tid>;` -- the stop names the thread that stopped."""
    return re.fullmatch(rf"T{sig}thread:[0-9a-f]+;", reply) is not None


def elf_head(program, vaddr=0x8000000000, n=64):
    """n bytes a program's host ELF puts at `vaddr`, as `m` returns them --
    read from the PT_LOAD that covers it."""
    sys.path.insert(0, TOOLS)
    import panic_resolve
    elf = panic_resolve.elf_for_program(program)
    with open(elf, "rb") as f:
        data = f.read()
    phoff, = struct.unpack_from("<Q", data, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    for i in range(phnum):
        p_type, _fl, p_off, p_vaddr, _pa, p_filesz = struct.unpack_from(
            "<IIQQQQ", data, phoff + i * phentsize)
        if p_type == 1 and p_vaddr <= vaddr and vaddr + n <= p_vaddr + p_filesz:
            return data[p_off + vaddr - p_vaddr:p_off + vaddr - p_vaddr + n].hex()
    return None


def rip_of(g_reply):
    """rip from a `g` reply: 16 GPRs of 8 bytes precede it, little-endian."""
    return int.from_bytes(bytes.fromhex(g_reply[256:272]), "little")


# --- the run -----------------------------------------------------------------

NET = None   # the key, when --net


def vm(inst, disk, log, *args, timeout=240):
    port = ["--kdebug-net"] if NET else ["--kdebug"]
    cmd = VM + ["--instance", str(inst), "--disk", disk, "--serial-log", log] + port + list(args)
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


def connect(inst):
    if NET:
        return kdebug_bridge.UdpLink("127.0.0.1", 51000 + inst, NET)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path(inst))
    return s


def auth_silence(inst, res, label, datagram):
    """A datagram the stub must ignore: no reply, authentic or not."""
    probe = kdebug_bridge.UdpLink("127.0.0.1", 51000 + inst, NET)
    probe.send_raw(datagram)
    probe.settimeout(3.0)
    raw = b""
    try:
        raw, _ = probe.s.recvfrom(65536)
    except socket.timeout:
        pass
    probe.close()
    res.check(label, raw == b"", f"got {raw[:40]!r}")


def run(inst, disk, log, res):
    boot = vm(inst, disk, log, "start", timeout=300)
    if "ready" not in boot:
        res.check("the guest boots", False, boot[-400:] + serial(log)[-400:])
        return
    armed = ("kdebug: GDB stub armed on the network" if NET
             else "kdebug: GDB stub armed on ttyS2") in serial(log)
    res.check("the boot log says the stub is armed", armed,
              "\n".join(l for l in serial(log).splitlines() if "kdebug" in l)[-300:])

    if NET:
        wrong = bytes(b ^ 0xFF for b in NET)
        auth_silence(inst, res, "a datagram under the WRONG key gets silence",
                     kdebug_bridge.seal(wrong, b"TKDH", time.time_ns(), b"$?#3f"))

    link = connect(inst)
    rsp = Rsp(link)
    try:
        steps(inst, disk, log, res, rsp)
    except (TimeoutError, ConnectionError) as e:
        res.check("the protocol exchange completes", False, f"{type(e).__name__}: {e}")
    finally:
        rsp.close()

    if NET and link.first:
        auth_silence(inst, res, "an exact REPLAY of an accepted datagram gets silence", link.first)

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
    stop = rsp.cmd("?")
    res.check("the stop reason is SIGINT, with its thread", is_stop(stop, "02"), stop)

    threads = rsp.cmd("qfThreadInfo")
    res.check("qfThreadInfo lists the kernel context and pid 1",
              threads.startswith("m3e8,") and "1" in threads[5:].split(","), threads)
    extra = rsp.cmd("qThreadExtraInfo,1")
    name = bytes.fromhex(extra).decode(errors="replace") if re.fullmatch(r"[0-9a-f]+", extra) else extra
    res.check("qThreadExtraInfo names pid 1 `init`", name.startswith("init, "), name)
    # A thread that is NOT the one that stopped -- attach can land while
    # any process runs, init included.
    current = rsp.cmd("qC")[2:]
    other = next((t for t in threads[1:].split(",") if t != current), "1")
    ok = rsp.cmd(f"Hg{other}")
    parked = rsp.cmd("g")
    rsp.cmd("Hg1")
    init_head = rsp.cmd("m8000000000,40")
    rsp.cmd("Hg0")
    # Memory per thread: the same user address in two processes is two
    # programs' bytes, each checked against its own ELF on the host.
    names = {}
    for t in threads[1:].split(","):
        x = rsp.cmd(f"qThreadExtraInfo,{t}")
        if re.fullmatch(r"[0-9a-f]+", x):
            names[bytes.fromhex(x).decode(errors="replace").split(",")[0]] = t
    tosh_head = ""
    if "tosh" in names:
        rsp.cmd(f"Hg{names['tosh']}")
        tosh_head = rsp.cmd("m8000000000,40")
        rsp.cmd("Hg0")
    want_init = elf_head("/bin/init")
    want_tosh = elf_head("/bin/tosh")
    res.check("memory is read in the SELECTED thread's address space (init, tosh)",
              init_head == want_init and tosh_head == want_tosh and want_init != want_tosh,
              f"init {init_head[:24]} want {(want_init or '')[:24]}; tosh {tosh_head[:24]} "
              f"want {(want_tosh or '')[:24]}; threads {names}")
    res.check("`g` on a parked thread gives rip and rsp, and 'xx' for what was never saved",
              ok == "OK" and len(parked) == 328 and "xx" in parked and
              "x" not in parked[256:272], parked[:80])

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
    res.check("running `meminfo` stops at the breakpoint (T05)", is_stop(stop, "05"), stop)
    pc = rip_of(rsp.cmd("g"))
    res.check("...with the PC ON the breakpoint, not one past its int3", pc == bp,
              f"pc 0x{pc:x}, breakpoint 0x{bp:x}")

    # One instruction on. The breakpoint is still inserted; a step from it
    # must execute the REAL first instruction, not the int3.
    rsp.send("s")
    stop = rsp.recv(timeout=10)
    pc2 = rip_of(rsp.cmd("g"))
    res.check("a single step stops again one instruction on",
              is_stop(stop, "05") and bp < pc2 <= bp + 15, f"{stop}, pc 0x{pc2:x}")
    res.check("z0 removes it", rsp.cmd(f"z0,{bp:x},1") == "OK")

    # A hardware write watchpoint.
    w = symbol(WATCH_VAR) + delta
    res.check("Z2 (write watch, 8 bytes) is accepted", rsp.cmd(f"Z2,{w:x},8") == "OK")
    rsp.send("c")
    stop = rsp.recv(timeout=10)
    res.check("the tick's write to the counter fires it, and names the address",
              re.fullmatch(rf"T05thread:[0-9a-f]+;watch:{w:x};", stop) is not None, stop)
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
    res.check("an NMI from QEMU's monitor breaks in (T02)", is_stop(stop, "02"), stop)
    rsp.send("c")
    time.sleep(0.5)
    rsp.interrupt()
    stop = rsp.recv(timeout=10)
    res.check("a ^C byte breaks in (T02)", is_stop(stop, "02"), stop)

    res.check("D detaches", rsp.cmd("D") == "OK")
    try:
        out, _ = trigger.communicate(timeout=60)
    except subprocess.TimeoutExpired:
        trigger.kill()
        out = "(still stopped)"
    res.check("the `meminfo` that hit the breakpoint completes once resumed",
              trigger.returncode == 0 and "free" in out.lower(), out[-200:])


def real_gdb(inst, res):
    """A real GDB: symbols relocated by qOffsets, a disassembly by NAME --
    over the bridge, with --net."""
    target, bridge = sock_path(inst), None
    if NET:
        lport = 1300 + inst
        bridge = subprocess.Popen(
            [sys.executable, os.path.join(TOOLS, "kdebug_bridge.py"),
             "--target", f"127.0.0.1:{51000 + inst}", "--listen", f"127.0.0.1:{lport}",
             "--key", NET.hex()], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        bridge.stdout.readline()   # listening
        target = f"localhost:{lport}"
    try:
        real_gdb_run(target, res)
    finally:
        if bridge:
            bridge.kill()
            bridge.wait()


def real_gdb_run(target, res):
    r = subprocess.run(
        ["gdb", "-nx", "-batch", "-x", os.path.join(TOOLS, "gdb", "toyos.py"), KERNEL,
         "-ex", "set pagination off",
         "-ex", f"target remote {target}",
         "-ex", f"x/2i {BP_FUNC}",
         "-ex", "info registers rip",
         "-ex", "echo @@helpers\\n",
         "-ex", "toy-ps",
         "-ex", "toy-dmesg 3",
         "-ex", "info threads",
         "-ex", "thread 2",
         "-ex", "bt",
         "-ex", "detach"],
        cwd=REPO, capture_output=True, text=True, timeout=120)
    out = r.stdout + r.stderr
    # Each check reads its own part: a bt that walks into garbage must
    # not fail the disassembly check with its "Cannot access memory".
    head = out.split("@@helpers")[0]
    dis = [l for l in head.splitlines() if f"<{BP_FUNC}" in l]
    # ATTACHED, not read off the file: GDB disassembles kernel.bin from disk
    # when the connection fails, so the live register read is the proof.
    live = re.search(r"^rip\s+0x[0-9a-f]+", head, re.M) is not None
    res.check("a real GDB attaches and disassembles a function by name",
              live and len(dis) >= 2 and "Cannot access memory" not in head
              and "no registers" not in head, head[-600:])
    res.check("...and detaches cleanly", "Detaching" in out or "detached" in out.lower(), out[-300:])
    res.check("toy-ps lists pid 1 as init", re.search(r"^\s+1\s+0\s+\S+\s+init$", out, re.M) is not None,
              out[:600])
    res.check("toy-dmesg prints the log ring", re.search(r"^\[\s*\d+\.\d+\]", out, re.M) is not None,
              out[:600])
    res.check("info threads shows the processes by name",
              "Thread 1000 (kernel" in out and "(init, " in out, out[:900])
    # thread 2 is the first process: parked in a syscall, so its stack
    # runs down through isr_common into ring 3 -- and stops there.
    res.check("a parked thread's bt goes PAST isr_common and stops at the user frame",
              "isr_common" in out and "syscall_dispatch" in out and
              "Backtrace stopped: frame did not save the PC" in out, out[-900:])

    # toy-symbols: a module, then a dynamic program and its libraries.
    tosh = re.search(r"^\*?\s*(\d+)\s+Thread \d+ \(tosh,", out, re.M)
    r = subprocess.run(
        ["gdb", "-nx", "-batch", "-x", os.path.join(TOOLS, "gdb", "toyos.py"), KERNEL,
         "-ex", "set pagination off",
         "-ex", f"target remote {target}",
         "-ex", "info threads",
         "-ex", f"thread {tosh.group(1) if tosh else 2}",
         "-ex", "toy-symbols",
         "-ex", "x/2i e1000_transmit",
         "-ex", "bt",
         "-ex", "detach"],
        cwd=REPO, capture_output=True, text=True, timeout=120)
    sym = r.stdout + r.stderr
    mod = re.search(r"module e1000 at (0x[0-9a-f]+)", sym)
    dis = re.search(r"^\s*(0x[0-9a-f]+) <e1000_transmit>:", sym, re.M)
    res.check("toy-symbols loads a module: e1000_transmit disassembles by name, inside it",
              mod is not None and dis is not None and int(dis.group(1), 16) >= int(mod.group(1), 16),
              sym[-700:])
    res.check("...and a dynamic program with its /lib libraries: tosh's bt reaches its main",
              tosh is not None and "/lib/libc.so at" in sym and "isr_common" in sym and
              re.search(r" main \(.*tosh\.c:\d+", sym) is not None, sym[-900:])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--instance", type=int, default=6)
    ap.add_argument("--disk", default=os.path.join(REPO, "disk.img"),
                    help="seed image to COPY (never written to directly)")
    ap.add_argument("--net", action="store_true",
                    help="the network transport: a debugger-owned e1000 and keyed UDP")
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
    global NET
    word = "kdebug=ttyS2"
    if args.net:
        NET = os.urandom(32)
        word = f"kdebug=net,ip=10.0.2.15,key={NET.hex()}"
    if not args.positive_control:
        ok, why = install_grub.add_boot_word(disk, word)
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
