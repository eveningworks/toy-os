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
#include "errno.h"
#include "setting.h"
#include "query.h"     // SYS_QUERY -- the fact registry
#include "power.h"     // SYS_POWEROFF -- the desktop's shut down/restart
#include "crashtest.h" // SYS_CRASHTEST -- deliberate faults, see crash_abi.h
#include "tz.h"
#include "ktime.h"
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
            regs[14] = (uint64_t)(int64_t)-EFAULT;
            return;
        }
        done += n;
    }
    regs[14] = len;
}

SYSCALL_HANDLER sys_do_setting(uint64_t *regs, uint64_t rdi) {
    uint64_t pml4 = vmm_current_pml4();
    struct setting_msg msg;
    if (!vmm_copy_from_user(pml4, &msg, rdi, sizeof msg)) {
        klog_write("syscall: setting() rejected -- invalid user pointer\n");
        regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else {
        int ok = setting_dispatch(&msg);
        if (!vmm_copy_to_user(pml4, rdi, &msg, sizeof msg)) ok = 0; // a bad range reads as a refusal
        // setting_dispatch() answers yes or no. EINVAL covers both of
        // its refusals -- an unknown setting and a value it will not
        // accept -- because it does not distinguish them either, and a
        // code invented here would be a claim this kernel cannot make.
        regs[14] = ok ? 0 : (uint64_t)(int64_t)-EINVAL;
    }
}

int sys_gettime(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    // UTC. Converting to a local time is ring 3's (userland/lib/utz.h);
    // the kernel has not known a zone since the database left it.
    struct rtc_time t;
    ktime_read(&t);
    if (!vmm_copy_to_user(pml4, c->a0, &t, sizeof t)) {
        klog_write("syscall: gettime() rejected -- invalid pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else {
        c->regs[14] = 0;
    }
    return 0;
}

int sys_settime(struct syscall_ctx *c) {
    // NO PERMISSION CHECK, and that is a gap rather than a decision:
    // there is one user here and no capability model, so any process can
    // move the clock. Linux gates this behind CAP_SYS_TIME. When
    // docs/roadmap.md's multi-user work lands, this is one of the calls
    // that grows a check.
    if (!ktime_set(c->a0, (uint32_t)c->a1)) {
        c->regs[14] = (uint64_t)(int64_t)-EINVAL;
        return 0;
    }
    // LOGGED, because a clock stepping under a running system explains
    // otherwise inexplicable things -- a timeout that fired instantly, a
    // file whose mtime is in the future -- and `dmesg` is where somebody
    // looks. Real kernels log the same event for the same reason.
    klog_printf("ktime: clock stepped by %lld s\n", (long long)ktime_last_step());
    c->regs[14] = 0;
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
    if (!dev || !vmm_copy_to_user(pml4, c->a1, dev, sizeof *dev)) {
        klog_write("syscall: pci_info() rejected -- bad index or invalid pointer\n");
        c->regs[14] = (uint64_t)(int64_t)(dev ? -EFAULT : -EINVAL);
    } else {
        c->regs[14] = 0;
    }
    return 0;
}

int sys_cpu_info(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    struct cpu_info ci;
    cpu_info_get(&ci);
    if (!vmm_copy_to_user(pml4, c->a0, &ci, sizeof ci)) {
        klog_write("syscall: cpu_info() rejected -- invalid user pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else {
        c->regs[14] = 0;
    }
    return 0;
}

int sys_getrandom(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    if (c->a1 > SYS_GETRANDOM_MAX) {
        klog_write("syscall: getrandom() rejected -- count over SYS_GETRANDOM_MAX\n");
        c->regs[14] = (uint64_t)(int64_t)-EINVAL;
    } else if (c->a1 == 0) {
        // A zero-length request is a legal no-op, NOT an error --
        // validating a zero-length range would reject a NULL
        // pointer that is never going to be dereferenced.
        c->regs[14] = 0;
    } else if (!vmm_validate_user_range(pml4, c->a0, c->a1)) {
        klog_write("syscall: getrandom() rejected -- invalid user pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
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
    uint64_t used = 0, total = 0;
    k_memset(&info, 0, sizeof info);
    // THROUGH THE REGISTRY, not straight to pmm. This syscall and
    // the kernel shell's `meminfo` and /bin/meminfo are now one
    // reader rather than three that agree -- which is the property
    // that makes them unable to drift, instead of a test that has
    // to notice afterwards when they have.
    //
    // The KB conversion derives the frame size rather than assuming
    // 4096, which is what the `* 4` here used to do.
    struct query_meminfo mem;
    if (query_read(QUERY_MEMINFO, 0, &mem, sizeof mem) > 0) {
        info.mem_free_kb  = mem.frame_free * mem.frame_bytes / 1024;
        info.mem_total_kb = mem.frame_total * mem.frame_bytes / 1024;
    }
    if (fs_disk_usage(&used, &total)) {
        info.disk_used_bytes = used;
        info.disk_total_bytes = total;
        info.flags |= SYS_INFO_DISK_VALID;
    }
    if (!vmm_copy_to_user(pml4, c->a0, &info, sizeof info)) {
        klog_write("syscall: sysinfo() rejected -- invalid user pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else {
        c->regs[14] = 0;
    }
    return 0;
}

int sys_query(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    struct query_msg msg;
    if (!vmm_copy_from_user(pml4, &msg, c->a0, sizeof msg)) {
        klog_write("syscall: query() rejected -- invalid message pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }

    int err = 0;
    switch (msg.op) {
    case QUERY_OP_RECORD: {
        // The record is filled in KERNEL memory and copied out, never
        // written through the user pointer (vmm.h). QUERY_RECORD_MAX
        // bounds the stack cost -- this kernel has a frame budget and a
        // guard page, so an unbounded local here is exactly the shape
        // -Wframe-larger-than exists to catch.
        uint8_t rec[QUERY_RECORD_MAX];
        int n = query_read(msg.cls, (int)msg.index, rec, sizeof rec);
        if (n < 0) { err = n; break; }

        // min(len, record) -- the version tolerance the ABI promises. A
        // client built against an older, shorter struct gets what it
        // asked for and `returned` says how much, rather than the call
        // failing because the kernel's struct grew.
        uint32_t want = (uint32_t)n;
        if (msg.len < want) want = msg.len;
        if (!want || !vmm_copy_to_user(pml4, msg.buf, rec, want)) {
            err = -EFAULT;
            break;
        }
        msg.returned = want;
        break;
    }
    case QUERY_OP_FIELD_COUNT: {
        const struct query_provider *p = query_find(msg.cls);
        if (!p) { err = -ENOENT; break; }
        msg.returned = p->field_count;
        break;
    }
    case QUERY_OP_FIELD_INFO: {
        const struct query_provider *p = query_find(msg.cls);
        if (!p) { err = -ENOENT; break; }
        if (msg.index >= p->field_count) { err = -ERANGE; break; }
        // The NAME and the TYPE cross; the offset does not. See
        // api/query.h -- that is what keeps a record's layout free to
        // grow append-only without any client caring.
        k_strlcpy(msg.name, p->fields[msg.index].name, sizeof msg.name);
        msg.type = p->fields[msg.index].type;
        break;
    }
    case QUERY_OP_FIELD_GET: {
        // The caller's `name` came out of user memory in the copy above,
        // so terminate it rather than trusting it -- a name with no NUL
        // would run k_strcmp off the end of the message.
        msg.name[sizeof msg.name - 1] = '\0';
        uint64_t value = 0;
        uint32_t type = 0;
        err = query_field_get(msg.name, &value, &type);
        if (err) break;
        msg.value = value;
        msg.type = type;
        break;
    }
    default:
        klog_printf("syscall: query() rejected -- unknown op %u\n", msg.op);
        err = -EINVAL;
        break;
    }

    if (err) {
        c->regs[14] = (uint64_t)(int64_t)err;
        return 0;
    }
    // Copied back whole: every op writes at least one out field, and
    // writing the message back in one place means no op can forget to.
    if (!vmm_copy_to_user(pml4, c->a0, &msg, sizeof msg)) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    c->regs[14] = 0;
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
    // Only reached by an op we do not know, or a platform that refused
    // to stop. EINVAL for the first; the second is EIO in spirit and
    // indistinguishable here, since system_poweroff() does not report.
    c->regs[14] = (uint64_t)(int64_t)(c->a0 > 1 ? -EINVAL : -EIO);
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
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else if (m.op == CRASH_OP_LIST) {
        m.count = crash_kind_count();
        m.flags = crash_armed() ? CRASH_F_ARMED : 0;
        m.name[0] = m.desc[0] = '\0';
        const struct crash_kind *k = crash_kind_at(m.index);
        if (k) {
            k_strlcpy(m.name, k->name, sizeof m.name);
            k_strlcpy(m.desc, k->desc, sizeof m.desc);
        }
        c->regs[14] = vmm_copy_to_user(pml4, c->a0, &m, sizeof m) ? 0 : (uint64_t)(int64_t)-EFAULT;
    } else if (m.op == CRASH_OP_TRIGGER) {
        // Does not return when it works -- the machine panics.
        int ok = crash_trigger(m.index);
        m.flags = crash_armed() ? CRASH_F_ARMED : 0;
        m.count = crash_kind_count();
        if (!vmm_copy_to_user(pml4, c->a0, &m, sizeof m)) ok = 0;
        // Refused because the build is not armed with `faultinject`,
        // or the index names no kind. EPERM either way: the caller may
        // not do this, which is exactly what the Crash Test app shows.
        c->regs[14] = ok ? 0 : (uint64_t)(int64_t)-EPERM;
    } else {
        c->regs[14] = (uint64_t)(int64_t)-EINVAL; // no such op
    }
    return 0;
}

int sys_set_color(struct syscall_ctx *c) {
    if (c->a0 > VGA_WHITE || c->a1 > VGA_WHITE) {
        c->regs[14] = (uint64_t)(int64_t)-EINVAL;
    } else {
        vga_set_color((enum vga_color)c->a0, (enum vga_color)c->a1);
        c->regs[14] = 0;
    }
    return 0;
}

// The console's size in text cells, rows in the low 32 bits and columns
// in the high 32.
//
// vga_rows()/vga_cols() already answer this kernel-side -- they are
// derived from the active font, which is why this is a syscall rather
// than a constant in a header: `font_size` is a runtime setting, so a
// baked number is wrong the moment somebody changes it.
int sys_console_size(struct syscall_ctx *c) {
    uint64_t rows = vga_rows();
    uint64_t cols = vga_cols();
    c->regs[14] = (cols << 32) | (rows & 0xFFFFFFFFull);
    return 0;
}
