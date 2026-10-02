// Making a process: the spawn path, fork, threads, exec, and the image
// loader under all of them -- including `#!`, which is the loader's job.
// Split out of scheduler.c; sched_internal.h has the map.

#include "sched_internal.h"
#include "futex.h"
#include "mmap.h"   // a fork's view of the mmap arena
#include "shm.h"    // shm_process_gone() -- undoing a half-inherited arena, and an exec
#include "sound.h"  // sound_process_gone() -- an exec drops the stream
#include "syscalls.h" // the fd table: a child inherits its parent's descriptors
#include "tty.h"      // a kernel-context terminal session (spawn_kernel())
#include "remote_log.h" // a session created from a socket is a REMOTE session
#include "pmm.h"
#include "elf.h"
#include "auxv.h" // the dynamic handoff, see spawn_from_fs()
#include "elf_run.h"
#include "win_role.h"
#include "diag.h"     // diag_provider_gone() -- drop a dead service's name
#include "kfmt.h"      // klog_printf, vga_printf
#include "syscall_abi.h" // SYS_RETRY -- the wake value a blocked waiter sees
#include "heap.h"
#include "gdt.h"
#include "tls.h"    // FS.base -- a thread pointer is per THREAD, see switch_to()
#include "klog.h"
#include "strace.h"
#include "string.h" // k_strlcpy -- proc_name_from_path()

// The user stack's address and size, plus the guard region below it,
// come from uaddr.h -- this spawn path and elf_run.c's legacy loader
// build the SAME ring-3 layout, and used to say so in two places with
// nothing keeping them equal.

// --- `#!`, which is the LOADER's job and not the shell's ------------
//
// Linux does this in binfmt_script, and the reason to copy the position
// rather than the mechanism is that a script then runs the same way from
// `spawn`, from execve() and from either shell -- instead of from
// whichever one remembered to look for the line.
//
// Two limits, both Linux's: the line is read out of the first
// SHEBANG_MAX_LINE bytes (BINPRM_BUF_SIZE is 128 there), and a script
// whose interpreter is itself a script nests at most SHEBANG_MAX_DEPTH
// times, which is what stops `#!/x` in a file named `/x` looping the
// kernel. ONE optional argument after the interpreter, not a split word
// list -- every Unix does exactly this, and a shell writing
// `#!/bin/dash -e -x` gets `-e -x` as a single argument on all of them.
#define SHEBANG_MAX_LINE  128
#define SHEBANG_MAX_DEPTH 4

// Builds the interpreter's argument vector: [interp, arg?, script,
// caller's args after argv[0]]. argv[0] is the interpreter AS WRITTEN
// and the script's own path arrives as the next entry, which is how the
// interpreter learns what to open. Returns a kmalloc'd vector and its
// length, or 0 if it cannot fit -- refused, never truncated, the same
// rule elf_argv_from_string() follows.
static char *shebang_argv(const char *interp, const char *arg, const char *script,
                          const char *argvec, size_t argvec_len, size_t *out_len) {
    // The caller's args MINUS argv[0]: the interpreter replaces it.
    const char *rest = 0;
    size_t rest_len = 0;
    if (argvec && argvec_len) {
        size_t first = 0;
        while (first < argvec_len && argvec[first]) first++;
        if (first < argvec_len) {           // there is something after argv[0]
            rest = argvec + first + 1;
            rest_len = argvec_len - first - 1;
        }
    }

    size_t need = k_strlen(interp) + 1 + k_strlen(script) + 1 + rest_len;
    if (arg) need += k_strlen(arg) + 1;
    if (need > SPAWN_ARGS_MAX) return 0;

    char *out = kmalloc(need);
    if (!out) return 0;
    size_t n = 0;
    n += (size_t)k_strlcpy(out + n, interp, need - n) + 1;
    if (arg) n += (size_t)k_strlcpy(out + n, arg, need - n) + 1;
    n += (size_t)k_strlcpy(out + n, script, need - n) + 1;
    if (rest_len) { k_memcpy(out + n, rest, rest_len); n += rest_len; }
    *out_len = n;
    return out;
}

// Reads `path`'s first line and, if it is a `#!`, writes the interpreter
// into `interp` (cap `interp_cap`) and the optional single argument into
// `arg`. Returns 1 if this file is a script, 0 if it is not one (or the
// line is malformed, which is treated as "not a script" so the ordinary
// "not an ELF" error reaches the caller rather than a second one).
static int shebang_read(const char *path, char *interp, size_t interp_cap,
                        char *arg, size_t arg_cap) {
    char line[SHEBANG_MAX_LINE];
    uint32_t got = fs_read_range(path, 0, line, sizeof line);
    if (got < 3 || line[0] != '#' || line[1] != '!') return 0;

    size_t i = 2, n = got < sizeof line ? got : sizeof line;
    while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
    size_t start = i;
    while (i < n && line[i] != ' ' && line[i] != '\t' &&
           line[i] != '\n' && line[i] != '\r') i++;
    if (i == start || i - start >= interp_cap) return 0;
    k_memcpy(interp, line + start, i - start);
    interp[i - start] = 0;

    // The optional argument: everything left on the line, trimmed at
    // both ends, as ONE string.
    arg[0] = 0;
    while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
    size_t astart = i;
    while (i < n && line[i] != '\n' && line[i] != '\r') i++;
    while (i > astart && (line[i - 1] == ' ' || line[i - 1] == '\t')) i--;
    if (i > astart && i - astart < arg_cap) {
        k_memcpy(arg, line + astart, i - astart);
        arg[i - astart] = 0;
    }
    return 1;
}

