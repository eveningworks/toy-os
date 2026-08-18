// The machine's own syscalls: the clock, PCI, the CPU, entropy, the
// settings registry, system information, the console's colours, power,
// and the deliberate-fault gate.
//
// What unites them is that none of them is about a process, a file or a
// window -- they answer questions about, or act on, the machine.
#include "syscalls.h"
#include "syscall_abi.h"
#include "setting_abi.h"
#include "klog.h"
#include "kfmt.h"      // klog_printf
#include "vmm.h"
#include "scheduler.h"
#include "vga.h"
#include "pmm.h"
#include "fs.h"
#include "pci.h"
#include "cpuinfo.h"
#include "krandom.h"
#include "setting.h"
#include "power.h"     // SYS_POWEROFF -- the desktop's shut down/restart
#include "crashtest.h" // SYS_CRASHTEST -- deliberate faults, see crash_abi.h
#include "tz.h"
#include "string.h"
#include <stddef.h>

SYSCALL_HANDLER sys_do_getrandom(uint64_t *regs, uint64_t pml4,
                                  uint64_t dst, uint64_t len) {
    // Into a kernel buffer, then out -- krandom_bytes() must not write
    // through a ring-3 pointer (vmm.h).
    //
    // IN CHUNKS, and that is the whole point of this function existing.
    // A single bounce buffer here is SYS_GETRANDOM_MAX -- 4096 bytes --
    // and it lived on syscall_dispatch()'s frame, which made EVERY
    // syscall, including a bare yield, pay 4 KiB of a 16 KiB per-process
    // kernel stack before its handler ran. It was that frame's whole
    // 4832 bytes, near enough: every other local in the dispatcher
    // overlapped in its shadow, which is why removing a kilobyte of
    // other buffers changed the total by nothing.
    //
    // A chunk costs a few more krandom_bytes() calls on a big request
    // and bounds the cost at 256 bytes wherever this ends up inlined or
    // called from.
    unsigned char chunk[256];
    uint64_t done = 0;
    while (done < len) {
        uint64_t n = len - done;
        if (n > sizeof chunk) n = sizeof chunk;
        krandom_bytes(chunk, (size_t)n);
        // The range was validated as a whole before the loop, so a
        // partial copy here means the mapping changed underneath us --
        // report the failure rather than the count.
        if (!vmm_copy_to_user(pml4, dst + done, chunk, n)) {
            regs[14] = (uint64_t)-1;
            return;
        }
        done += n;
    }
    regs[14] = len;
}

SYSCALL_HANDLER sys_do_setting(uint64_t *regs, uint64_t rdi) {
    uint64_t pml4 = vmm_current_pml4();
    struct setting_msg msg;
    if (!vmm_validate_user_range(pml4, rdi, sizeof msg)) {
        klog_write("syscall: setting() rejected -- invalid user pointer\n");
        regs[14] = (uint64_t)-1;
    } else {
        vmm_copy_from_user(pml4, &msg, rdi, sizeof msg);
        int ok = setting_dispatch(&msg);
        vmm_copy_to_user(pml4, rdi, &msg, sizeof msg); // validated above
        regs[14] = ok ? 0 : (uint64_t)-1;
    }
}

int sys_gettime(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    if (!vmm_validate_user_range(pml4, c->a0, sizeof(struct rtc_time))) {
        klog_write("syscall: gettime() rejected -- invalid pointer\n");
        c->regs[14] = 0;
    } else {
        struct rtc_time t;
        rtc_read_local(&t);
        vmm_copy_to_user(pml4, c->a0, &t, sizeof t); // range validated just above
        c->regs[14] = 1;
    }
    return 0;
}

int sys_pci_count(struct syscall_ctx *c) {
    c->regs[14] = (uint64_t)pci_device_count();
    return 0;
}

int sys_pci_info(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    int index = (int)c->a0;
    const struct pci_device *dev = pci_device_at(index);
    if (!dev || !vmm_validate_user_range(pml4, c->a1, sizeof(struct pci_device))) {
        klog_write("syscall: pci_info() rejected -- bad index or invalid pointer\n");
        c->regs[14] = (uint64_t)-1;
    } else {
        vmm_copy_to_user(pml4, c->a1, dev, sizeof *dev); // range validated just above
        c->regs[14] = 1;
    }
    return 0;
}

int sys_cpu_info(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    if (!vmm_validate_user_range(pml4, c->a0, sizeof(struct cpu_info))) {
        klog_write("syscall: cpu_info() rejected -- invalid user pointer\n");
        c->regs[14] = (uint64_t)-1;
    } else {
        struct cpu_info ci;
        cpu_info_get(&ci);
        vmm_copy_to_user(pml4, c->a0, &ci, sizeof ci); // range validated just above
        c->regs[14] = 1;
    }
    return 0;
}

