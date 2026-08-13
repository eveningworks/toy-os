#include "syscall.h"
#include "process.h"
#include "klog.h"
#include "vga.h"
#include "vmm.h"
#include "gfx.h"
#include "keyboard.h"
#include "scheduler.h"
#include "pmm.h"
#include "fs.h"
#include "string.h"
#include "tz.h"
#include "pci.h"
#include "cpuinfo.h"
#include "strace_internal.h"
#include <stddef.h>

// SYS_OPEN/SYS_READ/SYS_CLOSE state: a small global table of open files,
// same single-process-at-a-time scoping as the heap/window state below
// (see syscall_abi.h's comment on SYS_OPEN) -- each entry records which
// process (by CR3) it belongs to, so one process's fds can't be read,
// written, or closed by a different one, same isolation trick used
// throughout this file.
#define FD_TABLE_SIZE 8
#define FD_BASE 3 // fds 0/1/2 are reserved for stdin/stdout/stderr

enum fd_mode { FD_MODE_READ, FD_MODE_WRITE };

// FD_KIND_SOCKET added alongside FD_KIND_FILE (see SYS_SOCKET's doc
// comment in syscall_abi.h) -- one shared fd namespace/table for both,
// same as real Unix, rather than a second parallel table: SYS_CLOSE and
// syscall_process_exit_cleanup() below already only look at `used`/
// `owner_pml4`, so they work on a socket fd for free with no changes.
// Socket fds carry no real state yet (no domain/type distinction, no
// transport) -- the `socket` arm of the union below is deliberately
// empty; it exists so a socket slot has *some* member to be valid C,
// and as the obvious place to grow real per-socket state once a NIC
// driver exists.
enum fd_kind { FD_KIND_FILE, FD_KIND_SOCKET };

struct open_file {
    int used;
    uint64_t owner_pml4;
    enum fd_kind kind;
    union {
        struct {
            char name[FS_PATH_MAX];
            enum fd_mode mode;
            uint32_t offset; // read position; unused in write mode (always appends)
        } file;
        struct {
            int unused_placeholder; // no real socket state yet -- see SYS_SOCKET's doc comment
        } socket;
    };
};

static struct open_file fd_table[FD_TABLE_SIZE];

// SYS_LISTDIR scratch state -- fs_list() (fs.c) takes a plain callback
// with no context/userdata parameter, so there's nowhere to thread "which
// output array, how much room is left" through it directly. Bounce
// through these file-scope globals for the duration of a single
// SYS_LISTDIR call instead: safe because syscalls in this kernel are
// never reentrant or concurrent (same assumption SYS_WIN_* above already
// relies on).
static struct dirent *g_listdir_out = 0;
static uint32_t g_listdir_max = 0;
static uint32_t g_listdir_count = 0;
// Holds the (already-validated, NUL-terminated) directory path for the
// call in progress -- needed alongside `name` to build each entry's own
// full path for the fs_stat() call below (fs_list()'s callback only
// ever hands back the bare last component, per its own doc comment).
static char g_listdir_dir_path[FS_PATH_MAX];

static void listdir_collect(const char *name, uint32_t size, int is_dir) {
    if (g_listdir_count >= g_listdir_max) return;
    struct dirent *e = &g_listdir_out[g_listdir_count];
    k_strcpy(e->name, name);
    e->size = size;
    e->is_dir = (uint32_t)is_dir;

    // Build "<dir>/<name>" (or "/<name>" when dir is just "/") to look
    // up this entry's own timestamps -- see struct dirent's `modified`
    // field comment (syscall_abi.h) for why this is here at all.
    // Zeroed rather than left uninitialized on the rare failure path
    // (shouldn't happen for anything fs_list() itself just reported),
    // so a bug here shows up as an obviously-wrong 0000-00-00 rather
    // than reading stale/garbage struct bytes.
    char full_path[FS_PATH_MAX];
    size_t dl = k_strlen(g_listdir_dir_path);
    k_strcpy(full_path, g_listdir_dir_path);
    if (dl > 1) { // dir isn't just "/" -- needs a separating slash
        if (dl + 1 < FS_PATH_MAX) { full_path[dl] = '/'; full_path[dl + 1] = '\0'; dl++; }
    }
    size_t nl = k_strlen(name);
    if (dl + nl < FS_PATH_MAX) k_strcpy(full_path + dl, name);

    struct fs_timestamps ts;
    if (fs_stat(full_path, &ts)) {
        e->modified = ts.modified;
    } else {
        k_memset(&e->modified, 0, sizeof(e->modified));
    }

    g_listdir_count++;
}