// THE IMAGE HALF OF A SPAWN, shared with exec (docs/fork-design.md):
// read the ELF, create the address space, load it and its interpreter,
// map the initial stack and lay argv/env/auxv out on it. On success
// `*out_as` holds an address space nothing else refers to yet; on
// failure nothing is held. `*out_image_end` is where the heap starts.
static int build_elf_image(const char *path, const char *argvec, size_t argvec_len,
                       const char *env, uint64_t *out_as, uint64_t *out_entry,
                       uint64_t *out_rsp, uint64_t *out_image_end) {
    // THE IMAGE IS READ INTO MEMORY THIS FUNCTION OWNS (fs_read_into),
    // never the backend's staging buffer: this runs in the kernel
    // context as well as under a syscall, and a ring-3 file read could
    // free that buffer under elf_load(). kmalloc memory is identity-
    // mapped, so elf_load() takes its address directly (elf_run.c).
    uint64_t fsz = fs_size(path);
    if (fsz == 0 || fsz > 0xFFFFFFFFu - 1) return 0;
    char *data = kmalloc((size_t)fsz + 1);
    if (!data) return 0;
    uint32_t size = fs_read_into(path, data, (uint32_t)fsz + 1);
    if (size == 0) { kfree(data); return 0; }
    uint64_t elf_phys = (uint64_t)(uintptr_t)data;

    uint64_t as = vmm_create_address_space();
    if (!as) { kfree(data); return 0; }

    // Same one-line hook elf_run_from_fs() has -- a no-op unless the
    // shell's `strace` armed tracing, which keeps the mechanism
    // process-creation-path-agnostic rather than tied to the blocking
    // loader (see kernel/proc/strace.c).
    strace_claim(as);

    uint64_t entry = 0, image_end = 0;
    struct elf_dyn_info dyn;
    // On failure the address space is destroyed rather than leaked --
    // it owns whatever elf_load() mapped before giving up, and every
    // failure path below this point owes the same cleanup. This used to
    // be a bare `return -1`, leaking the PML4, every page table under
    // it and every segment frame.
    if (!elf_load(elf_phys, size, as, &entry, &image_end, &dyn)) {
        vmm_destroy_address_space(as);
        kfree(data);
        return 0;
    }
    kfree(data);   // everything the process needs from it is in the address space now

    // A DYNAMIC executable: load the interpreter it names as a second
    // image and enter THAT (Linux's split -- the kernel's part in
    // dynamic linking ends here; /lib/ld-toy.so finishes the job in
    // ring 3 and jumps to the auxv's AT_ENTRY). The interpreter is a
    // fixed-base ET_EXEC at ELF_LDSO_BASE, so the same elf_load() and
    // the same bounds serve both images; the guard is the executable
    // growing up into it, which no real program here approaches.
    //
    uint64_t auxv[4][2];
    int auxc = 0;
    if (dyn.interp[0]) {
        if (image_end > ELF_LDSO_BASE) {
            klog_printf(KLOG_ERR "spawn: %s reaches %#lx, into the interpreter -- refused\n",
                        path, image_end);
            vmm_destroy_address_space(as);
            return 0;
        }
        uint64_t ifsz = fs_size(dyn.interp);
        char *idata = ifsz && ifsz < 0xFFFFFFFFu - 1 ? kmalloc((size_t)ifsz + 1) : 0;
        uint32_t isize = idata ? fs_read_into(dyn.interp, idata, (uint32_t)ifsz + 1) : 0;
        if (isize == 0) {
            klog_printf("spawn: interpreter %s missing\n", dyn.interp);
            if (idata) kfree(idata);
            vmm_destroy_address_space(as);
            return 0;
        }
        uint64_t ientry = 0, iend = 0;
        int iok = elf_load((uint64_t)(uintptr_t)idata, isize, as, &ientry, &iend, 0);
        kfree(idata);
        if (!iok) {
            klog_printf("spawn: interpreter %s did not load\n", dyn.interp);
            vmm_destroy_address_space(as);
            return 0;
        }
        if (iend > image_end) image_end = iend; // the heap starts after BOTH

        auxv[auxc][0] = AT_PHDR;  auxv[auxc][1] = ELF_IMAGE_BASE + dyn.phoff; auxc++;
        auxv[auxc][0] = AT_PHENT; auxv[auxc][1] = 56;         auxc++;
        auxv[auxc][0] = AT_PHNUM; auxv[auxc][1] = dyn.phnum;  auxc++;
        auxv[auxc][0] = AT_ENTRY; auxv[auxc][1] = entry;      auxc++;
        entry = ientry;
    }

    // The TOP page is where argv is laid out and where RSP starts; the
    // rest are this process's starting WORKING SET, so the common client
    // never takes a growth fault at all. Everything below them is
    // reserved address space that uheap_fault() maps on demand -- see
    // kernel/uaddr.h.
    uint64_t stack_phys = 0;
    for (int pg = 0; pg < UADDR_STACK_INIT_PAGES; pg++) {
        uint64_t frame = pmm_alloc_frame(PMM_ZONE_ANY);
        if (!frame) { vmm_destroy_address_space(as); return 0; }
        uint64_t va = UADDR_STACK_VADDR - (uint64_t)pg * 4096;
        if (!vmm_map_user_page(as, va, frame)) {
            // The frame is not mapped, so destroying the address space
            // will not reclaim it -- free it here, then let the address
            // space take everything that IS mapped.
            pmm_free_frame(frame);
            vmm_destroy_address_space(as);
            return 0;
        }
        if (pg == 0) stack_phys = frame;
    }

    uint64_t argc = 0, argv = 0, user_rsp = 0;
    if (!elf_build_argv_on_stack(stack_phys, UADDR_STACK_VADDR, path, argvec, argvec_len, env,
                                  auxc ? auxv : 0, auxc,
                                  &argc, &argv, &user_rsp)) {
        vmm_destroy_address_space(as);
        return 0;
    }
    (void)argc; (void)argv; // argc/argv reach the process on its STACK
    *out_as = as;
    *out_entry = entry;
    *out_rsp = user_rsp;
    *out_image_end = image_end;
    return 1;
}

