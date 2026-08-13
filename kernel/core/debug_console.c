// See debug_console.h for the design writeup. Output goes through
// klog_write()/klog_write_dec()/klog_write_hex() rather than raw
// serial_write() calls -- same "every kernel diagnostic goes through
// klog" convention every other kernel-side message already follows,
// which also means this console's own responses land in the `dmesg`
// ring buffer alongside everything else (occasionally noisy, but
// consistent, and reuses klog's number-formatting instead of this file
// needing its own). Typed input is echoed with a raw serial_putc()
// call instead, since that's local terminal echo, not a diagnostic
// message -- it has no business in the dmesg ring buffer.
//
// A second, unrelated writer sharing COM1's TX line with ordinary
// klog_write() calls from anywhere else in the kernel (a driver
// logging something while this console is mid-response, say) can
// interleave byte-by-byte on the wire -- there's no output lock
// anywhere in this kernel (see serial.c/klog.c), and adding one here
// alone wouldn't fix the general case. Accepted as a known rough edge
// for a debug-only tool rather than solved with new synchronization
// machinery; see docs/decisions.md.
#include "debug_console.h"
#include "serial.h"
#include "klog.h"
#include "string.h"
#include "pmm.h"
#include "heap.h"
#include "pci.h"
#include "fs.h"
#include "ktest_run.h"
#include "vga.h"

#define DBG_LINE_MAX 128
#define DBG_PROMPT "\r\ndbg> "

static char line_buf[DBG_LINE_MAX];
static int line_len = 0;

static void dbg_cmd_help(void) {
    klog_write("Commands:\r\n");
    klog_write("  help        - this list\r\n");
    klog_write("  meminfo     - physical frame + kernel heap usage\r\n");
    klog_write("  lsdev       - enumerated PCI devices\r\n");
    klog_write("  lsfs [path] - list a filesystem directory (default /)\r\n");
    klog_write("  ktest [suite] - run the in-kernel test suite\r\n");
}

// The one command here that isn't read-only inspection. This console's
// scope note (debug_console.h) says no filesystem mutation and no
// process control, and the filesystem tests do write -- they create and
// delete /.ktest_tmp. It's here anyway because this is the only input
// path that works with no display, no QMP and no keyboard emulation,
// which is exactly what `make test` and CI need (tools/ktest_run.py
// drives this). Running tests is the deliberate exception, not the
// start of a general-purpose serial shell.
//
// ktest_run_all() prints through vga_write(), which on this path is the
// console's own output -- the report comes back down the same wire the
// command arrived on.
// ktest_run_all() reports through vga_write(), which by default paints
// the physical screen -- so run it behind a sink that redirects that
// output down this wire instead. Without this the tests genuinely run
// over serial but their report is invisible here (only the klog_write()
// lines from inside individual tests come through), which is exactly
// how the first version of tools/ktest_run.py managed to time out
// waiting for a verdict that was being printed to a screen nobody was
// looking at.
//
// The sink interface (vga.h) is the same one apps/terminal.c uses to
// put shell output into a window; only putc is meaningful here, since a
// serial line has no cursor to back over, no page to clear and no
// colours.
static void dbg_sink_putc(void *ctx, char c) {
    (void)ctx;
    if (c == '\n') serial_putc('\r'); // serial terminals want CRLF
    serial_putc(c);
}
static void dbg_sink_backspace(void *ctx) { (void)ctx; }
static void dbg_sink_clear(void *ctx) { (void)ctx; }
static void dbg_sink_set_color(void *ctx, enum vga_color fg, enum vga_color bg) {
    (void)ctx; (void)fg; (void)bg;
}

static void dbg_cmd_ktest(const char *suite) {
    const struct vga_sink serial_sink = {
        .ctx = 0,
        .putc = dbg_sink_putc,
        .backspace = dbg_sink_backspace,
        .clear = dbg_sink_clear,
        .set_color = dbg_sink_set_color,
        .rows = 0, // no page height -- vga_rows() falls back to a default
    };
    const struct vga_sink *prev = vga_set_sink(&serial_sink);
    ktest_run_all(suite && suite[0] ? suite : 0);
    vga_set_sink(prev);
}

