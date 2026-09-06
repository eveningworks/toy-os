// The `storage.sync` setting: a registry descriptor and the live flag
// TFS3's journal reads. See storage_config.h for what it is for.
//
// WHAT `batched` GIVES UP, which is much less than `lazy`. A write's
// DATA blocks and its allocation bitmaps reach the disk before the
// transaction is even opened (tfs3.c's do_write_inner) -- the
// transaction covers the INODE block alone. So deferring the commit
// risks the inode update, not the data: a just-extended file comes back
// at its old size with the blocks past it unreferenced. That is a LEAK,
// which `fsck` reclaims, and it is the ordering this filesystem was
// built on ("prefer a leak to a double-allocation").
//
// WHAT `lazy` ACTUALLY GIVES UP, because a durability knob whose
// description is vague is worse than no knob. Both of txn_commit()'s
// barriers are load-bearing and neither is merely an optimisation:
//
//   * the first orders the JOURNAL against the targets, so a crash
//     while the targets are half-written can be replayed;
//   * the second orders the TARGETS against clearing the commit flag,
//     so a crash cannot leave the journal saying "nothing to do" over
//     work that never landed.
//
// Without them a crash or power loss can leave the filesystem in a
// state replay cannot repair -- not merely lose the last few writes.
// This is exactly ext4's `nobarrier`, which carries the same warning
// and is meant for a device whose cache is battery-backed. A clean
// shutdown, `sync`, and the ATA cache's idle timer all still flush, so
// `lazy` is about CRASHES, not about whether data eventually lands.
//
// STRICT IS THE DEFAULT and an unparseable value leaves it strict --
// the same tolerance every other /etc reader here has, chosen the safe
// way round because this one trades correctness for speed.
#include "setting.h"
#include "etc_config.h"
#include "string.h"
#include "kfmt.h"      // k_snprintf
#include "timer.h"     // PIT_HZ
#include "storage_config.h"
#include "initcall.h"

#define STORAGE_CONFIG_FILE "/etc/storage.conf"
#define SYNC_KEY "sync"
#define WRITEBACK_KEY "writeback_interval"
#define RAMFS_SIZE_KEY "ramfs_size"

// SECONDS OF QUIET before a deferred commit is forced. The visible half
// of what ext4 spells `commit=5` and Linux spells
// dirty_expire_centisecs. Only `batched` consults it; in `strict` every
// write commits before it returns and there is nothing to expire.
//
// 1 second by default, matching the sector cache's own idle threshold
// (ATAC_IDLE_TICKS) so the two halves of "a quiet machine ends up on
// the platter" agree. The ceiling is deliberately low: this is how long
// a crash can cost you, and a number nobody would accept as a data-loss
// window is not a number to offer.
#define WRITEBACK_MIN 1
#define WRITEBACK_MAX 30
#define WRITEBACK_DEFAULT 1

// MiB a ramfs mount may hold. **THE DEFAULT IS 0 AND MUST STAY 0**: 0
// means "half of free memory", ramfs's own rule and tmpfs's default,
// and it is what a DISKLESS ROOT gets -- that mount happens in
// fs_init(), before /etc is readable and before this file's init() has
// run, so whatever is compiled in here is what it sees. A non-zero
// default would silently shrink a diskless root to it.
//
// The ceiling is a sanity bound, not a memory limit: ramfs refuses an
// allocation past its budget exactly as a full disk does, so asking for
// more than the machine has costs nothing until something writes.
#define RAMFS_SIZE_MIN 0
#define RAMFS_SIZE_MAX 4096
#define RAMFS_SIZE_DEFAULT 0

// ORDERED BY SAFETY, strongest first, because that is the order a
// person reads a choice list in and the default must be the first thing
// they see.
static const char *const g_modes[] = { "strict", "batched", "lazy" };
#define MODE_COUNT ((int)(sizeof g_modes / sizeof g_modes[0]))

