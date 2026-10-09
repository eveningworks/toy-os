// See ufileinfo.h.
#include "lib/ufileinfo.h"
#include "lib/uvid.h"
#include <stdio.h>
#include <string.h>
#include "kpath.h"
#include "lib/human.h"
#include "lib/ufiletype.h"
#include "lib/ufile.h"
#include "lib/uimg.h"
#include "lib/utags.h"
#include "lib/uopen.h"
#include "query_abi.h"

#define HEAD_BYTES (64 * 1024)   // a JPEG's SOF, a PNG's IHDR and a QOI header all sit well inside it

// --- the folder walk: a queue, not recursion (one listing buffer) ------------

#define WALK_QUEUE 64
static char g_queue[WALK_QUEUE][UFI_PATH];
static int g_qhead, g_qtail, g_qcount;
static struct sys_dirent g_ents[SYS_LISTDIR_MAX];

static void push(struct ufileinfo *fi, const char *path) {
    if (g_qcount >= WALK_QUEUE) { fi->overflow = 1; return; }
    strlcpy(g_queue[g_qtail], path, UFI_PATH);
    g_qtail = (g_qtail + 1) % WALK_QUEUE;
    g_qcount++;
}

int ufileinfo_walk(struct ufileinfo *fi, int steps) {
    for (; steps > 0 && g_qcount > 0; steps--) {
        char dir[UFI_PATH];
        strlcpy(dir, g_queue[g_qhead], UFI_PATH);
        g_qhead = (g_qhead + 1) % WALK_QUEUE;
        g_qcount--;
        // A directory gone mid-walk is left out silently: the rest of
        // the total is still right.
        int n = sys_listdir(dir, g_ents, SYS_LISTDIR_MAX);
        for (int i = 0; i < n; i++) {
            char child[UFI_PATH];
            if (!k_path_join(dir, g_ents[i].name, child, sizeof child)) { fi->overflow = 1; continue; }
            if (g_ents[i].is_dir) { fi->dirs++; push(fi, child); }
            else { fi->files++; fi->bytes += g_ents[i].size; }
        }
        if (n == SYS_LISTDIR_MAX) fi->overflow = 1;   // a full listing may have left some out
    }
    fi->walking = g_qcount > 0;
    return fi->walking;
}

// --- the facts ---------------------------------------------------------------

// The mount holding `path`: the longest mount point that prefixes it.
static void volume(struct ufileinfo *fi) {
    struct query_fsinfo q;
    int best = -1;
    fi->vol_point[0] = fi->vol_fs[0] = 0;
    QUERY_FOREACH(QUERY_FSINFO, q, i) {
        if (!(q.flags & QUERY_FS_MOUNTED)) continue;
        int n = (int)strlen(q.point);
        int under = !strncmp(fi->path, q.point, (size_t)n) &&
                    (n == 1 || fi->path[n] == '/' || fi->path[n] == 0);
        if (!under || n <= best) continue;
        best = n;
        strlcpy(fi->vol_point, q.point, sizeof fi->vol_point);
        strlcpy(fi->vol_fs, q.name, sizeof fi->vol_fs);
        fi->vol_total = q.total_bytes;
        fi->vol_used = q.used_bytes;
    }
}

static void headers(struct ufileinfo *fi) {
    if (fi->icon && !strcmp(fi->icon, "file-video")) {
        struct uvid_info in;
        if (uvid_load_info(fi->path, &in) != 0) return;
        fi->vid_w = in.w;
        fi->vid_h = in.h;
        fi->length_ms = in.ms;
        if (in.fps_num && in.fps_den) fi->vid_fps100 = (uint32_t)(100ull * in.fps_num / in.fps_den);
        strlcpy(fi->vid_detail, in.detail, sizeof fi->vid_detail);
        strlcpy(fi->vid_audio, in.audio, sizeof fi->vid_audio);
        return;
    }
    static uint8_t head[HEAD_BYTES];
    size_t n = ufile_read_head(fi->path, head, sizeof head);
    struct uimg_info info;
    if (n && uimg_info(head, n, &info) == 0) {
        fi->img_w = info.w;
        fi->img_h = info.h;
        strlcpy(fi->img_format, info.format ? info.format : "", sizeof fi->img_format);
        strlcpy(fi->img_detail, info.detail, sizeof fi->img_detail);
        return;
    }
    struct utags t;
    memset(&t, 0, sizeof t);
    if (utags_read(fi->path, &t, 0) > 0) {
        fi->has_tags = 1;
        strlcpy(fi->title, t.title, sizeof fi->title);
        strlcpy(fi->artist, t.artist, sizeof fi->artist);
        strlcpy(fi->album, t.album, sizeof fi->album);
        fi->length_ms = t.length_ms;
    }
    utags_free(&t);
}

