// See debug_console.h for the design writeup.
//
// REPLIES ARE NOT LOG LINES. Everything this console prints goes to its
// own port through dbg_write()/dbg_printf() -- COM2 when a UART is there,
// COM1 otherwise (serial.h) -- and NOT through klog: a reply is not an
// event, it has no business in dmesg, and while the two shared a wire a
// log line could land inside a reply and tear it. With COM2 present the
// log owns COM1 alone and nothing can interleave with a reply at all;
// on a one-port machine `readfile` still frames a file atomically.
#include "clocksource.h"
#include "debug_console.h"
#include "serial.h"
#include "klog.h"
#include "display.h"  // lsdev names the active display driver
#include "usb.h"
#include "aml.h"      // the ACPI namespace, for `aml`
#include "lapic.h"       // lsdev reports the LAPIC and its MSI vectors
#include "clockevent.h"  // ...and which device is driving the tick
#include "idt.h"      // idt_spurious_count()
#include "sound.h"    // lsdev names the sound devices and which is active
#include "input.h"    // ...and every registered input source
#include "virtio_input.h" // ...and whether virtio input is actually delivering
#include "win_role.h"
#include "diag.h"       // `gui` is one provider on the diagnostic registry
#include "kfmt.h"
#include "string.h"
#include "pmm.h"
#include "heap.h"
#include "pci.h"
#include "fs.h"
#include "scheduler.h" // scheduler_preempt_disable() -- readfile's atomic frame
#include "ktest_run.h"
#include "shell.h" // shell_dispatch -- `sh` runs the real shell, it doesn't reimplement one
#include "vga.h"
#include <stdarg.h>
#include "multiboot.h" // multiboot_cmdline() -- `debugcon`
#include "kerrno.h" // EBUSY -- the gui channel is one slot
#include "tty.h"        // the console reads its terminal, and leads a session per command
#include "serial_tty.h"
#include "syscalls.h"   // fd_set_kernel_tty() -- the command's fds 0/1/2

#define DBG_LINE_MAX 128
#define DBG_PROMPT "\r\ndbg> "

static char line_buf[DBG_LINE_MAX];
static int line_len = 0;
static int line_overlong; // the rest of this line is dropped

// The serial terminal (kernel/tty/serial_tty.c): input arrives through
// its line discipline, and a command's program gets it as 0/1/2.
static struct tty *g_tty;

static void dbg_write(const char *s) { serial_dbg_write(s); }

static void dbg_printf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    k_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    dbg_write(buf);
}

static void dbg_cmd_help(void) {
    dbg_write("Commands:\r\n");
    dbg_write("  help        - this list\r\n");
    dbg_write("  meminfo     - physical frame + kernel heap usage\r\n");
    dbg_write("  readfile P  - file P as one framed block, nothing interleaved\r\n");
    dbg_write("  lsdev       - enumerated PCI devices\r\n");
    dbg_write("  usb         - xHCI registers, rings and root ports\r\n");
    dbg_write("  aml         - the ACPI namespace (declarations, not methods)\r\n");
    dbg_write("  lsfs [path] - list a filesystem directory (default /)\r\n");
    dbg_write("  ktest [suite] - run the in-kernel test suite\r\n");
    dbg_write("  sh <command>  - run any shell command, output back here\r\n");
    dbg_write("  gui <sub>     - inspect/drive the window manager (`gui help`)\r\n");
    dbg_write("  diag [name [cmd]] - ask any registered service; no args lists them\r\n");
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
// over serial but their report is invisible here (only the dbg_write()
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
    if (c == '\n') serial_dbg_putc('\r'); // serial terminals want CRLF
    serial_dbg_putc(c);
}
static void dbg_sink_backspace(void *ctx) { (void)ctx; }
static void dbg_sink_clear(void *ctx) { (void)ctx; }
static void dbg_sink_set_color(void *ctx, enum vga_color fg, enum vga_color bg) {
    (void)ctx; (void)fg; (void)bg;
}

// Shared by dbg_cmd_ktest() and dbg_cmd_sh() -- both need vga_write()
// output to come back down this wire instead of painting the screen.
static const struct vga_sink g_serial_sink = {
    .ctx = 0,
    .putc = dbg_sink_putc,
    .backspace = dbg_sink_backspace,
    .clear = dbg_sink_clear,
    .set_color = dbg_sink_set_color,
    .rows = 0, // no page height -- vga_rows() falls back to a default
};

