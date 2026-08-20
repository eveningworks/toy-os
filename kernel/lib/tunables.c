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
#include "kfmt.h"

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
    // is inverted from ata_dma_active(). Named for the state it sets
    // rather than the one it reports, matching the `ata nodma` command
    // it replaces -- renaming it to `ata_dma` would flip the meaning of
    // every note and test that mentions it.
    k_strlcpy(out, ata_dma_active() ? "off" : "on", cap);
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

void tunables_register(void) {
    setting_register(&heap_debug_setting);
    setting_register(&ata_nodma_setting);
    setting_register(&kstack_track_setting);
}
