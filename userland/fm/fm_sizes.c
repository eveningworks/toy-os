// Folder sizes (View > Folder sizes): the bytes under each folder of the
// active pane, counted by a worker with lib/uwalk.h and handed to
// uui_fileview's dirsize hook as they arrive. Explorer never shows them
// (a walk per folder is too slow for a default); Dolphin and macOS's
// "Calculate all sizes" offer them as an option, which is this.
//
// One folder is counted at a time, in listing order, and each is posted
// when it is done; a navigation or a Refresh starts over. Counts are not
// kept across navigations -- a stale size is worse than a "...".
#include "fm_internal.h"
#include <string.h>
#include <pthread.h>
#include "kpath.h"
#include "lib/uwalk.h"
#include "lib/uconf.h"
#include "ui/ulog.h"

#define SIZES_MAX PANE_FILES

struct counted {
    char path[PATH_MAX_LEN];
    long long bytes;            // -1 until counted
    int logged;                 // the main thread has reported it
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct counted g_dirs[SIZES_MAX];
static int g_ndirs;
static char g_dir[PATH_MAX_LEN];          // the folder whose folders these are
static volatile int g_running, g_cancel;
static pthread_t g_thread;
static struct uwalk g_walk;               // static: several KB
static struct sys_dirent g_page[64];
static unsigned long long g_sum;
int g_sizes_on;

static int add_file(void *ctx, const char *path, const struct sys_dirent *e) {
    (void)ctx; (void)path;
    if (g_cancel) return -1;
    if (!e->is_dir) g_sum += e->size;
    return 1;
}

static void *worker(void *arg) {
    (void)arg;
    // The folders first, all of them, so the rows show "..." at once.
    for (int start = 0; !g_cancel; ) {
        int n = sys_listdir_at(g_dir, g_page, 64, start);
        if (n <= 0) break;
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < n && g_ndirs < SIZES_MAX; i++) {
            if (!g_page[i].is_dir) continue;
            if (k_path_join(g_dir, g_page[i].name, g_dirs[g_ndirs].path, PATH_MAX_LEN)) {
                g_dirs[g_ndirs].bytes = -1;
                g_dirs[g_ndirs].logged = 0;
                g_ndirs++;
            }
        }
        pthread_mutex_unlock(&g_lock);
        start += n;
        if (n < 64) break;
    }
    for (int i = 0; i < g_ndirs && !g_cancel; i++) {
        g_sum = 0;
        if (uwalk_begin(&g_walk, g_dirs[i].path))
            while (!g_cancel && uwalk_step(&g_walk, add_file, 0)) {}
        if (g_cancel) break;
        pthread_mutex_lock(&g_lock);
        g_dirs[i].bytes = (long long)g_sum;
        pthread_mutex_unlock(&g_lock);
        uapp_post(g_app, POST_SIZES, 0);
    }
    g_running = 0;
    return 0;
}

static void stop(void) {
    if (!g_running) return;
    g_cancel = 1;
    pthread_join(g_thread, 0);
    g_running = 0;
}

static long long dirsize(void *ctx, const char *path) {
    (void)ctx;
    long long b = -1;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_ndirs; i++)
        if (!strcmp(g_dirs[i].path, path)) { b = g_dirs[i].bytes; break; }
    pthread_mutex_unlock(&g_lock);
    return b;
}

// Count the folders of `dir`, from scratch -- unless they already are.
void sizes_follow(const char *dir, int again) {
    if (!g_sizes_on || dir[0] != '/') return;
    if (!again && !strcmp(dir, g_dir)) return;
    stop();
    pthread_mutex_lock(&g_lock);
    g_ndirs = 0;
    strlcpy(g_dir, dir, sizeof g_dir);
    pthread_mutex_unlock(&g_lock);
    g_cancel = 0;
    g_running = 1;
    if (pthread_create(&g_thread, 0, worker, 0) != 0) g_running = 0;
}

// The option, on or off, for both panes; saved like the rest of View.
void sizes_set(int on) {
    g_sizes_on = on;
    for (int i = 0; i < 2; i++)
        uui_fileview_set_dirsize(&g_pane[i], on ? dirsize : 0, 0);
    if (!on) {
        stop();
        g_dir[0] = '\0';
    } else {
        sizes_follow(uui_fileview_dir(active()), 1);
    }
    uconf_set(FILES_CONF, "folder_sizes", on ? "1" : "0");
}

// A count arrived: the panes re-total and re-sort.
void sizes_posted(void) {
    for (int i = 0; i < 2; i++) uui_fileview_sizes_changed(&g_pane[i]);
    // One line per folder as its count lands -- here, on the main
    // thread, since the log is not the worker's to write.
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_ndirs; i++)
        if (g_dirs[i].bytes >= 0 && !g_dirs[i].logged) {
            g_dirs[i].logged = 1;
            ulogf("files: size %lld %s\n", g_dirs[i].bytes, g_dirs[i].path);
        }
    pthread_mutex_unlock(&g_lock);
}