// Commands that must not run from here. Same list and same reasoning as
// apps/terminal.c's BLOCKED_CMDS: `gui` takes over the framebuffer and
// this console's caller (a poll from an idle loop) never returns from
// it, `ring3test` deliberately never returns at all, and `schedtest`
// blocks the calling context until both demo processes exit. `edit`/
// `nano` are additionally blocked here (but not in Terminal, which can
// render them) because a full-screen editor over a serial line has no
// way to read keys back -- it would hang waiting for input this console
// can't deliver.
static const char *const DBG_BLOCKED_CMDS[] = {
    "gui", "gui3", "ring3test", "schedtest", "edit",
};

static int dbg_is_blocked(const char *cmd) {
    for (unsigned i = 0; i < sizeof(DBG_BLOCKED_CMDS) / sizeof(DBG_BLOCKED_CMDS[0]); i++) {
        if (k_strcmp(cmd, DBG_BLOCKED_CMDS[i]) == 0) return 1;
    }
    return 0;
}

// `sh <command>` -- runs a real shell command line and sends its output
// back down this wire.
//
// This is what made the console stop being read-only, and it's a
// deliberate trade: verifying kernel behaviour used to mean emulating
// keystrokes over QMP and reading the result out of a screenshot, which
// depends on the guest's keyboard layout, drops keys under load, and
// can't be asserted on programmatically. With this, a host script gets
// TEXT back (tools/shell_serial.py). See docs/decisions.md.
//
// It runs through shell_dispatch() rather than reimplementing anything,
// exactly as apps/terminal.c does -- one dispatcher, one set of
// commands, no drift.
static void dbg_cmd_sh(char *line) {
    if (!line || line[0] == '\0') {
        dbg_write("usage: sh <command>  (any shell command; try 'sh help')\r\n");
        return;
    }

    // First word only, for the block check.
    char cmd[32];
    int i = 0;
    while (line[i] && line[i] != ' ' && i < (int)sizeof(cmd) - 1) { cmd[i] = line[i]; i++; }
    cmd[i] = '\0';
    if (dbg_is_blocked(cmd)) {
        dbg_write("sh: '");
        dbg_write(cmd);
        dbg_write("' can't run from the serial console (it takes over the\r\n");
        dbg_write("screen, never returns, or needs keyboard input this console\r\n");
        dbg_write("can't provide). See debug_console.c's DBG_BLOCKED_CMDS.\r\n");
        return;
    }

    // ONE COMMAND, ONE SESSION. The program the command runs gets the
    // serial terminal as 0/1/2 (fd_set_kernel_tty()) and leads a session
    // on it, so it can read the line and Ctrl-C reaches it. The sink swap
    // stays for the kernel shell's OWN output, which is vga_write(). When
    // the command returns the session is hung up: a job it left running
    // reads and writes the machine console from then on, never a later
    // reply (docs/decisions.md, "The kernel log and the debug console are
    // two serial ports"), and the line gets its default settings back.
    const struct vga_sink *prev = vga_set_sink(&g_serial_sink);
    fd_set_kernel_tty(g_tty);
    shell_dispatch(line, &g_serial_sink);
    fd_set_kernel_tty(0);
    tty_hangup(g_tty);
    serial_tty_reset();
    vga_set_sink(prev);
}


static void dbg_cmd_ktest(const char *suite) {
    const struct vga_sink *prev = vga_set_sink(&g_serial_sink);
    ktest_run_all(suite && suite[0] ? suite : 0);
    vga_set_sink(prev);
}

static void dbg_cmd_meminfo(void) {
    uint64_t total = pmm_total_frames(), free = pmm_free_frames();
    uint64_t used = total - free;
    dbg_printf("phys: %luK used / %luK total (%luK free)\r\n",
                 used * 4, total * 4, free * 4);
    dbg_printf("heap: %lu bytes used / %lu bytes claimed from pmm\r\n",
                 (unsigned long)heap_used_bytes(), (unsigned long)heap_total_bytes());
}