// The per-address-space half of a slot's memory state, from a freshly
// built image: the heap starts where the image ends (elf.c's
// out_image_end, which is what lets a ring-3 binary be any size), the
// stack has its initial working set, and no mmap region exists yet. A
// recycled slot still holds its previous owner's regions, and the
// frames behind them are long freed, so the wipe is load-bearing.
static void mm_reset(int slot, uint64_t image_end) {
    uint64_t heap_base = image_end > UADDR_HEAP_MIN_BASE
                              ? image_end : UADDR_HEAP_MIN_BASE;
    procs[slot].mm.heap_base    = heap_base;
    procs[slot].mm.brk          = heap_base;
    procs[slot].mm.stack_bottom = UADDR_STACK_INIT_BOTTOM;
    // Frees any list a previous occupant of this slot left behind:
    // the teardown path should have, and a slot recycled without one
    // must not leak. Idempotent, so both calling is correct.
    mmap_regions_reset(&procs[slot].mm);
}

// The loader every caller actually reaches: resolves `#!` first, then
// loads a real ELF. A script's interpreter may itself be a script, so
// this loops rather than recursing once -- bounded by SHEBANG_MAX_DEPTH.
static int build_image(const char *path, const char *argvec, size_t argvec_len,
                       const char *env, uint64_t *out_as, uint64_t *out_entry,
                       uint64_t *out_rsp, uint64_t *out_image_end) {
    char interp[SHEBANG_MAX_LINE], arg[SHEBANG_MAX_LINE];
    char *owned = 0;

    for (int depth = 0; shebang_read(path, interp, sizeof interp,
                                     arg, sizeof arg); depth++) {
        if (depth >= SHEBANG_MAX_DEPTH) { kfree(owned); return 0; }
        size_t len = 0;
        char *next = shebang_argv(interp, arg[0] ? arg : 0, path,
                                  argvec, argvec_len, &len);
        if (!next) { kfree(owned); return 0; }
        kfree(owned);               // the vector this one was built from
        owned = next;
        argvec = next;
        argvec_len = len;
        path = interp;
        // `path` now aliases `interp`, which the next pass overwrites --
        // safe only because shebang_argv() above has already copied it
        // into `owned` before that happens.
    }

    int rc = build_elf_image(path, argvec, argvec_len, env,
                             out_as, out_entry, out_rsp, out_image_end);
    kfree(owned);
    return rc;
}

