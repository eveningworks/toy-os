// File operations, as a QUEUE of spawned children.
//
// One of the File Manager's units -- see fm_internal.h for what is
// where and why these share their state directly.
#include "fm_internal.h"
#include "lib/icon_cache.h"
#include "lib/ufiletype.h"
#include "lib/udate.h"
#include "lib/human.h"
#include "lib/ufileop.h"
#include "ui/uui_dialog.h"
#include <pthread.h>
#include "lib/uclip.h"
#include "ui/ulog.h"
#include "kpath.h"
#include <string.h>
#include <stdio.h>

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

static char g_job_what[32];
static char g_job_path[JOB_MAX][PATH_MAX_LEN];
static int g_job_isdir[JOB_MAX];
int g_job_count, g_job_at;
static int g_job_op;
static char g_job_dest[PATH_MAX_LEN];
static int g_job_failures;

// --- jobs -------------------------------------------------------------
//
// A path handed to a CHILD goes as an argv vector (sys_spawn_argv(),
// uapp_spawn()), never through sys_spawn()'s string, which splits on
// whitespace -- names with spaces exist (F2 can make one).
static int queue_from(int op, const char *what, struct uui_fileview *fv,
                      const char *dest) {
    if (g_job_count > 0) { set_note("busy"); return 0; }

    g_job_count = g_job_at = g_job_failures = 0;
    g_job_op = op;
    strlcpy(g_job_what, what, sizeof g_job_what);
    strlcpy(g_job_dest, dest, sizeof g_job_dest);

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

// The commander's verbs: the ACTIVE pane's operands into the OTHER pane.
static int queue_from_selection(int op, const char *what) {
    return queue_from(op, what, active(), uui_fileview_dir(other()));
}

static int start_job(void);   // defined with the worker, below

// The queue from a list of PATHS -- a drop whose source is another
// window, whose files are in the drag slot rather than in a pane.
static int queue_from_paths(int op, const char *what, const struct uclip *c,
                            const char *dest) {
    if (g_job_count > 0) { set_note("busy"); return 0; }
    g_job_count = g_job_at = g_job_failures = 0;
    g_job_op = op;
    strlcpy(g_job_what, what, sizeof g_job_what);
    strlcpy(g_job_dest, dest, sizeof g_job_dest);
    for (int i = 0; i < uclip_count(c) && g_job_count < JOB_MAX; i++) {
        const char *p = uclip_path(c, i);
        if (!p) break;
        strlcpy(g_job_path[g_job_count], p, PATH_MAX_LEN);
        struct sys_stat st;
        g_job_isdir[g_job_count] = sys_stat(p, &st) == 0 && st.is_dir;
        g_job_count++;
    }
    if (g_job_count == 0) { set_note("nothing to drop"); return 0; }
    return 1;
}

void do_drop_extern(const char *dest, int copy) {
    static struct uclip c;
    uclip_drag_load(&c);
    if (!queue_from_paths(copy ? CMD_COPY : CMD_MOVE, copy ? "Copy" : "Move", &c, dest))
        return;
    for (int i = 0; i < g_job_count; i++) {
        size_t n = strlen(g_job_path[i]);
        if (strncmp(g_job_dest, g_job_path[i], n) == 0 &&
            (g_job_dest[n] == '\0' || g_job_dest[n] == '/')) {
            g_job_count = 0;
            set_note("cannot move a folder into itself");
            return;
        }
    }
    start_job();
}

void do_drop(struct uui_fileview *src, const char *dest, int copy) {
    if (!queue_from(copy ? CMD_COPY : CMD_MOVE, copy ? "Copy" : "Move", src, dest))
        return;
    // A FOLDER INTO ITSELF is refused before a byte moves: the engine
    // would copy the tree into its own child until the disk filled.
    // Its own directory is refused by the target already; this is the
    // case the target cannot see -- a drop on a tree node under the
    // folder being dragged.
    for (int i = 0; i < g_job_count; i++) {
        size_t n = strlen(g_job_path[i]);
        if (strncmp(g_job_dest, g_job_path[i], n) == 0 &&
            (g_job_dest[n] == '\0' || g_job_dest[n] == '/')) {
            g_job_count = 0;
            set_note("cannot move a folder into itself");
            return;
        }
    }
    // ...and where they already are is a no-op said out loud, for the
    // one target (a tree node) that does not know the items' directory.
    if (strcmp(g_job_dest, uui_fileview_dir(src)) == 0) {
        g_job_count = 0;
        set_note("already there");
        return;
    }
    start_job();
}

// --- the clipboard ----------------------------------------------------
//
// Ctrl+C and Ctrl+X put the marked set (or the selection) on the system
// clipboard; Ctrl+V acts on it. A CUT MOVES NOTHING until the paste --
// Explorer's and Dolphin's rule, and the reason the files are still
// where they were until then.
//
// This does NOT replace F5/F6. The commander's two-pane copy needs no
// carrier at all and stays the faster gesture; the clipboard is what
// lets a copy cross a navigation, which is the thing two panes cannot
// do.
static int clip_put(int op, const char *what) {
    struct uui_fileview *fv = active();
    // STATIC: struct uclip embeds the whole 64 KiB payload, which is
    // thirty times the ring-3 frame budget (lib/uclip.h).
    static struct uclip c;
    uclip_begin(&c, op);

    int marks = uui_fileview_mark_count(fv);
    char path[PATH_MAX_LEN];
    int n = 0, refused = 0;
    if (marks > 0) {
        for (int i = 0; i < marks; i++) {
            if (!uui_fileview_marked_path(fv, i, path, sizeof path)) continue;
            if (!uclip_add(&c, path)) { refused = 1; break; }
            n++;
        }
    } else if (uui_fileview_selected_path(fv, path, sizeof path)) {
        if (uclip_add(&c, path)) n = 1; else refused = 1;
    }

    if (n == 0) { set_note("nothing selected"); return 0; }
    // A REFUSAL IS SAID OUT LOUD and puts nothing on the clipboard: a
    // partial cut set pasted is files silently left behind.
    if (refused || !uclip_commit(&c)) {
        set_note("too many to copy at once");
        ulogf("files: clipboard refused %d entries\n", n);
        return 0;
    }
    snprintf(g_stat_note, sizeof g_stat_note, "%s %d item%s", what, n,
              n == 1 ? "" : "s");
    return 1;
}

void clip_copy(void) { clip_put(UCLIP_COPY, "Copied"); }
void clip_cut(void)  { clip_put(UCLIP_CUT, "Cut"); }

// The paste. The destination is the ACTIVE pane's directory -- where you
// are -- which is both Explorer's rule and the commander's.
void clip_paste(void) {
    static struct uclip c;   // see clip_put() on why it is static
    uclip_load(&c);
    int op = uclip_op(&c), n = uclip_count(&c);
    ulogf("files: paste op=%d n=%d jobs=%d into %s\n", op, n, g_job_count,
          uui_fileview_dir(active()));
    if (g_job_count > 0) { set_note("busy"); return; }
    if (op == UCLIP_NONE || n == 0) { set_note("clipboard is empty"); return; }

    g_job_count = g_job_at = g_job_failures = 0;
    g_job_op = (op == UCLIP_CUT) ? CMD_MOVE : CMD_COPY;
    strlcpy(g_job_what, op == UCLIP_CUT ? "Move" : "Copy", sizeof g_job_what);
    strlcpy(g_job_dest, uui_fileview_dir(active()), sizeof g_job_dest);

    for (int i = 0; i < n && g_job_count < JOB_MAX; i++) {
        const char *src = uclip_path(&c, i);
        if (!src) break;
        // PASTING INTO THE DIRECTORY A FILE IS ALREADY IN would ask
        // /bin/cp to copy a file onto itself. Skipped rather than
        // refused: pasting a mixed set where one happens to be here
        // should still move the rest.
        char dir[PATH_MAX_LEN];
        k_path_dirname(src, dir, sizeof dir);
        if (strcmp(dir, g_job_dest) == 0) continue;
        strlcpy(g_job_path[g_job_count], src, PATH_MAX_LEN);
        g_job_isdir[g_job_count] = 0;   // /bin/cp -r decides; see start_next_job
        g_job_count++;
    }

    if (g_job_count == 0) { set_note("already here"); return; }

    // A CUT IS SPENT BY ITS PASTE. Explorer clears the clipboard after a
    // cut-paste for the reason that matters: the files are no longer
    // where the clipboard says they are, so a second paste would fail
    // on every one of them.
    if (op == UCLIP_CUT) { (void)uclip_clear(); refresh_dim(); }
    start_job();
}

// --- the worker -------------------------------------------------------
//
// THE OPERATION RUNS ON A THREAD, not on the event loop and not in a
// spawned child. A loop here would freeze the window (CLAUDE.md's
// long-work rule); a child cannot report progress, be cancelled, or be
// asked anything. A thread can do all three, and lib/ufileop.h is the
// engine it drives -- the same one /bin/cp runs, so the copying is
// still tested as text at a prompt.
//
// **THE WORKER TOUCHES NOTHING IN TOYKIT.** It writes into the block
// below under `g_lock` and posts; every widget, every draw and every
// decision happens on the main thread (ui/uapp.h's rule). The one thing
// it reads without the lock is `g_cancel`, which is a single int that
// only ever goes 0 -> 1.
static pthread_t g_worker;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ufileop g_engine;     // ~30 KB; the worker's alone
// g_app is fm_internal.h's now -- one app pointer, set in on_open(),
// used here for uapp_post and by files.c for uapp_spawn.

static volatile int g_running;      // a job is in flight
static volatile int g_cancel;       // the main thread asked it to stop

// Progress. Written by the worker under the lock and read by the main
// thread ON ITS TICK -- NOT posted per chunk, which at 4 KB a post
// would be thousands of events for one file. A status line does not
// need finer than the tick it is already drawn on.
static char g_cur_name[PATH_MAX_LEN];
static unsigned long long g_cur_done, g_cur_size;
static int g_cur_index;

// The conflict handshake. The worker parks here until the main thread
// answers -- see fm_conflict_answer().
static volatile int g_conflict_pending;
static volatile int g_conflict_answer = -1;
static char g_conflict_src[PATH_MAX_LEN], g_conflict_dst[PATH_MAX_LEN];
static char g_conflict_rename[PATH_MAX_LEN];

// --- the policy the worker supplies to the engine ---------------------

static int worker_progress(void *ctx, const char *path,
                            uint64_t done, uint64_t total) {
    (void)ctx;
    pthread_mutex_lock(&g_lock);
    strlcpy(g_cur_name, k_path_basename(path), sizeof g_cur_name);
    g_cur_done = done;
    g_cur_size = total;
    pthread_mutex_unlock(&g_lock);
    return !g_cancel;      // 0 cancels the whole operation
}

static void worker_error(void *ctx, const char *path, int err) {
    (void)ctx;
    // COUNTED, not shown one by one: a tree with fifty unreadable files
    // would otherwise be fifty dialogs. The count reaches the status
    // line and the detail reaches the log.
    pthread_mutex_lock(&g_lock);
    g_job_failures++;
    pthread_mutex_unlock(&g_lock);
    ulogf("files: %s: errno %d\n", path, err);
}

static int worker_conflict(void *ctx, const char *src, const char *dst,
                            char *rename_out, int cap) {
    (void)ctx;
    pthread_mutex_lock(&g_lock);
    strlcpy(g_conflict_src, src, sizeof g_conflict_src);
    strlcpy(g_conflict_dst, dst, sizeof g_conflict_dst);
    g_conflict_rename[0] = '\0';
    g_conflict_answer = -1;
    g_conflict_pending = 1;
    pthread_mutex_unlock(&g_lock);

    uapp_post(g_app, POST_CONFLICT, 0);

    // PARKED BY SLEEPING, not by pthread_cond_wait, and not by yielding.
    // This wait lasts as long as a person takes to read a dialog --
    // seconds, sometimes minutes -- and cond_wait spins on sys_yield()
    // (userland/libc/pthread.c), which would burn a scheduler slot the
    // whole time. 30 ms is invisible to a human and costs nothing.
    // `< 0` IS THE WAIT, not `!answer`. -1 is the no-answer sentinel and
    // it is TRUTHY, so `!g_conflict_answer` fell through instantly: the
    // worker never waited, read -1 as a decision, and overwrote the file
    // while the dialog was still on screen asking about it.
    while (g_conflict_answer < 0 && !g_cancel) sys_sleep_ms(30);
    if (g_cancel) return UFILEOP_CANCEL;

    pthread_mutex_lock(&g_lock);
    int d = g_conflict_answer;
    if (d == UFILEOP_RENAME) strlcpy(rename_out, g_conflict_rename, (size_t)cap);
    g_conflict_pending = 0;
    pthread_mutex_unlock(&g_lock);
    ulogf("files: conflict answered d=%d name=%s\n", d,
          d == UFILEOP_RENAME ? rename_out : "-");
    return d;
}

static void *worker_main(void *arg) {
    (void)arg;
    struct ufileop_policy policy = {
        .on_conflict = worker_conflict,
        .on_progress = worker_progress,
        .on_error = worker_error,
    };

    for (int i = 0; i < g_job_count && !g_cancel; i++) {
        pthread_mutex_lock(&g_lock);
        g_cur_index = i;
        g_job_at = i;
        strlcpy(g_cur_name, k_path_basename(g_job_path[i]), sizeof g_cur_name);
        g_cur_done = g_cur_size = 0;
        pthread_mutex_unlock(&g_lock);

        switch (g_job_op) {
        case CMD_COPY:
            ufileop_copy(g_job_path[i], g_job_dest, &g_engine, &policy);
            break;
        case CMD_MOVE:
            ufileop_move(g_job_path[i], g_job_dest, &g_engine, &policy);
            break;
        case CMD_DELETE:
            ufileop_remove(g_job_path[i], 1, &g_engine, &policy);
            break;
        default: break;
        }
    }

    g_running = 0;
    uapp_post(g_app, POST_DONE, 0);
    return 0;
}

// --- what the app calls -----------------------------------------------

int fm_job_running(void) { return g_running; }

int fm_conflict_pending(void) { return g_conflict_pending; }
const char *fm_conflict_src(void) { return g_conflict_src; }
const char *fm_conflict_dst(void) { return g_conflict_dst; }

// The main thread's answer. `rename` is a basename and only read for
// UFILEOP_RENAME.
void fm_conflict_answer(int decision, const char *rename) {
    pthread_mutex_lock(&g_lock);
    if (rename) strlcpy(g_conflict_rename, rename, sizeof g_conflict_rename);
    // LAST, and after the payload: the worker wakes on this one word,
    // so anything it will read has to be in place before it is set.
    g_conflict_answer = decision;
    pthread_mutex_unlock(&g_lock);
}

void fm_job_cancel(void) { g_cancel = 1; }

// The status line's text, built on the MAIN thread from what the worker
// last recorded.
void fm_job_status(char *out, int cap) {
    pthread_mutex_lock(&g_lock);
    char name[PATH_MAX_LEN];
    strlcpy(name, g_cur_name, sizeof name);
    unsigned long long done = g_cur_done, size = g_cur_size;
    int idx = g_cur_index, total = g_job_count;
    pthread_mutex_unlock(&g_lock);

    if (size > 0 && done < size)
        snprintf(out, (size_t)cap, "%s %d/%d  %s  %d%%", g_job_what, idx + 1,
                  total, name, (int)((done * 100) / size));
    else
        snprintf(out, (size_t)cap, "%s %d/%d  %s", g_job_what, idx + 1, total, name);
}

static int start_job(void) {
    if (g_job_count == 0) return 0;
    g_cancel = 0;
    g_running = 1;
    g_job_failures = 0;
    if (pthread_create(&g_worker, 0, worker_main, 0) != 0) {
        g_running = 0;
        set_note("could not start the operation");
        ulog("files: pthread_create for the file worker FAILED\n");
        return 0;
    }
    // DETACHED IN EFFECT: nothing joins it. The app learns it finished
    // from the post, and a join on the main thread would be the freeze
    // this whole arrangement exists to avoid.
    return 1;
}

// Called from the tick. Returns 1 if the screen should be repainted.
int poll_job(void) {
    if (!g_running) return 0;
    return 1;      // the status line moves while it runs
}

// The post arrived: the operation is over.
void fm_job_finished(void) {
    snprintf(g_stat_note, sizeof g_stat_note, "%s %s", g_job_what,
              g_cancel ? "cancelled" : (g_job_failures ? "FAILED" : "done"));
    g_job_count = g_job_at = 0;
    reload_panes();
    g_seen_generation = sys_fs_generation();   // our own writes (on_pane_dir)
    refresh_status();
}

// How many files an operation would act on, and what to call them.
int operand_count(void) {
    int marks = uui_fileview_mark_count(active());
    if (marks > 0) return marks;
    return uui_fileview_selected_name(active()) ? 1 : 0;
}

void do_copy(void) {
    if (queue_from_selection(CMD_COPY, "Copy")) start_job();
}

void do_move(void) {
    if (queue_from_selection(CMD_MOVE, "Move")) start_job();
}

// The dialog's rows are caller-owned for as long as it is up
// (ui/uui_dialog.h), so they live here rather than on the stack.
// The delete card's words, and its picture. File-scope because the dialog
// keeps pointers (ui/uui_dialog.h).
static char g_del_title[PATH_MAX_LEN + 24];
static char g_del_facts[96];
static char g_del_where[PATH_MAX_LEN + 8];
static const char *const g_del_rows[] = { g_del_facts, g_del_where };
static char g_del_path[PATH_MAX_LEN];   // the one file shown, for its thumbnail

// The picture on the card: the file's own thumbnail when one is ready,
// else its type's icon. Asked again when a thumbnail lands (files.c's
// on_user), since the first ask usually only queued the decode.
void delete_picture(void) {
    if (g_dialog_kind != DIALOG_DELETE || !uui_dialog_is_open(&g_dialog)) return;
    int px = ugfx_char_h() * 4;
    const struct uimg *pic = 0;
    if (g_del_path[0]) {
        struct sys_stat st;
        if (sys_stat(g_del_path, &st) == 0) {
            if (!st.is_dir && g_opt.thumbs) {
                struct sys_dirent e;
                memset(&e, 0, sizeof e);
                strlcpy(e.name, k_path_basename(g_del_path), sizeof e.name);
                e.size = (uint32_t)st.size;
                e.modified = st.modified;
                char dir[PATH_MAX_LEN];
                k_path_dirname(g_del_path, dir, sizeof dir);
                pic = pane_thumb(0, dir, &e, px);
            }
            if (!pic) pic = icon_get(ufiletype_icon(k_path_basename(g_del_path), st.is_dir), px);
        }
    } else {
        pic = icon_get("file", px);
    }
    if (pic != g_dialog.picture) uui_dialog_set_picture(&g_dialog, pic);
}

void do_delete(void) {
    int n = operand_count();
    if (n == 0) { set_note("nothing selected"); return; }
    // Options can say not to ask -- and then this is the whole question.
    if (!g_opt.confirm_delete) { commit_delete(); return; }

    struct uui_fileview *fv = active();
    int marks = uui_fileview_mark_count(fv);
    g_del_path[0] = '\0';
    if (n == 1) {
        if (marks == 1) uui_fileview_marked_path(fv, 0, g_del_path, sizeof g_del_path);
        else uui_fileview_selected_path(fv, g_del_path, sizeof g_del_path);
        const char *name = k_path_basename(g_del_path);
        snprintf(g_del_title, sizeof g_del_title, "Delete %s?", name);
        struct sys_stat st;
        char h[24], when[48];
        if (sys_stat(g_del_path, &st) == 0) {
            udate_format(when, sizeof when, &st.modified, UDATE_DATE | UDATE_TIME);
            if (st.is_dir) {
                snprintf(g_del_facts, sizeof g_del_facts, "Folder, modified %s -- and everything in it", when);
            } else {
                human_size(h, sizeof h, st.size);
                snprintf(g_del_facts, sizeof g_del_facts, "%s, %s, modified %s",
                         ufiletype_name(name, 0), h, when);
            }
        } else {
            g_del_facts[0] = '\0';
        }
    } else {
        unsigned long long bytes = 0;
        int dirs = 0;
        char path[PATH_MAX_LEN], h[24];
        for (int i = 0; i < marks; i++) {
            struct sys_stat st;
            if (!uui_fileview_marked_path(fv, i, path, sizeof path)) continue;
            if (uui_fileview_marked_is_dir(fv, i)) dirs++;
            else if (sys_stat(path, &st) == 0) bytes += st.size;
        }
        human_size(h, sizeof h, bytes);
        snprintf(g_del_title, sizeof g_del_title, "Delete %d items?", n);
        if (dirs)
            snprintf(g_del_facts, sizeof g_del_facts, "%d file%s (%s) and %d folder%s with what is in them",
                     n - dirs, n - dirs == 1 ? "" : "s", h, dirs, dirs == 1 ? "" : "s");
        else
            snprintf(g_del_facts, sizeof g_del_facts, "%d files, %s in all", n, h);
    }
    snprintf(g_del_where, sizeof g_del_where, "in %s", uui_fileview_dir(fv));
    // Verb buttons, the KDE/GNOME/macOS rule: the button says what it
    // does. Delete is the default, as it was when Enter meant yes, and
    // red, because it cannot be taken back.
    static const struct uui_dialog_button btns[] = {
        { "Cancel", DLG_CANCEL, 0 },
        { "Delete", DLG_DELETE, UUI_DLG_DANGER },
    };
    g_dialog_kind = DIALOG_DELETE;
    uui_dialog_open(&g_dialog, g_del_title, g_del_rows, 2, btns, 2, 1, DLG_CANCEL);
    uui_dialog_set_note(&g_dialog,
                        "It is deleted for good -- there is no Recycle Bin to get it back from.");
    delete_picture();
}

void commit_delete(void) {
    if (queue_from_selection(CMD_DELETE, "Delete")) start_job();
}

void commit_mkdir(const char *name) {
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
        reload_pane(active());
        g_seen_generation = sys_fs_generation();
        uui_fileview_select_name(active(), name);
    }
    refresh_status();
}

