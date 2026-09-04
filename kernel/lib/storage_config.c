// The `storage.sync` setting: a registry descriptor and the live flag
// TFS3's journal reads. See storage_config.h for what it is for.
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
#include "storage_config.h"
#include "initcall.h"

#define STORAGE_CONFIG_FILE "/etc/storage.conf"
#define SYNC_KEY "sync"

static const char *const g_modes[] = { "strict", "lazy" };
#define MODE_COUNT ((int)(sizeof g_modes / sizeof g_modes[0]))

// 1 until /etc says otherwise. The journal runs during MOUNT and during
// replay, both before storage_config_init(), so the safe answer has to
// be the one that needs no file to have been read.
static int g_strict = 1;

int storage_sync_strict(void) { return g_strict; }

static int mode_choice(int index, char *out, uint32_t out_size) {
    if (index < 0 || index >= MODE_COUNT) return 0;
    k_strlcpy(out, g_modes[index], out_size);
    return 1;
}

static void mode_get(char *out, uint32_t out_size) {
    k_strlcpy(out, g_modes[g_strict ? 0 : 1], out_size);
}

// Applies AND persists, which is the contract (`setting.h`). The
// in-memory half is split out so the boot reader can use it without
// writing the file back it just read.
static int mode_set(const char *value) {
    if (!value) return 0;
    if (k_strcmp(value, "strict") == 0) { g_strict = 1; return 1; }
    if (k_strcmp(value, "lazy") == 0)   { g_strict = 0; return 1; }
    return 0;
}

static int mode_apply(const char *value) {
    if (!mode_set(value)) return SETTING_INVALID;
    return etc_config_set(STORAGE_CONFIG_FILE, SYNC_KEY, value)
               ? SETTING_SAVED : SETTING_UNSAVED;
}

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
}

void storage_config_set_strict_for_test(int strict) { g_strict = strict ? 1 : 0; }

static struct etc_config_buf g_cfg;

void storage_config_init(void) {
    char value[16];
    etc_config_load(STORAGE_CONFIG_FILE, &g_cfg);
    if (etc_config_buf_get(&g_cfg, SYNC_KEY, value, sizeof value)) {
        // A hand-edited file reaches this reader without passing
        // through setting_set(), so it is validated here too -- and an
        // unrecognised value leaves STRICT rather than guessing.
        mode_set(value);
    }
}
INITCALL(storage_config_init, INIT_CONFIG);