// Loads a real ELF64 binary from the persistent filesystem as a fresh
// ring-3 process and marks it READY -- the scheduler's own counterpart
// to elf_run_from_fs() (elf_run.c), which does the same load but then
// blocks synchronously via process_run_ring3() instead of handing the
// process to this scheduler. Used to spawn both `schedtest` counter
// processes from /bin now that they're disk-hosted binaries rather
// than GRUB modules (this used to be spawn_from_module(int
// module_index), sourcing bytes via multiboot_get_module() -- replaced
// outright rather than kept alongside once nothing needed it anymore,
// see docs/decisions.md). `argvec` is the child's argument VECTOR in
// elf_build_argv_on_stack()'s blob form (NULL for argv = {path}; the
// string-taking wrappers below convert) -- laid out via that function into
// this process's own stack page, the same layout elf_run_from_fs() uses
// for a legacy-blocking process, so a scheduler-managed one gets a real
// argv[0]/argc too instead of the rdi=rsi=0/bare-top-of-page RSP this
// function used to synthesize unconditionally. Returns the slot index
// (>= 0) or -1 on any failure (no free slot, missing/unreadable file,
// `args` too long to fit the one stack page, or the same allocation
// failures every other ELF-loading path already handles the same way).
int spawn_from_fs(const char *path, const char *argvec, size_t argvec_len,
                          int stdout_desc, int stdin_desc, int stderr_desc,
                          const char *env, int want_pgid, uint64_t parent_pml4) {
    int slot = slot_claim();   // BEFORE build_image(), which sleeps
    if (slot < 0) return -1;

    uint64_t as = 0, entry = 0, user_rsp = 0, image_end = 0;
    if (!build_image(path, argvec, argvec_len, env, &as, &entry, &user_rsp, &image_end)) {
        slot_unclaim(slot);
        return -1;
    }

    // Synthesize this process's very first trapframe, at the top of its
    // own dedicated kernel stack -- laid out exactly like a real one
    // isr_common would have saved, so the ordinary epilogue can launch
    // it the first time exactly the same way it resumes it later.
    uint64_t *tf = (uint64_t *)(kernel_stack_top(slot) - TRAPFRAME_WORDS * 8);
    for (int i = 0; i < TF_VECTOR; i++) tf[i] = 0; // r15..rax start at 0
    // rdi/rsi stay 0: argc/argv reach the process on its STACK, in the
    // SysV layout elf_build_argv_on_stack() built and crt0.asm reads
    // (user_rsp points at argc).
    tf[TF_VECTOR]  = 0; // unused -- epilogue discards vector+error_code
    tf[TF_ERRCODE] = 0; //          via `add rsp, 16` without reading them
    tf[TF_RIP]     = entry;
    tf[TF_CS]      = SEL_USER_CODE;
    tf[TF_RFLAGS]  = 0x200; // IF set
    tf[TF_RSP]     = user_rsp;
    tf[TF_SS]      = SEL_USER_DATA;

    procs[slot].pml4_phys  = as;
    procs[slot].kernel_rsp = (uint64_t)tf;
    proc_start_context(slot, tf);
    kstack_arm_slot(slot);
    // A pristine FP state, not whatever the previous tenant of this
    // slot left behind -- slots get reused (scheduler_poll() reaps back
    // to SCHED_UNUSED), and inheriting the last process's registers
    // would be both wrong and an information leak between processes.
    fpu_init_state(procs[slot].fpu);
    procs[slot].wait_chan = 0;
    procs[slot].wait_reason = 0;
    // A NEW PROCESS LEADS ITS OWN THREAD GROUP -- everything below this
    // line is per-group state, and it has exactly one member.
    procs[slot].tgid     = slot + 1;
    procs[slot].fs_base  = 0;
    procs[slot].detached = 0;

    // THE CHILD'S FILE DESCRIPTORS, built here because this is where
    // its address space first exists. It inherits the caller's whole
    // table (sharing every description, refcounted), which is what
    // lets a shell redirect a child by redirecting ITSELF around the
    // spawn -- the dance fork() normally exists to make possible:
    //
    //     saved = dup(1); dup2(f, 1); spawn(...); dup2(saved, 1);
    //
    // A kernel-context spawn has no table to inherit and the child
    // gets the console on all three -- fd 2 then overridden with the
    // kernel log for a detached start (spawn_kernel()).
    // THE PARENT IS WHOEVER IS EXECUTING, and that is CR3 -- not
    // procs[current_index]. The legacy blocking loader (`run` at the
    // physical shell) has its own address space and NO scheduler slot,
    // so a slot lookup answers "no parent" for it and its children
    // silently inherited nothing. The fd table is keyed by CR3
    // precisely so that path is not a special case; asking the
    // scheduler instead reintroduced the special case at the one site
    // that mattered.
    //
    // The parent is NAMED, never read off CR3. vmm_current_pml4() was
    // the parent here, and it was wrong in exactly one situation that
    // took an afternoon to find: a KERNEL-context caller (the shell's
    // bare-name spawn) runs with whatever address space the scheduler
    // last loaded, so the child inherited some arbitrary interrupted
    // process's terminal -- `ps` typed at the console printed into a
    // Terminal window. SYS_SPAWN passes its caller's pml4; a kernel
    // caller passes 0 and its child gets the standard three.
    fd_inherit(as, parent_pml4);
    if (stdout_desc >= 0) {
        // SYS_SPAWN's explicit stream overrides, which predate
        // inheritance and stay as the one-call shortcut. Applied
        // AFTER inheriting, so they win.
        fd_set_desc(as, FD_STDOUT, stdout_desc);
    }
    if (stdin_desc >= 0) fd_set_desc(as, FD_STDIN, stdin_desc);
    if (stderr_desc >= 0) fd_set_desc(as, FD_STDERR, stderr_desc);
    // The CALLER is the parent. 0 when the kernel context spawned this
    // -- scheduler_current_pid() returns 0 there, which is exactly the
    // "no parent" value, so this needs no special case.
    procs[slot].ppid = scheduler_current_tgid();
    // SET, NOT LEFT: a reused slot otherwise hands the next tenant its
    // last one's priority. Inherited, as posix_spawn does.
    procs[slot].prio = current_index >= 0 ? procs[current_index].prio : 0;
    procs[slot].vruntime = g_min_vruntime;   // a newcomer joins the pack, not ahead of it
    // NOTHING IS PENDING AND NOTHING IS IGNORED for a fresh process --
    // reset rather than inherited, and both matter. A slot is reused, so
    // a leftover pending bit would kill the NEXT tenant on its first
    // instruction; and dispositions do not survive an exec on Unix
    // either (an ignored signal is the documented exception there, and
    // this kernel has no fork/exec pair to make that distinction from).
    signal_state_reset(slot);
    // Nothing has announced anything yet. See the field's comment: this
    // is the same slot-reuse hazard signal_state_reset() covers.
    procs[slot].ready = 0;
    procs[slot].group_dying = 0;
    // THE GROUP: what the caller asked for, else the spawner's, else a
    // group of this process's own. The third case is the kernel context
    // -- init, the demo, a KTEST -- which has no group to lend, and
    // leading its own is what keeps `pgid` non-zero for every live slot.
    if (want_pgid > 0) {
        procs[slot].pgid = want_pgid;              // join that group
    } else if (want_pgid == PGID_NEW) {
        procs[slot].pgid = slot + 1;               // lead one of its own
    } else {
        int parent_pgid = scheduler_pgid(procs[slot].ppid);
        procs[slot].pgid = parent_pgid > 0 ? parent_pgid : slot + 1;
    }
    // THE SESSION IS ALWAYS INHERITED -- there is no spawn-time way to
    // ask for a new one, and deliberately: POSIX creates a session with
    // setsid() in the child, and a shell's children MUST stay in the
    // shell's session or none of them could ever take the terminal.
    // A kernel-context spawn has no session to lend and leads its own.
    {
        int parent_sid = scheduler_sid(procs[slot].ppid);
        procs[slot].sid = parent_sid > 0 ? parent_sid : slot + 1;
    }
    // Reset, not inherited: slots are reused, and a reaped process's
    // name and CPU time showing up on its successor would be a
    // reporting bug that looks like a scheduling one.
    proc_name_from_path(procs[slot].name, sizeof procs[slot].name, path);
    k_strlcpy(procs[slot].exec_path, path ? path : "", sizeof procs[slot].exec_path);
    procs[slot].cpu_ns = 0;
    // Armed here, at creation, rather than by a separate "set up this
    // process's heap" call the way the legacy loader does it: an init
    // step reachable by only one entry point is a bug waiting for a
    // second entry point, and this one already had that bug -- nothing
    // in the spawn path ever armed a heap, so SYS_SBRK refused every
    // scheduled process.
    //
    // heap_base comes from the IMAGE rather than from a constant, which
    // is what lets a ring-3 binary be any size (elf.c's out_image_end).
    // The max() is belt and braces: elf_load() cannot report an end
    // below ELF_IMAGE_BASE, but a heap starting under the floor would be
    // a silent aliasing bug rather than a loud one.
    mm_reset(slot, image_end);
    // INHERITED, unlike the name and the CPU time above: the cwd is the
    // one piece of a parent's state a child is supposed to start with,
    // which is what makes `mkdir docs` from a shell standing in /tmp
    // create /tmp/docs rather than /docs. syscall_current_cwd() answers
    // for the kernel context too (the legacy loader's single slot), so
    // this needs no special case for a process the shell's `spawn`
    // started.
    k_strlcpy(procs[slot].cwd.path, scheduler_cwd(), sizeof procs[slot].cwd.path);

    // WHAT A REMOTE SESSION STARTS IS RECORDED, auditd's execve shape --
    // here rather than in sys_spawn() because every spawn funnels
    // through this function, including the ones a script makes. A local
    // session logs nothing: remote_log_session_of() answers 0 and this
    // costs one walk of eight slots.
    uint32_t rip = remote_log_session_of(procs[slot].sid);
    if (rip) {
        char line[QUERY_REMOTELOG_TEXT_MAX];
        // The path plus the first argument, which is what makes `ls
        // /boot` distinguishable from `ls`. The whole vector would not
        // fit and the interesting part is the front of it.
        const char *arg = 0;
        if (argvec && argvec_len) {   // the same walk shebang_argv() makes
            size_t first = 0;
            while (first < argvec_len && argvec[first]) first++;
            if (first + 1 < argvec_len) arg = argvec + first + 1;
        }
        if (arg) k_snprintf(line, sizeof line, "%s %s", path, arg);
        else     k_strlcpy(line, path, sizeof line);
        remote_log_record(QUERY_REMOTE_SPAWN, rip, slot + 1,
                          procs[slot].name, line);
    }

    procs[slot].state      = SCHED_READY;
    slot_unclaim(slot);
    alive_count++;
    return slot;
}

