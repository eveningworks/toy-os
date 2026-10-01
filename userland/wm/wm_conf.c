// See wm_conf.h.
#include "wm/wm_conf.h"
#include "lib/uconf.h"
#include "rt/sys.h"
#include <string.h>

// THE FILE HALF MOVED TO lib/uconf.h when it got a second caller (the
// File Manager's /etc/files.conf). These three stay as the WM's own
// names because every call site here reads better for it, and because
// wm_setting_generation() below -- which is about the SETTINGS REGISTRY
// rather than about a file -- has no business in a general config
// helper.

int wm_conf_load(const char *path, struct etc_config_buf *buf) {
    return uconf_load(path, buf);
}

int wm_conf_get(const char *path, const char *key, char *out, uint32_t out_size) {
    return uconf_get(path, key, out, out_size);
}

int wm_conf_set(const char *path, const char *key, const char *value) {
    return uconf_set(path, key, value);
}

int wm_conf_unset(const char *path, const char *key) {
    return uconf_unset(path, key);
}

uint32_t wm_setting_generation(void) {
    struct setting_msg msg;
    for (unsigned i = 0; i < sizeof msg; i++) ((uint8_t *)&msg)[i] = 0;
    // Any op fills `generation`; COUNT is the cheapest and has no
    // arguments to get wrong.
    msg.op = SETTING_OP_COUNT;
    if (sys_setting(&msg) != 0) return 0;
    return msg.generation;
}
