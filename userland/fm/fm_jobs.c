// File operations, as a QUEUE of spawned children.
//
// One of the File Manager's units -- see fm_internal.h for what is
// where and why these share their state directly.
#include "fm_internal.h"
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

static int g_job_pid = -1;
static char g_job_what[32];
static char g_job_path[JOB_MAX][PATH_MAX_LEN];
static int g_job_isdir[JOB_MAX];
int g_job_count, g_job_at;
static int g_job_op;
static char g_job_dest[PATH_MAX_LEN];
static int g_job_failures;

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
    struct uclip c;
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
    if (g_job_count > 0) { set_note("busy"); return; }

    struct uclip c;
    uclip_load(&c);
    int op = uclip_op(&c), n = uclip_count(&c);
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
    if (op == UCLIP_CUT) (void)uclip_clear();
    start_next_job();
}

// Reaps a finished child and starts the next. Returns 1 if anything
// changed on screen.
int poll_job(void) {
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

// How many files an operation would act on, and what to call them.
int operand_count(void) {
    int marks = uui_fileview_mark_count(active());
    if (marks > 0) return marks;
    return uui_fileview_selected_name(active()) ? 1 : 0;
}

void do_copy(void) {
    if (queue_from_selection(CMD_COPY, "Copy")) start_next_job();
}

void do_move(void) {
    if (queue_from_selection(CMD_MOVE, "Move")) start_next_job();
}

void do_delete(void) {
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

void commit_delete(void) {
    if (queue_from_selection(CMD_DELETE, "Delete")) start_next_job();
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
        uui_fileview_reload(active());
        uui_fileview_select_name(active(), name);
    }
    refresh_status();
}

void commit_rename(const char *name) {
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