// --- threads ---------------------------------------------------------
//
// A thread is an ordinary slot with somebody else's `tgid`. It gets its
// own kernel stack, FP state, trapframe, signal table and thread
// pointer; it shares its leader's address space, and through the
// address space the fd table, because that is keyed by CR3 and never
// learned about pids at all (syscall_fd.c).
//
// What this is NOT is fork(): there is no copy of anything. The new
// thread starts at an address ring 3 named, on a stack ring 3
// allocated, which is clone(CLONE_VM|CLONE_FILES)'s shape rather than
// pthread_create()'s -- the library half lives in ring 3 where it
// belongs (userland/libc/pthread.c).
int scheduler_thread_create(uint64_t entry, uint64_t user_rsp, uint64_t arg,
                             uint64_t fs_base, int detached) {
    // The kernel context has no address space to share, and the legacy
    // loader has no slot to lead a group -- both are "not a process".
    if (current_index < 0) return -EPERM;
    int caller = current_index;
    int leader = leader_index(caller);

    if (!entry || !user_rsp) return -EFAULT;
    // THE STACK IS THE CALLER'S, so a bad pointer must fail HERE, where
    // the caller can see -EFAULT, rather than as a page fault on the new
    // thread's first push -- which would kill the whole process for a
    // mistake one call made. Validating also faults the page in, which
    // is what makes a freshly malloc'd stack usable: the heap is
    // demand-paged, so the memory the caller "has" is not mapped yet.
    if (!vmm_validate_user_range(procs[leader].pml4_phys, user_rsp - 64, 64))
        return -EFAULT;

    int slot = slot_claim();
    if (slot < 0) return -EAGAIN;

    // The same synthesized first trapframe a spawn builds, minus
    // everything about loading an image: this thread's code is already
    // mapped, because it is its creator's.
    uint64_t *tf = (uint64_t *)(kernel_stack_top(slot) - TRAPFRAME_WORDS * 8);
    for (int i = 0; i < TF_VECTOR; i++) tf[i] = 0;
    tf[TF_RDI]     = arg;   // the SysV first argument: void *arg
    tf[TF_VECTOR]  = 0;
    tf[TF_ERRCODE] = 0;
    tf[TF_RIP]     = entry;
    tf[TF_CS]      = SEL_USER_CODE;
    tf[TF_RFLAGS]  = 0x200; // IF set
    // **RSP % 16 == 8 AT ENTRY, NOT 0**, which is the same trap
    // crt0.asm documents and which this line got backwards for one
    // build. SysV states the rule at the CALLEE: a `call` has just
    // pushed 8 bytes, so a function begins with RSP % 16 == 8 and GCC
    // sizes its prologue from that. Hand it a 16-ALIGNED RSP and every
    // `movaps` it emits against a stack slot faults with a #GP.
    //
    // `(x & ~15) - 8`, not `(x - 8) & ~15` -- the second is always
    // 16-aligned, i.e. always the broken case. It passed every
    // thread test in the tree, because none of those workers used SSE;
    // the first GUI client to run one crashed on its first snprintf.
    tf[TF_RSP]     = (user_rsp & ~15ull) - 8;
    tf[TF_SS]      = SEL_USER_DATA;

    procs[slot].pml4_phys  = procs[leader].pml4_phys; // SHARED, not created
    procs[slot].kernel_rsp = (uint64_t)tf;
    proc_start_context(slot, tf);
    kstack_arm_slot(slot);
    fpu_init_state(procs[slot].fpu);
    procs[slot].wait_chan   = 0;
    procs[slot].wait_reason = 0;
    procs[slot].tgid     = leader + 1;
    procs[slot].fs_base  = fs_base;
    procs[slot].detached = detached ? 1 : 0;
    // Its parent is its leader, which is what makes `ps --tree` show a
    // thread under the process it belongs to. Every walk over a
    // process's CHILDREN skips threads, so this is a display fact and
    // never a wait() one.
    procs[slot].ppid     = leader + 1;
    procs[slot].pgid     = procs[leader].pgid;
    procs[slot].prio     = procs[caller].prio;
    procs[slot].vruntime = g_min_vruntime;
    signal_state_reset(slot);
    // DISPOSITIONS ARE INHERITED, which is as close to POSIX's
    // per-process disposition as a per-thread table gets: a thread
    // created after signal(SIGINT, h) runs the same handler its creator
    // would. `pending` is not inherited -- a signal raised before this
    // thread existed was not raised at it.
    for (int i = 0; i <= SIGNAL_MAX; i++)
        procs[slot].actions[i] = procs[caller].actions[i];
    procs[slot].ready   = 0;
    procs[slot].cpu_ns  = 0;
    k_strlcpy(procs[slot].name, procs[leader].name, sizeof procs[slot].name);
    k_strlcpy(procs[slot].exec_path, procs[leader].exec_path,
              sizeof procs[slot].exec_path);
    // The heap and the cwd belong to the GROUP and are read through the
    // leader (scheduler_current_mm/_cwd). Zeroed rather than copied, so
    // a reader that forgets gets an obvious 0 instead of a second copy
    // that drifts.
    k_memset(&procs[slot].mm, 0, sizeof procs[slot].mm);
    procs[slot].cwd.path[0] = '\0';
    procs[slot].state = SCHED_READY;
    slot_unclaim(slot);
    alive_count++;
    return slot + 1;
}