// An empty file, named by the prompt. Refused, not overwritten, when
// the name is taken: "New" never destroys anything.
void commit_newfile(const char *name) {
    char path[PATH_MAX_LEN];
    if (!name[0]) return;
    if (!k_path_join(uui_fileview_dir(active()), name, path, sizeof path)) {
        set_note("path too long");
        return;
    }
    struct sys_stat st;
    if (sys_stat(path, &st) == 0) {
        snprintf(g_stat_note, sizeof g_stat_note, "%s already exists", name);
        return;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(g_stat_note, sizeof g_stat_note, "could not create %s", name);
        return;
    }
    fclose(f);
    set_note("created");
    reload_pane(active());
    g_seen_generation = sys_fs_generation();
    uui_fileview_select_name(active(), name);
    refresh_status();
}

void commit_rename(const char *name) {
    const char *from = uui_fileview_selected_name(active());
    if (from) commit_rename_named(from, name);
}

void commit_rename_named(const char *from_name, const char *name) {
    char from[PATH_MAX_LEN], to[PATH_MAX_LEN];
    ulogf("files: rename %s to %s in %s\n", from_name, name, uui_fileview_dir(active()));
    if (!name[0]) return;
    if (!k_path_join(uui_fileview_dir(active()), from_name, from, sizeof from)) return;
    if (!k_path_join(uui_fileview_dir(active()), name, to, sizeof to)) {
        set_note("path too long");
        return;
    }
    if (sys_rename(from, to) < 0) {
        snprintf(g_stat_note, sizeof g_stat_note, "could not rename to %s", name);
    } else {
        set_note("renamed");
        reload_pane(active());
        g_seen_generation = sys_fs_generation();
        uui_fileview_select_name(active(), name);
    }
    refresh_status();
}