int sys_getrandom(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    if (c->a1 > SYS_GETRANDOM_MAX) {
        klog_write("syscall: getrandom() rejected -- count over SYS_GETRANDOM_MAX\n");
        c->regs[14] = (uint64_t)-1;
    } else if (c->a1 == 0) {
        // A zero-length request is a legal no-op, NOT an error --
        // validating a zero-length range would reject a NULL
        // pointer that is never going to be dereferenced.
        c->regs[14] = 0;
    } else if (!vmm_validate_user_range(pml4, c->a0, c->a1)) {
        klog_write("syscall: getrandom() rejected -- invalid user pointer\n");
        c->regs[14] = (uint64_t)-1;
    } else {
        sys_do_getrandom(c->regs, pml4, c->a0, c->a1);
    }
    return 0;
}

int sys_setting(struct syscall_ctx *c) {
    sys_do_setting(c->regs, c->a0);
    return 0;
}

int sys_sysinfo(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    struct sys_info info;
    if (!vmm_validate_user_range(pml4, c->a0, sizeof info)) {
        klog_write("syscall: sysinfo() rejected -- invalid user pointer\n");
        c->regs[14] = (uint64_t)-1;
    } else {
        uint64_t used = 0, total = 0;
        k_memset(&info, 0, sizeof info);
        info.mem_free_kb  = (uint64_t)pmm_free_frames() * 4;
        info.mem_total_kb = (uint64_t)pmm_total_frames() * 4;
        if (fs_disk_usage(&used, &total)) {
            info.disk_used_bytes = used;
            info.disk_total_bytes = total;
            info.flags |= SYS_INFO_DISK_VALID;
        }
        vmm_copy_to_user(pml4, c->a0, &info, sizeof info); // validated above
        c->regs[14] = 0;
    }
    return 0;
}

int sys_poweroff(struct syscall_ctx *c) {
    // The desktop's Start-menu Shut down / Restart. Unprivileged for
    // the same reason SYS_KILL is: there is no user model here to
    // gate it on, so a gate would be decoration -- anything that can
    // spawn a process can already end the session.
    //
    // Neither call returns on success, so there is no "it worked" to
    // report; the only outcome that reaches the line below is an op
    // we do not know, or a platform that refused to stop.
    klog_printf("syscall: poweroff(%d) by pid %d\n",
                (int)c->a0, scheduler_current_pid());
    if (c->a0 == 0) {
        system_poweroff();
    } else if (c->a0 == 1) {
        system_reboot();
    }
    c->regs[14] = (uint64_t)-1;
    return 0;
}

int sys_crashtest(struct syscall_ctx *c) {
    // Deliberate faults, for testing the panic path (crash_abi.h).
    // Gated on `faultinject`; the LIST is always readable so a UI
    // can show the kinds and report the refusal rather than
    // presenting an empty window.
    uint64_t pml4 = c->pml4;
    struct crash_msg m;
    if (!vmm_copy_from_user(pml4, &m, c->a0, sizeof m)) {
        c->regs[14] = (uint64_t)-1;
    } else if (m.op == CRASH_OP_LIST) {
        m.count = crash_kind_count();
        m.flags = crash_armed() ? CRASH_F_ARMED : 0;
        m.name[0] = m.desc[0] = '\0';
        const struct crash_kind *k = crash_kind_at(m.index);
        if (k) {
            k_strlcpy(m.name, k->name, sizeof m.name);
            k_strlcpy(m.desc, k->desc, sizeof m.desc);
        }
        vmm_copy_to_user(pml4, c->a0, &m, sizeof m);
        c->regs[14] = 0;
    } else if (m.op == CRASH_OP_TRIGGER) {
        // Does not return when it works -- the machine panics.
        int ok = crash_trigger(m.index);
        m.flags = crash_armed() ? CRASH_F_ARMED : 0;
        m.count = crash_kind_count();
        vmm_copy_to_user(pml4, c->a0, &m, sizeof m);
        c->regs[14] = ok ? 0 : (uint64_t)-1;
    } else {
        c->regs[14] = (uint64_t)-1;
    }
    return 0;
}

int sys_set_color(struct syscall_ctx *c) {
    if (c->a0 > VGA_WHITE || c->a1 > VGA_WHITE) {
        c->regs[14] = (uint64_t)-1;
    } else {
        vga_set_color((enum vga_color)c->a0, (enum vga_color)c->a1);
        c->regs[14] = 1;
    }
    return 0;
}