// THE THREE MODES, as two flags rather than an enum, because the two
// questions they answer are independent and every caller asks only one:
//
//   strict   barriers real, commit per write   -- the default
//   batched  barriers real, commit DEFERRED    -- one commit for many
//   lazy     barriers skipped entirely         -- ext4's nobarrier
//
// `batched` IS THE DEFAULT (2026-09-04), on a measurement rather than a
// preference: on the bare-metal laptop it is 6.7x faster than `strict`
// on a sequential write and 8.9x at 4 KiB. What a crash costs is an
// inode update -- a just-extended file returns at its old size with the
// blocks past it unreferenced, which `fsck` reclaims. `strict` is one
// `config set` away for anyone who wants every write durable before it
// returns.
//
// BARRIERS STAY REAL, which is the half that keeps this defensible:
// `g_strict` is still 1 here. Only `lazy` turns them off, and only
// `lazy` risks a journal that cannot be replayed.
//
// These values also stand during MOUNT and journal replay, both of
// which run before storage_config_init(). That is safe: deferral only
// affects do_write_inner()'s path, replay writes its targets directly,
// and anything opening a transaction commits a deferred one on its way
// past.
//
// TWO THINGS HAD TO BE FIXED BEFORE THIS COULD BE THE DEFAULT, both
// found by making it one: `fsck` had to commit a deferred transaction
// before walking the disk (blocks referenced by an uncommitted inode
// are not leaked), and a fault-injection test had to make its fixture
// durable before arming, because a deferred commit is otherwise flushed
// into somebody else's injected failure.
//
// BARRIERS STAY REAL, which is the half that keeps this defensible:
// `g_strict` is still 1 here. Only `lazy` turns them off, and only
// `lazy` risks a journal that cannot be replayed.
//
// These values also stand during MOUNT and journal replay, both of
// which run before storage_config_init(). That is safe: deferral only
// affects do_write_inner()'s path, replay writes its targets directly,
// and anything opening a transaction commits a deferred one on its way
// past.
static int g_strict = 1;    // 0 only in `lazy`: whether barriers are issued
static int g_batched = 1;   // 1 only in `batched`: whether commits defer
static int g_writeback_s = WRITEBACK_DEFAULT;
static int g_ramfs_size_mib = RAMFS_SIZE_DEFAULT;

int storage_sync_strict(void) { return g_strict; }

// Bytes, converted here rather than at the call site so the unit lives
// with the setting that owns the number. 0 means "the backend decides".
uint64_t storage_ramfs_size_bytes(void) {
    return (uint64_t)g_ramfs_size_mib * 1024 * 1024;
}
int storage_sync_batched(void) { return g_batched; }

// In PIT TICKS, converted here rather than at the call site so the
// unit conversion lives with the setting that owns the number.
uint32_t storage_writeback_ticks(void) {
    return (uint32_t)g_writeback_s * PIT_HZ;
}

static int mode_choice(int index, char *out, uint32_t out_size) {
    if (index < 0 || index >= MODE_COUNT) return 0;
    k_strlcpy(out, g_modes[index], out_size);
    return 1;
}

static void mode_get(char *out, uint32_t out_size) {
    k_strlcpy(out, g_modes[!g_strict ? 2 : (g_batched ? 1 : 0)], out_size);
}

// Applies AND persists, which is the contract (`setting.h`). The
// in-memory half is split out so the boot reader can use it without
// writing the file back it just read.
static int mode_set(const char *value) {
    if (!value) return 0;
    if (k_strcmp(value, "strict") == 0)  { g_strict = 1; g_batched = 0; return 1; }
    if (k_strcmp(value, "batched") == 0) { g_strict = 1; g_batched = 1; return 1; }
    if (k_strcmp(value, "lazy") == 0)    { g_strict = 0; g_batched = 0; return 1; }
    return 0;
}

static int mode_apply(const char *value) {
    if (!mode_set(value)) return SETTING_INVALID;
    return etc_config_set(STORAGE_CONFIG_FILE, SYNC_KEY, value)
               ? SETTING_SAVED : SETTING_UNSAVED;
}

static void writeback_get(char *out, uint32_t out_size) {
    k_snprintf(out, out_size, "%d", g_writeback_s);
}

static int writeback_apply(const char *value) {
    if (!value) return SETTING_INVALID;
    int n = 0;
    for (const char *p = value; *p; p++) {
        if (*p < '0' || *p > '9') return SETTING_INVALID;
        n = n * 10 + (*p - '0');
        if (n > WRITEBACK_MAX) return SETTING_INVALID;
    }
    if (n < WRITEBACK_MIN) return SETTING_INVALID;
    g_writeback_s = n;
    return etc_config_set(STORAGE_CONFIG_FILE, WRITEBACK_KEY, value)
               ? SETTING_SAVED : SETTING_UNSAVED;
}

