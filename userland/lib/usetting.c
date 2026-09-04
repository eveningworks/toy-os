// See usetting.h.
#include "lib/usetting.h"
#include "rt/sys.h"
#include "knum.h"
#include <stdio.h>
#include <string.h>

int usetting_get(const char *name, char *out, size_t cap) {
    struct setting_msg m;
    if (cap) out[0] = '\0';
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_GET;
    strlcpy(m.name, name, sizeof m.name);
    if (sys_setting(&m) != 0) return 0;
    strlcpy(out, m.value, cap);
    return 1;
}

int usetting_get_int(const char *name, int *out) {
    char v[SETTING_ABI_VALUE_MAX];
    uint32_t n;
    if (!usetting_get(name, v, sizeof v)) return 0;
    if (!k_parse_u32(v, &n) || n > 0x7fffffff) return 0;
    *out = (int)n;
    return 1;
}

int usetting_set(const char *name, const char *value) {
    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_SET;
    strlcpy(m.name, name, sizeof m.name);
    strlcpy(m.value, value, sizeof m.value);
    if (sys_setting(&m) != 0) return -1;
    return (int)m.result;
}

int usetting_find(const char *name, struct setting_msg *out) {
    struct setting_msg m;
    memset(out, 0, sizeof *out);
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_COUNT;
    if (sys_setting(&m) != 0) return -1;
    int count = (int)m.count;
    for (int i = 0; i < count; i++) {
        memset(&m, 0, sizeof m);
        m.op = SETTING_OP_INFO;
        m.index = i;
        if (sys_setting(&m) != 0) continue;
        // Qualified, because a bare name is only unique until something
        // else registers one.
        char qualified[SETTING_ABI_QUALIFIED_MAX];
        snprintf(qualified, sizeof qualified, "%s.%s", m.ns, m.name);
        if (strcmp(qualified, name) != 0) continue;
        *out = m;
        return i;
    }
    return -1;
}

int usetting_set_int(const char *name, int value) {
    char v[16];
    snprintf(v, sizeof v, "%d", value);
    return usetting_set(name, v);
}
