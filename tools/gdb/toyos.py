"""GDB helpers for a stopped toy-os kernel -- Linux's scripts/gdb shape.

    gdb build/kernel.bin -x tools/gdb/toyos.py -ex "target remote localhost:1235"

Everything here READS the stopped machine's memory from the host, so the
kernel runs no code for it -- the reason Linux (`lx-dmesg`, `lx-ps`) and
WinDbg (`!process`) put these on the debugger's side too:

  toy-dmesg [N]   the last N lines of the kernel log ring (default 40)
  toy-ps          the process table: pid, state, name -- `info threads`
                  shows the same processes as threads you can `thread N`
  toy-symbols     symbols for what kernel.bin does not hold: every loaded
                  kernel MODULE, and the SELECTED thread's program,
                  ld-toy and /lib libraries -- run it again after
                  `thread N` to switch programs
  (unwinder)      `bt` goes on PAST isr_common: the trap frame isr.asm
                  pushed is read, and the frame it interrupted is next.
                  A ring-3 frame ends it (user code has no symbols
                  here), and so does kernel_main (boot.asm has no CFI).

It works over QEMU's own stub (`make debug`) as well as the kernel's.
"""

import os
import re
import struct
import sys

import gdb
import gdb.unwinder

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
import panic_resolve  # noqa: E402  -- elf_for_program(), the repo's one name->ELF map

# Memory now depends on the SELECTED thread (the stub reads through its
# page tables), so nothing cached for one thread may answer for another.
gdb.execute("set stack-cache off")
gdb.execute("set code-cache off")

# isr.asm's frame, in uint64_t slots from its lowest address: r15 first.
FRAME = ["r15", "r14", "r13", "r12", "r11", "r10", "r9", "r8",
         "rbp", "rdi", "rsi", "rdx", "rcx", "rbx", "rax",
         "vector", "error", "rip", "cs", "rflags", "rsp", "ss"]
STATES = {0: "unused", 1: "ready", 2: "running", 3: "zombie", 4: "blocked"}


def addr(name):
    return int(gdb.parse_and_eval(f"(unsigned long)&{name}"))


class Dmesg(gdb.Command):
    """toy-dmesg [N]: the last N lines of the stopped kernel's log ring."""

    def __init__(self):
        super().__init__("toy-dmesg", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        n = int(arg) if arg.strip() else 40
        size = int(gdb.parse_and_eval("sizeof(klog_buf)"))
        head = int(gdb.parse_and_eval("klog_head"))
        count = int(gdb.parse_and_eval("klog_count"))
        raw = bytes(gdb.selected_inferior().read_memory(addr("klog_buf"), size))
        start = (head - count) % size
        text = (raw[start:] + raw[:start])[:count] if count == size else raw[start:start + count]
        # A NUL in the ring (a torn write, an unused tail) must not end the
        # command: gdb refuses to print a string that carries one.
        lines = text.replace(b"\0", b"").decode("utf-8", "replace").splitlines()
        for line in lines[-n:]:
            print(line)


class Ps(gdb.Command):
    """toy-ps: the stopped kernel's process table."""

    def __init__(self):
        super().__init__("toy-ps", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        procs = gdb.parse_and_eval("procs")
        cur = int(gdb.parse_and_eval("current_index"))
        n = int(procs.type.range()[1]) + 1
        print(f"{'PID':>5} {'PPID':>5}  {'STATE':<8} NAME")
        if cur < 0:
            print(f"{'-':>5} {'-':>5}  {'running':<8} (the kernel context)")
        for i in range(n):
            p = procs[i]
            st = int(p["state"])
            if st == 0:
                continue
            name = p["name"].string(errors="replace")
            state = "running" if i == cur else STATES.get(st, str(st))
            if int(p["stopped"]):
                state += ",T"
            # The slot's own pid field: a pid is not a slot index.
            print(f"{int(p['pid']):>5} {int(p['ppid']):>5}  {state:<8} {name}")


def slot_of(procs, pid):
    """The slot whose pid field is `pid`, or None -- a pid is not an index."""
    for i in range(int(procs.type.range()[1]) + 1):
        if int(procs[i]["state"]) != 0 and int(procs[i]["pid"]) == pid:
            return procs[i]
    return None


def elf_sections(path):
    """(name, flags, size, addralign) for every section, in index order."""
    with open(path, "rb") as f:
        d = f.read()
    shoff, = struct.unpack_from("<Q", d, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x3A)
    hdrs = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize) for i in range(shnum)]
    stroff = hdrs[shstrndx][4]
    out = []
    for h in hdrs:
        name = d[stroff + h[0]:d.index(b"\0", stroff + h[0])].decode()
        out.append((name, h[2], h[5], h[8]))
    return out