// UNAVAILABLE IN EVERY MODE BUT `batched`, with the reason said rather
// than the control merely greyed out. In `strict` a write commits
// before it returns and in `lazy` nothing is deferred either, so there
// is no interval to set -- and a spinbox that silently does nothing is
// the failure `setting.unavailable` exists to prevent.
static const char *writeback_unavailable(void) {
    return g_batched ? 0 : "Only Grouped writes defer a commit long enough to expire";
}

static const struct setting g_writeback_setting = {
    .name  = WRITEBACK_KEY,
    .label = "Group writes for",
    .type  = SETTING_TYPE_INT,
    .file  = STORAGE_CONFIG_FILE,
    .category = "Storage",
    .group    = "Filesystem",
    .min   = WRITEBACK_MIN,
    .max   = WRITEBACK_MAX,
    .step  = 1,
    .unit  = "s",
    .get   = writeback_get,
    .apply = writeback_apply,
    .unavailable = writeback_unavailable,
};

static void ramfs_size_get(char *out, uint32_t out_size) {
    k_snprintf(out, out_size, "%d", g_ramfs_size_mib);
}

static int ramfs_size_apply(const char *value) {
    if (!value || !value[0]) return SETTING_INVALID;
    int n = 0;
    for (const char *p = value; *p; p++) {
        if (*p < '0' || *p > '9') return SETTING_INVALID;
        n = n * 10 + (*p - '0');
        if (n > RAMFS_SIZE_MAX) return SETTING_INVALID;
    }
    if (n < RAMFS_SIZE_MIN) return SETTING_INVALID;
    g_ramfs_size_mib = n;
    return etc_config_set(STORAGE_CONFIG_FILE, RAMFS_SIZE_KEY, value)
               ? SETTING_SAVED : SETTING_UNSAVED;
}

// TAKEN AT MOUNT, so changing it moves nothing that is already mounted.
// Said here because a size control that appears to do nothing is worse
// than one that is not offered.
static const struct setting g_ramfs_size_setting = {
    .name  = RAMFS_SIZE_KEY,
    .label = "Scratch filesystem size",
    .type  = SETTING_TYPE_INT,
    .file  = STORAGE_CONFIG_FILE,
    .category = "Storage",
    .group    = "Filesystem",
    .min   = RAMFS_SIZE_MIN,
    .max   = RAMFS_SIZE_MAX,
    .step  = 16,
    .unit  = "MiB",
    .get   = ramfs_size_get,
    .apply = ramfs_size_apply,
};

static const struct setting g_sync_setting = {
    .name  = SYNC_KEY,
    .label = "Write durability",
    .type  = SETTING_TYPE_ENUM,
    .file  = STORAGE_CONFIG_FILE,
    .category = "Storage",
    .group    = "Filesystem",
    .choice = mode_choice,
    .get    = mode_get,
    .apply  = mode_apply,
};

void storage_config_setting_register(void) {
    setting_register(&g_sync_setting);
    setting_register(&g_writeback_setting);
    setting_register(&g_ramfs_size_setting);
}

void storage_config_set_mode_for_test(int strict, int batched) {
    g_strict = strict ? 1 : 0;
    g_batched = batched ? 1 : 0;
}

static struct etc_config_buf g_cfg;

void storage_config_init(void) {
    char value[16];
    etc_config_load(STORAGE_CONFIG_FILE, &g_cfg);
    if (etc_config_buf_get(&g_cfg, WRITEBACK_KEY, value, sizeof value)) {
        int n = 0, ok = value[0] != '\0';
        for (const char *p = value; *p && ok; p++) {
            if (*p < '0' || *p > '9') ok = 0; else n = n * 10 + (*p - '0');
        }
        if (ok && n >= WRITEBACK_MIN && n <= WRITEBACK_MAX) g_writeback_s = n;
    }
    if (etc_config_buf_get(&g_cfg, RAMFS_SIZE_KEY, value, sizeof value)) {
        int n = 0, ok = value[0] != '\0';
        for (const char *p = value; *p && ok; p++) {
            if (*p < '0' || *p > '9') ok = 0; else n = n * 10 + (*p - '0');
        }
        if (ok && n >= RAMFS_SIZE_MIN && n <= RAMFS_SIZE_MAX) g_ramfs_size_mib = n;
    }
    if (etc_config_buf_get(&g_cfg, SYNC_KEY, value, sizeof value)) {
        // A hand-edited file reaches this reader without passing
        // through setting_set(), so it is validated here too -- and an
        // unrecognised value leaves STRICT rather than guessing.
        mode_set(value);
    }
}
INITCALL(storage_config_init, INIT_CONFIG);