// SYS_SBRK state for whichever single process syscall_reset_heap() was
// last armed for (see syscall.h's comment on why there's only one, same
// reasoning as g_process_ctx above). g_heap_mapped_end tracks how far
// physical pages have actually been allocated+mapped so far -- separate
// from g_heap_brk (the process-visible break) because SYS_SBRK only
// needs to map a new page the first time the break crosses into it, not
// on every call.
static uint64_t g_heap_pml4 = 0;
static uint64_t g_heap_base = 0;
static uint64_t g_heap_brk = 0;
static uint64_t g_heap_mapped_end = 0;

void syscall_reset_heap(uint64_t pml4_phys, uint64_t heap_base) {
    g_heap_pml4 = pml4_phys;
    g_heap_base = heap_base;
    g_heap_brk = heap_base;
    g_heap_mapped_end = heap_base;
}

// SYS_WIN_* state -- like the heap above, single-window/single-process
// at a time (see syscall_abi.h's comment on SYS_WIN_CREATE). Unlike the
// heap, this doesn't need an explicit "arm" call from whoever spawns
// the process: SYS_WIN_CREATE itself sets g_win_pml4 to the CALLING
// process's own CR3, so it's self-arming, and SYS_WIN_PRESENT's
// pml4-mismatch check (same trick as SYS_SBRK's) rejects any process
// that calls it without having created a window first, including a
// later, unrelated process that happens to run after this one exits.
//
// g_win_frames tracks each backing page's PHYSICAL address individually
// rather than assuming the run pmm_alloc_frame() hands back is
// contiguous (it usually is, for a fresh process with nothing else
// allocating concurrently, but pmm.h makes no such promise) -- so
// SYS_WIN_PRESENT can read the buffer directly via each frame's
// identity-mapped physical address without trusting that.
#define WIN_MAX_PAGES ((WIN_MAX_W * WIN_MAX_H * 4 + 4095) / 4096)
static uint64_t g_win_pml4 = 0;
static uint64_t g_win_frames[WIN_MAX_PAGES];
static uint32_t g_win_pages = 0;
static uint32_t g_win_w = 0, g_win_h = 0, g_win_pitch = 0;
static int32_t g_win_x = 0, g_win_y = 0;

#define WIN_TITLEBAR_H (gfx_char_h() + 8)

static int win_close_size(void) {
    int s = WIN_TITLEBAR_H - 6;
    return s < 14 ? 14 : s;
}

// Same hand-drawn diagonal cross as apps/wm.c's draw_close_icon() (see
// CHANGELOG for why a font glyph doesn't work in a small button) --
// duplicated rather than shared, since wm.c's version is `static` in a
// completely different translation unit (the GUI app layer), and
// kernel/core has no existing reason to link against apps/.
static void win_draw_close_icon(int x, int y, int size, uint32_t color) {
    int pad = size / 4;
    if (pad < 2) pad = 2;
    int thick = size >= 24 ? 1 : 0;
    for (int i = pad; i < size - pad; i++) {
        for (int t = -thick; t <= thick; t++) {
            gfx_put_pixel(x + i + t, y + i, color);
            gfx_put_pixel(x + i, y + i + t, color);
            gfx_put_pixel(x + i + t, y + (size - 1 - i), color);
            gfx_put_pixel(x + i, y + (size - 1 - i) + t, color);
        }
    }
}

// Reads one already-native-packed pixel (see userland/win_test.c's
// comment on why it can write raw values with no gfx_rgb()-equivalent
// of its own) out of the process's private window buffer.
// byte_offset is always a multiple of 4 (pitch is w*4, no padding), and
// 4096 is itself a multiple of 4, so a 4-byte pixel read here never
// straddles a page boundary -- each one lives entirely in exactly one
// tracked frame.
static uint32_t win_buf_read_pixel(uint64_t byte_offset) {
    uint32_t page = (uint32_t)(byte_offset / 4096);
    uint32_t off = (uint32_t)(byte_offset % 4096);
    if (page >= g_win_pages) return 0;
    return *(volatile uint32_t *)(uintptr_t)(g_win_frames[page] + off);
}