static void dbg_cmd_lsdev(void) {
    // The active DISPLAY first, because it is the one device whose
    // driver is chosen by a probe race rather than being visible in the
    // PCI list -- `0x1af4:0x1050` below says a virtio GPU is on the bus,
    // not that anything claimed it. A test asking "which driver won?"
    // had only `dmesg` to read, and the kernel log is a ring buffer that
    // has rolled over long before a GUI test gets to ask.
    const struct display_driver *disp = display_active();
    if (disp) {
        struct display_surface s;
        display_get_surface(&s);
        dbg_printf("Display: %s  %ux%u x%u pitch %u  caps:%s%s%s%s%s\r\n",
                     disp->name, s.width, s.height, (unsigned)s.bpp, s.pitch,
                     display_has(DISPLAY_CAP_NEEDS_FLUSH) ? " flush" : "",
                     display_has(DISPLAY_CAP_CURSOR)      ? " cursor" : "",
                     display_has(DISPLAY_CAP_ACCEL_FILL)  ? " fill" : "",
                     display_has(DISPLAY_CAP_ACCEL_COPY)  ? " copy" : "",
                     display_has(DISPLAY_CAP_MODESET)     ? " modeset" : "");
    } else {
        dbg_write("Display: none claimed\r\n");
    }

    // The Local APIC, when there is one. Reported beside the USB
    // controller because that is the device using its vectors, and
    // because "is this machine on MSI or on pins?" has no other answer
    // from outside the kernel.
    {
        char apic[96];
        if (lapic_summary(apic, sizeof apic))
            dbg_printf("LAPIC: %s, %u spurious\r\n", apic, idt_spurious_count());
        else
            dbg_write("LAPIC: not enabled -- every device is on the 8259 PIC\r\n");
    }

    // WHICH DEVICE DRIVES THE TICK, and how many it has delivered. The
    // count is the half that matters: a LAPIC timer configured and not
    // delivering is a machine that has already stopped, so a rising
    // number here is the only proof the switch actually took.
    {
        char tick[96];
        clockevent_summary(tick, sizeof tick);
        dbg_printf("Tick: %s, %u lapic-timer interrupt(s)\r\n",
                    tick, lapic_timer_ticks());
        struct clockevent_stats cs;
        clockevent_get_stats(&cs);
        dbg_printf("Tick: %llu event(s), %llu tick(s), %llu idle stop(s), "
                    "%llu ms stopped of %llu\r\n",
                    (unsigned long long)cs.events, (unsigned long long)cs.ticks,
                    (unsigned long long)cs.idle_stops,
                    (unsigned long long)(cs.stopped_ns / 1000000),
                    (unsigned long long)(clocksource_now_ns() / 1000000));
    }

    char usbline[96];
    if (usb_controller_summary(usbline, sizeof usbline))
        dbg_printf("USB: %s\r\n", usbline);
    else
        dbg_write("USB: no controller\r\n");

    // The ACTIVE marker is the load-bearing half: a machine with two
    // cards looks identical to one with two working cards until you ask
    // which of them the next SYS_SND_OPEN would reach.
    int nsnd = sound_device_count();
    if (nsnd == 0) {
        dbg_write("Sound: no device\r\n");
    } else {
        dbg_printf("Sound (%d, preference %s):\r\n", nsnd, sound_preference());
        for (int i = 0; i < nsnd; i++)
            dbg_printf("  %s%s  \"%s\"\r\n", sound_device_name(i),
                        sound_device_is_active(i) ? "  [active]" : "",
                        sound_device_label(i));
    }

    int ns = input_source_count();
    dbg_printf("Input sources (%d):\r\n", ns);
    for (int i = 0; i < ns; i++) {
        const struct input_source *src = input_source_at(i);
        if (!src) continue;
        // The SERVICING is printed beside the capabilities because the
        // two answer different questions -- what a device can report,
        // and whether anything is listening. A device that is claimed
        // and polled looks identical to one that is claimed and
        // interrupt-driven until you ask.
        char how[16];
        if (src->msi_vector)
            k_snprintf(how, sizeof how, "msi %u", (unsigned)src->msi_vector);
        else if (src->irq)
            k_snprintf(how, sizeof how, "irq %u", (unsigned)src->irq);
        else k_strlcpy(how, "polled", sizeof how);
        dbg_printf("  %s%s%s%s%s  [%s]\r\n", src->name,
                     (src->caps & INPUT_CAP_KEYS)  ? "  keys" : "",
                     (src->caps & INPUT_CAP_REL)   ? "  rel" : "",
                     (src->caps & INPUT_CAP_ABS)   ? "  abs" : "",
                     (src->caps & INPUT_CAP_WHEEL) ? "  wheel" : "", how);
    }

    if (virtio_input_count() > 0) {
        // The event count, not just the device list: "is it claimed?"
        // and "is it delivering?" are different questions, and only the
        // second one distinguishes a working driver from a present one.
        dbg_printf("  (virtio-input: %u event(s) decoded)\r\n",
                     (unsigned)virtio_input_events());
    }

    int n = pci_device_count();
    dbg_printf("PCI devices (%d):\r\n", n);
    for (int i = 0; i < n; i++) {
        const struct pci_device *d = pci_device_at(i);
        if (!d) continue;
        dbg_printf("  %u:%u.%u  0x%x:0x%x  %s\r\n",
                     d->bus, d->device, d->function,
                     d->vendor_id, d->device_id,
                     pci_class_name(d->class_code, d->subclass));
    }
}