def module_layout(sections, base):
    """Where module.c's layout() put each section -- the kernel keeps only
    the base, so the host repeats the same two passes: executable
    sections first from offset 0, then the rest of SHF_ALLOC after the
    page-rounded text, each in index order at its own alignment."""
    ALLOC, EXEC, PAGE = 0x2, 0x4, 4096
    addrs, off = {}, 0
    for pass_ in (0, 1):
        for name, flags, size, align in sections:
            if not flags & ALLOC or size == 0 or bool(flags & EXEC) != (pass_ == 0):
                continue
            a = align or 1
            off = (off + a - 1) & ~(a - 1)
            addrs[name] = base + off
            off += size
        if pass_ == 0:
            off = (off + PAGE - 1) & ~(PAGE - 1)
    return addrs


class Symbols(gdb.Command):
    """toy-symbols: load symbols for the loaded kernel modules and the
    selected thread's program, ld-toy and shared libraries."""

    def __init__(self):
        super().__init__("toy-symbols", gdb.COMMAND_USER)
        self.modules = set()     # (object, base) already loaded
        self.user = []           # files loaded for the last program

    def add(self, cmd):
        gdb.execute("set confirm off")
        try:
            gdb.execute(cmd, to_string=True)
        finally:
            gdb.execute("set confirm on")

    def load_modules(self):
        mods = gdb.parse_and_eval("g_mods")
        used = gdb.parse_and_eval("g_mod_used")
        for k in range(int(mods.type.range()[1]) + 1):
            if not int(used[k]):
                continue
            name = mods[k]["name"].string()
            base = int(mods[k]["base"])
            obj = os.path.join(REPO, "build", "modules", name + ".o")
            if (obj, base) in self.modules:
                continue
            if not os.path.isfile(obj):
                print(f"toy-symbols: module {name}: no {obj}")
                continue
            addrs = module_layout(elf_sections(obj), base)
            text = addrs.pop(".text", None)
            if text is None:
                print(f"toy-symbols: module {name}: no .text")
                continue
            self.add(f"add-symbol-file {obj} {text:#x} " +
                     " ".join(f"-s {n} {a:#x}" for n, a in addrs.items()))
            self.modules.add((obj, base))
            print(f"toy-symbols: module {name} at {base:#x}")

    def load_process(self):
        for f in self.user:
            try:
                gdb.execute(f"remove-symbol-file {f}", to_string=True)
            except gdb.error:
                pass
        self.user = []
        thread = gdb.selected_thread()
        tid = (thread.ptid[1] or thread.ptid[2]) if thread else 0
        if not tid or tid >= 1000:
            print("toy-symbols: the kernel context has no user program")
            return
        procs = gdb.parse_and_eval("procs")
        p = slot_of(procs, tid)
        if p is None:
            print(f"toy-symbols: no process with pid {tid}")
            return
        path = p["exec_path"].string()
        elf = panic_resolve.elf_for_program(path)
        if not elf:
            print(f"toy-symbols: pid {tid} ({path}): no ELF under build/userland")
            return
        self.add(f"add-symbol-file {elf} -o 0")   # ET_EXEC, at its link address
        self.user.append(elf)
        print(f"toy-symbols: pid {tid} {path} <- {os.path.relpath(elf, REPO)}")
        # Libraries: a file mapping of /lib/X.so at file offset 0 is where
        # its first PT_LOAD -- vaddr 0 -- went, which makes it the bias.
        leader = slot_of(procs, int(p["tgid"])) or p
        mm = leader["mm"]
        regions = mm["regions"]
        if int(regions) == 0:
            return
        ldso = os.path.join(REPO, "build", "lib", "ld-toy.so")
        if os.path.isfile(ldso):
            self.add(f"add-symbol-file {ldso} -o 0")   # a fixed-base ET_EXEC too
            self.user.append(ldso)
        for j in range(int(mm["region_cap"])):
            r = regions[j]
            base, kind = int(r["base"]), int(r["kind"])
            lib = r["path"].string()
            if not base or kind != 1 or int(r["file_off"]) or not lib.endswith(".so"):
                continue
            host = os.path.join(REPO, "build", lib.lstrip("/"))
            if not os.path.isfile(host) or host in self.user:
                continue
            self.add(f"add-symbol-file {host} -o {base:#x}")
            self.user.append(host)
            print(f"toy-symbols:   {lib} at {base:#x}")

    def invoke(self, arg, from_tty):
        self.load_modules()
        self.load_process()


