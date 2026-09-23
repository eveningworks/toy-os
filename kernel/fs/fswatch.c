// Path watches for the compositor -- see kernel/fswatch.h.
#include "fswatch.h"
#include "errno.h"
#include "win_role.h" // win_server_fswatch_fired()

static struct {
    int pid;         // 0 = free
    uint64_t hash;
    uint32_t fires;  // for a test, and nothing else
} g_w[FSWATCH_MAX];

static uint64_t fnv(const char *s, uint32_t n) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 1099511628211ull; }
    return h;
}

void fswatch_hash(const char *path, uint64_t *self, uint64_t *parent) {
    uint32_t n = 0;
    while (path[n]) n++;
    while (n > 1 && path[n - 1] == '/') n--;
    uint32_t cut = n;
    while (cut > 0 && path[cut - 1] != '/') cut--;
    // "/a" -> parent "/", "/a/b" -> parent "/a"
    uint32_t pn = cut > 1 ? cut - 1 : 1;
    if (self) *self = fnv(path, n);
    if (parent) *parent = fnv(path, pn);
}

int fswatch_add(int pid, const char *path) {
    uint64_t h;
    fswatch_hash(path, &h, 0);
    int free_slot = -1;
    for (int i = 0; i < FSWATCH_MAX; i++) {
        if (g_w[i].pid == pid && g_w[i].hash == h) return i + 1;
        if (!g_w[i].pid && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return -ENOSPC;
    g_w[free_slot].pid = pid;
    g_w[free_slot].hash = h;
    g_w[free_slot].fires = 0;
    return free_slot + 1;
}

void fswatch_note(uint64_t self, uint64_t parent) {
    for (int i = 0; i < FSWATCH_MAX; i++) {
        if (!g_w[i].pid) continue;
        if (g_w[i].hash != self && g_w[i].hash != parent) continue;
        g_w[i].fires++;
        win_server_fswatch_fired(i + 1);
    }
}

void fswatch_owner_gone(int pid) {
    for (int i = 0; i < FSWATCH_MAX; i++)
        if (g_w[i].pid == pid) g_w[i].pid = 0;
}

uint32_t fswatch_fires(int id) {
    return (id >= 1 && id <= FSWATCH_MAX) ? g_w[id - 1].fires : 0;
}
