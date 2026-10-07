// The kernel's runtime tunables.
//
// A tunable is a SETTING whose `apply` writes a live kernel variable
// (docs/settings-and-queries.md's vocabulary), and these three
// deliberately do NOT survive a reboot: they declare
// CONFIG_PATH_RUNTIME as their file, which puts them in the `kernel`
// namespace with nothing to persist. `kernel.heap_debug` reads the way
// sysctl's `kernel.printk` does, and for the same reason -- these are
// machine knobs, not preferences.
//
// WHY THEY ARE HERE AND NOT IN THEIR SUBSYSTEMS, which is the usual
// rule for a provider. Each of these is three lines over an API that
// already exists (heap.h, ata.h, scheduler.h) and none needs anything
// private to its subsystem -- putting them beside the code they poke
// would mean heap.c, ata.c and scheduler.c each growing a settings
// dependency to gain nothing. If a tunable ever needs a subsystem's
// internals, it moves there; that is the same test `mem_query.c`
// passes by living in kernel/mm/.
//
// WHY THEY EXIST AT ALL: each is the WRITE half of a shell command
// whose read half is a query provider. `heap`, `ata` and `kstack` were
// the last builtins that could not move to /bin, because moving only
// the read half would put one command in two rings. See
// docs/conventions/shell.md.
#include "setting.h"
#include "config_file.h"
#include "heap.h"
#include "ata.h"
#include "scheduler.h"
#include "string.h"
#include "kfmt.h"          // k_snprintf -- the reason names the live device
#include "setting_abi.h"   // SETTING_ABI_DESC_MAX -- the reason's own budget
#include "block.h"        // blk_root_name() -- what IS carrying the transfers
#include "keyboard_tap.h" // kbdtap_enabled()/_set_enabled() -- kernel.kbdtap
#include "syscall_stall.h" // syscall_stall_get()/_set() -- kernel.syscall_stall
#include "sound.h"        // hda_diag_tone() -- kernel.hda_tone
#include "usb.h"          // usb_diag_reset_port() -- kernel.usb_reset
#include "intel_display.h" // intel_display_pipe_cycle()/_link_retrain() -- kernel.intel_cycle
#include "crashtest.h"     // crash_trigger() -- kernel.crash
#include "panic_store.h"   // panic_store_clear() -- kernel.panic_record

#define TUNABLE_CATEGORY "Kernel"

// on/off is spelled once. Three tunables reading "true"/"1"/"yes"
// differently is exactly the drift a shared parser prevents, and a
// REJECTION is the right answer for anything else -- a tunable that
// treats an unrecognised word as "off" silently disarms itself.
static int parse_onoff(const char *v, int *out) {
    if (k_strcmp(v, "on") == 0)  { *out = 1; return 1; }
    if (k_strcmp(v, "off") == 0) { *out = 0; return 1; }
    return 0;
}

static int onoff_choice(int index, char *out, uint32_t cap) {
    if (index == 0) { k_strlcpy(out, "on", cap); return 1; }
    if (index == 1) { k_strlcpy(out, "off", cap); return 1; }
    return 0;
}

// ---- kernel.heap_debug ----------------------------------------------

static void heap_debug_get(char *out, uint32_t cap) {
    k_strlcpy(out, heap_debug() ? "on" : "off", cap);
}

static int heap_debug_apply(const char *value) {
    int on;
    if (!parse_onoff(value, &on)) return SETTING_INVALID;
    heap_set_debug(on);
    // SAVED, not UNSAVED: the value is exactly where it belongs, which
    // is memory. SETTING_UNSAVED means "applied but will not survive a
    // reboot" -- true of every tunable by design, and reporting it
    // would train a caller to ignore the one case where it is a real
    // warning (see etc_config.h's enum setting_result).
    return SETTING_SAVED;
}

static const struct setting heap_debug_setting = {
    .name = "heap_debug",
    .label = "Heap red-zone debugging",
    .type = SETTING_TYPE_ENUM,
    .file = CONFIG_PATH_RUNTIME,
    .category = TUNABLE_CATEGORY,
    .group = "Memory",
    .choice = onoff_choice,
    .get = heap_debug_get,
    .apply = heap_debug_apply,
};