class FrameId:
    def __init__(self, sp, pc):
        self.sp, self.pc = sp, pc


class TrapFrameUnwinder(gdb.unwinder.Unwinder):
    """isr_common has no CFI, so GDB stops there. From its `mov rdi, rsp`
    (the frame just completed) to isr_return_to, and at isr_resume_frame
    (where a context that never ran starts), RSP points AT the pushed
    frame -- read it and hand GDB the interrupted code's registers.

    NOT FROM isr_common ITSELF: during the pushes the frame is half
    built, and an NMI or a step that stops there would be handed
    garbage registers as the interrupted code's."""

    def __init__(self):
        super().__init__("toy-os trap frame")
        self.lo = self.hi = self.resume = None
        # (sp, pc) of every RING-3 frame this produced. User code has no
        # symbols here, so GDB's fallback would walk its stack into
        # garbage; asked about one of these, this ends the backtrace.
        self.user = set()

    def _bounds(self, arch=None):
        if self.lo is None:
            lo, hi = addr("isr_common"), addr("isr_return_to")
            arch = arch or gdb.selected_inferior().architecture()
            ready = [i["addr"] for i in arch.disassemble(lo, hi - 1)
                     if re.search(r"\bmov\s+(%rsp,\s*%rdi|rdi,\s*rsp)\b", i["asm"])]
            # Not found: claim no part of isr_common rather than guess.
            self.lo, self.hi = (ready[0], hi) if ready else (hi, hi)
            self.resume = addr("isr_resume_frame")

    def claims(self, pc, arch=None):
        """Whether RSP at `pc` points at a whole trap frame."""
        self._bounds(arch)
        return self.lo <= pc < self.hi or pc == self.resume

    # A ring-3 frame goes on only once toy-symbols has loaded its program:
    # without them GDB's fallback walks the user stack into garbage.
    @staticmethod
    def _has_symbols(pc):
        try:
            return gdb.current_progspace().objfile_for_address(pc) is not None
        except (AttributeError, gdb.error):
            return gdb.block_for_pc(pc) is not None

    # kernel_main's caller is boot.asm, with no CFI: stop there too.
    @staticmethod
    def _is_entry(pc):
        block = gdb.block_for_pc(pc)
        fn = block.function if block else None
        while block and not fn:
            block = block.superblock
            fn = block.function if block else None
        return fn is not None and fn.name == "kernel_main"

    def __call__(self, pending):
        pc = int(pending.read_register("rip"))
        sp = int(pending.read_register("rsp"))
        try:
            ours = self.claims(pc, pending.architecture())
        except gdb.error:
            return None
        if ((sp, pc) in self.user and not self._has_symbols(pc)) or self._is_entry(pc):
            return pending.create_unwind_info(FrameId(sp, pc))   # no saved PC: stop
        if not ours:
            return None
        mem = gdb.selected_inferior().read_memory(sp, 8 * len(FRAME))
        v = dict(zip(FRAME, (int.from_bytes(bytes(mem[i * 8:i * 8 + 8]), "little")
                             for i in range(len(FRAME)))))
        if v["cs"] & 3:
            self.user.add((v["rsp"], v["rip"]))
        info = pending.create_unwind_info(FrameId(sp, pc))
        for r in ("rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
                  "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rip"):
            info.add_saved_register(r, gdb.Value(v[r]).cast(pending.read_register(r).type))
        return info


Dmesg()
Ps()
Symbols()
TRAP_UNWINDER = TrapFrameUnwinder()
gdb.unwinder.register_unwinder(None, TRAP_UNWINDER, replace=True)
