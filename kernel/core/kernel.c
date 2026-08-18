// Kernel entry point. Does hardware bring-up only -- once interrupts and
// the drivers are up, it hands off to apps_start() and never looks at
// app-level code again. Compare with apps/apps.c, which knows nothing
// about hardware.
#include "vga.h"
#include "gfx.h"
#include "serial.h"
#include "klog.h"
#include "kfmt.h"      // klog_printf() -- the SMEP/SMAP line
#include "idt.h"
#include "gdt.h"
#include "paging.h"
#include "fpu.h"
#include "cpuinfo.h"
#include "clocksource.h"
#include "multiboot.h"
#include "pmm.h"
#include "heap.h"
#include "rammeter.h"
#include "pci.h"
#include "vmsvga.h"
#include "vesafb.h"
#include "display.h"
#include "fs.h"
#include "json.h"
#include "tz.h"
#include "timer.h"
#include "font_config.h"
#include "setting.h"
#include "cursor_config.h"
#include "keyboard_config.h"
#include "apps.h"
#include "demo.h"
#include "gui.h"
#include "scheduler.h"
#include "debug_console.h"
#include "krandom.h"      // entropy source -- krandom_init()
#include "reloc.h"        // the image's own relocation table -- kernel_relocate()

// Where the running image starts -- a relocated symbol, so under kernel
// ASLR it reports where the kernel actually IS, not where it was linked.
extern char __kimage_start[];
#include "stack_guard.h"  // stack_guard_randomize() -- read its header before moving the call
#include "knum.h"
#include <stdint.h>

// Zero-padded 2-digit decimal, for the RTC boot-time log line below.
// klog_write_dec() (klog.h) deliberately doesn't pad, so this used to
// be a hand-rolled two-character helper; knum.h has the padded
// converter now, and klog.c's own "[secs.hh]" prefix -- which this
// helper's comment used to point at as the other copy -- goes through
// the same one.
static void klog_write_dec2(uint8_t n) {
    char buf[8];
    k_utoa_pad(n % 100, buf, sizeof buf, 2);
    klog_write(buf);
}