// ---- kernel.ata_nodma -----------------------------------------------

static void ata_nodma_get(char *out, uint32_t cap) {
    // The question this answers is "is DMA forced OFF?", so the sense
    // is inverted from DMA being in use. Named for the state it sets
    // rather than the one it reports, matching the `ata nodma` command
    // it replaces -- renaming it to `ata_dma` would flip the meaning of
    // every note and test that mentions it.
    //
    // THE FORCING FLAG, NOT ata_dma_active(). This used to report the
    // effective state, which is `hardware_available && !forced_off` --
    // so on a machine whose controller has no DMA engine (any virtio
    // boot) it answered "on" whatever anyone set, and the Settings
    // radio snapped back to On the instant it was applied. A getter
    // must return what apply() last accepted; the effective state is
    // reported by `/bin/ata` and by unavailable() below, where it
    // cannot be mistaken for the knob. See api/ata.h.
    k_strlcpy(out, ata_dma_forced_off() ? "on" : "off", cap);
}

// WHY THIS SETTING CANNOT BE CHANGED HERE -- and there are TWO reasons,
// which is the whole point of it being a sentence rather than a flag.
//
// The first version collapsed them into one and was WRONG on the machine
// that matters most. It said "every transfer already goes through PIO"
// whenever DMA was unavailable -- but the common way for that to be true
// is that there is NO ATA DISK AT ALL (any virtio boot), where nothing
// goes through ATA in either mode and the disk is a virtio-blk device
// whose virtqueues the host reads and writes directly. Telling somebody
// their virtio disk is running in PIO is not a rounding error; it is a
// wrong answer to the question they asked.
//
// `/bin/ata` has drawn the same distinction all along ("no ATA drive
// present" versus "PIO (this machine has no Bus-Master DMA)"), which is
// what made the collapse a regression against a fact this tree already
// knew rather than a hard call.
static const char *ata_nodma_unavailable(void) {
    // No ATA disk: this setting is about the ATA driver, and the ATA
    // driver is not carrying anything. Name what IS, so the reader is
    // not left to guess -- `blk_root_name()` is what `df` prints.
    if (!ata_present()) {
        // Two variants, because a machine with no disk at all should not
        // be told it is using one. Static buffer rather than a format at
        // the call site: a setting's reason must outlive the call (it is
        // copied at the ABI boundary, but the kernel-side caller holds
        // the pointer first).
        //
        // Two readers CAN now reach this at once, and it is benign
        // rather than guarded: every input here is a fact about the
        // MACHINE (ata_present(), blk_root_name()), so both writers put the
        // same bytes in the same buffer. A guard would narrow a window
        // with nothing different on either side of it. That stops being
        // true the moment a reason depends on who is asking.
        static char why[SETTING_ABI_DESC_MAX];
        const char *dev = blk_root_name();
        if (dev && k_strcmp(dev, "none") != 0) {
            k_snprintf(why, sizeof why,
                       "This machine has no ATA disk -- storage is on %s, "
                       "which this setting does not affect.", dev);
        } else {
            k_strlcpy(why, "This machine has no ATA disk, so there are no "
                           "ATA transfers to force through PIO.", sizeof why);
        }
        return why;
    }
    // An ATA disk with no Bus-Master DMA engine behind it. NOW the
    // original sentence is true, and only now.
    if (!ata_dma_hardware_available())
        return "This machine's ATA controller has no Bus-Master DMA engine, "
               "so every ATA transfer already goes through PIO.";
    return 0;
}

static int ata_nodma_apply(const char *value) {
    int on;
    if (!parse_onoff(value, &on)) return SETTING_INVALID;
    // THIS ONE CAN REFUSE. ata_set_dma_forced_off() returns 0 when a
    // non-blocking transfer is in flight, because switching modes
    // underneath one would strand its poller. INVALID is the honest
    // answer -- the value was not applied -- and it is why this apply
    // is not a two-liner like the others.
    if (!ata_set_dma_forced_off(on)) return SETTING_INVALID;
    return SETTING_SAVED;
}