int ufileinfo_load(struct ufileinfo *fi, const char *path, unsigned what) {
    memset(fi, 0, sizeof *fi);
    strlcpy(fi->path, path, sizeof fi->path);
    strlcpy(fi->name, (path[0] == '/' && !path[1]) ? "/" : k_path_basename(path), sizeof fi->name);
    if (!k_path_dirname(path, fi->dir, sizeof fi->dir)) fi->dir[0] = 0;
    fi->ok = sys_stat(path, &fi->st) == 0;
    int is_dir = fi->ok && fi->st.is_dir;
    fi->type = ufiletype_name(fi->name, is_dir);
    fi->icon = ufiletype_icon(fi->name, is_dir);
    volume(fi);
    if (!fi->ok) return 0;
    if (!is_dir) {
        if (what & UFI_OPENS) {
            struct uopen_app apps[8];
            int cur;
            int n = uopen_apps_for(path, apps, 8, &cur);
            if (n > 0 && cur >= 0) strlcpy(fi->opens, apps[cur].name, sizeof fi->opens);
        }
        // Only what the extension calls a picture or a sound is read: the
        // details pane asks on every selection, and a binary's 64 KiB and
        // a tag scan per keypress made scrolling /bin paint half frames.
        int media = fi->icon && (!strcmp(fi->icon, "file-image") || !strcmp(fi->icon, "file-audio") ||
                                 !strcmp(fi->icon, "file-video"));
        if ((what & UFI_HEADERS) && media) headers(fi);
    } else if (what & UFI_WALK) {
        g_qhead = g_qtail = g_qcount = 0;
        push(fi, path);
        fi->walking = 1;
    }
    return 1;
}

void ufileinfo_size_text(const struct ufileinfo *fi, char *out, int cap) {
    unsigned long long n = fi->st.is_dir ? fi->bytes : fi->st.size;
    char h[24], b[32];
    human_size(h, sizeof h, n);
    // Thousands grouped, as Explorer writes the exact figure.
    char raw[24];
    int len = snprintf(raw, sizeof raw, "%llu", n), k = 0;
    for (int i = 0; i < len; i++) {
        if (i && (len - i) % 3 == 0) b[k++] = ',';
        b[k++] = raw[i];
    }
    b[k] = 0;
    snprintf(out, (size_t)cap, "%s%s (%s byte%s)", fi->overflow ? "at least " : "", h, b, n == 1 ? "" : "s");
}

int ufileinfo_rename(struct ufileinfo *fi, const char *newname, const char **why) {
    if (!*newname || strchr(newname, '/')) { *why = "a name may not be empty or contain /"; return -1; }
    if (!strcmp(newname, fi->name)) return 0;
    char to[UFI_PATH];
    if (!k_path_join(fi->dir, newname, to, sizeof to)) { *why = "that name is too long"; return -1; }
    struct sys_stat st;
    if (sys_stat(to, &st) == 0) { *why = "something with that name is already there"; return -1; }
    if (sys_rename(fi->path, to) < 0) { *why = sys_strerror(sys_errno()); return -1; }
    char keep[UFI_PATH];
    strlcpy(keep, to, sizeof keep);
    int walk = fi->st.is_dir;
    struct ufileinfo was = *fi;
    ufileinfo_load(fi, keep, UFI_HEADERS | UFI_OPENS);
    // A folder's count is still true under its new name.
    if (walk) { fi->files = was.files; fi->dirs = was.dirs; fi->bytes = was.bytes;
                fi->walking = was.walking; fi->overflow = was.overflow; }
    return 0;
}

int ufileinfo_chmod(struct ufileinfo *fi, unsigned mode, const char **why) {
    if (sys_chmod(fi->path, mode & 07777) < 0) { *why = sys_strerror(sys_errno()); return -1; }
    sys_stat(fi->path, &fi->st);
    return 0;
}
