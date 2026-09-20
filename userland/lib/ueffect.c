// See lib/ueffect.h. Two paths and a delegation -- the parser, the
// defaults and the value resolution are usaver.c's, which is the point:
// one format, one implementation, two owners.
#include <stdio.h>
#include "lib/ueffect.h"

void ueffect_conf_path(const char *effect, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s.conf", UEFFECT_CONF_DIR, effect);
}

int ueffect_load(const char *effect, struct usaver *out) {
    char desc[96], conf[96];
    snprintf(desc, sizeof desc, "%s/%s.effect", UEFFECT_DESC_DIR, effect);
    ueffect_conf_path(effect, conf, sizeof conf);
    return usaver_load_files(effect, desc, conf, out);
}