static const struct setting ata_nodma_setting = {
    .name = "ata_nodma",
    .label = "Force ATA transfers through PIO",
    .type = SETTING_TYPE_ENUM,
    .file = CONFIG_PATH_RUNTIME,
    .category = TUNABLE_CATEGORY,
    .group = "Storage",
    .choice = onoff_choice,
    .get = ata_nodma_get,
    .apply = ata_nodma_apply,
    .unavailable = ata_nodma_unavailable,
};

// ---- kernel.kstack_track --------------------------------------------

static void kstack_track_get(char *out, uint32_t cap) {
    k_strlcpy(out, scheduler_kstack_track_get() ? "on" : "off", cap);
}

static int kstack_track_apply(const char *value) {
    int on;
    if (!parse_onoff(value, &on)) return SETTING_INVALID;
    scheduler_kstack_track_set(on);
    return SETTING_SAVED;
}

static const struct setting kstack_track_setting = {
    .name = "kstack_track",
    .label = "Per-syscall kernel stack tracking",
    .type = SETTING_TYPE_ENUM,
    .file = CONFIG_PATH_RUNTIME,
    .category = TUNABLE_CATEGORY,
    .group = "Diagnostics",
    .choice = onoff_choice,
    .get = kstack_track_get,
    .apply = kstack_track_apply,
};

// ---- kernel.syscall_stall --------------------------------------------
//
// OFF BY DEFAULT BECAUSE THE INSTRUMENT IS NOT FREE, unlike kbdtap below
// whose default is about privacy: armed, it reads the TSC twice per
// syscall. Arming from off ZEROES the counters
// (kernel/proc/syscall_stall.c), so `off` then `on` is a reset.

static void syscall_stall_get_str(char *out, uint32_t cap) {
    k_strlcpy(out, syscall_stall_get() ? "on" : "off", cap);
}

static int syscall_stall_apply(const char *value) {
    int on;
    if (!parse_onoff(value, &on)) return SETTING_INVALID;
    // THIS ONE CAN REFUSE, like kernel.ata_nodma above: a machine whose
    // TSC frequency was never calibrated cannot convert the counter to
    // time. INVALID is the honest answer -- nothing was applied.
    if (!syscall_stall_set(on)) return SETTING_INVALID;
    return SETTING_SAVED;
}

static const struct setting syscall_stall_setting = {
    .name = "syscall_stall",
    .label = "Per-syscall stall timing",
    .type = SETTING_TYPE_ENUM,
    .file = CONFIG_PATH_RUNTIME,
    .category = TUNABLE_CATEGORY,
    .group = "Diagnostics",
    .choice = onoff_choice,
    .get = syscall_stall_get_str,
    .apply = syscall_stall_apply,
};

// ---- kernel.kbdtap ---------------------------------------------------
//
// **THE ONE TUNABLE HERE WHOSE DEFAULT IS A PRIVACY DECISION.** While it
// is on, the kernel keeps the last ~128 keystrokes (kernel/keyboard_tap.h)
// and `SYS_QUERY` checks nothing, so any ring-3 process can read what was
// typed. The cost of recording is trivial; being off by default is about
// what the machine holds, not about what it costs. Turning it off WIPES
// the ring, which is the half that makes the switch mean anything.

static void kbdtap_get(char *out, uint32_t cap) {
    k_strlcpy(out, kbdtap_enabled() ? "on" : "off", cap);
}

static int kbdtap_apply(const char *value) {
    int on;
    if (!parse_onoff(value, &on)) return SETTING_INVALID;
    kbdtap_set_enabled(on);
    return SETTING_SAVED;
}

static const struct setting kbdtap_setting = {
    .name = "kbdtap",
    .label = "Record recent key events for `kbd`",
    .type = SETTING_TYPE_ENUM,
    .file = CONFIG_PATH_RUNTIME,
    .category = TUNABLE_CATEGORY,
    .group = "Diagnostics",
    .choice = onoff_choice,
    .get = kbdtap_get,
    .apply = kbdtap_apply,
};