static void dbg_cmd_meminfo(void) {
    uint64_t total = pmm_total_frames(), free = pmm_free_frames();
    uint64_t used = total - free;
    klog_write("phys: ");
    klog_write_dec((uint32_t)(used * 4));
    klog_write("K used / ");
    klog_write_dec((uint32_t)(total * 4));
    klog_write("K total (");
    klog_write_dec((uint32_t)(free * 4));
    klog_write("K free)\r\n");

    klog_write("heap: ");
    klog_write_dec((uint32_t)heap_used_bytes());
    klog_write(" bytes used / ");
    klog_write_dec((uint32_t)heap_total_bytes());
    klog_write(" bytes claimed from pmm\r\n");
}

static void dbg_cmd_lsdev(void) {
    int n = pci_device_count();
    klog_write("PCI devices (");
    klog_write_dec((uint32_t)n);
    klog_write("):\r\n");
    for (int i = 0; i < n; i++) {
        const struct pci_device *d = pci_device_at(i);
        if (!d) continue;
        klog_write("  ");
        klog_write_dec(d->bus);
        klog_write(":");
        klog_write_dec(d->device);
        klog_write(".");
        klog_write_dec(d->function);
        klog_write("  ");
        klog_write_hex(d->vendor_id);
        klog_write(":");
        klog_write_hex(d->device_id);
        klog_write("  ");
        klog_write(pci_class_name(d->class_code, d->subclass));
        klog_write("\r\n");
    }
}

// fs_list()'s callback has no userdata parameter (see fs.h's own
// comment on that convention) -- nothing here needs cross-call state,
// so a plain static function is enough.
static void dbg_lsfs_cb(const char *name, uint32_t size, int is_dir) {
    klog_write("  ");
    klog_write(name);
    if (is_dir) {
        klog_write("/\r\n");
    } else {
        klog_write("  (");
        klog_write_dec(size);
        klog_write(" bytes)\r\n");
    }
}

static void dbg_cmd_lsfs(const char *arg) {
    const char *path = (arg && arg[0]) ? arg : "/";
    klog_write("ls ");
    klog_write(path);
    klog_write(":\r\n");
    fs_list(path, dbg_lsfs_cb);
}

// Splits `line` into a command word and (optionally) one trailing
// argument at the first space -- same lightweight convention
// apps/shell.c's own dispatcher uses; no quoting, no multiple args,
// this is a debug tool, not a real shell. Mutates `line` in place
// (null-terminates the command word), same as shell_dispatch() does to
// its own line buffer.
static void dbg_dispatch(char *line) {
    int i = 0;
    while (line[i] && line[i] != ' ') i++;
    int had_space = (line[i] == ' ');
    line[i] = '\0';
    const char *arg = had_space ? line + i + 1 : "";

    if (k_strcmp(line, "help") == 0) dbg_cmd_help();
    else if (k_strcmp(line, "meminfo") == 0) dbg_cmd_meminfo();
    else if (k_strcmp(line, "lsdev") == 0) dbg_cmd_lsdev();
    else if (k_strcmp(line, "lsfs") == 0) dbg_cmd_lsfs(arg);
    else if (k_strcmp(line, "ktest") == 0) dbg_cmd_ktest(arg);
    else {
        klog_write("unknown command: ");
        klog_write(line);
        klog_write(" (try 'help')\r\n");
    }
}

void debug_console_init(void) {
    line_len = 0;
    klog_write("dbg: serial debug console ready (COM1) -- type 'help'");
    serial_write(DBG_PROMPT);
}

void debug_console_poll(void) {
    int c;
    while ((c = serial_try_getc()) >= 0) {
        if (c == '\r' || c == '\n') {
            serial_write("\r\n");
            line_buf[line_len] = '\0';
            if (line_len > 0) dbg_dispatch(line_buf);
            line_len = 0;
            serial_write(DBG_PROMPT);
        } else if (c == '\b' || c == 0x7F) {
            if (line_len > 0) {
                line_len--;
                serial_write("\b \b");
            }
        } else if (line_len < DBG_LINE_MAX - 1 && c >= 32 && c < 127) {
            line_buf[line_len++] = (char)c;
            serial_putc((char)c); // local echo -- see this file's top comment
        }
        // other control bytes (Tab, Ctrl+*, ...) silently ignored
    }
}
