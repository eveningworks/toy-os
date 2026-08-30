// The folder tree: a LAZY uui_tree over the set of directories the
// user has expanded.
//
// One of the File Manager's units -- see fm_internal.h for what is
// where and why these share their state directly.
#include "fm_internal.h"
#include "kpath.h"
#include "lib/dirsort.h"
#include <string.h>

// --- the directory tree -----------------------------------------------
//
// A LAZY uui_tree (UUI_TREE_CLOSED/OPEN): only the directories the user
// has expanded are listed at all, so the node array is a view of the
// open set, rebuilt on every toggle. The app owns which PATHS are open
// -- paths, not node indices, because a rebuild renumbers every slot.
#define TREE_MAX 96
#define TREE_OPEN_MAX 24

struct uui_tree g_tree;
static struct uui_tree_node g_tree_nodes[TREE_MAX];
char g_tree_path[TREE_MAX][PATH_MAX_LEN];
int g_tree_count;
static char g_tree_open[TREE_OPEN_MAX][PATH_MAX_LEN];
static int g_tree_open_count;
static struct sys_dirent g_tree_scratch[SYS_LISTDIR_MAX];

// The tree's own setup: the node array it draws from, and the open set
// seeded with the root. Here rather than in main() because the storage
// is this file's and an app should not have to know its shape.
void tree_init(void) {
    uui_tree_init(&g_tree, 0, 0, 100, 100, g_tree_nodes, 0);
    uui_tree_set_on_toggle(&g_tree, tree_toggle, 0);
    strlcpy(g_tree_open[0], "/", PATH_MAX_LEN);
    g_tree_open_count = 1;
}

// The open set speaks PATHS. "/" is put in it at startup.
static int tree_is_open(const char *path) {
    for (int i = 0; i < g_tree_open_count; i++)
        if (strcmp(g_tree_open[i], path) == 0) return 1;
    return 0;
}

// Closing a directory also closes everything UNDER it: a reopened
// parent showing grandchildren the user never re-expanded would mean
// the open set held paths the tree no longer shows.
static void tree_set_open(const char *path, int open) {
    if (open) {
        if (tree_is_open(path)) return;
        if (g_tree_open_count >= TREE_OPEN_MAX) return;
        strlcpy(g_tree_open[g_tree_open_count++], path, PATH_MAX_LEN);
        return;
    }
    int len = (int)strlen(path);
    int at_root = (len == 1 && path[0] == '/');
    int kept = 0;
    for (int i = 0; i < g_tree_open_count; i++) {
        const char *q = g_tree_open[i];
        int under = strncmp(q, path, (size_t)len) == 0 &&
                     (at_root || q[len] == '/' || q[len] == '\0');
        if (under) continue;
        if (kept != i) strlcpy(g_tree_open[kept], q, PATH_MAX_LEN);
        kept++;
    }
    g_tree_open_count = kept;
}

static int tree_find_path(const char *path) {
    for (int i = 0; i < g_tree_count; i++)
        if (strcmp(g_tree_path[i], path) == 0) return i;
    return -1;
}

void tree_select_path(const char *path) {
    int i = tree_find_path(path);
    if (i >= 0) uui_tree_select_id(&g_tree, i);
}

// Rebuild the node array from the open set. Each open directory's
// subdirectories are INSERTED right after it and the scan continues
// forward, which reaches them in DFS (display) order with no recursion
// and ONE scratch listing -- a recursive walk needs a listing per
// level, which the 2 KiB ring-3 frame budget cannot hold.
void tree_rebuild(void) {
    // The selection survives by PATH: ids are slots and slots move.
    char sel[PATH_MAX_LEN];
    sel[0] = '\0';
    int id = uui_tree_selected_id(&g_tree);
    if (id >= 0 && id < g_tree_count) strlcpy(sel, g_tree_path[id], sizeof sel);

    strlcpy(g_tree_path[0], "/", PATH_MAX_LEN);
    g_tree_nodes[0].depth = 0;
    g_tree_nodes[0].kind = tree_is_open("/") ? UUI_TREE_OPEN : UUI_TREE_CLOSED;
    g_tree_count = 1;

    for (int i = 0; i < g_tree_count; i++) {
        if (g_tree_nodes[i].kind != UUI_TREE_OPEN) continue;
        int n = sys_listdir(g_tree_path[i], g_tree_scratch, SYS_LISTDIR_MAX);
        if (n < 0) continue;

        // Directories only, joinable only (a path past FS_PATH_MAX is
        // skipped, not truncated), in name order.
        int nd = 0;
        char probe[PATH_MAX_LEN];
        for (int j = 0; j < n; j++) {
            if (!g_tree_scratch[j].is_dir) continue;
            if (!k_path_join(g_tree_path[i], g_tree_scratch[j].name, probe,
                              sizeof probe)) continue;
            if (nd != j) g_tree_scratch[nd] = g_tree_scratch[j];
            nd++;
        }
        dirsort(g_tree_scratch, nd, DIRSORT_NAME, 0);
        int room = TREE_MAX - g_tree_count;
        if (nd > room) nd = room; // a full tree stops growing, silently

        if (nd <= 0) continue;
        memmove(&g_tree_nodes[i + 1 + nd], &g_tree_nodes[i + 1],
                (size_t)(g_tree_count - i - 1) * sizeof g_tree_nodes[0]);
        memmove(&g_tree_path[i + 1 + nd], &g_tree_path[i + 1],
                (size_t)(g_tree_count - i - 1) * sizeof g_tree_path[0]);
        for (int j = 0; j < nd; j++) {
            char *dst = g_tree_path[i + 1 + j];
            k_path_join(g_tree_path[i], g_tree_scratch[j].name, dst, PATH_MAX_LEN);
            g_tree_nodes[i + 1 + j].depth = g_tree_nodes[i].depth + 1;
            g_tree_nodes[i + 1 + j].kind =
                tree_is_open(dst) ? UUI_TREE_OPEN : UUI_TREE_CLOSED;
        }
        g_tree_count += nd;
    }

    // Labels point INTO g_tree_path (static, so they outlive the
    // widget); ids are slots, valid until the next rebuild.
    for (int i = 0; i < g_tree_count; i++) {
        g_tree_nodes[i].id = i;
        g_tree_nodes[i].label = i == 0 ? "/" : k_path_basename(g_tree_path[i]);
    }
    uui_tree_set_nodes_keep(&g_tree, g_tree_nodes, g_tree_count);
    if (sel[0]) tree_select_path(sel);
}

void tree_toggle(void *ctx, int id, int expand) {
    (void)ctx;
    if (id < 0 || id >= g_tree_count) return;
    tree_set_open(g_tree_path[id], expand);
    tree_rebuild();
}