// ---- kernel.hda_tone ------------------------------------------------
//
// Write-only: "on" plays three seconds of a kernel-generated tone through
// the HDA controller with no app and no zeroing (sound.h), then reads
// back as "off". What a crackle on real hardware is split against.

static void hda_tone_get(char *out, uint32_t cap) { k_strlcpy(out, "off", cap); }

static int hda_tone_apply(const char *value) {
    int on;
    if (!parse_onoff(value, &on)) return SETTING_INVALID;
    if (on) hda_diag_tone();
    return SETTING_SAVED;
}

static const struct setting hda_tone_setting = {
    .name = "hda_tone",
    .label = "HD Audio diagnostic tone",
    .type = SETTING_TYPE_ENUM,
    .file = CONFIG_PATH_RUNTIME,
    .category = TUNABLE_CATEGORY,
    .group = "Sound",
    .choice = onoff_choice,
    .get = hda_tone_get,
    .apply = hda_tone_apply,
};

// ---- kernel.usb_reset ------------------------------------------------
//
// Write-only, like hda_tone: a PORT NUMBER forces that root port through
// a real reset and re-enumerates it, and it reads back as "off". The
// experiment for docs/bugs.md's intermittent enumeration failure, whose
// only known cure is replugging the device -- this does the re-connect
// half without the power-cycle half, so which one matters becomes a
// measurement instead of an argument.

static void usb_reset_get(char *out, uint32_t cap) { k_strlcpy(out, "off", cap); }

static int usb_reset_apply(const char *value) {
    if (!value || !value[0]) return SETTING_INVALID;
    unsigned port = 0;
    for (const char *c = value; *c; c++) {
        if (*c < '0' || *c > '9') return SETTING_INVALID;
        port = port * 10 + (unsigned)(*c - '0');
        if (port > 255) return SETTING_INVALID;
    }
    if (!port) return SETTING_INVALID;   // ports are 1-based, as logged
    // QUEUED, not performed: this runs in a SYSCALL, with interrupts
    // off, and the reset's recovery wait spins on a counter only the
    // timer interrupt advances -- done inline it never returns, which
    // is how the first version of this froze a laptop. The result is in
    // the kernel log a moment later, which is also the honest place for
    // it: a refused port and a device that would not come back are both
    // "we tried", and calling the second an invalid SETTING would be a
    // lie about what was wrong.
    usb_diag_reset_port(port);
    return SETTING_SAVED;
}

static const struct setting usb_reset_setting = {
    .name = "usb_reset",
    .label = "Force a USB port reset",
    .type = SETTING_TYPE_STRING,
    .file = CONFIG_PATH_RUNTIME,
    .category = TUNABLE_CATEGORY,
    .group = "Diagnostics",
    .get = usb_reset_get,
    .apply = usb_reset_apply,
};

// ---- kernel.usb_replug -----------------------------------------------
//
// The other half of a replug, and the sibling of usb_reset above: that
// one re-connects the port, this one takes the port AWAY from the
// device and gives it back -- power where the controller has Port
// Power Control, the Intel port mux where it does not. Both are
// write-only port numbers reading back "off".
//
// Worth having as a knob rather than only as the driver's own recovery,
// because the failure it exists for is intermittent: the knob answers
// "does the mechanism work at all" on a device that is currently fine,
// which is a different question from "did it rescue a wedged one" and
// can be asked on any boot.

static void usb_replug_get(char *out, uint32_t cap) { k_strlcpy(out, "off", cap); }

static int usb_replug_apply(const char *value) {
    if (!value || !value[0]) return SETTING_INVALID;
    unsigned port = 0;
    for (const char *c = value; *c; c++) {
        if (*c < '0' || *c > '9') return SETTING_INVALID;
        port = port * 10 + (unsigned)(*c - '0');
        if (port > 255) return SETTING_INVALID;
    }
    if (!port) return SETTING_INVALID;   // ports are 1-based, as logged
    usb_diag_replug_port(port);
    return SETTING_SAVED;
}

static const struct setting usb_replug_setting = {
    .name = "usb_replug",
    .label = "Replug a USB port in software",
    .type = SETTING_TYPE_STRING,
    .file = CONFIG_PATH_RUNTIME,
    .category = TUNABLE_CATEGORY,
    .group = "Diagnostics",
    .get = usb_replug_get,
    .apply = usb_replug_apply,
};

