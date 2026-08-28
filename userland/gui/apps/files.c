// File Manager -- two directory panes, side by side.
//
// IT IS A COMMANDER, NOT AN EXPLORER, AND THAT IS THE DESIGN DECISION
// WORTH STATING (docs/filemanager-design.md has the long form). This
// system has no clipboard and no drag-and-drop; both are their own
// roadmap milestone. Explorer's two primary verbs are copy/paste and
// drag-onto-a-window, so building that shape first would mean shipping
// a file manager whose main actions are greyed out.
//
// Norton Commander answered this in 1986 and Midnight Commander, Total
// Commander and Krusader have kept the answer: put TWO directories on
// screen and copying needs no transfer mechanism at all -- the source
// is the active pane, the destination is the other one, and F5 is copy.
// Nothing is carried, so nothing needs a carrier. The keymap is theirs
// too (F5/F6/F7/F8, Tab, Enter, Backspace), which is free familiarity.
//
// THE FILE OPERATIONS ARE CHILD PROCESSES, not loops in this window.
// F5 spawns /bin/cp and F8 spawns /bin/rm, and on_tick() reaps them
// with sys_waitpid_nohang(). There is one implementation of what
// copying means, it is testable as text at a shell prompt, and a copy
// that fails cannot take the window with it. The cost, stated rather
// than discovered: no byte-level progress, because a child reports an
// exit code and not a percentage.
//
// The panes are uui_fileview (ui/uui_fileview.h), which is where the
// listing, the ordering, ".." and descend-on-activate live -- shared
// with Image Viewer, Notepad's dialog and the WM's file picker, so this
// app contains no directory-reading code at all.
#include <stdint.h>
#include <string.h>
#include <strings.h>  // strncasecmp -- an extension is not case-sensitive
#include <stdio.h>
#include "rt/sys.h"
#include "kpath.h"
#include "lib/human.h"
#include "lib/uconf.h"
#include "etc_config.h"
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "ui/uui_fileview.h"
#include "ui/uui_tree.h"
#include "lib/dirsort.h"
#include "ui/uapp.h"
#include "ui/ulog.h"
#include "ui/utheme.h"
#include "keyboard.h"

#define WIN_W 720
#define WIN_H 440
#define PATH_MAX_LEN 64          // FS_PATH_MAX
#define PANE_FILES  SYS_LISTDIR_MAX

#define ID_LEFT  1
#define ID_RIGHT 2
#define ID_TREE  3
#define ID_MENU  4

// Each pane's directory, remembered across runs. The per-app
// `/etc/<app>.conf` convention has existed since desktop.conf and had
// exactly one user; this is the second.
#define FILES_CONF "/etc/files.conf"

// Where the desktop entries live (docs/filesystem-layout.md). Read here
// for `Handles=`, so that what opens a .txt is declared by the app that
// opens it rather than tabulated inside this one.
#define DESKTOP_ENTRY_DIR "/usr/wm/desktop"

enum {
    CMD_COPY = 1, CMD_MOVE, CMD_MKDIR, CMD_RENAME, CMD_DELETE,
    CMD_REFRESH, CMD_SWAP, CMD_EXIT,
    CMD_VIEW_DETAILS, CMD_VIEW_ICONS, CMD_VIEW_PANES, CMD_VIEW_TREE,
};

// The listings. 256 entries x 80 bytes = 20 KB per pane, which is why
// these are file-scope: a ring-3 frame is capped at 2 KiB
// (USERLAND_CFLAGS), and uui_fileview does not own its storage.
static struct sys_dirent g_left_entries[PANE_FILES];
static struct sys_dirent g_right_entries[PANE_FILES];

static struct uui_fileview g_pane[2];
static int g_active;            // 0 = left, 1 = right

// View options, all persisted in FILES_CONF. `g_single` shows only the
// ACTIVE pane (Tab still swaps which one that is, and F5/F6 still act
// toward the hidden one's directory -- the pane keeps existing, it just
// is not shown). The commander shape stays the default.
static int g_single;
static int g_tree_on;

// --- the directory tree -----------------------------------------------
//
// A LAZY uui_tree (UUI_TREE_CLOSED/OPEN): only the directories the user
// has expanded are listed at all, so the node array is a view of the
// open set, rebuilt on every toggle. The app owns which PATHS are open
// -- paths, not node indices, because a rebuild renumbers every slot.
#define TREE_MAX 96
#define TREE_OPEN_MAX 24

static struct uui_tree g_tree;
static struct uui_tree_node g_tree_nodes[TREE_MAX];
static char g_tree_path[TREE_MAX][PATH_MAX_LEN];
static int g_tree_count;
static char g_tree_open[TREE_OPEN_MAX][PATH_MAX_LEN];
static int g_tree_open_count;
static struct sys_dirent g_tree_scratch[SYS_LISTDIR_MAX];

static struct uui_menubar g_menu;
static struct uui_statusbar g_status;
static struct uui_button g_keys[5];

static char g_stat_dir[PATH_MAX_LEN + 8];
static char g_stat_items[48];
static char g_stat_note[64];

// --- the job queue ----------------------------------------------------
//
// ONE CHILD AT A TIME, over a QUEUE. Marking a set of files means an
// operation is N operations, and running them concurrently would need N
// pids, N exit codes and a way to say which one failed -- with one
// status line to say it in. Sequential is also what a person expects
// from a progress that counts "3 of 7".
//
// THE PATHS ARE SNAPSHOT WHEN THE QUEUE IS BUILT, not read from the
// marks as it runs. Marks name ROWS, every reload clears them
// (ui/uui_fileview.h says why), and this app reloads whenever
// SYS_FS_GENERATION moves -- which a copy in progress makes it do. A
// queue reading marks as it went would lose them halfway through its
// own work.
#define JOB_MAX 64