// Nothing here needs state across entries, so the context is unused.
static void dbg_lsfs_cb(void *ctx, const char *name, uint32_t size, int is_dir) {
    (void)ctx;
    if (is_dir) dbg_printf("  %s/\r\n", name);
    else        dbg_printf("  %s  (%u bytes)\r\n", name, size);
}

// A FILE AS ONE FRAMED BLOCK, for a harness to parse by frame rather
// than by prompt. `sh cat` writes through a ring-3 process in chunks,
// and the kernel log lands between them on the same wire -- a verdict
// line came back as "wrap_test: " + two log lines + "all checks
// passed". Here nothing else can run (preemption off) and the log is
// held off the wire until the frame is out:
//
//   <<<FILE <n>\n  <exactly n bytes>  \n>>>END\n     (n = -1: no such file)
//
// Bounded: a verdict file is a few KiB, and this is not a transfer tool.
#define READFILE_MAX (64u * 1024u)

static void dbg_cmd_readfile(const char *path) {
    uint32_t n = 0;
    char *buf = NULL;
    int missing = !path || path[0] != '/' || !fs_exists(path) || fs_is_dir(path);
    if (!missing) {
        buf = kmalloc(READFILE_MAX);
        if (buf) n = fs_read_into(path, buf, READFILE_MAX);   // 0: empty, or refused
        else missing = 1;
    }
    char hdr[32];
    if (missing) k_snprintf(hdr, sizeof hdr, "\n<<<FILE -1\n");
    else         k_snprintf(hdr, sizeof hdr, "\n<<<FILE %u\n", n);

    scheduler_preempt_disable();
    klog_serial_hold();
    dbg_write(hdr);
    for (uint32_t i = 0; i < n; i++) {
        serial_dbg_putc(buf[i]);
        if ((i & 1023) == 1023) serial_dbg_flush();   // never outrun the 4 KiB TX ring
    }
    dbg_write("\n>>>END\n");
    klog_serial_release();
    scheduler_preempt_enable();
    kfree(buf);
}

static void dbg_cmd_lsfs(const char *arg) {
    const char *path = (arg && arg[0]) ? arg : "/";
    dbg_write("ls ");
    dbg_write(path);
    dbg_write(":\r\n");
    fs_list(path, dbg_lsfs_cb, NULL);
}