// The "server" half of the protocol: composites the current window
// buffer plus kernel-drawn chrome onto the real screen. Runs entirely
// in kernel space (this is a syscall handler), so -- unlike the
// process that owns the buffer -- it can call gfx_* directly with no
// syscall of its own needed to reach the real framebuffer.
static void win_present(void) {
    int titlebar_h = WIN_TITLEBAR_H;
    int close_size = win_close_size();
    uint32_t border = gfx_rgb(60, 60, 60);
    uint32_t titlebar = gfx_rgb(70, 110, 180);
    uint32_t titletext = gfx_rgb(255, 255, 255);
    uint32_t winbg = gfx_rgb(235, 235, 235);

    int win_w = (int)g_win_w;
    int win_h = (int)g_win_h;
    int total_h = titlebar_h + win_h;

    gfx_fill_rect(g_win_x, g_win_y, win_w, total_h, winbg);
    gfx_draw_rect(g_win_x, g_win_y, win_w, total_h, border);
    gfx_fill_rect(g_win_x + 1, g_win_y + 1, win_w - 2, titlebar_h, titlebar);
    gfx_draw_string(g_win_x + 6, g_win_y + (titlebar_h - gfx_char_h()) / 2,
                     "App", titletext, titlebar);

    int close_x = g_win_x + win_w - 6 - close_size;
    int close_y = g_win_y + (titlebar_h - close_size) / 2;
    gfx_fill_rect(close_x, close_y, close_size, close_size, gfx_rgb(190, 60, 60));
    win_draw_close_icon(close_x, close_y, close_size, gfx_rgb(255, 255, 255));

    int content_y = g_win_y + titlebar_h;
    for (int y = 0; y < win_h; y++) {
        for (int x = 0; x < win_w; x++) {
            uint32_t pixel = win_buf_read_pixel((uint64_t)y * g_win_pitch + (uint64_t)x * 4);
            gfx_put_pixel(g_win_x + x, content_y + y, pixel);
        }
    }
}

void syscall_process_exit_cleanup(uint64_t pml4_phys) {
    // Stop tracing before anything else -- this address space is about
    // to be destroyed, and a recycled CR3 landing on the same value
    // later must not silently inherit the trace (see strace.c).
    strace_release(pml4_phys);

    // Open fds this process never closed -- not part of any address
    // space (fs.c is a separate kernel resource, nothing about it is
    // memory-mapped into the process), so vmm_destroy_address_space()
    // below wouldn't reclaim these on its own.
    for (int i = 0; i < FD_TABLE_SIZE; i++) {
        if (fd_table[i].used && fd_table[i].owner_pml4 == pml4_phys) {
            fd_table[i].used = 0;
        }
    }

    // The single-slot heap/window "armed for this pml4" bookkeeping --
    // also not part of the address space itself (these are globals
    // right here in syscall.c), even though the PAGES they describe
    // (the heap's mapped range, the window's pixel buffer) ARE part of
    // it and get freed along with everything else below.
    if (g_heap_pml4 == pml4_phys) {
        g_heap_pml4 = 0;
        g_heap_base = 0;
        g_heap_brk = 0;
        g_heap_mapped_end = 0;
    }
    if (g_win_pml4 == pml4_phys) {
        g_win_pml4 = 0;
        g_win_pages = 0;
        g_win_w = 0;
        g_win_h = 0;
        g_win_pitch = 0;
        g_win_x = 0;
        g_win_y = 0;
    }

    // CR3 first -- see vmm_destroy_address_space()'s comment for why
    // freeing the frame CR3 still points at, before switching away from
    // it, would be a use-after-free.
    vmm_switch_address_space(vmm_kernel_pml4_phys());
    vmm_destroy_address_space(pml4_phys);
}