// --- fork ------------------------------------------------------------
//
// What a fork copies is stated once, in docs/fork-design.md's table;
// this function is that table in order. Two things are not obvious
// from the table. THE FRAMES THE KERNEL HOLDS A PHYSICAL POINTER INTO
// ARE COPIED EAGERLY, not shared: a futex waiter is parked on its
// word's physical address and the wakeword is written by one, so if the
// parent un-shared such a page its waiters would be keyed to the frame
// the child now owns -- a lost wakeup. And the FP state is the
// caller's LIVE registers (the kernel is -mno-sse, so they are still in
// the CPU), which is what a fork means.
static int fork_inherits_borrowed(void *ctx, uint64_t va) {
    return mmap_inherits_at(ctx, va);
}

int scheduler_fork(const uint64_t *regs) {
    if (current_index < 0) return -EPERM; // the kernel context, or the legacy loader
    int caller = current_index;
    int leader = leader_index(caller);

    int slot = slot_claim();
    if (slot < 0) return -EAGAIN;

    uint64_t pinned[MAX_PROCS + 1];
    int npin = 0;
    uint64_t ww = futex_wakeword_phys(leader + 1);
    if (ww) pinned[npin++] = ww;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state != SCHED_BLOCKED || procs[i].tgid != leader + 1) continue;
        if (procs[i].wait_chan) pinned[npin++] = (uint64_t)(uintptr_t)procs[i].wait_chan;
    }
    struct vmm_fork_opts o = { pinned, npin, fork_inherits_borrowed, &procs[leader].mm };
    uint64_t as = vmm_fork_address_space(procs[leader].pml4_phys, &o);
    if (!as) { slot_unclaim(slot); return -ENOMEM; }
    if (mmap_inherit_shm(as, &procs[leader].mm) < 0) {
        vmm_destroy_address_space(as);
        slot_unclaim(slot);
        return -ENOMEM;
    }

    // The caller's trapframe, verbatim, on the child's own kernel stack
    // -- so the child resumes at the instruction after the `int $0x80`
    // with every register the parent had, except the one that tells
    // them apart.
    uint64_t *tf = (uint64_t *)(kernel_stack_top(slot) - TRAPFRAME_WORDS * 8);
    for (int i = 0; i < TRAPFRAME_WORDS; i++) tf[i] = regs[i];
    tf[TF_RAX] = 0;

    procs[slot].pml4_phys  = as;
    procs[slot].kernel_rsp = (uint64_t)tf;
    proc_start_context(slot, tf);
    kstack_arm_slot(slot);
    fpu_save(procs[slot].fpu);
    procs[slot].wait_chan   = 0;
    procs[slot].wait_reason = 0;
    procs[slot].wake_at_ns  = 0;
    procs[slot].tgid     = slot + 1;
    procs[slot].fs_base  = procs[caller].fs_base;
    procs[slot].detached = 0;
    fd_clone(as, procs[leader].pml4_phys);
    procs[slot].ppid = leader + 1;
    procs[slot].prio = procs[caller].prio;
    procs[slot].vruntime = g_min_vruntime;
    signal_state_reset(slot);
    for (int i = 0; i <= SIGNAL_MAX; i++)
        procs[slot].actions[i] = procs[caller].actions[i];
    procs[slot].blocked = procs[caller].blocked;
    procs[slot].syscall_reissue = 0;
    procs[slot].ready   = 0;
    procs[slot].pgid    = procs[leader].pgid;
    // THE SESSION TOO. A fork that copied the group and not the session
    // put the child in session 0, so it could not take the terminal its
    // parent owned -- which is the whole point of having sessions.
    procs[slot].sid     = procs[leader].sid;
    k_strlcpy(procs[slot].name, procs[leader].name, sizeof procs[slot].name);
    k_strlcpy(procs[slot].exec_path, procs[leader].exec_path,
              sizeof procs[slot].exec_path);
    procs[slot].cpu_ns = 0;
    k_memcpy(&procs[slot].mm, &procs[leader].mm, sizeof procs[slot].mm);
    // ...which copied the region POINTER. Give the child its own, or
    // the two of them free one array twice.
    if (!mmap_clone_regions(&procs[slot].mm, &procs[leader].mm)) {
        // After fd_clone(): the child's table holds references to the
        // parent's descriptions (a pipe end among them), so it has to be
        // released, or that pipe never reaches EOF -- and the address
        // space was leaked outright.
        fd_release_all(as);
        vmm_destroy_address_space(as);
        procs[slot].state = SCHED_UNUSED;
        slot_unclaim(slot);
        return -1;
    }
    k_memcpy(&procs[slot].cwd, &procs[leader].cwd, sizeof procs[slot].cwd);
    procs[slot].state = SCHED_READY;
    slot_unclaim(slot);
    alive_count++;
    return slot + 1;
}