static int g_job_pid = -1;
static char g_job_what[32];
static char g_job_path[JOB_MAX][PATH_MAX_LEN];
static int g_job_isdir[JOB_MAX];
static int g_job_count, g_job_at;
static int g_job_op;
static char g_job_dest[PATH_MAX_LEN];
static int g_job_failures;

// --- live refresh -------------------------------------------------------
//
// SYS_FS_GENERATION, the desktop's idiom: one integer compare per tick
// and no disk I/O. A copy finishing in another process shows up here
// without anyone pressing anything, which is the whole reason this app
// does not have to be told when its own child is done either.
static unsigned long long g_seen_generation;

static struct uui_fileview *active(void)  { return &g_pane[g_active]; }
static struct uui_fileview *other(void)   { return &g_pane[!g_active]; }

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

static void tree_select_path(const char *path) {
    int i = tree_find_path(path);
    if (i >= 0) uui_tree_select_id(&g_tree, i);
}

// Rebuild the node array from the open set. Each open directory's
// subdirectories are INSERTED right after it and the scan continues
// forward, which reaches them in DFS (display) order with no recursion
// and ONE scratch listing -- a recursive walk needs a listing per
// level, which the 2 KiB ring-3 frame budget cannot hold.
static void tree_rebuild(void) {
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

static void tree_toggle(void *ctx, int id, int expand) {
    (void)ctx;
    if (id < 0 || id >= g_tree_count) return;
    tree_set_open(g_tree_path[id], expand);
    tree_rebuild();
}

// --- the modal ---------------------------------------------------------
//
// Its own, in this window, exactly as Notepad's dialog is: a client
// cannot open a WM-level dialog (there is no such request), and a file
// manager that deleted without asking would be the one app here that
// does something irreversible on a single keystroke.
enum modal_kind { MODAL_NONE, MODAL_CONFIRM, MODAL_PROMPT };

static enum modal_kind g_modal;
static char g_modal_title[64];
static char g_modal_body[PATH_MAX_LEN + 32];
static struct uui_textbox g_modal_field;
static int g_modal_cmd;          // what to do when it commits

static const struct uui_menu_item file_items[] = {
    UUI_MENU("Copy",           CMD_COPY,    "F5"),
    UUI_MENU("Move",           CMD_MOVE,    "F6"),
    UUI_MENU("New folder",     CMD_MKDIR,   "F7"),
    UUI_MENU("Rename",         CMD_RENAME,  "F2"),
    UUI_MENU("Delete",         CMD_DELETE,  "F8"),
    UUI_MENU_SEP,
    UUI_MENU("Exit",           CMD_EXIT,    "Alt+F4"),
};

static const struct uui_menu_item view_items[] = {
    UUI_MENU("Details",     CMD_VIEW_DETAILS, 0),
    UUI_MENU("Icons",       CMD_VIEW_ICONS,   0),
    UUI_MENU_SEP,
    UUI_MENU("Second pane", CMD_VIEW_PANES,   0),
    UUI_MENU("Folder tree", CMD_VIEW_TREE,    0),
};

static const struct uui_menu_item go_items[] = {
    UUI_MENU("Other pane",     CMD_SWAP,    "Tab"),
    UUI_MENU("Refresh",        CMD_REFRESH, "Ctrl+R"),
};

static const struct uui_menu_item menu_items[] = {
    UUI_SUBMENU("File", file_items),
    UUI_SUBMENU("View", view_items),
    UUI_SUBMENU("Go",   go_items),
};

// Details/Icons tick as a pair (the ACTIVE pane's current mode) and the
// two toggles tick when their thing is SHOWN -- so "Second pane" is
// checked in the default two-pane state, not when the option was used.
static unsigned menu_item_flags(int code) {
    switch (code) {
    case CMD_VIEW_DETAILS:
        return g_pane[g_active].mode == UUI_FILEVIEW_DETAILS ? UUI_MI_CHECKED : 0;
    case CMD_VIEW_ICONS:
        return g_pane[g_active].mode == UUI_FILEVIEW_ICONS ? UUI_MI_CHECKED : 0;
    case CMD_VIEW_PANES:
        return g_single ? 0 : UUI_MI_CHECKED;
    case CMD_VIEW_TREE:
        return g_tree_on ? UUI_MI_CHECKED : 0;
    default:
        return 0;
    }
}

// THE MENU BAR IS ROUTED, not hand-dispatched: this app has routed
// widgets, and the router runs before on_press -- hand-routing the
// popup made a click on a View item ALSO select the folder-tree row
// under it (CLAUDE.md's exact rule; the tree made it visible).
static struct uui_item g_widgets[] = {
    { &uui_menubar_ops,  &g_menu,    0, 0, ID_MENU },
    { &uui_fileview_ops, &g_pane[0], 0, 0, ID_LEFT },
    { &uui_fileview_ops, &g_pane[1], 0, 0, ID_RIGHT },
    { &uui_button_ops,   &g_keys[0], 0, 0, CMD_COPY },
    { &uui_button_ops,   &g_keys[1], 0, 0, CMD_MOVE },
    { &uui_button_ops,   &g_keys[2], 0, 0, CMD_MKDIR },
    { &uui_button_ops,   &g_keys[3], 0, 0, CMD_RENAME },
    { &uui_button_ops,   &g_keys[4], 0, 0, CMD_DELETE },
    { &uui_tree_ops,     &g_tree,    0, 0, ID_TREE },
};
#define WIDGET_PANE0 1
#define WIDGET_PANE1 2
#define WIDGET_TREE ((int)(sizeof g_widgets / sizeof g_widgets[0]) - 1)

static void set_note(const char *s) { strlcpy(g_stat_note, s, sizeof g_stat_note); }

// --- status -----------------------------------------------------------

static void refresh_status(void) {
    struct uui_fileview *fv = active();
    snprintf(g_stat_dir, sizeof g_stat_dir, "%s%s",
              uui_fileview_dir(fv), g_active ? "  [right]" : "  [left]");

    char total[24];
    human_size(total, sizeof total, uui_fileview_total_bytes(fv));
    int marks = uui_fileview_mark_count(fv);
    if (marks > 0)
        snprintf(g_stat_items, sizeof g_stat_items, "%d marked of %d, %s",
                  marks, uui_fileview_count(fv), total);
    else
        snprintf(g_stat_items, sizeof g_stat_items, "%d item%s, %s%s",
                  uui_fileview_count(fv), uui_fileview_count(fv) == 1 ? "" : "s",
                  total, uui_fileview_truncated(fv) ? " (more)" : "");
}

// --- jobs -------------------------------------------------------------
//
// sys_spawn() takes ONE whitespace-separated argument string, so a path
// containing a space cannot be passed. FS_PATH_MAX paths here are made
// by this OS's own tools and do not contain one, but that is a limit of
// the spawn ABI rather than a property of the filesystem -- worth
// knowing before someone adds a rename that can produce one.
static int spawn_job(const char *program, const char *args) {
    int pid = sys_spawn(program, args, -1);
    if (pid < 0) {
        ulogf("files: spawn %s %s FAILED\n", program, args);
        return 0;
    }
    g_job_pid = pid;
    ulogf("files: %s %s (pid %d)\n", program, args, pid);
    return 1;
}

static void job_progress(void) {
    if (g_job_count > 1)
        snprintf(g_stat_note, sizeof g_stat_note, "%s %d/%d...", g_job_what,
                  g_job_at + 1, g_job_count);
    else
        snprintf(g_stat_note, sizeof g_stat_note, "%s...", g_job_what);
}

// Starts the queue's next item, or finishes the run. Returns 1 while
// work remains.
static int start_next_job(void) {
    if (g_job_at >= g_job_count) {
        snprintf(g_stat_note, sizeof g_stat_note, "%s %s", g_job_what,
                  g_job_failures ? "FAILED" : "done");
        g_job_count = g_job_at = 0;
        return 0;
    }

    const char *src = g_job_path[g_job_at];
    int is_dir = g_job_isdir[g_job_at];
    char args[PATH_MAX_LEN * 2 + 8];
    const char *program = "/bin/cp";

    switch (g_job_op) {
    case CMD_COPY:
        // -r for a directory: cp REFUSES one without it, and asking
        // "did you mean the folder?" about something the user selected
        // and pressed Copy on is a question with one answer.
        program = "/bin/cp";
        snprintf(args, sizeof args, "%s%s %s", is_dir ? "-r " : "", src, g_job_dest);
        break;
    case CMD_MOVE: {
        // MOVE IS A RENAME, and only because there is one filesystem.
        // The day a second is mounted this has to become
        // copy-then-delete when the two differ, exactly as mv does.
        char dst[PATH_MAX_LEN];
        program = "/bin/mv";
        if (!k_path_join(g_job_dest, k_path_basename(src), dst, sizeof dst)) {
            g_job_failures++;
            g_job_at++;
            return start_next_job();
        }
        snprintf(args, sizeof args, "%s %s", src, dst);
        break;
    }
    case CMD_DELETE:
        program = "/bin/rm";
        snprintf(args, sizeof args, "%s%s", is_dir ? "-r " : "", src);
        break;
    default:
        g_job_count = g_job_at = 0;
        return 0;
    }

    if (!spawn_job(program, args)) {
        g_job_failures++;
        g_job_at++;
        return start_next_job();
    }
    job_progress();
    return 1;
}

// Fills the queue from the marks, or from the selection when nothing is
// marked -- "act on the selection when no set exists" is what every
// commander does, and it is why marking can stay optional.
static int queue_from_selection(int op, const char *what) {
    if (g_job_count > 0) { set_note("busy"); return 0; }

    struct uui_fileview *fv = active();
    g_job_count = g_job_at = g_job_failures = 0;
    g_job_op = op;
    strlcpy(g_job_what, what, sizeof g_job_what);
    strlcpy(g_job_dest, uui_fileview_dir(other()), sizeof g_job_dest);

    int marks = uui_fileview_mark_count(fv);
    if (marks > 0) {
        for (int i = 0; i < marks && g_job_count < JOB_MAX; i++) {
            if (!uui_fileview_marked_path(fv, i, g_job_path[g_job_count],
                                           PATH_MAX_LEN)) continue;
            g_job_isdir[g_job_count] = uui_fileview_marked_is_dir(fv, i);
            g_job_count++;
        }
        if (marks > JOB_MAX)
            ulogf("files: %d marked, queue holds %d\n", marks, JOB_MAX);
    } else if (uui_fileview_selected_path(fv, g_job_path[0], PATH_MAX_LEN)) {
        g_job_isdir[0] = uui_fileview_selected_is_dir(fv);
        g_job_count = 1;
    }

    if (g_job_count == 0) { set_note("nothing selected"); return 0; }
    return 1;
}

// Reaps a finished child and starts the next. Returns 1 if anything
// changed on screen.
static int poll_job(void) {
    if (g_job_pid <= 0) return 0;

    int code = 0;
    int r = sys_waitpid_nohang(g_job_pid, &code);
    if (r == 0) return 0; // still running -- the whole point of nohang

    g_job_pid = -1;
    if (r < 0 || code != 0) {
        g_job_failures++;
        ulogf("files: %s of %s failed rc=%d code=%d\n", g_job_what,
              g_job_path[g_job_at], r, code);
    }
    g_job_at++;
    start_next_job();

    // Both panes: a copy changes the destination, a move changes both.
    uui_fileview_reload(&g_pane[0]);
    uui_fileview_reload(&g_pane[1]);
    refresh_status();
    return 1;
}

// --- opening a file -----------------------------------------------------
//
// THE ASSOCIATION IS THE OTHER APP'S STATEMENT, NOT THIS ONE'S. A
// `.desktop` entry declares `Handles=.txt .md`, and this walks those
// entries looking for one that claims the extension. That is
// freedesktop's mimeapps.list shape with the MIME database left out
// (docs/filemanager-design.md says why), and it is what keeps a table of
// other applications from accumulating inside the file manager -- which
// is precisely what Explorer, Finder and every desktop moved OUT of it.
static int handles_ext(const char *list, const char *ext) {
    // A space/comma-separated list, matched whole -- ".md" must not
    // match ".mdx", which a substring search would.
    for (const char *p = list; *p;) {
        while (*p == ' ' || *p == ',') p++;
        const char *start = p;
        while (*p && *p != ' ' && *p != ',') p++;
        int n = (int)(p - start);
        if (n > 0 && (int)strlen(ext) == n && strncasecmp(start, ext, (size_t)n) == 0)
            return 1;
    }
    return 0;
}

// The program that claims `path`'s extension, written into `out`.
static int handler_for(const char *path, char *out, int cap) {
    const char *dot = strrchr(k_path_basename(path), '.');
    if (!dot || !dot[1]) return 0;

    static struct sys_dirent entries[SYS_LISTDIR_MAX];
    int n = sys_listdir(DESKTOP_ENTRY_DIR, entries, SYS_LISTDIR_MAX);
    for (int i = 0; i < n; i++) {
        if (entries[i].is_dir) continue;

        char entry[PATH_MAX_LEN];
        if (!k_path_join(DESKTOP_ENTRY_DIR, entries[i].name, entry, sizeof entry))
            continue;

        // Loaded ONCE and asked twice: etc_config_get() re-reads the
        // whole file per key, which is what made a nine-entry desktop
        // reload cost 54 whole-file reads (CLAUDE.md).
        struct etc_config_buf cfg;
        if (!uconf_load(entry, &cfg)) continue;

        char list[ETC_CONFIG_MAX / 4];
        if (!etc_config_buf_get(&cfg, "Handles", list, sizeof list)) continue;
        if (!handles_ext(list, dot)) continue;
        if (etc_config_buf_get(&cfg, "Exec", out, (uint32_t)cap)) return 1;
    }
    return 0;
}

// --- commands ---------------------------------------------------------

static void open_prompt(int cmd, const char *title, const char *initial) {
    g_modal = MODAL_PROMPT;
    g_modal_cmd = cmd;
    strlcpy(g_modal_title, title, sizeof g_modal_title);
    g_modal_body[0] = '\0';
    uui_textbox_init(&g_modal_field, initial ? initial : "");
    uui_textbox_set_active(&g_modal_field, 1);
}

static void open_confirm(int cmd, const char *title, const char *body) {
    g_modal = MODAL_CONFIRM;
    g_modal_cmd = cmd;
    strlcpy(g_modal_title, title, sizeof g_modal_title);
    strlcpy(g_modal_body, body, sizeof g_modal_body);
}

// How many files an operation would act on, and what to call them.
static int operand_count(void) {
    int marks = uui_fileview_mark_count(active());
    if (marks > 0) return marks;
    return uui_fileview_selected_name(active()) ? 1 : 0;
}

static void do_copy(void) {
    if (queue_from_selection(CMD_COPY, "Copy")) start_next_job();
}

static void do_move(void) {
    if (queue_from_selection(CMD_MOVE, "Move")) start_next_job();
}

static void do_delete(void) {
    int n = operand_count();
    if (n == 0) { set_note("nothing selected"); return; }

    char body[PATH_MAX_LEN + 48];
    if (n == 1) {
        const char *name = uui_fileview_selected_name(active());
        int marks = uui_fileview_mark_count(active());
        char one[PATH_MAX_LEN];
        if (marks == 1) {
            uui_fileview_marked_path(active(), 0, one, sizeof one);
            name = k_path_basename(one);
        }
        snprintf(body, sizeof body, "Delete %s?", name ? name : "");
    } else {
        snprintf(body, sizeof body, "Delete %d marked items?", n);
    }
    open_confirm(CMD_DELETE, "Delete", body);
}

static void commit_delete(void) {
    if (queue_from_selection(CMD_DELETE, "Delete")) start_next_job();
}

static void commit_mkdir(const char *name) {
    char path[PATH_MAX_LEN];
    ulogf("files: mkdir %s in %s\n", name, uui_fileview_dir(active()));
    if (!name[0]) return;
    if (!k_path_join(uui_fileview_dir(active()), name, path, sizeof path)) {
        set_note("path too long");
        return;
    }
    // Straight to the syscall: mkdir is one call that cannot block, so
    // spawning /bin/mkdir would buy the process isolation a long copy
    // needs and nothing else.
    if (sys_mkdir(path) < 0) {
        snprintf(g_stat_note, sizeof g_stat_note, "could not create %s", name);
    } else {
        set_note("created");
        uui_fileview_reload(active());
        uui_fileview_select_name(active(), name);
    }
    refresh_status();
}

static void commit_rename(const char *name) {
    char from[PATH_MAX_LEN], to[PATH_MAX_LEN];
    ulogf("files: rename to %s in %s\n", name, uui_fileview_dir(active()));
    if (!name[0]) return;
    if (!uui_fileview_selected_path(active(), from, sizeof from)) return;
    if (!k_path_join(uui_fileview_dir(active()), name, to, sizeof to)) {
        set_note("path too long");
        return;
    }
    if (sys_rename(from, to) < 0) {
        snprintf(g_stat_note, sizeof g_stat_note, "could not rename to %s", name);
    } else {
        set_note("renamed");
        uui_fileview_reload(active());
        uui_fileview_select_name(active(), name);
    }
    refresh_status();
}

static void do_command(struct uapp *a, int code) {
    switch (code) {
    case CMD_COPY:   do_copy(); break;
    case CMD_MOVE:   do_move(); break;
    case CMD_DELETE: do_delete(); break;
    case CMD_MKDIR:  open_prompt(CMD_MKDIR, "New folder", ""); break;
    case CMD_RENAME: {
        const char *name = uui_fileview_selected_name(active());
        if (!name) { set_note("nothing selected"); break; }
        open_prompt(CMD_RENAME, "Rename", name);
        break;
    }
    case CMD_REFRESH:
        uui_fileview_reload(&g_pane[0]);
        uui_fileview_reload(&g_pane[1]);
        set_note("refreshed");
        break;
    case CMD_SWAP:
        g_active = !g_active;
        break;
    case CMD_VIEW_DETAILS:
    case CMD_VIEW_ICONS: {
        // The ACTIVE pane's, not the window's: two panes with two modes
        // is normal in every commander that grew a thumbnail view.
        enum uui_fileview_mode m = code == CMD_VIEW_ICONS ? UUI_FILEVIEW_ICONS
                                                           : UUI_FILEVIEW_DETAILS;
        uui_fileview_set_mode(active(), m);
        uconf_set(FILES_CONF, g_active ? "right_view" : "left_view",
                   m == UUI_FILEVIEW_ICONS ? "icons" : "details");
        g_seen_generation = sys_fs_generation(); // adopt our own write
        break;
    }
    case CMD_VIEW_PANES:
        g_single = !g_single;
        uconf_set(FILES_CONF, "panes", g_single ? "1" : "2");
        g_seen_generation = sys_fs_generation();
        break;
    case CMD_VIEW_TREE:
        g_tree_on = !g_tree_on;
        if (g_tree_on) {
            tree_rebuild();
            tree_select_path(uui_fileview_dir(active()));
        }
        uconf_set(FILES_CONF, "tree", g_tree_on ? "1" : "0");
        g_seen_generation = sys_fs_generation();
        break;
    case CMD_EXIT:
        uapp_quit(a, 0);
        return;
    default:
        return;
    }
    refresh_status();
    uapp_redraw(a);
}

// --- the modal's own input --------------------------------------------

static int modal_key(struct uapp *a, int key) {
    if (g_modal == MODAL_NONE) return 0;

    if (key == 0x1B) { g_modal = MODAL_NONE; set_note("cancelled"); uapp_redraw(a); return 1; }

    if (key == '\n' || key == '\r') {
        enum modal_kind kind = g_modal;
        int cmd = g_modal_cmd;
        char text[UUI_TEXTBOX_MAX];
        strlcpy(text, uui_textbox_text(&g_modal_field), sizeof text);
        // Closed BEFORE the action runs: an action that opens another
        // modal (or logs) must not find this one still up.
        g_modal = MODAL_NONE;
        if (kind == MODAL_CONFIRM) {
            if (cmd == CMD_DELETE) commit_delete();
        } else {
            if (cmd == CMD_MKDIR) commit_mkdir(text);
            else if (cmd == CMD_RENAME) commit_rename(text);
        }
        uapp_redraw(a);
        return 1;
    }

    if (g_modal == MODAL_PROMPT && uui_textbox_key(&g_modal_field, key)) uapp_redraw(a);
    return 1; // modal: swallow everything else
}

// --- layout and drawing ------------------------------------------------

static int menubar_h(void) { int h; uui_menubar_natural_size(&g_menu, 0, &h); return h; }
static int statusbar_h(void) { int h; uui_statusbar_natural_size(&g_status, 0, &h); return h; }
static int keyrow_h(void) { return utheme_control_h() + utheme_gap(); }

// Each pane carries its OWN path above it. One shared status line
// cannot say where two panes are, and "which directory does F5 copy
// into" is a question the window has to answer without being asked.
static int panehdr_h(void) { return ugfx_char_h() + utheme_gap(); }

static void layout_all(int cw, int ch) {
    int mb = menubar_h(), sb = statusbar_h(), kr = keyrow_h();

    uui_menubar_set_geometry(&g_menu, 0, 0, cw, mb);
    uui_menubar_set_bounds(&g_menu, 0, 0, cw, ch);
    uui_statusbar_set_geometry(&g_status, 0, ch - sb, cw, sb);

    int hdr = panehdr_h();
    int panes_y = mb + hdr;
    int panes_h = ch - mb - sb - kr - hdr;
    if (panes_h < 1) panes_h = 1;

    // The tree column sits left of the panes and spans their headers
    // too -- it has no path strip of its own.
    int tx = 0;
    if (g_tree_on) {
        int tw = ugfx_char_w() * 18;
        if (tw > cw / 3) tw = cw / 3;
        uui_tree_ops.set_geometry(&g_tree, 0, mb, tw, ch - mb - sb - kr);
        tx = tw;
    }
    int pw = cw - tx;

    if (g_single) {
        // Both panes get the full rect; only the active one is SHOWN.
        // The hidden one keeps sane geometry so nothing draws from junk
        // the frame it comes back.
        uui_fileview_set_geometry(&g_pane[0], tx, panes_y, pw, panes_h);
        uui_fileview_set_geometry(&g_pane[1], tx, panes_y, pw, panes_h);
    } else {
        int half = pw / 2;
        uui_fileview_set_geometry(&g_pane[0], tx, panes_y, half, panes_h);
        uui_fileview_set_geometry(&g_pane[1], tx + half, panes_y, pw - half, panes_h);
    }

    // Visibility is decided beside the geometry: the router skips a
    // hidden item, so a hidden pane cannot be clicked either.
    g_widgets[WIDGET_PANE0].hidden = g_single && g_active != 0;
    g_widgets[WIDGET_PANE1].hidden = g_single && g_active != 1;
    g_widgets[WIDGET_TREE].hidden = !g_tree_on;

    int n = (int)(sizeof g_keys / sizeof g_keys[0]);
    int gap = utheme_gap();
    int bw = (cw - gap * (n + 1)) / n;
    if (bw < 1) bw = 1;
    for (int i = 0; i < n; i++)
        uui_button_set_geometry(&g_keys[i], gap + i * (bw + gap),
                                 ch - sb - kr + gap / 2, bw, utheme_control_h());
}

// The modal's box, centred. Derived, never constant: every size here
// comes from the font (docs/gui-guidelines.md).
static void modal_rect(int cw, int ch, int *x, int *y, int *w, int *h) {
    int pad = utheme_pad();
    int lines = (g_modal == MODAL_PROMPT) ? 3 : 3;
    *w = cw * 3 / 4;
    *h = pad * 2 + lines * (ugfx_char_h() + utheme_gap()) + utheme_control_h();
    *x = (cw - *w) / 2;
    *y = (ch - *h) / 2;
}

static void draw_modal(struct ugfx_surface *s) {
    if (g_modal == MODAL_NONE) return;

    int x, y, w, h;
    modal_rect(s->w, s->h, &x, &y, &w, &h);
    int pad = utheme_pad(), lh = ugfx_char_h() + utheme_gap();

    ugfx_fill_rect(s, x, y, w, h, UTHEME_PANEL_BG);
    ugfx_draw_rect(s, x, y, w, h, UTHEME_BORDER);
    ugfx_draw_string_clipped(s, x + pad, y + pad, w - pad * 2, g_modal_title,
                              UTHEME_TEXT, UTHEME_PANEL_BG);

    if (g_modal == MODAL_CONFIRM) {
        ugfx_draw_string_clipped(s, x + pad, y + pad + lh, w - pad * 2, g_modal_body,
                                  UTHEME_TEXT, UTHEME_PANEL_BG);
        ugfx_draw_string_clipped(s, x + pad, y + pad + lh * 2, w - pad * 2,
                                  "Enter = yes, Esc = no", UTHEME_TEXT, UTHEME_PANEL_BG);
    } else {
        uui_textbox_set_geometry(&g_modal_field, x + pad, y + pad + lh,
                                  w - pad * 2, utheme_control_h());
        uui_textbox_draw(s, &g_modal_field);
        ugfx_draw_string_clipped(s, x + pad, y + pad + lh + utheme_control_h() + utheme_gap(),
                                  w - pad * 2, "Enter = ok, Esc = cancel",
                                  UTHEME_TEXT, UTHEME_PANEL_BG);
    }
}

// The path strip above each pane. The ACTIVE one is drawn in the accent
// colour, which is the same thing the outline says and deliberately so:
// the mark that answers "which pane" should be readable at a glance and
// from the text you are already looking at.
static void draw_pane_headers(struct ugfx_surface *s) {
    int hdr = panehdr_h();
    for (int i = 0; i < 2; i++) {
        if (g_single && i != g_active) continue;
        int x, y, w, h;
        uui_fileview_ops.bounds(&g_pane[i], &x, &y, &w, &h);
        (void)h;
        int active_pane = (i == g_active);
        uint32_t bg = active_pane ? UTHEME_ACCENT : UTHEME_PANEL_BG;
        uint32_t fg = active_pane ? UTHEME_ACCENT_TEXT : UTHEME_TEXT;
        ugfx_fill_rect(s, x, y - hdr, w, hdr, bg);
        // Clipped, always: a path is longer than a half-window
        // routinely, and ugfx_draw_string() does not clip
        // (docs/gui-guidelines.md's oldest trap).
        ugfx_draw_string_clipped(s, x + utheme_gap(), y - hdr + utheme_gap() / 2,
                                  w - utheme_gap() * 2, uui_fileview_dir(&g_pane[i]),
                                  fg, bg);
    }
}

// docs/gui-guidelines.md: a GUI test asks the app where things are
// rather than re-deriving geometry in Python. The grammar is notepad's
// and imgview's, deliberately -- one parser in tools/.
static void log_layout(void) {
    int x, y, w, h;
    for (int i = 0; i < 2; i++) {
        uui_fileview_ops.bounds(&g_pane[i], &x, &y, &w, &h);
        uapp_logf_layout("files: layout pane %d %d %d %d %d\n", i, x, y, w, h);
        uapp_logf_layout("files: layout dir %d %s\n", i, uui_fileview_dir(&g_pane[i]));
        uapp_logf_layout("files: layout rows %d %d\n", i, uui_fileview_row_count(&g_pane[i]));
    }
    const char *sel = uui_fileview_selected_name(active());
    uapp_logf_layout("files: layout active %d\n", g_active);
    if (uui_menubar_is_open(&g_menu)) {
        int mx, my, mw, mh;
        if (uui_menubar_popup_rect(&g_menu, 0, &mx, &my, &mw, &mh))
            uapp_logf_layout("files: layout menu %d %d %d %d\n", mx, my, mw, mh);
    }
    uapp_logf_layout("files: layout view %d %d single %d tree %d %d\n",
          (int)g_pane[0].mode, (int)g_pane[1].mode, g_single, g_tree_on,
          g_tree_on ? g_tree_count : 0);
    if (g_tree_on) {
        uui_tree_ops.bounds(&g_tree, &x, &y, &w, &h);
        uapp_logf_layout("files: layout treebox %d %d %d %d %d %d\n", x, y, w, h,
              uui_tree_row_h(&g_tree), uui_tree_selected_id(&g_tree));
    }
    uapp_logf_layout("files: layout selected %s\n", sel ? sel : "-");
    uapp_logf_layout("files: layout modal %d\n", (int)g_modal);
    uapp_logf_layout("files: layout marked %d %d\n", uui_fileview_mark_count(&g_pane[0]),
          uui_fileview_mark_count(&g_pane[1]));
    uapp_logf_layout("files: layout job %d %d\n", g_job_at, g_job_count);
    for (int i = 0; i < (int)(sizeof g_keys / sizeof g_keys[0]); i++)
        uapp_logf_layout("files: layout key %d %d %d %d %d\n", i, g_keys[i].x, g_keys[i].y,
              g_keys[i].w, g_keys[i].h);
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    layout_all(d->surface->w, d->surface->h);
    ugfx_fill_rect(d->surface, 0, 0, d->surface->w, d->surface->h, UTHEME_PANEL_BG);
    draw_pane_headers(d->surface);
    uui_statusbar_draw(d->surface, &g_status);
    log_layout();
}

static void on_draw_over(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    // The ACTIVE pane is outlined, because with two identical panes and
    // no other mark, "which one does F5 copy FROM" is unanswerable --
    // every commander marks it, and a wrong guess here deletes the wrong
    // file.
    int x, y, w, h;
    uui_fileview_ops.bounds(active(), &x, &y, &w, &h);
    ugfx_draw_rect(d->surface, x, y, w, h, UTHEME_ACCENT);
    ugfx_draw_rect(d->surface, x + 1, y + 1, w - 2, h - 2, UTHEME_ACCENT);

    draw_modal(d->surface);
}

// --- input --------------------------------------------------------------

static void on_widget(struct uapp *a, int id, int reason) {
    // The modal owns the window: a routed widget can still be clicked
    // under it, and acting on that could open a second modal over the
    // first. The menu's parked code is TAKEN so it cannot replay later.
    if (g_modal != MODAL_NONE) {
        if (id == ID_MENU) (void)uui_menubar_take_code(&g_menu);
        return;
    }
    if (id == ID_MENU) {
        // The commit is PARKED in the widget (ui/uui_menubar.h): the
        // ops release slot can only say "changed", not which item.
        int code = uui_menubar_take_code(&g_menu);
        if (code >= 0) do_command(a, code);
        return;
    }
    if (reason != UUI_REASON_RELEASE) return;

    if (id == ID_LEFT || id == ID_RIGHT) {
        // Clicking a pane makes it the active one, which is what makes
        // "the other pane" a thing the mouse can choose.
        g_active = (id == ID_RIGHT);
        refresh_status();
        uapp_redraw(a);
        return;
    }
    if (id == ID_TREE) {
        // Navigate the ACTIVE pane there. Guarded against the selection
        // that did not move -- an expander click also releases here, and
        // re-entering the same directory would reset its selection.
        int nid = uui_tree_selected_id(&g_tree);
        if (nid >= 0 && nid < g_tree_count &&
            strcmp(g_tree_path[nid], uui_fileview_dir(active())) != 0)
            uui_fileview_set_dir(active(), g_tree_path[nid]);
        refresh_status();
        uapp_redraw(a);
        return;
    }
    do_command(a, id); // the function-key buttons carry their command as their id
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    if (modal_key(a, key)) return;

    int code = 0;
    if (uui_menubar_key(&g_menu, key, &code)) {
        if (code > 0) do_command(a, code);
        else uapp_redraw(a);
        return;
    }

    switch (key) {
    case '\t':       do_command(a, CMD_SWAP);    return;
    case KEY_F5:     do_command(a, CMD_COPY);    return;
    case KEY_F6:     do_command(a, CMD_MOVE);    return;
    case KEY_F7:     do_command(a, CMD_MKDIR);   return;
    case KEY_F2:     do_command(a, CMD_RENAME);  return;
    case KEY_F8:
    case KEY_DELETE: do_command(a, CMD_DELETE);  return;
    default: break;
    }
    if (key == 0x12 && (mods & KEY_MOD_CTRL)) { do_command(a, CMD_REFRESH); return; } // Ctrl-R

    // Everything else is the active pane's: arrows, Home/End, PageUp/
    // PageDown, Enter (descend) and Backspace (up) are all one call,
    // because uui_fileview owns what a directory listing does.
    if (uui_fileview_key(active(), key)) {
        refresh_status();
        uapp_redraw(a);
    }
}

static int on_tick(struct uapp *a) {
    (void)a;
    int changed = poll_job();

    // Not under a rubber band: a reload clears the marks the band is
    // mid-way through choosing (the desktop's desktop_drag_active() rule).
    if (uui_fileview_band_active(&g_pane[0]) ||
        uui_fileview_band_active(&g_pane[1]))
        return changed;

    unsigned long long gen = sys_fs_generation();
    if (gen != g_seen_generation) {
        g_seen_generation = gen;
        uui_fileview_reload(&g_pane[0]);
        uui_fileview_reload(&g_pane[1]);
        if (g_tree_on) tree_rebuild(); // a dir can have appeared or gone
        refresh_status();
        changed = 1;
    }
    return changed;
}

static void on_pane_open(void *ctx, const char *path) {
    (void)ctx;
    char exec[PATH_MAX_LEN];
    if (!handler_for(path, exec, sizeof exec)) {
        // Said out loud rather than doing nothing: a double click that
        // produces silence reads as a broken app.
        snprintf(g_stat_note, sizeof g_stat_note, "no app for %s",
                  k_path_basename(path));
        ulogf("files: open %s -- no handler\n", path);
        return;
    }
    // NOT a tracked job: this is a launch, not an operation on files.
    // Waiting for a text editor to exit would freeze the manager for as
    // long as someone was editing.
    if (sys_spawn(exec, path, -1) < 0) {
        snprintf(g_stat_note, sizeof g_stat_note, "could not start %s", exec);
        ulogf("files: open %s -- spawn %s FAILED\n", path, exec);
    } else {
        snprintf(g_stat_note, sizeof g_stat_note, "opened %s", k_path_basename(path));
        ulogf("files: open %s -- %s\n", path, exec);
    }
}

// `ctx` is the pane index: the callback is the widget's, so which pane
// it came from has to be carried rather than guessed from g_active,
// which the WIDGET does not know about.
static void on_pane_dir(void *ctx, const char *dir) {
    int i = (int)(intptr_t)ctx;
    // Written on every change rather than at exit, because a window
    // manager can Force Quit this process and an exit-time save is a
    // save that does not happen. Two keys, so the whole document is
    // rewritten twice per navigation -- 512 bytes, and the alternative
    // is a dirty flag that has to be right.
    uconf_set(FILES_CONF, i ? "right" : "left", dir);

    // ADOPT THE GENERATION OUR OWN WRITE JUST PRODUCED. Without this the
    // app watches the filesystem, changes it, and then reacts to itself:
    // every navigation wrote this file, the write bumped
    // SYS_FS_GENERATION, and the next tick read that as "somebody
    // changed the disk" and reloaded BOTH panes -- a second full repaint
    // half a second after the first, which is visible as a flicker on
    // every single directory change. Measured at 2 frames per
    // navigation before, 1 after.
    //
    // Every watcher needs this: it is why inotify consumers track their
    // own writes and why a settings daemon ignores the change it just
    // made. The cost is bounded and worth stating -- a write by ANOTHER
    // process landing in the same instant is adopted too and its refresh
    // is skipped, until the next change moves the counter again.
    g_seen_generation = sys_fs_generation();

    // The tree follows the ACTIVE pane, Explorer's rule -- but only to a
    // node that is already visible; navigating does not force dirs open.
    if (g_tree_on && i == g_active) tree_select_path(dir);

    refresh_status();
}

static void on_open(struct uapp *a) {
    layout_all(uapp_width(a), uapp_height(a));
    refresh_status();
}

static void on_resize(struct uapp *a, int w, int h) {
    (void)a;
    layout_all(w, h);
}

int main(int argc, char **argv) {
    // Argument first, then the remembered directory, then the root. An
    // explicit argument must win: "open the file manager HERE" is a
    // statement about this launch, not a new preference.
    char saved_left[PATH_MAX_LEN], saved_right[PATH_MAX_LEN];
    if (!uconf_get(FILES_CONF, "left", saved_left, sizeof saved_left)) saved_left[0] = '\0';
    if (!uconf_get(FILES_CONF, "right", saved_right, sizeof saved_right)) saved_right[0] = '\0';

    const char *left = (argc > 1 && argv[1][0]) ? argv[1]
                        : (saved_left[0] ? saved_left : "/");
    const char *right = (argc > 2 && argv[2][0]) ? argv[2]
                         : (saved_right[0] ? saved_right : "/");

    uui_menubar_init(&g_menu, menu_items,
                      (int)(sizeof menu_items / sizeof menu_items[0]));
    g_menu.item_flags = menu_item_flags;
    uui_statusbar_init(&g_status);
    g_status.panes[0].text = g_stat_dir;
    g_status.panes[0].chars = 0;
    g_status.panes[1].text = g_stat_items;
    g_status.panes[1].chars = 20;
    g_status.panes[2].text = g_stat_note;
    g_status.panes[2].chars = 18;
    g_status.count = 3;

    static const char *const labels[] = { "Copy", "Move", "New folder", "Rename", "Delete" };
    static const int codes[] = { CMD_COPY, CMD_MOVE, CMD_MKDIR, CMD_RENAME, CMD_DELETE };
    for (int i = 0; i < (int)(sizeof g_keys / sizeof g_keys[0]); i++)
        uui_button_init(&g_keys[i], 0, 0, 10, 10, labels[i],
                         UTHEME_BUTTON_BG, UTHEME_TEXT, codes[i]);

    for (int i = 0; i < 2; i++) {
        uui_fileview_init(&g_pane[i], 0, 0, 100, 100,
                           i ? g_right_entries : g_left_entries, PANE_FILES);
        g_pane[i].on_open = on_pane_open;
        g_pane[i].ctx = (void *)(intptr_t)i;
    }

    // The remembered view options. Unknown values fall back to the
    // defaults they misspell, deliberately -- a config file is not a
    // place to fail from.
    char opt[16];
    if (uconf_get(FILES_CONF, "panes", opt, sizeof opt))
        g_single = (opt[0] == '1');
    if (uconf_get(FILES_CONF, "tree", opt, sizeof opt))
        g_tree_on = (opt[0] == '1');
    if (uconf_get(FILES_CONF, "left_view", opt, sizeof opt) && !strcmp(opt, "icons"))
        uui_fileview_set_mode(&g_pane[0], UUI_FILEVIEW_ICONS);
    if (uconf_get(FILES_CONF, "right_view", opt, sizeof opt) && !strcmp(opt, "icons"))
        uui_fileview_set_mode(&g_pane[1], UUI_FILEVIEW_ICONS);

    uui_tree_init(&g_tree, 0, 0, 100, 100, g_tree_nodes, 0);
    uui_tree_set_on_toggle(&g_tree, tree_toggle, 0);
    strlcpy(g_tree_open[0], "/", PATH_MAX_LEN);
    g_tree_open_count = 1;

    uui_fileview_set_dir(&g_pane[0], left);
    uui_fileview_set_dir(&g_pane[1], right);
    // Hooked up AFTER the opening directories are set, so starting the
    // app with an explicit argument does not silently rewrite the
    // remembered pair -- an argument is a statement about this launch.
    for (int i = 0; i < 2; i++) g_pane[i].on_dir_changed = on_pane_dir;
    if (g_tree_on) {
        tree_rebuild();
        tree_select_path(uui_fileview_dir(active()));
    }
    set_note("F5 copy  F6 move  F7 new  F8 delete");

    // One line, once: which directories this instance opened with and
    // where they came from. It is what turned "the pane is in the wrong
    // place" from a guess into a diagnosis in one run.
    ulogf("files: start left=%s right=%s (saved %s|%s)\n", left, right,
          saved_left, saved_right);

    struct uapp_desc desc = {
        .title        = "File Manager",
        .app_id       = "files",
        .w            = WIN_W,
        .h            = WIN_H,
        .flags        = UAPP_RESIZABLE,
        .min_w        = 420,
        .min_h        = 260,
        .tick_ms      = 500,
        .widgets      = g_widgets,
        .widget_count = (int)(sizeof g_widgets / sizeof g_widgets[0]),
        .on_open      = on_open,
        .on_draw      = on_draw,
        .on_draw_over = on_draw_over,
        .on_widget    = on_widget,
        .on_key       = on_key,
        .on_tick      = on_tick,
        .on_resize    = on_resize,
    };
    return uapp_run(&desc);
}
