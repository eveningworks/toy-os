// The navigation tree: the whole left pane, Windows 11 Explorer's shape
// (chosen from mockups, 2026-10-01) -- the places, a "This computer"
// heading, then each mounted volume as a root whose folders open under
// it. A LAZY uui_tree over the set of directories the user has expanded.
//
// One of the File Manager's units -- see fm_internal.h for what is
// where and why these share their state directly.
#include "fm_internal.h"
#include "kpath.h"
#include "lib/dirsort.h"
#include "lib/ufiletype.h"
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
static char g_tree_note[UUI_PLACES_MAX][24];

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

// The volume `path` is on: the device row with the longest mount prefix.
static int mount_of(const char *path) {
    int best = -1, best_len = -1, len = (int)strlen(path);
    for (int i = g_places.places; i < g_places.count; i++) {
        const char *m = g_places.row[i].path;
        int ml = (int)strlen(m);
        int under = ml == 1 || (strncmp(path, m, (size_t)ml) == 0 &&
                                 (ml == len || path[ml] == '/'));
        if (under && ml > best_len) { best = i; best_len = ml; }
    }
    return best;
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
        // ...on the SAME volume: collapsing System must not close Boot.
        if (under && strcmp(q, path) != 0 && mount_of(q) != mount_of(path)) under = 0;
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

// The FIRST row that is this directory -- a place before the same folder
// under its volume -- or none: a folder off the tree is not its parent.
void tree_select_path(const char *path) {
    int i = tree_find_path(path);
    if (i >= 0) uui_tree_select_id(&g_tree, i);
    else g_tree.selected = -1;
}

// Another volume's mount point is that volume's root, never a folder of
// its parent's: /boot is Boot, not a "boot" under System.
static int is_other_mount(const char *path) {
    for (int i = g_places.places; i < g_places.count; i++)
        if (strcmp(g_places.row[i].path, path) == 0) return 1;
    return 0;
}

// A volume's note and meter, from its place row.
static void head_device(struct uui_tree_node *nd, const struct uui_place *r, int i) {
    uui_places_short_note(r, g_tree_note[i], sizeof g_tree_note[i]);
    nd->note = g_tree_note[i][0] ? g_tree_note[i] : 0;
    nd->meter_on = r->total != 0;
    nd->meter_pm = r->total ? (int)((r->used > r->total ? r->total : r->used)
                                    * 1000 / r->total) : 0;
    nd->meter_color = uui_places_bar_colour(r);
}

// The fixed head of the node array. Labels and icons point into
// g_places, which outlives the widget; paths are copied so every node
// answers the same way. Volumes are lazy roots only with the folder
// tree on -- off, they are plain rows and nothing opens.
static int tree_build_head(void) {
    int n = 0;
    for (int i = 0; i < g_places.count && n < TREE_MAX - 1; i++) {
        const struct uui_place *r = &g_places.row[i];
        if (i == g_places.places) {
            g_tree_path[n][0] = '\0';
            g_tree_nodes[n] = (struct uui_tree_node){ .label = "This computer",
                                                       .kind = UUI_TREE_HEADER };
            n++;
        }
        strlcpy(g_tree_path[n], r->path, PATH_MAX_LEN);
        struct uui_tree_node *nd = &g_tree_nodes[n];
        *nd = (struct uui_tree_node){ .label = r->label, .icon = r->icon };
        if (r->device) {
            nd->kind = !g_tree_on ? UUI_TREE_AUTO
                     : tree_is_open(r->path) ? UUI_TREE_OPEN : UUI_TREE_CLOSED;
            head_device(nd, r, i);
        }
        n++;
    }
    return n;
}

// Open every ANCESTOR of `path` (not the node itself), rebuild, select.
// Called on a NAVIGATION and when the panel is shown, never on a node's
// toggle, which is what keeps it from fighting a branch the user
// collapsed while standing in it: the
// collapse stays until the next directory change (Dolphin's folder
// panel behaves this way; Explorer's "expand to current folder" is the
// same, as an option).
void tree_reveal_path(const char *path) {
    if (!path) return;
    if (path[0] != '/') {   // a virtual folder: a place row, no ancestors
        tree_rebuild();
        tree_select_path(path);
        return;
    }
    char prefix[PATH_MAX_LEN];
    int len = (int)strlen(path);
    for (int i = 1; i < len && i < PATH_MAX_LEN; i++) {
        if (path[i] != '/') continue;
        memcpy(prefix, path, (size_t)i);
        prefix[i] = '\0';
        tree_set_open(prefix, 1);
    }
    // ...and the VOLUME it is on, unless it IS that volume's root.
    int vol = mount_of(path);
    if (vol >= 0 && strcmp(g_places.row[vol].path, path) != 0)
        tree_set_open(g_places.row[vol].path, 1);
    tree_rebuild();
    tree_select_path(path);
}

// THE SUM OF THE OPEN FOLDERS' CHANGE COUNTERS. Each only ever grows,
// so the sum moves whenever any one of them does -- one number answers
// "did anything the tree shows change" without keeping a list.
static long long g_tree_gen;

static long long tree_generation(void) {
    long long sum = 0;
    for (int i = 0; i < g_tree_count; i++)
        if (g_tree_nodes[i].kind == UUI_TREE_OPEN)
            sum += sys_fs_generation_of(g_tree_path[i]);
    return sum;
}

int tree_poll(void) {
    if (tree_generation() == g_tree_gen) return 0;
    tree_rebuild();
    return 1;
}

// Free space moves on every write; only the volumes' rows say it, so
// they are updated in place rather than by re-listing the open folders.
void tree_refresh_meters(void) {
    for (int i = 0; i < g_tree_count; i++)
        for (int j = g_places.places; j < g_places.count; j++)
            if (g_places.row[j].device && !strcmp(g_places.row[j].path, g_tree_path[i]) &&
                g_tree_nodes[i].label == g_places.row[j].label) {
                head_device(&g_tree_nodes[i], &g_places.row[j], j);
                break;
            }
    uui_tree_set_nodes_keep(&g_tree, g_tree_nodes, g_tree_count);
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

    g_tree_count = tree_build_head();   // the places, the heading, the volumes

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
            // Options' "hidden files", as the panes obey it -- or a bin
            // (/home/.Trash) shows up in the tree.
            if (!g_opt.hidden && g_tree_scratch[j].name[0] == '.') continue;
            if (!k_path_join(g_tree_path[i], g_tree_scratch[j].name, probe,
                              sizeof probe)) continue;
            if (is_other_mount(probe)) continue;
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
            g_tree_nodes[i + 1 + j] = (struct uui_tree_node){
                .depth = g_tree_nodes[i].depth + 1,
                .kind = tree_is_open(dst) ? UUI_TREE_OPEN : UUI_TREE_CLOSED,
            };
        }
        g_tree_count += nd;
    }

    // Labels point INTO g_tree_path (static, so they outlive the
    // widget); ids are slots, valid until the next rebuild.
    for (int i = 0; i < g_tree_count; i++) {
        g_tree_nodes[i].id = i;
        // The head's rows already have labels; a volume's folders are
        // INSERTED among them, so this goes by label, not by index.
        if (g_tree_nodes[i].label) continue;
        g_tree_nodes[i].label = k_path_basename(g_tree_path[i]);
        g_tree_nodes[i].icon = ufiletype_icon(g_tree_nodes[i].label, 1);
    }
    uui_tree_set_nodes_keep(&g_tree, g_tree_nodes, g_tree_count);
    if (sel[0]) tree_select_path(sel);
    g_tree_gen = tree_generation();
}

void tree_toggle(void *ctx, int id, int expand) {
    (void)ctx;
    if (id < 0 || id >= g_tree_count) return;
    tree_set_open(g_tree_path[id], expand);
    tree_rebuild();
}