// --- exec ------------------------------------------------------------
//
// The caller's slot keeps its pid, parent, group, cwd and descriptors
// and gets a new image. THE NEW ADDRESS SPACE IS BUILT BEFORE THE OLD
// ONE IS TOUCHED, so a program that cannot be loaded is reported to a
// caller that still exists (POSIX: exec fails in place). Everything
// keyed by the old address space is either re-keyed (descriptors, the
// trace) or dropped through the same hooks an exit uses (shared
// mappings, the wakeword, windows, sound) -- an exec'd program has no
// idea it holds any of them. Returns 0 into a rewritten trapframe, or
// -errno with nothing changed.
int scheduler_exec(const char *path, const char *argvec, size_t argvec_len,
                   const char *env, uint64_t *regs) {
    if (current_index < 0) return -EPERM;
    int me = current_index;
    if (is_thread(me)) {
        // POSIX makes the exec'ing thread the leader, pid and all. Not
        // worth a second exit path: refuse, loudly.
        klog_printf(KLOG_ERR "exec: refused from thread %d -- only a process may exec\n", me + 1);
        return -EPERM;
    }
    uint64_t as = 0, entry = 0, user_rsp = 0, image_end = 0;
    if (!build_image(path, argvec, argvec_len, env, &as, &entry, &user_rsp, &image_end))
        return -ENOENT;

    uint64_t old = procs[me].pml4_phys;
    group_release_threads(me);
    strace_rekey(old, as);
    fd_rekey(old, as);
    proc_syscall_release(old);
    win_server_client_gone(me + 1);
    diag_provider_gone(me + 1);
    sound_process_gone(old);
    shm_process_gone(old);
    futex_wakeword_release(old);

    // A caught signal goes back to its default; an ignored one stays
    // ignored (POSIX). The pending set and the blocked mask are kept.
    for (int i = 0; i <= SIGNAL_MAX; i++)
        if (procs[me].actions[i].handler > SIG_IGN)
            procs[me].actions[i] = (struct k_sigaction){ 0, 0, 0, 0 };
    procs[me].syscall_reissue = 0;
    procs[me].ready = 0;
    procs[me].fs_base = 0;
    arch_set_fs_base(0);            // this return does not go through switch_to()
    fpu_init_state(procs[me].fpu);
    fpu_restore(procs[me].fpu);     // ...so the CPU's state is loaded here too
    mm_reset(me, image_end);
    proc_name_from_path(procs[me].name, sizeof procs[me].name, path);
    k_strlcpy(procs[me].exec_path, path ? path : "", sizeof procs[me].exec_path);

    // The caller's own trapframe, rewritten: it resumes at the new
    // image's entry with every register clear, as a spawn's first frame.
    for (int i = 0; i < TF_VECTOR; i++) regs[i] = 0;
    regs[TF_RIP]    = entry;
    regs[TF_CS]     = SEL_USER_CODE;
    regs[TF_RFLAGS] = 0x200;
    regs[TF_RSP]    = user_rsp;
    regs[TF_SS]     = SEL_USER_DATA;

    procs[me].pml4_phys = as;
    vmm_switch_address_space(as);   // CR3 first -- see vmm_destroy_address_space()
    vmm_destroy_address_space(old);
    return 0;
}

