// See elf_run.h. Mirrors file_test.c/newsyscalls_test.c's own
// elf_load()+process_run_ring3() shape almost exactly -- the only real
// difference is where the raw ELF bytes come from.
#include "elf_run.h"
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "process.h"
#include "fs.h"
#include "syscall.h"
#include "vga.h"
#include "klog.h"
#include "string.h"
#include "strace.h"
#include "uaddr.h"

// Stack/heap/guard addresses come from uaddr.h -- this loader and the
// scheduler's build the SAME ring-3 layout, and used to say so in two
// places with nothing keeping them equal.

// Max argv entries (including argv[0], the path itself) a single
// elf_run_from_fs() call can hand off -- plenty for anything this
// kernel's own /bin binaries take (ls's -a/-l/-al plus one path
// argument is at most 3), well short of the one stack page's real
// limit (see build_argv_on_stack()'s own overflow check below, which
// is what actually enforces the hard limit).
#define ELF_RUN_MAX_ARGC 16

// Lays argv[0]=path plus each whitespace-separated token of `args`
// (NULL/"" for none) into the identity-mapped stack page at
// `stack_phys`/`stack_vaddr`, per this file's own top-of-header
// layout comment (elf_run.h): argument strings written down from the
// page's top, followed by the argv pointer array (each pointer a
// *vaddr*, since that's what ring-3 code will dereference) at a lower
// address, with `*out_user_rsp` set to that pointer array's own
// address -- so a subsequent `push` from ring 3 only ever writes to
// fresh, lower, previously-unused stack space. Returns 1 on success,
// 0 if `args` has too many tokens (ELF_RUN_MAX_ARGC) or the strings +
// pointer array don't fit in the one 4096-byte page -- callers must
// treat that as a hard failure, not silently truncate.
//
// Not static -- exposed via elf_run.h as elf_build_argv_on_stack() so
// scheduler.c's spawn_from_fs() (the scheduler's own, non-blocking
// counterpart to elf_run_from_fs() below) can lay out a real argv the
// same way, instead of the trapframe it synthesizes just zeroing
// rdi/rsi (see scheduler.c's own comment on this, Milestone 1's
// Terminal async-spawn item).
int elf_build_argv_on_stack(uint64_t stack_phys, uint64_t stack_vaddr,
                             const char *path, const char *args,
                             const char *env,
                             const uint64_t (*auxv)[2], int auxc,
                             uint64_t *out_argc, uint64_t *out_argv,
                             uint64_t *out_user_rsp) {
    uint8_t *page = (uint8_t *)(uintptr_t)stack_phys;

    // Token boundaries (into `path`/`args`, not copies) -- collected
    // first so the write-downward-from-the-top pass below can place
    // argv[0] (path) closest to the top, then each `args` token below
    // it in order, matching the argv[] index order.
    const char *tok_start[ELF_RUN_MAX_ARGC];
    size_t tok_len[ELF_RUN_MAX_ARGC];
    int argc = 0;
    tok_start[argc] = path;
    tok_len[argc] = k_strlen(path);
    argc++;

    if (args) {
        const char *p = args;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            const char *start = p;
            while (*p && *p != ' ') p++;
            if (argc >= ELF_RUN_MAX_ARGC) return 0; // too many arguments
            tok_start[argc] = start;
            tok_len[argc] = (size_t)(p - start);
            argc++;
        }
    }

    // Write each token's bytes downward, starting FS_PATH_MAX bytes
    // below the page's true top rather than right at it -- reserved,
    // never-written padding. Several syscalls that take a path argument
    // (SYS_LISTDIR chief among them, see syscall.c) validate a full
    // FS_PATH_MAX-byte range starting at whatever pointer userland
    // passes in, not just up to its NUL -- a real, blocking bug hit
    // testing this feature: `ls /` crashed vmm_validate_user_range()'s
    // check because argv[0] ("/bin/ls") landed close enough to the
    // page's literal end that FS_PATH_MAX bytes past it ran off the
    // mapped page. Reserving this margin guarantees every token's start
    // address, no matter which one ends up closest to the top, still
    // has a full FS_PATH_MAX mapped bytes after it.
    size_t offset = 4096 - FS_PATH_MAX;
    uint64_t str_vaddr[ELF_RUN_MAX_ARGC];
    for (int i = 0; i < argc; i++) {
        size_t len = tok_len[i] + 1; // include the NUL
        if (len > offset) return 0; // doesn't fit in the page
        offset -= len;
        k_memcpy(page + offset, tok_start[i], tok_len[i]);
        page[offset + tok_len[i]] = '\0';
        str_vaddr[i] = stack_vaddr + offset;
    }

    // The ENVIRONMENT's strings, placed the same way and above the
    // block that will point at them. `env` is a NUL-separated run of
    // "KEY=VALUE" terminated by an empty string -- one blob rather than
    // a char** the kernel would have to walk pointer by pointer,
    // validating each one out of user memory. The same shape `args`
    // already has, for the same reason.
    uint64_t env_vaddr[ELF_RUN_MAX_ENVC];
    int envc = 0;
    if (env) {
        for (const char *e = env; *e; ) {
            size_t len = 0;
            while (e[len]) len++;
            if (envc >= ELF_RUN_MAX_ENVC) return 0;   // refuse, never truncate
            if (len + 1 > offset) return 0;
            offset -= len + 1;
            k_memcpy(page + offset, e, len);
            page[offset + len] = '\0';
            env_vaddr[envc++] = stack_vaddr + offset;
            e += len + 1;
        }
    }

    // The SysV process-entry block, laid out below every string it
    // points at. From RSP upward:
    //
    //     (%rsp)          argc
    //     8(%rsp)         argv[0] .. argv[argc-1]
    //                     NULL              (argv terminator)
    //                     envp[0] .. envp[envc-1]
    //                     NULL              (envp terminator)
    //
    // This is the standard layout a real crt0 expects, and userland/
    // crt0.asm is what reads it. An auxv (abi/auxv.h) follows the envp
    // terminator ONLY when the caller passed one -- i.e. only for a
    // dynamic executable, whose interpreter is the one consumer. A
    // static program's stack is byte-identical to what it always was.
    //
    // **RSP is 16-byte ALIGNED at entry**, per SysV. That is a change,
    // and the previous convention is worth recording because it looked
    // wrong and wasn't: this used to hand over RSP % 16 == 8, because
    // every _start was a plain C function. GCC compiles such a function
    // like any other -- assuming a return address was pushed, i.e.
    // RSP % 16 == 8 on entry -- and sizes its prologue to land
    // 16-aligned locals from there. Handing THAT a 16-aligned RSP put
    // every aligned stack slot off by exactly 8, which surfaced as
    // userland/fpu_test.c's `movapd %xmm0,(%rsp)` taking a #GP at ring
    // 3 (established by testing, not by reading the ABI; both this and
    // the plain 8-alignment before it were invisible while userland was
    // built -mno-sse and nothing could emit an alignment-sensitive
    // instruction at all).
    //
    // A hand-written assembly _start does not have that problem -- it
    // makes no assumption about a pushed return address and realigns
    // before calling main. So the entry point moving into crt0.asm is
    // exactly what makes the standard 16-alignment correct here.
    if (!auxv) auxc = 0;
    size_t block_bytes = 8                              // argc
                        + (size_t)(argc + 1) * 8        // argv[] + NULL
                        + (size_t)(envc + 1) * 8        // envp[] + NULL
                        + (auxc ? (size_t)(auxc + 1) * 16 : 0); // auxv + AT_NULL
    if (block_bytes + 16 > offset) return 0; // no room for the block plus alignment slack
    offset -= block_bytes;
    offset &= ~(size_t)15; // SysV: 16-aligned AT ENTRY

    uint64_t *blk = (uint64_t *)(page + offset);
    blk[0] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) blk[1 + i] = str_vaddr[i];
    blk[1 + argc] = 0; // argv terminator
    for (int i = 0; i < envc; i++) blk[2 + argc + i] = env_vaddr[i];
    blk[2 + argc + envc] = 0; // envp terminator
    if (auxc) {
        uint64_t *av = blk + 3 + argc + envc;
        for (int i = 0; i < auxc; i++) {
            av[2 * i]     = auxv[i][0];
            av[2 * i + 1] = auxv[i][1];
        }
        av[2 * auxc] = 0; // AT_NULL
        av[2 * auxc + 1] = 0;
    }

    *out_argc = (uint64_t)argc;
    *out_argv = stack_vaddr + offset + 8; // &argv[0], for callers that want it
    *out_user_rsp = stack_vaddr + offset; // &argc -- what RSP must be at entry
    return 1;
}