void kernel_main(uint64_t multiboot_info_addr) {
    serial_init();
    klog_write("toy-os: kernel_main reached, initializing...\n");

    multiboot_set_info(multiboot_info_addr);

    // Kernel ASLR already happened, in long_mode_start, before this
    // function was reached -- the image copied itself to a random
    // 2MiB-aligned base and applied its own relocation table. It has to
    // run there rather than here: it moves the stack, so it cannot
    // return into the frame that called it.
    //
    // All this does is report it, because that code cannot log at all
    // (serial_init() had not run) -- which is why every decision it
    // made is sitting in a global waiting to be printed.
    uint64_t reloc_delta = kernel_reloc_delta();
    if (reloc_delta) {
        klog_printf("toy-os: kernel relocated +0x%lx, running at 0x%lx (1 of %lu candidate bases, %s entropy)\n",
                    reloc_delta, (uint64_t)(uintptr_t)__kimage_start,
                    kernel_reloc_slots(),
                    kernel_reloc_entropy_hw() ? "hardware" : "TSC -- weak");
    } else {
        klog_printf("toy-os: kernel NOT relocated (%s)\n", kernel_reloc_note());
    }
    klog_printf("toy-os: relocation table: %lu fixups, %lu bytes\n",
                kernel_reloc_count(), kernel_reloc_table_bytes());

    // As early as the log allows. boot.asm hands over a 4GiB identity map
    // that is uniformly present+writable with no NX anywhere, so until
    // this runs the kernel's own .text is writable and every byte of RAM,
    // the framebuffer included, is executable. It needs nothing that is
    // initialized below -- no heap, no allocator, no interrupts -- only
    // the linker symbols, so there is no reason to run any of the rest of
    // this function unprotected first.
    if (paging_enforce_wx()) {
        klog_write("toy-os: kernel W^X applied (.text read-only+exec, everything else NX)\n");
    } else {
        klog_write("toy-os: WARNING -- kernel W^X NOT applied; split table pool exhausted\n");
    }

    // The CPU's own half of the same idea, right beside it: SMEP stops
    // ring 0 executing a user page, SMAP stops it reading or writing
    // one. Both are absent on QEMU's default qemu64 model, so a boot
    // that reports neither is normal there rather than a failure --
    // `--cpu max` is what exercises them. See paging.h for the rule SMAP
    // imposes on every kernel access to user memory.
    int prot = paging_enable_smep_smap();
    klog_printf("toy-os: SMEP %s, SMAP %s\n",
                 (prot & PAGING_SMEP_ON) ? "on" : "unavailable",
                 (prot & PAGING_SMAP_ON) ? "on" : "unavailable");

    // Before any device driver: PCI enumeration is what a driver probes
    // against. Safe this early -- pci.c is a port-I/O scan into a static
    // table, needing neither the heap nor interrupts. It used to sit
    // after heap_init() purely because nothing before that point cared.
    pci_init();
    klog_write("toy-os: PCI bus enumerated\n");

    // Display drivers register, then probe -- specific cards first, the
    // generic GRUB framebuffer last as the fallback that always claims.
    // Must happen before vga_init(), which calls gfx_init() and needs a
    // surface to exist. vmsvga's probe does its own PCI config-space
    // read rather than waiting for pci_init(), because the console has
    // to come up before that.
    vmsvga_register();
    vesafb_register();
    display_probe();

    // Reads the command line only -- nothing is drawn until something
    // with a frame to sit on top of calls rammeter_tick().
    rammeter_init();

    vga_init();
    // Mirror the kernel log to the screen for the rest of boot, the way
    // a real kernel shows its init sequence. Switched off again just
    // before apps_start() below -- and the console's scrollback (see
    // vga.h) keeps these lines readable with PageUp once the shell is
    // up, which is the whole point: they used to go past far too fast
    // to read and only existed on the serial line afterwards.
    klog_set_console_echo(1);
    vga_write("toy-os booting...\n");

    gdt_init();
    klog_write("toy-os: GDT/TSS initialized\n");

    idt_init();
    klog_write("toy-os: IDT/PIC/PIT initialized, interrupts enabled\n");

    // Monotonic time, on the PIT to begin with. Before anything wants a
    // timestamp and before cpu_info_init() below, which calibrates the
    // TSC against the PIT and so cannot itself depend on a TSC-backed
    // clock existing yet. The better source is registered after it.
    clocksource_init();

    // Before anything can reach ring 3. The kernel itself never uses FP
    // (see fpu.h on why it's ring-3-only), so nothing above this line
    // cares -- but a process starting without it would #UD on its first
    // SSE instruction.
    if (fpu_init()) {
        klog_write("toy-os: FPU/SSE enabled (ring-3 only, eager FXSAVE per switch)\n");
    } else {
        klog_write("toy-os: WARNING -- no FXSR/SSE2 reported; ring-3 float unavailable\n");
    }

    // Needs the PIT already ticking (idt_init above) -- it calibrates
    // RDTSC against it. Deliberately here and not on first use: see
    // cpuinfo.h, a lazy calibration inside a syscall can't ever finish.
    cpu_info_init();
    klog_write("toy-os: CPU identified, clock calibrated\n");

    // Now that a calibrated frequency exists, offer the TSC. It takes
    // over from the PIT source only if this CPU's TSC is INVARIANT --
    // see clocksource_tsc.c, where a varying-rate counter is refused
    // rather than installed with a calibration that quietly stops being
    // true.
    clocksource_init_tsc();

    // Entropy (Milestone 2, docs/roadmap.md). Has to be after
    // cpu_info_init() -- it asks CPUID for RDSEED/RDRAND through
    // cpu_info -- and after idt_init(), because with neither
    // instruction it falls back to timing jitter measured against the
    // PIT, which has to be ticking.
    krandom_init();

    // ...and immediately: a random stack canary. This call must stay
    // HERE, a statement in kernel_main() itself rather than tucked
    // inside a helper -- changing __stack_chk_guard while any
    // instrumented frame is live makes that frame panic on return, and
    // kernel_main()'s own frame is the only one that can be live at
    // this point (it never returns). See kernel/lib/stack_protector.c.
    stack_guard_randomize();

    serial_irq_init(); // COM1 RX -- see serial.c for why this can't run inside serial_init() itself
    klog_write("toy-os: serial RX enabled (debug console on COM1, see docs/decisions.md)\n");

    pmm_init();
    klog_write("toy-os: physical frame allocator initialized\n");

    heap_init(); // kmalloc()/kfree() -- built on pmm, needs it initialized first
    klog_write("toy-os: kernel heap initialized\n");

    // No self-tests run here any more. pmm/heap/json/tfs each used to be
    // checked on every single boot -- see kernel/include/kernel/ktest.h
    // for why that stopped being a good idea and `ktest` for how to run
    // them now.


    // /etc and /tmp used to be created here, right after this call.
    // They are made by fs_init() itself now, because a mount also
    // happens when `fsformat` reformats a live disk and that path
    // skipped this line entirely -- see vfs.c's ensure_layout().
    fs_init();
    // /bin binaries (e.g. lspci) are no longer bootstrap-installed here
    // at boot time -- tools/tfs2_writer.py seeds them into disk.img at
    // BUILD time now (see the Makefile's `seed` step), so by the time
    // toy-os actually boots they're already on disk. See
    // docs/decisions.md for why this replaced the old GRUB-module/
    // BIN_BOOTSTRAP-table approach.
    tz_init(); // loads the persisted timezone choice, if any -- needs fs_init()/"/etc" first
    font_config_init(); // loads the persisted font size, if any -- see kernel/lib/font_config.c
    cursor_config_init(); // console cursor style, same /etc plumbing as the font size
    keyboard_config_init(); // loads the persisted keyboard layout, if any -- see kernel/lib/keyboard_config.c
    vga_reflow(); // apply it to the console's cell layout (no-op if nothing was persisted)
    // Announce those four to the settings registry, AFTER their own
    // init(): a timezone registered before tz_init() would offer an
    // empty city list, since the choices ARE the loaded database.
    settings_init();

    // One-shot boot-time CMOS/RTC readout, logged for the same reason a
    // real kernel's dmesg has an "rtc_cmos ...: setting system clock"
    // line -- proves the hardware clock is readable and shows what it
    // says at boot. Deliberately NOT logged from rtc_read() itself
    // (timer.c) -- that function is called continuously by the taskbar
    // clock/tz code every time it redraws, so logging there would flood
    // the ring buffer; this is the one call site that only ever runs
    // once. Raw rtc_read(), not tz.h's rtc_read_local() -- this is
    // "what the hardware says," unadjusted for timezone/DST, matching
    // what a real kernel's own RTC probe logs before any timezone
    // config is even in the picture.
    struct rtc_time boot_time;
    rtc_read(&boot_time);
    klog_write("rtc: hardware clock reads ");
    klog_write_dec(boot_time.year); // already a full 4-digit year -- see rtc_read()'s "2000 + year" in timer.c
    klog_write("-");
    klog_write_dec2(boot_time.month);
    klog_write("-");
    klog_write_dec2(boot_time.day);
    klog_write(" ");
    klog_write_dec2(boot_time.hour);
    klog_write(":");
    klog_write_dec2(boot_time.minute);
    klog_write(":");
    klog_write_dec2(boot_time.second);
    klog_write(" UTC\n");

    scheduler_init();
    klog_write("toy-os: scheduler initialized (continuously armed; see `schedtest`)\n");

    // AFTER paging_enforce_wx() above, which rewrites every PDE and
    // would put the huge pages back over these. A kernel stack that
    // overflows now faults on its guard page instead of silently
    // overwriting the next slot's saved trapframe -- which is exactly
    // what it used to do, presenting as a #GP on iretq in an innocent
    // process. See scheduler.c's PROC_KSTACK_SIZE comment.
    scheduler_guard_pages_init();

    debug_console_init(); // serial debug console (COM1) -- see docs/decisions.md; polled from keyboard_getchar()'s and wm_run()'s idle-wait loops

    // Boot is over; stop putting kernel log lines on top of whatever the
    // shell or the GUI draws. `dmesg`, the serial port and the console's
    // own scrollback all still have them.
    klog_set_console_echo(0);

    // A scripted tour, if the command line asked for one. Runs the CLI
    // half here and hands the rest to the desktop; see apps/demo.h.
    if (demo_requested() && demo_load("/usr/wm/demo.script") > 0) {
        if (demo_run_cli()) {
            gui_main();
        }
    }

    apps_start();

    for (;;) __asm__ volatile ("hlt");
}
