// The second real disk-hosted ELF64 program (after userland/lspci.c) --
// a genuine syscall-driven userland process that replaces the shell's
// old kernel-space `ls` built-in (apps/shell_fs.c's cmd_ls()/list_cb(),
// removed the same build this file was added -- see CHANGELOG.md). No
// libc (freestanding, same as every other userland/*.c here) -- same
// syscall-wrapper/print-helper shape newsyscalls_test.c/lspci.c already
// established.
//
// Takes real argc/argv (process_run_ring3_args(), process.c) instead of
// a minimal opaque-string syscall -- see docs/decisions.md for why:
// this is meant to be the first of several /bin binaries that want
// ordinary C-style arguments, not a one-off. argv[0] is whatever path
// the shell invoked this binary as (always "/bin/ls" today, from
// apps/shell_sys.c's cmd_ls_bin()); argv[1..] are `-a`/`-l`/`-al`/`-la`
// flags and/or a single positional directory path. The shell resolves
// a relative positional path against its own `cwd` before ever handing
// it to this binary (see cmd_ls_bin()'s comment in shell_sys.c) --
// fs.c/fs.h has no cwd concept at all, and neither does this file; it
// only ever receives (or defaults to) a ready-to-use absolute path.
//
// `-a` is accepted but a no-op: there's no dotfile-hiding convention on
// this filesystem (fs_list() already returns everything a directory
// has), so there's nothing for `-a` to additionally reveal. Accepted
// rather than rejected so a habitual `ls -la` doesn't produce a usage
// error.
//
// Default output is colored via the new SYS_SET_COLOR syscall
// (directories vs. files), mirroring GNU coreutils' `ls --color=auto`
// -- unconditionally, not gated on a flag, per this feature's own
// scope (see CHANGELOG.md/docs/decisions.md).
#include <stdint.h>
#include "syscall_abi.h"
#include "vga.h" // enum vga_color only -- see this file's top comment

static inline int64_t syscall0(uint64_t num) {
    int64_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(num) : "memory");
    (void)ret;
    return ret;
}

static inline int64_t syscall2(uint64_t num, uint64_t arg1, uint64_t arg2) {
    int64_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2)
        : "memory"
    );
    return ret;
}

static inline int64_t syscall3(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    int64_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3)
        : "memory"
    );
    return ret;
}

static inline int64_t sys_write(const char *buf, uint64_t len) {
    return syscall3(SYS_WRITE, 1, (uint64_t)(uintptr_t)buf, len);
}

static inline int64_t sys_listdir(const char *path, struct dirent *out, uint64_t max) {
    return syscall3(SYS_LISTDIR, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)out, max);
}

static inline int64_t sys_set_color(enum vga_color fg, enum vga_color bg) {
    return syscall2(SYS_SET_COLOR, (uint64_t)fg, (uint64_t)bg);
}

static inline void sys_exit(int code) __attribute__((noreturn));
static inline void sys_exit(int code) {
    syscall2(SYS_EXIT, (uint64_t)(int64_t)code, 0);
    for (;;) { }
}

static uint64_t my_strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

static void put(const char *s) {
    sys_write(s, my_strlen(s));
}

static void put_udec(uint32_t n) {
    char buf[11]; // max uint32_t is 10 digits + '\0'
    int i = 10;
    buf[10] = '\0';
    if (n == 0) {
        put("0");
        return;
    }
    while (n > 0 && i > 0) {
        buf[--i] = (char)('0' + (n % 10));
        n /= 10;
    }
    put(&buf[i]);
}

static void put_padded(uint32_t n, int width) {
    uint32_t div = 1;
    for (int i = 1; i < width; i++) div *= 10;
    while (div > 1 && n < div) { put("0"); div /= 10; }
    put_udec(n);
}

static void put_timestamp(const struct rtc_time *t) {
    put_padded(t->month, 2);
    put("/");
    put_padded(t->day, 2);
    put("/");
    put_padded(t->year, 4);
    put(" ");
    put_padded(t->hour, 2);
    put(":");
    put_padded(t->minute, 2);
    put(":");
    put_padded(t->second, 2);
}

void _start(int argc, char **argv) {
    int show_long = 0;
    int show_all = 0; // accepted, no-op -- see this file's top comment
    const char *path = "/"; // shell always passes a resolved absolute
                             // path (its own cwd if none given); this
                             // default only matters if ls is ever
                             // invoked with no path at all some other way

    int got_path = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-') {
            for (uint64_t j = 1; a[j]; j++) {
                if (a[j] == 'l') show_long = 1;
                else if (a[j] == 'a') show_all = 1;
            }
        } else if (!got_path) {
            path = a;
            got_path = 1;
        }
    }
    (void)show_all;

    static struct dirent entries[SYS_LISTDIR_MAX];
    int64_t count = sys_listdir(path, entries, SYS_LISTDIR_MAX);
    if (count < 0) {
        put("ls: cannot access '");
        put(path);
        put("'\n");
        sys_exit(1);
    }

    for (int64_t i = 0; i < count; i++) {
        struct dirent *e = &entries[i];
        enum vga_color color = e->is_dir ? VGA_LIGHT_CYAN : VGA_LIGHT_GREY;

        if (show_long) {
            put(e->is_dir ? "d " : "- ");
            if (e->is_dir) {
                put("           "); // no size column for directories
            } else {
                // Right-align the size into a fixed 10-char field, same
                // shape as apps/shell_fs.c's print_stat_timestamp()
                // neighbor print_padded() but for decimal width instead
                // of leading zeros (a size has no natural digit count
                // to pad to).
                uint32_t n = e->size, t = n;
                int w = 1;
                while (t >= 10) { t /= 10; w++; }
                for (int p = 0; p < 10 - w; p++) put(" ");
                put_udec(n);
                put(" ");
            }
            put_timestamp(&e->modified);
            put("  ");
        }

        sys_set_color(color, VGA_BLACK);
        put(e->name);
        sys_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        if (e->is_dir) put("/");
        put("\n");
    }

    sys_exit(0);
}