// ---- kernel.usb_attach_delay -------------------------------------------
//
// How long a freshly connected port is left alone before it is RESET,
// in milliseconds -- the USB2 attach debounce (TATTDB). Unlike the two
// knobs above this one READS BACK, because it is a state rather than an
// action.
//
// It exists as a knob for one reason: the enumeration failure in
// docs/bugs.md happens on about 1 boot in 50, and a lever that provokes
// it turns a boot lottery into an experiment. Set it to 0, replug a
// port in software, and see what speed the device negotiates.

static void usb_attach_delay_get(char *out, uint32_t cap) {
    k_snprintf(out, cap, "%u", usb_attach_delay_ms());
}

static int usb_attach_delay_apply(const char *value) {
    if (!value || !value[0]) return SETTING_INVALID;
    unsigned ms = 0;
    for (const char *c = value; *c; c++) {
        if (*c < '0' || *c > '9') return SETTING_INVALID;
        ms = ms * 10 + (unsigned)(*c - '0');
        if (ms > 1000) return SETTING_INVALID;
    }
    usb_set_attach_delay_ms(ms);
    return SETTING_SAVED;
}

static const struct setting usb_attach_delay_setting = {
    .name = "usb_attach_delay",
    .label = "USB attach debounce (ms)",
    .type = SETTING_TYPE_STRING,
    .file = CONFIG_PATH_RUNTIME,
    .category = TUNABLE_CATEGORY,
    .group = "Diagnostics",
    .get = usb_attach_delay_get,
    .apply = usb_attach_delay_apply,
};

// ---- kernel.timeslice_ms ----------------------------------------------
//
// How long a process runs before the scheduler rotates, when something
// else is runnable -- Linux's sched base_slice, and like it a runtime
// knob rather than a build option. The tick rate (`option hz`) only
// bounds how precisely it is enforced on the periodic path.

static void timeslice_get(char *out, uint32_t cap) {
    k_snprintf(out, cap, "%u", scheduler_timeslice_ms());
}

static int timeslice_apply(const char *value) {
    if (!value || !value[0]) return SETTING_INVALID;
    unsigned ms = 0;
    for (const char *c = value; *c; c++) {
        if (*c < '0' || *c > '9') return SETTING_INVALID;
        ms = ms * 10 + (unsigned)(*c - '0');
        if (ms > SCHED_TIMESLICE_MAX_MS) return SETTING_INVALID;
    }
    return scheduler_set_timeslice_ms(ms) ? SETTING_SAVED : SETTING_INVALID;
}

static const struct setting timeslice_setting = {
    .name = "timeslice_ms",
    .label = "Scheduler time slice (ms)",
    .type = SETTING_TYPE_STRING,
    .file = CONFIG_PATH_RUNTIME,
    .category = TUNABLE_CATEGORY,
    .group = "Scheduler",
    .get = timeslice_get,
    .apply = timeslice_apply,
};

// ---- system.usb_recover -----------------------------------------------
//
// POLICY, not a diagnostic: whether a port that has exhausted every
// cheaper lever may re-initialise the whole CONTROLLER. Persisted, and
// OFF by default, because the reset takes every USB device down with it
// for a moment -- on a laptop that is the keyboard and the trackpad.
//
// It is on for the machine with the fault (docs/bugs.md) and off
// everywhere else, the same shape `system.net_recover` has: a headless
// test machine wants a recovery a desktop would find alarming.
//
// READ WHEN THE LEVER FIRES, not cached at boot: USB enumeration gives
// up around 1.3 s and init has the filesystem up at 0.6 s, so the file
// is readable by then -- and a value cached earlier would be the one
// from before the user changed it.

static void usb_recover_get(char *out, uint32_t cap) {
    if (!etc_config_get("/etc/toyos.conf", "usb_recover", out, cap))
        k_strlcpy(out, "off", cap);
}

