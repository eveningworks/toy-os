// The human-facing text for a setting -- see api/setting_text.h for the
// format and for why prose lives in /etc rather than in the kernel.
//
// THE FILE IS READ ONCE PER LOOKUP, deliberately not cached. A settings
// UI asks for this while the user is looking at a page, which is about
// as far from a hot path as this kernel has; caching would need an
// invalidation rule, and `config reload` plus a hand edit is exactly the
// case a stale cache gets wrong. etc_config_get() re-reads per key, so
// the multi-key paths here use etc_config_load()/_buf_get() instead --
// one read answering many keys, which is the difference between 1 read
// and 12 for a page.
#include "setting_text.h"
#include "setting_abi.h"
#include "etc_config.h"
#include "fs.h"
#include "string.h"
#include "kfmt.h"
#include "knum.h" // k_parse_u32 for Order=
#include <stddef.h>

// A setting's file is named by its QUALIFIED name, because that is its
// identity -- two programs may each own a `theme`.
static int text_path(const char *ns, const char *name, char *out, uint32_t cap) {
    if (!name || !name[0]) return 0;
    int n;
    if (ns && ns[0])
        n = k_snprintf(out, cap, SETTING_TEXT_DIR "/%s.%s", ns, name);
    else
        n = k_snprintf(out, cap, SETTING_TEXT_DIR "/%s", name);
    // k_snprintf writes NOTHING rather than truncating when it does not
    // fit (kernel/lib's convention), so a 0 here means the path was too
    // long -- and a truncated path would name a DIFFERENT file, which is
    // the failure worth refusing.
    return n > 0;
}

int setting_text_description(const char *ns, const char *name,
                             char *out, uint32_t out_size) {
    if (out && out_size) out[0] = '\0';
    char path[FS_PATH_MAX];
    if (!text_path(ns, name, path, sizeof path)) return 0;
    if (!fs_exists(path)) return 0; // no text for this setting: normal
    return etc_config_get(path, SETTING_TEXT_KEY_DESC, out, out_size);
}

int setting_text_choice(const char *ns, const char *name, const char *value,
                        char *out, uint32_t out_size) {
    // THE FALLBACK IS THE VALUE ITSELF, written first so every return
    // path below leaves `out` usable. A caller may always draw what this
    // produces, which is what stops each client growing its own "did I
    // get a display name?" branch.
    if (out && out_size) k_strlcpy(out, value ? value : "", out_size);
    if (!value || !value[0]) return 0;

    char path[FS_PATH_MAX];
    if (!text_path(ns, name, path, sizeof path)) return 0;
    if (!fs_exists(path)) return 0;

    char key[64];
    if (k_snprintf(key, sizeof key, SETTING_TEXT_CHOICE_PREFIX "%s", value) <= 0)
        return 0;
    char display[SETTING_ABI_VALUE_MAX];
    if (!etc_config_get(path, key, display, sizeof display)) return 0;
    if (!display[0]) return 0; // an empty display name is not a name
    k_strlcpy(out, display, out_size);
    return 1;
}

uint32_t setting_text_widget(const char *ns, const char *name) {
    char path[FS_PATH_MAX];
    if (!text_path(ns, name, path, sizeof path)) return SETTING_ABI_WIDGET_AUTO;
    if (!fs_exists(path)) return SETTING_ABI_WIDGET_AUTO;

    char value[16];
    if (!etc_config_get(path, SETTING_TEXT_KEY_WIDGET, value, sizeof value))
        return SETTING_ABI_WIDGET_AUTO;
    if (k_strcmp(value, "radio") == 0) return SETTING_ABI_WIDGET_RADIO;
    if (k_strcmp(value, "dropdown") == 0) return SETTING_ABI_WIDGET_DROPDOWN;
    if (k_strcmp(value, "slider") == 0) return SETTING_ABI_WIDGET_SLIDER;
    // An unrecognised name is AUTO, not an error: a file naming a
    // control this build has no widget for must still leave the setting
    // changeable, and a later build may well understand it.
    return SETTING_ABI_WIDGET_AUTO;
}

// The two-key readers below load once (etc_config.h). Static: 4 KiB
// against a 1 KiB frame budget, and a syscall handler runs with
// interrupts off, so one buffer serves them.
static struct etc_config_buf g_text_buf;

uint32_t setting_text_sflags(const char *ns, const char *name) {
    char path[FS_PATH_MAX];
    if (!text_path(ns, name, path, sizeof path)) return 0;
    if (!fs_exists(path)) return 0;

    uint32_t flags = 0;
    char value[16];
    etc_config_load(path, &g_text_buf);   // one read, two keys
    if (etc_config_buf_get(&g_text_buf, SETTING_TEXT_KEY_APPLIES, value, sizeof value) &&
        k_strcmp(value, "reboot") == 0)
        flags |= SETTING_ABI_SF_REBOOT;
    // Anything other than an explicit "1" is not advanced. A key present
    // but empty means somebody started to write it and stopped, which is
    // not a reason to hide a setting from them.
    if (etc_config_buf_get(&g_text_buf, SETTING_TEXT_KEY_ADVANCED, value, sizeof value) &&
        k_strcmp(value, "1") == 0)
        flags |= SETTING_ABI_SF_ADVANCED;
    return flags;
}

int setting_text_order(const char *ns, const char *name) {
    char path[FS_PATH_MAX];
    if (!text_path(ns, name, path, sizeof path)) return 0;
    if (!fs_exists(path)) return 0;

    char value[16];
    if (!etc_config_get(path, SETTING_TEXT_KEY_ORDER, value, sizeof value)) return 0;
    uint32_t n = 0;
    // Unsigned, so no negative orders -- there is no reason for one, and
    // a parser that accepted them would need a sign convention nobody
    // asked for. A malformed value is 0, i.e. "no opinion".
    if (!k_parse_u32(value, &n)) return 0;
    return (int)n;
}

int setting_text_group(const char *category, const char *group,
                       char *out_label, uint32_t label_size,
                       char *out_desc, uint32_t desc_size) {
    if (out_label && label_size) out_label[0] = '\0';
    if (out_desc && desc_size) out_desc[0] = '\0';
    if (!category || !category[0] || !group || !group[0]) return 0;

    char path[FS_PATH_MAX];
    if (k_snprintf(path, sizeof path,
                   SETTING_TEXT_DIR "/" SETTING_TEXT_GROUP_PREFIX "%s.%s",
                   category, group) <= 0)
        return 0;
    if (!fs_exists(path)) return 0;

    etc_config_load(path, &g_text_buf);   // one read, two keys
    etc_config_buf_get(&g_text_buf, SETTING_TEXT_KEY_LABEL, out_label, label_size);
    etc_config_buf_get(&g_text_buf, SETTING_TEXT_KEY_DESC, out_desc, desc_size);
    return 1;
}
