// The ring-3 crash report. See kernel/include/kernel/crash_report.h for
// the contract; this file is the formatting and the two writes.
#include "crash_report.h"
#include "scheduler.h"
#include "vmm.h"
#include "uaddr.h"
#include "fs.h"
#include "klog.h"
#include "kfmt.h"
#include "kversion.h"
#include "string.h"
#include "kpath.h"

#define CRASH_DIR        "/var/crash"
#define HEADER_CAP       8192      // the text half, one static buffer
#define KLOG_TAIL        2048      // bytes of kernel log appended to it
#define STACK_MAX_BYTES  (16 * 4096)

// STATICS, NOT LOCALS: this runs on the fault handler's stack, which is
// the faulting process's kernel stack, and a page of buffer there is
// exactly the overflow the kstack guard exists to catch.
static char    g_header[HEADER_CAP];
static uint8_t g_page[4096];

static int hdr_put(int at, const char *fmt, ...) {
    if (at < 0 || at >= HEADER_CAP - 1) return at;
    va_list ap;
    va_start(ap, fmt);
    int n = k_vsnprintf(g_header + at, (unsigned)(HEADER_CAP - at), fmt, ap);
    va_end(ap);
    // k_vsnprintf writes NOTHING when it does not fit (kfmt.h's rule),
    // so a full header simply stops growing rather than truncating a line.
    if (n < 0 || n >= HEADER_CAP - at) return at;
    return at + n;
}

static const char *reg_names[15] = {
    "r15", "r14", "r13", "r12", "r11", "r10", "r9", "r8",
    "rbp", "rdi", "rsi", "rdx", "rcx", "rbx", "rax",
};

uint32_t crash_report_write(const char *what, const uint64_t *regs, uint64_t cr2,
                            int stack_overflow) {
    int pid = scheduler_current_pid();
    if (!pid) {
        klog_printf("crash: no report -- the legacy loader's process has no pid\n");
        return 0;
    }
    if (scheduler_preempt_depth() != 0) {
        klog_printf("crash: no report -- a filesystem operation is in flight "
                    "(preempt depth %d)\n", scheduler_preempt_depth());
        return 0;
    }
    if (!fs_is_persistent()) {
        klog_printf("crash: no report -- no persistent filesystem\n");
        return 0;
    }

    char exec[64];
    if (!scheduler_exec_path(pid, exec, sizeof exec)) k_strlcpy(exec, "?", sizeof exec);
    const char *base = k_path_basename(exec);

    char path[64];
    // <program>-<pid>.crash under CRASH_DIR, the program clipped so the
    // whole path fits FS_PATH_MAX.
    char name[24];
    unsigned i = 0;
    for (; i < sizeof name - 1 && base[i] && base[i] != '/'; i++) name[i] = base[i];
    name[i] = 0;
    k_snprintf(path, sizeof path, CRASH_DIR "/%s-%d.crash", name[0] ? name : "unknown", pid);

    uint64_t pml4 = vmm_current_pml4();
    uint64_t rip = regs[17], rsp = regs[20];
    struct sched_mm *mm = scheduler_current_mm();

    // The stack range: from RSP's page down to the top page. On a stack
    // overflow RSP is on the guard page and nothing there is readable,
    // so the copy starts at the first mapped page above it.
    uint64_t stack_top = UADDR_STACK_VADDR + 4096;
    uint64_t from = rsp & ~0xFFFULL;
    if (from < UADDR_STACK_FLOOR || from >= stack_top) from = stack_top;   // a wild RSP: no stack
    if (stack_top - from > STACK_MAX_BYTES) stack_top = from + STACK_MAX_BYTES;
    uint32_t stack_len = (uint32_t)(stack_top - from);

    int at = 0;
    at = hdr_put(at, "toy-os crash report v1\n");
    at = hdr_put(at, "kernel: %s\n", kversion_banner());
    at = hdr_put(at, "program: %s\n", exec);
    at = hdr_put(at, "pid: %d\n", pid);
    at = hdr_put(at, "fault: %s%s\n", what, stack_overflow ? " (stack overflow)" : "");
    at = hdr_put(at, "vector: %d error: 0x%lx\n", (int)regs[15], regs[16]);
    at = hdr_put(at, "rip: 0x%lx cs: 0x%lx rflags: 0x%lx\n", rip, regs[18], regs[19]);
    at = hdr_put(at, "rsp: 0x%lx ss: 0x%lx cr2: 0x%lx\n", rsp, regs[21], cr2);
    for (int r = 0; r < 15; r++) at = hdr_put(at, "%s: 0x%lx\n", reg_names[r], regs[r]);
    if (mm) {
        at = hdr_put(at, "map: image 0x%lx-0x%lx\n", (uint64_t)0x8000000000ULL /* the image base, uaddr.h */, mm->heap_base);
        at = hdr_put(at, "map: heap 0x%lx-0x%lx\n", mm->heap_base, mm->brk);
        at = hdr_put(at, "map: stack 0x%lx-0x%lx\n", mm->stack_bottom, UADDR_STACK_VADDR + 4096);
        for (int r = 0; mm->regions && r < mm->region_cap; r++) {
            const struct mmap_region *g = &mm->regions[r];
            if (!g->base) continue;
            at = hdr_put(at, "map: %s 0x%lx-0x%lx prot %u %s\n",
                         g->kind == MMAP_KIND_FILE ? "file" : "anon",
                         g->base, g->base + g->npages * 4096, (unsigned)g->prot,
                         g->kind == MMAP_KIND_FILE ? g->path : "");
        }
    }
    at = hdr_put(at, "stack: 0x%lx %u\n", from, stack_len);

    // The kernel log's tail, BEFORE this function logs anything of its own.
    at = hdr_put(at, "klog:\n");
    {
        uint64_t total = klog_total_bytes();
        uint64_t start = total > KLOG_TAIL ? total - KLOG_TAIL : 0;
        if (at < HEADER_CAP - 64) {
            uint64_t first = 0;
            uint32_t n = klog_read(start, g_header + at, (uint32_t)(HEADER_CAP - at - 2), &first);
            // Start at a line boundary: a slice begins mid-line.
            uint32_t skip = 0;
            while (skip < n && g_header[at + skip] != '\n') skip++;
            if (skip < n) { skip++; k_memmove(g_header + at, g_header + at + skip, n - skip); n -= skip; }
            else n = 0;
            at += (int)n;
            if (at && g_header[at - 1] != '\n') g_header[at++] = '\n';
        }
    }
    at = hdr_put(at, "---- stack ----\n");
    if (at <= 0) return 0;

    // The two directories, then the file: header first, the stack pages
    // appended one at a time. A page that is not mapped (never touched)
    // is written as zeros rather than faulted in.
    fs_mkdir("/var");
    fs_mkdir(CRASH_DIR);
    if (!fs_touch(path) || !fs_write_range(path, 0, g_header, (uint32_t)at)) {
        klog_printf(KLOG_ERR "crash: could not write %s\n", path);
        return 0;
    }
    uint32_t written = (uint32_t)at;
    for (uint64_t va = from; va < from + stack_len; va += 4096) {
        if (!vmm_validate_user_range(pml4, va, 4096) ||
            !vmm_copy_from_user(pml4, g_page, va, 4096))
            k_memset(g_page, 0, sizeof g_page);
        if (!fs_write_range(path, written, g_page, 4096)) break;
        written += 4096;
    }
    klog_printf("crash: report written to %s (%u bytes)\n", path, written);
    return written;
}