static int usb_recover_apply(const char *value) {
    if (k_strcmp(value, "on") != 0 && k_strcmp(value, "off") != 0) return SETTING_INVALID;
    return etc_config_set("/etc/toyos.conf", "usb_recover", value)
           ? SETTING_SAVED : SETTING_UNSAVED;
}

static int usb_recover_choice(int index, char *out, uint32_t cap) {
    if (index == 0) { k_strlcpy(out, "on", cap); return 1; }
    if (index == 1) { k_strlcpy(out, "off", cap); return 1; }
    return 0;
}

static const struct setting g_usb_recover_setting = {
    .name = "usb_recover",
    .label = "Reset USB controller to recover a port",
    .type = SETTING_TYPE_ENUM,
    .file = "/etc/toyos.conf",
    .category = TUNABLE_CATEGORY, .group = "Diagnostics",
    .choice = usb_recover_choice,
    .get = usb_recover_get, .apply = usb_recover_apply,
};

// What the driver asks. Kept here beside the setting so there is one
// definition of what "on" means.
int usb_recover_enabled(void) {
    char v[8];
    if (!etc_config_get("/etc/toyos.conf", "usb_recover", v, sizeof v)) return 0;
    return k_strcmp(v, "on") == 0;
}

// ---- kernel.usb_hcreset -----------------------------------------------
// The last recovery lever: re-initialise the whole controller. A knob
// for the reason usb_replug is one -- the mechanism can be checked on
// a machine where everything currently works.
static void usb_hcreset_get(char *out, uint32_t cap) { k_strlcpy(out, "off", cap); }

static int usb_hcreset_choice(int index, char *out, uint32_t cap) {
    if (index == 0) { k_strlcpy(out, "off", cap); return 1; }
    if (index == 1) { k_strlcpy(out, "on", cap); return 1; }
    return 0;
}

static const char *usb_hcreset_unavailable(void) {
    if (!usb_controller_present()) return "no xHCI controller on this machine";
    return 0;
}

static int usb_hcreset_apply(const char *value) {
    if (!value || k_strcmp(value, "on") != 0) return SETTING_INVALID;
    return usb_controller_reinit() ? SETTING_SAVED : SETTING_INVALID;
}

static const struct setting g_usb_hcreset_setting = {
    .name = "usb_hcreset", .label = "Re-initialise the USB controller",
    .type = SETTING_TYPE_ENUM, .file = CONFIG_PATH_RUNTIME,
    .category = TUNABLE_CATEGORY, .group = "Diagnostics",
    .choice = usb_hcreset_choice, .unavailable = usb_hcreset_unavailable,
    .get = usb_hcreset_get, .apply = usb_hcreset_apply,
};

// ---- kernel.intel_cycle -----------------------------------------------
//
// Write-only, like hda_tone: `pipe` turns the laptop panel's transcoder
// and pipe off and back on, `link` also drops the DP link and retrains
// it, `native` runs the whole modeset for the mode on screen (what
// set_mode does) -- stage 3 of Intel modesetting, one mechanism per flash, each
// logging every readback. Reads back as "off". A machine without the
// Intel display gets the sentence, not a silent no-op.

static void intel_cycle_get(char *out, uint32_t cap) { k_strlcpy(out, "off", cap); }

static int intel_cycle_choice(int index, char *out, uint32_t cap) {
    static const char *const names[] = { "off", "pipe", "link", "native" };
    if (index < 0 || index > 3) return 0;
    k_strlcpy(out, names[index], cap);
    return 1;
}

static const char *intel_cycle_unavailable(void) {
    if (!intel_display_active())
        return "This machine's display is not driven by the Intel display driver.";
    return 0;
}

static int intel_cycle_apply(const char *value) {
    if (k_strcmp(value, "off") == 0) return SETTING_SAVED;
    if (k_strcmp(value, "pipe") == 0) return intel_display_pipe_cycle() ? SETTING_SAVED : SETTING_INVALID;
    if (k_strcmp(value, "link") == 0) return intel_display_link_retrain() ? SETTING_SAVED : SETTING_INVALID;
    if (k_strcmp(value, "native") == 0) return intel_display_native() ? SETTING_SAVED : SETTING_INVALID;
    return SETTING_INVALID;
}

