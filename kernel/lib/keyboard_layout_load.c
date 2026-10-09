// The kernel's half of keyboard layouts: reading /usr/share/kbs/<name>
// and remembering which layout is active. Split from keyboard_layout.c,
// which is compiled into ring 3 as well and so may not touch the
// filesystem; ring 3 reads the same files through lib/ukeymap.c.
#include "keyboard_layout.h"
#include "fs.h"
#include "string.h"

#define KB_LAYOUT_DIR "/usr/share/kbs/"

static char g_current_name[KB_LAYOUT_NAME_MAX] = "us";

// Reads and applies /usr/share/kbs/<name>. Returns 1 if the file exists and
// mapped something, 0 otherwise (the caller falls back).
static int load_from_file(const char *name) {
    char path[32];
    uint32_t dir_len = (uint32_t)k_strlen(KB_LAYOUT_DIR);
    uint32_t name_len = (uint32_t)k_strlen(name);
    if (dir_len + name_len >= sizeof(path)) return 0; // refuse rather than truncate

    k_memcpy(path, KB_LAYOUT_DIR, dir_len);
    k_memcpy(path + dir_len, name, name_len);
    path[dir_len + name_len] = '\0';

    // Into memory this file owns (fs_read_into), never the backend's
    // staging buffer: a ring-3 syscall can preempt this parse. A layout
    // with four levels and its dead keys is ~5 KB; an oversize file is
    // refused.
    static char data[16384];
    uint32_t size = fs_read_into(path, data, sizeof data);
    if (size == 0) return 0;
    return keyboard_layout_load_text(data, size);
}

int keyboard_layout_load(const char *name) {
    if (load_from_file(name)) {
        k_strlcpy(g_current_name, name, sizeof g_current_name);
        return 1;
    }

    // Not found: /usr/share/kbs/us (unless that was the request), then the
    // compiled-in table. Either way the name says what is ACTIVE.
    if (k_strcmp(name, "us") != 0 && load_from_file("us")) {
        k_strlcpy(g_current_name, "us", sizeof g_current_name);
        return 0;
    }

    keyboard_layout_use_fallback();
    k_strlcpy(g_current_name, "us", sizeof g_current_name);
    return 0;
}

const char *keyboard_layout_current(void) {
    return g_current_name;
}