int scheduler_spawn(const char *path, const char *args) {
    return scheduler_spawn_piped(path, args, -1);
}

int scheduler_spawn_piped(const char *path, const char *args, int pipe_idx) {
    // No environment. Kernel-side spawners (init, the demo) have none
    // to pass -- an environment is a ring-3 idea that the kernel only
    // ever relays.
    return scheduler_spawn_env(path, args, pipe_idx, 0);
}

// The kernel-side spawns, all one path. `detached` decides fd 2 (below).
static int spawn_kernel(const char *path, const char *args, int pipe_idx,
                        const char *env, int detached) {
    // THE STRING FORM ENDS HERE: split into the vector everything below
    // carries. On the heap, since SPAWN_ARGS_MAX does not fit a frame.
    char *vec = kmalloc(SPAWN_ARGS_MAX + FS_PATH_MAX);
    if (!vec) return 0;
    int pid = 0;
    size_t vec_len = 0;
    if (elf_argv_from_string(path, args, vec, SPAWN_ARGS_MAX + FS_PATH_MAX, &vec_len)) {
        // 0 = inherit the spawner's group, which is what every kernel-side
        // caller wants: init's services and the demo's counters belong with
        // whatever started them. Parent 0 too: a kernel-side caller's child
        // gets the standard three fds (see spawn_from_fs()'s fd_inherit).
        //
        // **A DETACHED SPAWN'S STDERR IS THE KERNEL LOG** -- init (and
        // through it every service and the desktop), gui3's compositor,
        // the ktests' children. /bin/spawn reaches the same answer from
        // ring 3 with SPAWN_FD_KMSG. Nobody is waiting at the terminal for
        // them, so an error printed there lands on whatever the console
        // shows by then; `systemd-run` sends a transient unit's output to
        // the journal for the same reason. An ATTACHED one -- the shell
        // waiting on a command it ran -- keeps the console on all three.
        //
        // AND A DETACHED ONE NEVER GETS THE KERNEL CONTEXT'S TERMINAL:
        // while the serial debug console runs a command, a fresh table
        // names that terminal (fd_set_kernel_tty()), and a job started
        // in the background must not write into the console's replies.
        int err = detached ? fd_desc_alloc(&klog_fd_ops, -1) : -1;
        int con = detached && fd_kernel_tty() ? fd_desc_alloc(&console_fd_ops, -1) : -1;
        pid = scheduler_spawn_group(path, vec, vec_len, pipe_idx >= 0 ? pipe_idx : con,
                                    con, err, env, 0, 0);
        if (err >= 0) fd_desc_unref(err); // the child holds its own
        if (con >= 0) fd_desc_unref(con);
        // An ATTACHED one leads the kernel context's terminal session, if
        // there is one: its group is what Ctrl-C there interrupts.
        if (pid > 0 && !detached && fd_kernel_tty())
            tty_attach_kernel_session(fd_kernel_tty(), pid);
    }
    kfree(vec);
    return pid;
}

int scheduler_spawn_env(const char *path, const char *args, int pipe_idx,
                         const char *env) {
    return spawn_kernel(path, args, pipe_idx, env, 1);
}

int scheduler_spawn_attached(const char *path, const char *args) {
    return spawn_kernel(path, args, -1, 0, 0);
}

int scheduler_spawn_group(const char *path, const char *argv, size_t argv_len,
                           int pipe_idx, int stdin_desc, int stderr_desc,
                           const char *env, int pgid, uint64_t parent_pml4) {
    int slot = spawn_from_fs(path, argv, argv_len, pipe_idx, stdin_desc, stderr_desc,
                              env, pgid, parent_pml4);
    if (slot < 0) return 0;

    // Clear any events left over from the previous tenant of this slot.
    // Doing it at spawn rather than at reap is what makes this the only
    return slot + 1; // 1-based pid (see scheduler.h)
}