// Splits `line` into a command word and (optionally) one trailing
// argument at the first space -- same lightweight convention
// apps/shell.c's own dispatcher uses; no quoting, no multiple args,
// this is a debug tool, not a real shell. Mutates `line` in place
// (null-terminates the command word), same as shell_dispatch() does to
// its own line buffer.
// Runs one `gui` command over the window transport and prints the reply.
//
// The reply arrives in WIN_DEBUG_CHUNK-sized pieces because a message
// carries a fixed payload and `gui help` is ~1.8 KB. The loop is bounded
// rather than "until no MORE flag": a transport that answered with the
// flag permanently set would otherwise hang the console, and the console
// is the only way to talk to a wedged desktop. The bound is generous
// enough that no real reply reaches it.
// `diag <name> <command>`, and `gui ...` is the same thing against the
// provider named `gui`. The kernel holds the name table because THIS
// caller cannot be a ring-3 client: reaching a wedged service when no
// shell is available is the whole reason a kernel-side path exists.
static void dbg_cmd_diag(const char *name, const char *args) {
    struct diag_msg msg;
    k_memset(&msg, 0, sizeof msg);
    msg.type = DIAG_CMD;
    k_strlcpy(msg.name, name, DIAG_NAME_LEN);
    k_strlcpy(msg.text, args ? args : "", DIAG_CMD_LEN);

    int rc = diag_request(DIAG_PID_KERNEL, &msg);
    if (rc == -EBUSY) {
        // /bin/guictl issues the same command from ring 3 and the
        // channel is one slot, so a refusal here means somebody else is
        // mid-drain -- not a broken desktop.
        dbg_write("gui: busy -- another diagnostic is in flight\r\n");
        return;
    }
    if (!rc) {
        // A NAME NOBODY HOLDS AND A WEDGED PROVIDER ARE DIFFERENT FACTS,
        // and saying so is what stops "no answer" being read as "not
        // running".
        if (!diag_have_provider(name)) {
            char have[128];
            int n = diag_list(have, sizeof have);
            dbg_printf("diag: no provider named `%s'\r\n", name);
            if (n > 0) dbg_printf("      registered: %s\r\n", have);
            else dbg_write("      none registered\r\n");
        } else {
            dbg_printf("diag: %s did not answer\r\n", name);
        }
        return;
    }
    if (msg.flags & DIAG_F_UNKNOWN) {
        dbg_printf("unknown %s subcommand -- try `%s help`\r\n", name, name);
        return;
    }

    for (int guard = 0; guard < 64; guard++) {
        if (msg.len) dbg_write(msg.text);
        if (!(msg.flags & DIAG_F_MORE)) return;

        k_memset(&msg, 0, sizeof msg);
        msg.type = DIAG_MORE;
        if (!diag_request(DIAG_PID_KERNEL, &msg)) return;
    }
    dbg_write("\r\ndiag: (reply too long, stopped)\r\n");
}

static void dbg_cmd_gui(const char *args) { dbg_cmd_diag("gui", args); }

// `diag` alone lists the providers; `diag <name> <cmd>` asks one.
static void dbg_cmd_diag_cmd(char *line) {
    if (!line || !line[0]) {
        char have[128];
        int n = diag_list(have, sizeof have);
        if (n > 0) dbg_printf("providers: %s\r\n", have);
        else dbg_write("no diagnostic providers registered\r\n");
        return;
    }
    char name[DIAG_NAME_LEN];
    int i = 0;
    while (line[i] && line[i] != ' ' && i < (int)sizeof(name) - 1) { name[i] = line[i]; i++; }
    name[i] = '\0';
    while (line[i] == ' ') i++;
    dbg_cmd_diag(name, line + i);
}

static void dbg_dispatch(char *line) {
    int i = 0;
    while (line[i] && line[i] != ' ') i++;
    int had_space = (line[i] == ' ');
    line[i] = '\0';
    const char *arg = had_space ? line + i + 1 : "";

    if (k_strcmp(line, "gui") == 0) {
        // Asked of the PROVIDER named `gui` (abi/diag_abi.h), never
        // called into userland/wm/ directly -- this file used to do that
        // across the kernel/apps boundary, and every GUI test tool
        // arrives here, so that call is what had to stop before the WM
        // could become a process.
        //
        // NOT the same as `sh gui`, which is blocked: that would try to
        // ENTER GUI mode from inside the console and never return. These
        // subcommands inspect a desktop that is already up.
        dbg_cmd_gui(had_space ? line + i + 1 : line + i);
        return;
    }
    if (k_strcmp(line, "diag") == 0) {
        dbg_cmd_diag_cmd(had_space ? line + i + 1 : line + i);
        return;
    }
    if (k_strcmp(line, "help") == 0) dbg_cmd_help();
    else if (k_strcmp(line, "meminfo") == 0) dbg_cmd_meminfo();
    else if (k_strcmp(line, "lsdev") == 0) dbg_cmd_lsdev();
    else if (k_strcmp(line, "lsfs") == 0) dbg_cmd_lsfs(arg);
    else if (k_strcmp(line, "readfile") == 0) dbg_cmd_readfile(arg);
    // These two print through klog in their own files; tee the log to
    // this port for the length of the dump so the reply carries it.
    else if (k_strcmp(line, "usb") == 0) { klog_tee_dbg(1); usb_dump(); klog_tee_dbg(0); }
    else if (k_strcmp(line, "aml") == 0) { klog_tee_dbg(1); aml_dump(); klog_tee_dbg(0); }
    else if (k_strcmp(line, "ktest") == 0) dbg_cmd_ktest(arg);
    else if (k_strcmp(line, "sh") == 0) dbg_cmd_sh((char *)arg);
    else {
        dbg_write("unknown command: ");
        dbg_write(line);
        dbg_write(" (try 'help')\r\n");
    }
}

