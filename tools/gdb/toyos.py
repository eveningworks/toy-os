"""GDB helpers for a stopped toy-os kernel -- Linux's scripts/gdb shape.

    gdb build/kernel.bin -x tools/gdb/toyos.py -ex "target remote localhost:1235"

Everything here READS the stopped machine's memory from the host, so the
kernel runs no code for it -- the reason Linux (`lx-dmesg`, `lx-ps`) and
WinDbg (`!process`) put these on the debugger's side too:

  toy-dmesg [N]   the last N lines of the kernel log ring (default 40)
  toy-ps          the process table: pid, state, name -- `info threads`
                  shows the same processes as threads you can `thread N`
  (unwinder)      `bt` goes on PAST isr_common: the trap frame isr.asm
                  pushed is read, and the frame it interrupted is next.
                  A ring-3 frame ends it (user code has no symbols
                  here), and so does kernel_main (boot.asm has no CFI).

It works over QEMU's own stub (`make debug`) as well as the kernel's.
"""

import gdb
import gdb.unwinder

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
        lines = text.decode("utf-8", "replace").splitlines()
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
        print(f"{'PID':>4} {'PPID':>4}  {'STATE':<8} NAME")
        if cur < 0:
            print(f"{'-':>4} {'-':>4}  {'running':<8} (the kernel context)")
        for i in range(n):
            p = procs[i]
            st = int(p["state"])
            if st == 0:
                continue
            name = p["name"].string(errors="replace")
            state = "running" if i == cur else STATES.get(st, str(st))
            if int(p["stopped"]):
                state += ",T"
            print(f"{i + 1:>4} {int(p['ppid']):>4}  {state:<8} {name}")


class FrameId:
    def __init__(self, sp, pc):
        self.sp, self.pc = sp, pc


class TrapFrameUnwinder(gdb.unwinder.Unwinder):
    """isr_common has no CFI, so GDB stops there. At the return address
    of its `call isr_dispatch`, and at isr_resume_frame (where a context
    that never ran starts), RSP points AT the pushed frame -- read it and
    hand GDB the interrupted code's registers."""

    def __init__(self):
        super().__init__("toy-os trap frame")
        self.lo = self.hi = self.resume = None
        # (sp, pc) of every RING-3 frame this produced. User code has no
        # symbols here, so GDB's fallback would walk its stack into
        # garbage; asked about one of these, this ends the backtrace.
        self.user = set()

    def _bounds(self):
        if self.lo is None:
            self.lo = addr("isr_common")
            self.hi = addr("isr_return_to")
            self.resume = addr("isr_resume_frame")

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
        try:
            self._bounds()
        except gdb.error:
            return None
        pc = int(pending.read_register("rip"))
        sp = int(pending.read_register("rsp"))
        if (sp, pc) in self.user or self._is_entry(pc):
            return pending.create_unwind_info(FrameId(sp, pc))   # no saved PC: stop
        if not (self.lo <= pc < self.hi or pc == self.resume):
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
gdb.unwinder.register_unwinder(None, TrapFrameUnwinder(), replace=True)