static const struct setting intel_cycle_setting = {
    .name = "intel_cycle",
    .label = "Intel display cycle (diagnostic)",
    .type = SETTING_TYPE_ENUM,
    .file = CONFIG_PATH_RUNTIME,
    .category = TUNABLE_CATEGORY,
    .group = "Display",
    .choice = intel_cycle_choice,
    .get = intel_cycle_get,
    .apply = intel_cycle_apply,
    .unavailable = intel_cycle_unavailable,
};

// ---- kernel.crash ----------------------------------------------------
//
// Write-only: panics the machine the named way -- Linux's sysrq `c`,
// without the GUI the Crash Test app needs. The kinds are crashtest.c's,
// so both front ends list the same set, and the same `faultinject` boot
// word arms both. Reads back as "off".

static void crash_get(char *out, uint32_t cap) { k_strlcpy(out, "off", cap); }

static int crash_choice(int index, char *out, uint32_t cap) {
    if (index == 0) { k_strlcpy(out, "off", cap); return 1; }
    const struct crash_kind *k = crash_kind_at(index - 1);
    if (!k) return 0;
    k_strlcpy(out, k->name, cap);
    return 1;
}

static const char *crash_unavailable(void) {
    if (!crash_armed()) return "Boot with faultinject on the GRUB line to arm deliberate kernel faults.";
    return 0;
}

static int crash_apply(const char *value) {
    if (k_strcmp(value, "off") == 0) return SETTING_SAVED;
    for (int i = 0; i < crash_kind_count(); i++)
        if (k_strcmp(value, crash_kind_at(i)->name) == 0)
            return crash_trigger(i) ? SETTING_SAVED : SETTING_INVALID;
    return SETTING_INVALID;
}

static const struct setting crash_setting = {
    .name = "crash",
    .label = "Panic the kernel now (diagnostic)",
    .type = SETTING_TYPE_ENUM,
    .file = CONFIG_PATH_RUNTIME,
    .category = TUNABLE_CATEGORY,
    .group = "Diagnostics",
    .choice = crash_choice,
    .get = crash_get,
    .apply = crash_apply,
    .unavailable = crash_unavailable,
};

// ---- kernel.panic_record ---------------------------------------------
//
// Write-only `clear`: the previous boot's panic record has been filed
// (QUERY_PANIC), so stop reporting it. logd's call, after it appends the
// record to the dead boot's log -- what deleting a file under
// /sys/fs/pstore does on Linux.

static void panic_record_get(char *out, uint32_t cap) { k_strlcpy(out, "off", cap); }

static int panic_record_choice(int index, char *out, uint32_t cap) {
    if (index == 0) { k_strlcpy(out, "off", cap); return 1; }
    if (index == 1) { k_strlcpy(out, "clear", cap); return 1; }
    return 0;
}

static int panic_record_apply(const char *value) {
    if (k_strcmp(value, "off") == 0) return SETTING_SAVED;
    if (k_strcmp(value, "clear") == 0) { panic_store_clear(); return SETTING_SAVED; }
    return SETTING_INVALID;
}

static const struct setting panic_record_setting = {
    .name = "panic_record",
    .label = "Previous boot's panic record",
    .type = SETTING_TYPE_ENUM,
    .file = CONFIG_PATH_RUNTIME,
    .category = TUNABLE_CATEGORY,
    .group = "Diagnostics",
    .choice = panic_record_choice,
    .get = panic_record_get,
    .apply = panic_record_apply,
};

void tunables_register(void) {
    setting_register(&crash_setting);
    setting_register(&panic_record_setting);
    setting_register(&intel_cycle_setting);
    setting_register(&heap_debug_setting);
    setting_register(&hda_tone_setting);
    setting_register(&usb_reset_setting);
    setting_register(&usb_replug_setting);
    setting_register(&usb_attach_delay_setting);
    setting_register(&timeslice_setting);
    setting_register(&g_usb_hcreset_setting);
    setting_register(&g_usb_recover_setting);
    setting_register(&ata_nodma_setting);
    setting_register(&kstack_track_setting);
    setting_register(&syscall_stall_setting);
    setting_register(&kbdtap_setting);
}