// OFF UNLESS `debugcon` IS ON THE BOOT LINE. This console is an
// unauthenticated root shell on a serial port -- `sh` runs anything,
// `gui` drives the desktop, `readfile` reads any file -- so it listens
// only when asked, the way Linux needs `kgdboc=` or a getty and Windows
// needs `bcdedit /ems on`. `make iso` bakes the word into the dev and
// test media; release media leave it out (docs/boot-flags.md). The
// kernel LOG is unaffected: it only ever prints.
static int g_enabled;

static int boot_word(const char *cmdline, const char *w) {
    size_t n = k_strlen(w);
    for (const char *p = cmdline; (p = k_strstr(p, w)) != 0; p += n) {
        if (p != cmdline && p[-1] != ' ') continue;
        if (p[n] == 0 || p[n] == ' ') return 1;
    }
    return 0;
}

void debug_console_init(void) {
    line_len = 0;
    const char *cmdline = multiboot_cmdline();
    g_enabled = cmdline && (boot_word(cmdline, "debugcon") ||
                            boot_word(cmdline, "debugcon=ttyS0"));
    if (g_enabled && boot_word(cmdline, "debugcon=ttyS0")) serial_dbg_use_com1();
    if (!g_enabled) {
        klog_write("dbg: serial debug console off -- boot with `debugcon` to enable it\n");
        return;
    }
    // An EVENT as well as a greeting: the log records that the console
    // came up and where, and the console's own port shows it too.
    g_tty = serial_tty_create();
    const char *where = serial_dbg_separate() ? "COM2" : "COM1";
    klog_printf("dbg: serial debug console ready (%s)\n", where);
    dbg_printf("dbg: serial debug console ready (%s) -- type 'help'", where);
    dbg_write(DBG_PROMPT);
}

void debug_console_poll(void) {
    // NOT re-entrant, and it genuinely re-enters: a dispatched command
    // can run a long shell command, whose own wait loop calls back in
    // here (scheduler_idle()). `arg` in dbg_dispatch() points INTO
    // line_buf, so a nested call assembling the next command overwrites
    // the running one's arguments underneath it. Refusing the nested
    // call costs nothing -- the port's receive is interrupt-driven into
    // the terminal's queue, so the bytes wait there instead (and a
    // running command's program may be the one to read them).
    static int in_poll = 0;
    if (!g_enabled || in_poll) return;
    in_poll = 1;

    // WHOLE LINES, from the terminal's canonical mode: the discipline has
    // already echoed them and applied erase and kill, so what arrives is
    // what the user meant. The echo of Enter is the "\r\n" this console
    // used to write itself.
    //
    // ONE BYTE AT A TIME, stopping at each line's end to run it: anything
    // typed AFTER the command stays queued in the terminal, where the
    // program that command starts reads it -- a real terminal's type-ahead.
    char c;
    while (g_tty && tty_read(g_tty, &c, 1) == 1) {
        if (c == '\n') {
            line_buf[line_len] = '\0';
            if (line_len > 0 && !line_overlong) dbg_dispatch(line_buf);
            else if (line_overlong) dbg_write("dbg: line too long, ignored\r\n");
            line_len = 0;
            line_overlong = 0;
            dbg_write(DBG_PROMPT);
        } else if (line_len < DBG_LINE_MAX - 1) {
            line_buf[line_len++] = c;
        } else {
            line_overlong = 1;
        }
    }
    // Ctrl-D at the prompt means nothing here; consume it so it cannot
    // end the next command's input early.
    if (g_tty) (void)tty_eof_pending(g_tty);
    in_poll = 0;
}