void syscall_dispatch(uint64_t *regs) {
    uint64_t rax = regs[14]; // syscall number
    uint64_t rdi = regs[9];  // first argument
    uint64_t rsi = regs[10]; // second argument
    uint64_t rdx = regs[11]; // third argument (SYS_WRITE/SYS_READ's length)

    // `strace` (apps/shell_sys.c) hooks in here, and only here -- every
    // ring-3 syscall goes through this one dispatcher, so nothing
    // per-syscall is needed. An untraced process pays strace_active()'s
    // single global compare. The line is formatted now (the arguments
    // must be read before a handler can overwrite what they point at)
    // but emitted after the handler returns, once the return value is
    // known -- see kernel/proc/strace.c's top comment.
    int traced = strace_active();
    if (traced) {
        strace_begin(rax, rdi, rsi, rdx);
        if (rax == SYS_EXIT) {
            // The one handler that may never return (the legacy
            // process_context_exit() path doesn't), so its line has to
            // be closed out before dispatching rather than after.
            strace_end_noreturn();
            traced = 0;
        }
    }

    if (rax == SYS_EXIT) {
        int code = (int)rdi;
        klog_write("syscall: exit() called by ring-3 process\n");
        syscall_process_exit_cleanup(vmm_current_pml4());
        if (scheduler_current_pid()) {
            // Scheduler-managed process (spawned by scheduler_demo_run(),
            // see scheduler.c) -- hand its CPU slot to the next ready
            // process (or back to the shell) instead of the old
            // single-process longjmp-style return below. Returns
            // normally; isr_common's epilogue resumes whatever
            // scheduler_on_exit() picked, via g_next_kernel_rsp.
            scheduler_on_exit(code);
        } else {
            // Legacy path (process.c's process_context_exit()) --
            // doesn't return.
            process_context_exit(code);
        }
    } else if (rax == SYS_WRITE) {
        // ABI: RDI = fd (was the buffer pointer before SYS_OPEN/SYS_READ
        // existed -- see the "ABI NOTE" on SYS_WRITE in syscall_abi.h),
        // RSI = buffer pointer, RDX = length.
        int fd = (int)rdi;
        uint64_t buf_ptr = rsi;
        uint64_t len = rdx;
        if (len > SYS_WRITE_MAX) len = SYS_WRITE_MAX;

        // Validate the buffer before touching it -- CR3 is still the
        // calling process's own page tables at this point (a syscall
        // doesn't switch address spaces), so without this check a
        // process could hand the kernel a kernel-only address (still
        // *present*, since PML4 entry 0 is shared with every process --
        // see vmm.h) and get the kernel, running at full privilege, to
        // read it on the process's behalf, even though the process
        // could never legally read that address itself.
        uint64_t pml4 = vmm_current_pml4();

        if (fd == 1 || fd == 2) { // stdout / stderr -> the console, as before
            if (!vmm_validate_user_range(pml4, buf_ptr, len)) {
                klog_write("syscall: write() rejected -- invalid buffer pointer\n");
                regs[14] = (uint64_t)-1; // simplified error indicator (no errno yet)
            } else {
                const char *buf = (const char *)(uintptr_t)buf_ptr;
                for (uint64_t i = 0; i < len; i++) {
                    vga_putc(buf[i]);
                }
                regs[14] = len; // return value (bytes written) goes back via RAX
            }
        } else { // a real file, opened via SYS_OPEN
            int slot = fd - FD_BASE;
            // kind check: a socket fd (SYS_SOCKET) reaching here means
            // the caller used the wrong syscall -- SYS_SEND is the only
            // way to write to a socket fd -- so it's rejected the same
            // as any other bad fd, not silently treated as a file.
            if (slot < 0 || slot >= FD_TABLE_SIZE || !fd_table[slot].used ||
                fd_table[slot].kind != FD_KIND_FILE ||
                fd_table[slot].owner_pml4 != pml4 || fd_table[slot].file.mode != FD_MODE_WRITE) {
                klog_write("syscall: write() rejected -- bad fd\n");
                regs[14] = (uint64_t)-1;
            } else if (!vmm_validate_user_range(pml4, buf_ptr, len)) {
                klog_write("syscall: write() rejected -- invalid buffer pointer\n");
                regs[14] = (uint64_t)-1;
            } else {
                // fs_write() (fs.c) works on NUL-terminated C strings,
                // not explicit-length buffers -- see syscall_abi.h's
                // comment on SYS_OPEN for why. Copy into a NUL-terminated
                // scratch buffer (len is already capped at
                // SYS_WRITE_MAX, so this is always big enough) before
                // handing it to fs_write(), rather than changing fs.c
                // itself this round.
                char tmp[SYS_WRITE_MAX + 1];
                const char *ubuf = (const char *)(uintptr_t)buf_ptr;
                for (uint64_t i = 0; i < len; i++) tmp[i] = ubuf[i];
                tmp[len] = '\0';
                fs_write(fd_table[slot].file.name, tmp, 1); // 1 = append
                regs[14] = len;
            }
        }
    } else if (rax == SYS_READ) {
        int fd = (int)rdi;
        uint64_t buf_ptr = rsi;
        uint64_t len = rdx;
        if (len > SYS_WRITE_MAX) len = SYS_WRITE_MAX;

        uint64_t pml4 = vmm_current_pml4();
        int slot = fd - FD_BASE;
        // Same kind check as SYS_WRITE above -- SYS_RECV is the only
        // way to read from a socket fd.
        if (slot < 0 || slot >= FD_TABLE_SIZE || !fd_table[slot].used ||
            fd_table[slot].kind != FD_KIND_FILE ||
            fd_table[slot].owner_pml4 != pml4 || fd_table[slot].file.mode != FD_MODE_READ) {
            klog_write("syscall: read() rejected -- bad fd\n");
            regs[14] = (uint64_t)-1;
        } else if (!vmm_validate_user_range(pml4, buf_ptr, len)) {
            klog_write("syscall: read() rejected -- invalid buffer pointer\n");
            regs[14] = (uint64_t)-1;
        } else {
            // fs_read_range(), NOT fs_read(). This used to call
            // fs_read(), which reads the WHOLE file into a kmalloc'd
            // buffer, and then copied out just the `len` bytes at the
            // fd's offset -- so streaming a file cost (file size) of
            // disk reads per call. Fine while the only things ring 3
            // ever opened were a few hundred bytes; quadratic the
            // moment anything real showed up. /bin/lspci reading the
            // 1.6MB pci.ids in 1KB chunks turned that into ~2.6GB of
            // reads and took 35 seconds. With a range read it's ~0.6s.
            //
            // fs_read_range() reports 0 both at EOF and on any error
            // (fs.h says so explicitly), which happens to be exactly
            // the behaviour wanted here -- a file deleted mid-read by
            // another shell should read as EOF, not fabricate data or
            // fault.
            uint32_t off = fd_table[slot].file.offset;
            char *ubuf = (char *)(uintptr_t)buf_ptr;
            uint32_t n = fs_read_range(fd_table[slot].file.name, off, ubuf, (uint32_t)len);
            fd_table[slot].file.offset += n;
            regs[14] = n;
        }
    } else if (rax == SYS_OPEN) {
        uint64_t pml4 = vmm_current_pml4();

        // Path length isn't known up front -- validate the max any
        // fs.c name can be (FS_PATH_MAX) rather than dereference an
        // unvalidated pointer to find out. A caller whose actual buffer
        // is shorter than that (but still followed by unmapped memory)
        // would get rejected here even if the real string is safely
        // NUL-terminated well before the end -- an acceptable tradeoff
        // for a path buffer this small.
        if (!vmm_validate_user_range(pml4, rdi, FS_PATH_MAX)) {
            klog_write("syscall: open() rejected -- invalid path pointer\n");
            regs[14] = (uint64_t)-1;
        } else {
            const char *upath = (const char *)(uintptr_t)rdi;
            char name[FS_PATH_MAX];
            uint32_t n = 0;
            while (n < FS_PATH_MAX - 1 && upath[n] != '\0') { name[n] = upath[n]; n++; }
            name[n] = '\0';

            uint32_t flags = (uint32_t)rsi;
            int want_write = (flags & SYS_O_WRITE) != 0;
            int want_creat = (flags & SYS_O_CREAT) != 0;
            int want_trunc = (flags & SYS_O_TRUNC) != 0;

            uint32_t existing_size = 0;
            int exists = fs_read(name, &existing_size) != 0;

            if (!exists && !(want_write && want_creat)) {
                klog_write("syscall: open() rejected -- file not found\n");
                regs[14] = (uint64_t)-1;
            } else {
                int slot = -1;
                for (int i = 0; i < FD_TABLE_SIZE; i++) {
                    if (!fd_table[i].used) { slot = i; break; }
                }
                if (slot < 0) {
                    klog_write("syscall: open() rejected -- fd table full\n");
                    regs[14] = (uint64_t)-1;
                } else {
                    if (want_write) {
                        if (!exists) fs_touch(name);
                        if (want_trunc) fs_write(name, "", 0); // 0 = overwrite, not append
                    }
                    fd_table[slot].kind = FD_KIND_FILE;
                    k_strcpy(fd_table[slot].file.name, name);
                    fd_table[slot].used = 1;
                    fd_table[slot].owner_pml4 = pml4;
                    fd_table[slot].file.mode = want_write ? FD_MODE_WRITE : FD_MODE_READ;
                    fd_table[slot].file.offset = 0;
                    regs[14] = (uint64_t)(FD_BASE + slot);
                }
            }
        }
    } else if (rax == SYS_CLOSE) {
        // Kind-agnostic on purpose -- a socket fd (SYS_SOCKET) has no
        // file-specific state to tear down, so the same "just clear
        // `used`" logic that's always worked for file fds already works
        // for socket fds too, with no changes needed here.
        uint64_t pml4 = vmm_current_pml4();
        int fd = (int)rdi;
        int slot = fd - FD_BASE;
        if (slot < 0 || slot >= FD_TABLE_SIZE || !fd_table[slot].used ||
            fd_table[slot].owner_pml4 != pml4) {
            regs[14] = (uint64_t)-1;
        } else {
            fd_table[slot].used = 0;
            regs[14] = 0;
        }
    } else if (rax == SYS_SOCKET) {
        // See syscall_abi.h's SYS_SOCKET doc comment -- domain/type are
        // reserved for future use and must be 0 for now, rejected
        // otherwise so a caller relying on a real value being honored
        // fails loudly today rather than silently once one exists.
        uint64_t pml4 = vmm_current_pml4();
        uint64_t domain = rdi;
        uint64_t type = rsi;
        if (domain != 0 || type != 0) {
            klog_write("syscall: socket() rejected -- nonzero domain/type (not supported yet)\n");
            regs[14] = (uint64_t)-1;
        } else {
            int slot = -1;
            for (int i = 0; i < FD_TABLE_SIZE; i++) {
                if (!fd_table[i].used) { slot = i; break; }
            }
            if (slot < 0) {
                klog_write("syscall: socket() rejected -- fd table full\n");
                regs[14] = (uint64_t)-1;
            } else {
                fd_table[slot].kind = FD_KIND_SOCKET;
                fd_table[slot].used = 1;
                fd_table[slot].owner_pml4 = pml4;
                regs[14] = (uint64_t)(FD_BASE + slot);
            }
        }
    } else if (rax == SYS_SEND || rax == SYS_RECV) {
        // Both share one branch -- same fd validation, same "no
        // transport yet" outcome (see syscall_abi.h). Doesn't touch the
        // caller's buffer at all (nothing is actually sent/received),
        // so unlike SYS_WRITE/SYS_READ there's no buffer pointer to
        // validate here -- only the fd itself.
        uint64_t pml4 = vmm_current_pml4();
        int fd = (int)rdi;
        int slot = fd - FD_BASE;
        if (slot < 0 || slot >= FD_TABLE_SIZE || !fd_table[slot].used ||
            fd_table[slot].kind != FD_KIND_SOCKET || fd_table[slot].owner_pml4 != pml4) {
            klog_write(rax == SYS_SEND ? "syscall: send() rejected -- bad fd\n"
                                        : "syscall: recv() rejected -- bad fd\n");
        } else {
            klog_write(rax == SYS_SEND ? "syscall: send() -- no transport yet, failing\n"
                                        : "syscall: recv() -- no transport yet, failing\n");
        }
        regs[14] = (uint64_t)-1; // always fails for now -- see syscall_abi.h
    } else if (rax == SYS_GUI_INIT) {
        uint64_t pml4 = vmm_current_pml4();

        if (!vmm_validate_user_range(pml4, rdi, sizeof(struct gui_info))) {
            klog_write("syscall: gui_init() rejected -- invalid info pointer\n");
            regs[14] = 0;
        } else {
            struct gui_info info;
            info.width = (uint32_t)gfx_width();
            info.height = (uint32_t)gfx_height();
            info.pitch = gfx_framebuffer_pitch();
            info.bpp = gfx_framebuffer_bpp();
            *(struct gui_info *)(uintptr_t)rdi = info;

            uint64_t fb_phys = gfx_framebuffer_phys();
            uint64_t fb_size = (uint64_t)info.pitch * info.height;
            uint64_t pages = (fb_size + 4095) / 4096;

            int ok = 1;
            for (uint64_t i = 0; i < pages; i++) {
                if (!vmm_map_user_page(pml4, GUI_FB_VADDR + i * 4096, fb_phys + i * 4096)) {
                    ok = 0;
                    break;
                }
            }
            klog_write(ok ? "syscall: gui_init() mapped the framebuffer\n"
                             : "syscall: gui_init() failed to map the framebuffer\n");
            regs[14] = (uint64_t)ok;
        }
    } else if (rax == SYS_GUI_POLL_KEY) {
        int key = keyboard_try_getchar(); // already non-blocking
        regs[14] = (uint64_t)(int64_t)key;
    } else if (rax == SYS_READ_KEY) {
        // Non-blocking, same as SYS_GUI_POLL_KEY above (echo.c spins,
        // calling this again if it gets -1) -- NOT a design choice,
        // a hard requirement. A genuinely blocking version was tried
        // first: `sti` then keyboard_getchar()'s `hlt` loop, so a real
        // keyboard IRQ could land while this syscall was still on the
        // stack. It worked for exactly one keystroke and then hung --
        // g_next_kernel_rsp (idt.c) is a single global "where to resume"
        // pointer, correct for the scheduler's use (see scheduler.c's
        // design comment) but never meant to be reentrant: the nested
        // IRQ1 handler overwrites it while the outer int-0x80 handler
        // is still executing, so by the time THIS handler's own
        // isr_common epilogue runs, it resumes into a stale frame
        // instead of back into ring 3. Never make a syscall handler
        // block-with-interrupts-on in this codebase without fixing that
        // global first.
        int key = keyboard_try_getchar();
        regs[14] = (uint64_t)(int64_t)key;
    } else if (rax == SYS_SBRK) {
        uint64_t pml4 = vmm_current_pml4();
        uint64_t inc = rdi;

        // Refuse unless syscall_reset_heap() armed a heap for exactly
        // this address space -- e.g. a process that never had its heap
        // set up (heap_base defaults to 0, which can't equal a real
        // CR3) or, in principle, a scheduler-managed process (this
        // whole mechanism is legacy-single-process-only, see the header
        // comment) gets a clean -1 instead of silently mapping pages
        // into the wrong address space.
        if (g_heap_pml4 == 0 || pml4 != g_heap_pml4) {
            klog_write("syscall: sbrk() rejected -- no heap armed for this process\n");
            regs[14] = (uint64_t)-1;
        } else {
            uint64_t old_brk = g_heap_brk;
            uint64_t new_brk = old_brk + inc;
            int ok = 1;

            while (g_heap_mapped_end < new_brk) {
                uint64_t frame = pmm_alloc_frame();
                if (!frame) { ok = 0; break; }
                for (size_t i = 0; i < 4096; i++) ((uint8_t *)(uintptr_t)frame)[i] = 0;
                if (!vmm_map_user_page(pml4, g_heap_mapped_end, frame)) {
                    pmm_free_frame(frame);
                    ok = 0;
                    break;
                }
                g_heap_mapped_end += 4096;
            }

            if (!ok) {
                klog_write("syscall: sbrk() rejected -- out of physical memory\n");
                regs[14] = (uint64_t)-1;
            } else {
                g_heap_brk = new_brk;
                regs[14] = old_brk; // classic sbrk() contract: returns the OLD break
            }
        }
    } else if (rax == SYS_WIN_CREATE) {
        uint64_t pml4 = vmm_current_pml4();

        if (!vmm_validate_user_range(pml4, rdi, sizeof(struct win_request))) {
            klog_write("syscall: win_create() rejected -- invalid request pointer\n");
            regs[14] = 0;
        } else {
            struct win_request req = *(struct win_request *)(uintptr_t)rdi;
            int bad_size = (req.w == 0 || req.h == 0 || req.w > WIN_MAX_W || req.h > WIN_MAX_H);

            if (bad_size) {
                klog_write("syscall: win_create() rejected -- bad size\n");
                regs[14] = 0;
            } else {
                uint64_t size = (uint64_t)req.w * 4 * req.h;
                uint32_t pages_needed = (uint32_t)((size + 4095) / 4096);
                uint32_t i;
                int ok = 1;

                for (i = 0; i < pages_needed; i++) {
                    uint64_t frame = pmm_alloc_frame();
                    if (!frame) { ok = 0; break; }
                    for (size_t b = 0; b < 4096; b++) ((uint8_t *)(uintptr_t)frame)[b] = 0;
                    if (!vmm_map_user_page(pml4, WIN_BUF_VADDR + (uint64_t)i * 4096, frame)) {
                        pmm_free_frame(frame);
                        ok = 0;
                        break;
                    }
                    g_win_frames[i] = frame;
                }

                if (!ok) {
                    for (uint32_t j = 0; j < i; j++) pmm_free_frame(g_win_frames[j]);
                    klog_write("syscall: win_create() rejected -- out of physical memory\n");
                    regs[14] = 0;
                } else {
                    g_win_pml4 = pml4;
                    g_win_pages = pages_needed;
                    g_win_w = req.w;
                    g_win_h = req.h;
                    g_win_pitch = req.w * 4;
                    g_win_x = req.x;
                    g_win_y = req.y;

                    req.pitch = g_win_pitch;
                    req.bpp = 32;
                    *(struct win_request *)(uintptr_t)rdi = req;
                    regs[14] = 1;
                }
            }
        }
    } else if (rax == SYS_WIN_PRESENT) {
        uint64_t pml4 = vmm_current_pml4();
        if (g_win_pml4 == 0 || pml4 != g_win_pml4) {
            regs[14] = (uint64_t)-1;
        } else {
            win_present();
            regs[14] = 1;
        }
    } else if (rax == SYS_UNLINK) {
        uint64_t pml4 = vmm_current_pml4();
        if (!vmm_validate_user_range(pml4, rdi, FS_PATH_MAX)) {
            klog_write("syscall: unlink() rejected -- invalid path pointer\n");
            regs[14] = 0;
        } else {
            const char *upath = (const char *)(uintptr_t)rdi;
            char name[FS_PATH_MAX];
            uint32_t n = 0;
            while (n < FS_PATH_MAX - 1 && upath[n] != '\0') { name[n] = upath[n]; n++; }
            name[n] = '\0';
            regs[14] = (uint64_t)fs_delete(name);
        }
    } else if (rax == SYS_LISTDIR) {
        uint64_t pml4 = vmm_current_pml4();
        uint32_t max = (uint32_t)rdx;
        if (max > SYS_LISTDIR_MAX) max = SYS_LISTDIR_MAX;

        if (!vmm_validate_user_range(pml4, rdi, FS_PATH_MAX) ||
            !vmm_validate_user_range(pml4, rsi, (uint64_t)max * sizeof(struct dirent))) {
            klog_write("syscall: listdir() rejected -- invalid pointer\n");
            regs[14] = (uint64_t)-1;
        } else {
            const char *upath = (const char *)(uintptr_t)rdi;
            char path[FS_PATH_MAX];
            uint32_t n = 0;
            while (n < FS_PATH_MAX - 1 && upath[n] != '\0') { path[n] = upath[n]; n++; }
            path[n] = '\0';

            g_listdir_out = (struct dirent *)(uintptr_t)rsi;
            g_listdir_max = max;
            g_listdir_count = 0;
            k_strcpy(g_listdir_dir_path, path); // see listdir_collect()'s per-entry fs_stat()
            fs_list(path, listdir_collect);
            regs[14] = g_listdir_count;
            g_listdir_out = 0; // don't leave a stale user pointer armed
                                // between calls -- next call re-arms it
        }
    } else if (rax == SYS_GETTIME) {
        uint64_t pml4 = vmm_current_pml4();
        if (!vmm_validate_user_range(pml4, rdi, sizeof(struct rtc_time))) {
            klog_write("syscall: gettime() rejected -- invalid pointer\n");
            regs[14] = 0;
        } else {
            struct rtc_time t;
            rtc_read_local(&t);
            *(struct rtc_time *)(uintptr_t)rdi = t;
            regs[14] = 1;
        }
    } else if (rax == SYS_YIELD) {
        // Reuse scheduler_tick()'s exact mechanism (the same one the
        // 100Hz timer IRQ drives) instead of inventing a second
        // reschedule path -- `regs` is this process's own trapframe,
        // laid out identically to what idt.c hands scheduler_tick() on
        // a real timer interrupt, so calling it here is indistinguishable
        // from "the timer happened to fire right now." A no-op for
        // non-scheduler-managed processes (scheduler_current_pid() ==
        // 0) -- nothing to yield to under the older single-process path.
        if (scheduler_current_pid()) {
            scheduler_tick(regs);
        }
        regs[14] = 0;
    } else if (rax == SYS_PCI_COUNT) {
        regs[14] = (uint64_t)pci_device_count();
    } else if (rax == SYS_PCI_INFO) {
        uint64_t pml4 = vmm_current_pml4();
        int index = (int)rdi;
        const struct pci_device *dev = pci_device_at(index);
        if (!dev || !vmm_validate_user_range(pml4, rsi, sizeof(struct pci_device))) {
            klog_write("syscall: pci_info() rejected -- bad index or invalid pointer\n");
            regs[14] = (uint64_t)-1;
        } else {
            *(struct pci_device *)(uintptr_t)rsi = *dev;
            regs[14] = 1;
        }
    } else if (rax == SYS_CPU_INFO) {
        uint64_t pml4 = vmm_current_pml4();
        if (!vmm_validate_user_range(pml4, rdi, sizeof(struct cpu_info))) {
            klog_write("syscall: cpu_info() rejected -- invalid user pointer\n");
            regs[14] = (uint64_t)-1;
        } else {
            cpu_info_get((struct cpu_info *)(uintptr_t)rdi);
            regs[14] = 1;
        }
    } else if (rax == SYS_SET_COLOR) {
        if (rdi > VGA_WHITE || rsi > VGA_WHITE) {
            regs[14] = (uint64_t)-1;
        } else {
            vga_set_color((enum vga_color)rdi, (enum vga_color)rsi);
            regs[14] = 1;
        }
    }

    if (traced) strace_end(rax, regs[14]);

    // Unrecognized syscall number: no-op. Falling through here means
    // isr_dispatch returns normally, isr_common's usual epilogue runs,
    // and ring 3 resumes right after its `int 0x80`.
}