int elf_run_from_fs(const char *path, const char *args) {
    uint32_t size = 0;
    const char *data = fs_read(path, &size);
    if (!data) {
        vga_write("run: couldn't read "); vga_write(path); vga_write(" from disk\n");
        return -1;
    }

    // fs_read()'s buffer is a kmalloc()'d heap allocation -- and, like
    // every kmalloc() allocation, carved out of the same identity-mapped
    // low-4GiB physical range a GRUB module lives in (see heap_core.c's top
    // comment on why: this kernel identity-maps the whole low 4GiB as
    // kernel/supervisor-only, and heap_init() just hands out pieces of
    // that same range). elf_load() takes its address directly here with
    // just a cast, exactly like it takes a multiboot_module_info's
    // `start` field in file_test.c/newsyscalls_test.c -- no separate
    // copy into a scratch buffer needed, unlike what an earlier version
    // of this plan assumed (see docs/decisions.md). This DOES mean
    // nothing may call fs_read() again (on this or any other path)
    // until this function returns -- already true of every existing
    // fs_read() caller, since the backend only keeps one such buffer
    // alive at a time (see fs.h's fs_read() doc comment) -- process_run_
    // ring3() below runs entirely through syscalls (SYS_READ/SYS_WRITE/
    // etc.), none of which go through fs_read() itself, so this holds.
    uint64_t elf_phys = (uint64_t)(uintptr_t)data;

    uint64_t as = vmm_create_address_space();
    if (!as) {
        vga_write("run: vmm_create_address_space() failed\n");
        return -1;
    }

    // If the shell's `strace` armed tracing, this is the address space
    // it attaches to (a no-op otherwise) -- claimed here rather than
    // after elf_load() so the trace covers the process from its very
    // first syscall. See kernel/proc/strace.c.
    strace_claim(as);

    uint64_t entry = 0, image_end = 0;
    // `size` comes from fs_read() above and used to be discarded here;
    // it is what bounds every offset in the file. On failure the address
    // space is destroyed rather than leaked -- see elf.h.
    if (!elf_load(elf_phys, size, as, &entry, &image_end, 0)) {
        vga_write("run: "); vga_write(path); vga_write(" isn't a valid ELF64 executable\n");
        vmm_destroy_address_space(as);
        return -1;
    }

    // The TOP page holds argv and is where RSP starts; the rest are the
    // starting working set. Everything below them is reserved address
    // space uheap_fault() maps on demand -- see kernel/uaddr.h.
    uint64_t stack_phys = 0;
    for (int pg = 0; pg < UADDR_STACK_INIT_PAGES; pg++) {
        uint64_t frame = pmm_alloc_frame();
        if (!frame) {
            vga_write("run: out of physical memory for the stack\n");
            return -1;
        }
        if (!vmm_map_user_page(as, UADDR_STACK_VADDR - (uint64_t)pg * 4096, frame)) {
            vga_write("run: failed to map a stack page\n");
            return -1;
        }
        if (pg == 0) stack_phys = frame;
    }

    // Arms SYS_SBRK for this process unconditionally -- cheap
    // bookkeeping (see syscall.c's syscall_reset_mm()), not an
    // allocation, so it costs nothing for a binary that never calls
    // sbrk(). This used to be a case-by-case opt-in (only echo_test.c
    // called it, for the one binary that needed SYS_SBRK) -- made
    // universal here so any /bin binary can use it, not just the ones
    // whose bespoke kernel-side loader remembered to arm it.
    syscall_reset_mm(as, image_end);

    uint64_t argc = 0, argv = 0, user_rsp = 0;
    if (!elf_build_argv_on_stack(stack_phys, UADDR_STACK_VADDR, path, args, 0, 0, 0,
                                  &argc, &argv, &user_rsp)) {
        vga_write("run: arguments too long for ");
        vga_write(path);
        vga_write("\n");
        return -1;
    }

    klog_write("elf_run: calling process_run_ring3() for ");
    klog_write(path);
    klog_write("\n");

    // argc/argv are NOT passed in registers any more -- they live on
    // the stack this call hands over, in the SysV layout
    // elf_build_argv_on_stack() built and userland/crt0.asm reads. The
    // register variant that used to exist here is gone rather than kept
    // "just in case": two live conventions for the same thing is how an
    // ABI rots.
    (void)argc; (void)argv; // consumed via user_rsp, see above
    int exit_code = process_run_ring3(as, entry, user_rsp);

    klog_write("elf_run: process_run_ring3() returned\n");
    return exit_code;
}
