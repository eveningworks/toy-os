// See upd.h, and docs/update-design.md for the why.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#include "rt/sys.h"
#include "lib/usetting.h"
#include "update_abi.h"
#include "uhash.h"
#include "uhttp.h"
#include "update/upd.h"

#define MANIFEST_MAX (1024u * 1024u)
#define KERNEL_PATH  "/boot/boot/kernel.bin"
#define KERNEL_OLD   "/boot/boot/kernel.old"
#define GRUB_CFG     "/boot/boot/grub/grub.cfg"
#define GRUB_MODULES "/etc/grub-core.modules"
#define DEFAULT_SERVER "http://10.0.2.2:8080/dev"

static int g_log_fd = -1;

static void say(const struct upd_hooks *h, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void say(const struct upd_hooks *h, const char *fmt, ...) {
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_log_fd >= 0) {
        char stamp[16] = "";
        time_t now = time(0);
        struct tm tm;
        if (localtime_r(&now, &tm)) strftime(stamp, sizeof stamp, "%H:%M:%S ", &tm);
        write(g_log_fd, stamp, strlen(stamp));
        write(g_log_fd, line, strlen(line));
        write(g_log_fd, "\n", 1);
    }
    if (h && h->log) h->log(h->ctx, line);
}

static void progress(const struct upd_hooks *h, const struct upd_plan *p, int idx) {
    if (h && h->progress) h->progress(h->ctx, p, idx);
}

static int cancelled(const struct upd_hooks *h) {
    return h && h->cancelled && h->cancelled(h->ctx);
}

static void set_error(struct upd_plan *p, const struct upd_hooks *h, const char *msg) {
    snprintf(p->error, sizeof p->error, "%s", msg);
    say(h, "error: %s", msg);
}

// ---- small file helpers ---------------------------------------------------

static int exists(const char *path, uint64_t *size) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (size) *size = (uint64_t)st.st_size;
    return S_ISDIR(st.st_mode) ? 2 : 1;
}

// crc32 of a whole file, streamed: a library is megabytes and the heap
// need not hold it. 0 on success.
static int file_crc(const char *path, uint32_t *out) {
    const struct uhash_alg *alg = uhash_find("crc32");
    int fd = open(path, O_RDONLY);
    if (!alg || fd < 0) { if (fd >= 0) close(fd); return -1; }
    static unsigned char buf[32768];
    union uhash_ctx c;
    alg->init(&c);
    long n;
    while ((n = read(fd, buf, sizeof buf)) > 0) alg->update(&c, buf, (size_t)n);
    close(fd);
    if (n < 0) return -1;
    unsigned char d[UHASH_DIGEST_MAX];
    alg->final(&c, d);
    *out = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
    return 0;
}

static int mkdir_parents(const char *path) {
    char dir[UPD_PATH_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    for (char *s = dir + 1; *s; s++) {
        if (*s != '/') continue;
        *s = '\0';
        if (!exists(dir, 0) && mkdir(dir, 0755) != 0) return -1;
        *s = '/';
    }
    return 0;
}

static int file_contains(const char *path, const char *needle) {
    static char buf[4096];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    long n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    return strstr(buf, needle) != 0;
}

static void staged_name(const struct upd_file *f, char *out, size_t cap) {
    snprintf(out, cap, "%s%s", f->path, UPDATE_STAGED_SUFFIX);
}

// ---- /boot ----------------------------------------------------------------
//
// /boot is FAT32 and mounted read-only by policy (mount_boot_auto()).
// There is no remount: umount and mount again, as `remote.py flash` does.

static int boot_mount(int writable) {
    struct query_fsinfo fs;
    for (int i = 0; ; i++) {
        if (sys_query_record(QUERY_FSINFO, i, &fs, sizeof fs) <= 0) return -1;
        if (!(fs.flags & QUERY_FS_MOUNTED) || strcmp(fs.point, "/boot") != 0) continue;
        int ro = (fs.flags & QUERY_FS_RDONLY) != 0;
        if (ro == !writable) return 0;
        struct mount_request req;
        memset(&req, 0, sizeof req);
        snprintf(req.source, sizeof req.source, "%s", fs.device);
        snprintf(req.point, sizeof req.point, "/boot");
        if (!writable) req.flags = SYS_MNT_RDONLY;
        if (sys_umount("/boot") < 0) return -1;
        if (sys_mount(&req) == 0) return 0;
        req.flags = SYS_MNT_RDONLY;   // put it back as it was, at least
        sys_mount(&req);
        return -1;
    }
}

// Which kernel image this machine's GRUB can load. `install
// --bootloader` records the core image's modules; no record means the
// ELF, which every GRUB reads (remote.py flash makes the same call).
static unsigned kernel_variant(void) {
    return file_contains(GRUB_MODULES, "gzio") ? UPD_F_KERNEL_GZ : UPD_F_KERNEL;
}

// ---- the manifest ---------------------------------------------------------

struct growbuf { char *p; size_t len, cap; };

static int grow_sink(void *ctx, const void *data, size_t len) {
    struct growbuf *b = ctx;
    if (b->len + len + 1 > MANIFEST_MAX) return -1;
    if (b->len + len + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 65536;
        while (cap < b->len + len + 1) cap *= 2;
        char *np = realloc(b->p, cap);
        if (!np) return -1;
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->len, data, len);
    b->len += len;
    b->p[b->len] = '\0';
    return 0;
}

static void url_for(const struct upd_plan *p, const char *rest, char *out, size_t cap) {
    snprintf(out, cap, "%s%s", p->base, rest);
}

// %XX -> the byte, in place. 0, or -1 for a malformed escape (refused
// rather than passed through: it would name a different file).
static int unquote(char *s) {
    char *w = s;
    for (const char *r = s; *r; r++) {
        if (*r != '%') { *w++ = *r; continue; }
        int v = 0;
        for (int k = 1; k <= 2; k++) {
            char c = r[k];
            int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (d < 0) return -1;
            v = v * 16 + d;
        }
        if (!v) return -1;
        *w++ = (char)v;
        r += 2;
    }
    *w = '\0';
    return 0;
}

// `<crc> <size> <path> [opts]`, opts a comma list of new-only, kernel,
// kernel-gz and src=<server path>. <path> is URL-quoted -- some names
// have spaces -- and stays quoted as the request path. A line this
// parser does not understand is SKIPPED and counted, never guessed at.
static int parse_line(char *line, struct upd_file *f) {
    memset(f, 0, sizeof *f);
    char *save = 0;
    char *crc = strtok_r(line, " \t", &save);
    char *size = strtok_r(0, " \t", &save);
    char *path = strtok_r(0, " \t", &save);
    char *opts = strtok_r(0, " \t", &save);
    if (!crc || !size || !path || path[0] != '/') return -1;
    char *end;
    unsigned long c = strtoul(crc, &end, 10);
    if (*end) return -1;
    unsigned long long n = strtoull(size, &end, 10);
    if (*end) return -1;
    if (strlen(path) + sizeof UPDATE_STAGED_SUFFIX > sizeof f->path) return -1;
    f->crc = (uint32_t)c;
    f->size = n;
    snprintf(f->src, sizeof f->src, "%s", path);
    if (unquote(path) != 0) return -1;
    if (strstr(path, "/../") || !strcmp(path + strlen(path) - (strlen(path) >= 3 ? 3 : 0), "/.."))
        return -1;   // a path that climbs out is refused, not normalised
    snprintf(f->path, sizeof f->path, "%s", path);
    if (opts) {
        char *s2 = 0;
        for (char *o = strtok_r(opts, ",", &s2); o; o = strtok_r(0, ",", &s2)) {
            if (!strcmp(o, "new-only")) f->flags |= UPD_F_NEW_ONLY;
            else if (!strcmp(o, "kernel")) f->flags |= UPD_F_KERNEL;
            else if (!strcmp(o, "kernel-gz")) f->flags |= UPD_F_KERNEL_GZ;
            else if (!strncmp(o, "src=", 4) && o[4] == '/') snprintf(f->src, sizeof f->src, "%s", o + 4);
        }
    }
    return 0;
}

static int parse_manifest(struct upd_plan *p, char *text, const struct upd_hooks *h) {
    int lines = 0;
    for (char *c = text; *c; c++) if (*c == '\n') lines++;
    p->files = calloc((size_t)lines + 1, sizeof *p->files);
    if (!p->files) return -1;
    int bad = 0;
    char *save = 0;
    for (char *l = strtok_r(text, "\n", &save); l; l = strtok_r(0, "\n", &save)) {
        size_t len = strlen(l);
        if (len && l[len - 1] == '\r') l[len - 1] = '\0';
        if (l[0] == '#') {
            if (!strncmp(l, "# version ", 10)) snprintf(p->version, sizeof p->version, "%s", l + 10);
            else if (!strncmp(l, "# built ", 8)) snprintf(p->built, sizeof p->built, "%s", l + 8);
            continue;
        }
        if (!l[0]) continue;
        if (parse_line(l, &p->files[p->count]) == 0) p->count++;
        else bad++;
    }
    if (bad) say(h, "manifest: ignored %d line%s this client does not understand", bad,
                 bad == 1 ? "" : "s");
    return p->count ? 0 : -1;
}

// ---- the record of what was applied ----------------------------------------

static char *read_whole(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    struct growbuf b = { 0 };
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, fp)) > 0) {
        if (grow_sink(&b, chunk, n) != 0) { free(b.p); fclose(fp); return 0; }
    }
    fclose(fp);
    return b.p;
}

static int listed(const struct upd_plan *p, int n, const char *path) {
    for (int i = 0; i < n; i++) if (!strcmp(p->files[i].path, path)) return 1;
    return 0;
}

// What the last applied manifest listed and this one does not, appended
// as UPD_REMOVE rows. Only the managed trees: a new-only file (/etc,
// /home) is the machine's own once installed, and the kernel has its
// own two-name dance. NO RECORD, NO REMOVALS -- the first update after
// this existed only writes one.
static void find_stale(struct upd_plan *p, const struct upd_hooks *h) {
    char *old = read_whole(UPD_INSTALLED_PATH);
    if (!old) { say(h, "no record of an earlier update, so nothing is removed this time"); return; }
    int lines = 0;
    for (char *c = old; *c; c++) if (*c == '\n') lines++;
    struct upd_file *grown = realloc(p->files, ((size_t)p->count + (size_t)lines + 1) * sizeof *grown);
    if (!grown) { free(old); return; }
    p->files = grown;
    int n = p->count;
    char *save = 0;
    for (char *l = strtok_r(old, "\n", &save); l; l = strtok_r(0, "\n", &save)) {
        if (l[0] == '#' || !l[0]) continue;
        struct upd_file f;
        if (parse_line(l, &f) != 0) continue;
        if (f.flags & (UPD_F_KERNEL | UPD_F_KERNEL_GZ | UPD_F_NEW_ONLY)) continue;
        if (listed(p, n, f.path)) continue;
        uint64_t size = 0;
        if (exists(f.path, &size) != 1) continue;          // gone already, or a directory
        uint32_t crc;
        if (size != f.size || file_crc(f.path, &crc) != 0 || crc != f.crc) {
            say(h, "%s: no longer shipped, but changed on this machine -- kept", f.path);
            p->kept_edited++;
            continue;
        }
        f.change = UPD_REMOVE;
        p->files[p->count++] = f;
        p->removals++;
    }
    free(old);
    if (p->removals)
        say(h, "%d stale file%s to remove", p->removals, p->removals == 1 ? "" : "s");
}

static void write_record(struct upd_plan *p, const struct upd_hooks *h) {
    if (!p->manifest) return;
    mkdir_parents(UPD_INSTALLED_PATH);
    int fd = open(UPD_INSTALLED_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    size_t len = strlen(p->manifest);
    if (fd < 0 || write(fd, p->manifest, len) != (long)len)
        say(h, "could not record the manifest in %s -- the next update removes nothing",
            UPD_INSTALLED_PATH);
    if (fd >= 0) close(fd);
}

// ---- deciding -------------------------------------------------------------

static void compare_kernel(struct upd_plan *p, const struct upd_hooks *h) {
    unsigned want = kernel_variant();
    uint64_t have_size = 0;
    int have = exists(KERNEL_PATH, &have_size) == 1;
    uint32_t have_crc = 0;
    if (have && file_crc(KERNEL_PATH, &have_crc) != 0) have = 0;
    // EITHER variant matching means this machine runs this build.
    int current = 0;
    for (int i = 0; i < p->count; i++) {
        struct upd_file *f = &p->files[i];
        if ((f->flags & (UPD_F_KERNEL | UPD_F_KERNEL_GZ)) && have &&
            f->crc == have_crc && f->size == have_size) current = 1;
    }
    int refuse = !current && file_contains(GRUB_CFG, "set timeout=0");
    for (int i = 0; i < p->count; i++) {
        struct upd_file *f = &p->files[i];
        if (!(f->flags & (UPD_F_KERNEL | UPD_F_KERNEL_GZ))) continue;
        if (!(f->flags & want) || !have) { f->change = UPD_IGNORED; continue; }
        if (current) { f->change = UPD_SAME; continue; }
        f->change = UPD_CHANGED;
        if (refuse) {
            // The rescue entry is the undo for a kernel that will not
            // boot, and a zero timeout means nobody can pick it. And the
            // REST waits too: a userland newer than its kernel is how a
            // flashed laptop once came up with no network (remote.py).
            say(h, "kernel: cannot update -- %s has `set timeout=0`, so the previous "
                   "kernel could not be chosen if this one failed. Nothing will be installed.",
                GRUB_CFG);
            p->kernel_blocked = 1;
        }
    }
}

int upd_check(const char *base, struct upd_plan *p, const struct upd_hooks *h) {
    memset(p, 0, sizeof *p);
    snprintf(p->base, sizeof p->base, "%s", base);
    size_t bl = strlen(p->base);
    while (bl && p->base[bl - 1] == '/') p->base[--bl] = '\0';

    mkdir_parents(UPD_LOG_PATH);
    if (g_log_fd >= 0) close(g_log_fd);
    g_log_fd = open(UPD_LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC);

    say(h, "server %s", p->base);
    char url[UPD_URL_MAX + 32];
    url_for(p, "/manifest", url, sizeof url);
    struct growbuf b = { 0 };
    struct uhttp_request req;
    memset(&req, 0, sizeof req);
    req.url = url;
    req.sink = grow_sink;
    req.sink_ctx = &b;
    if (uhttp_fetch(&req) != 0 || req.status != 200 || !b.p) {
        char msg[sizeof req.err + 160];
        if (req.status && req.status != 200) {
            // The body says why (a stale build, no channel named): its
            // first line is worth more than the number.
            char why[96] = "";
            if (b.p) {
                snprintf(why, sizeof why, "%s", b.p);
                why[strcspn(why, "\r\n")] = '\0';
            }
            snprintf(msg, sizeof msg, "the server answered %d for %s%s%s", req.status, url,
                     why[0] ? ": " : "", why);
        }
        else
            snprintf(msg, sizeof msg, "could not fetch %s: %s", url,
                     req.err[0] ? req.err : "no data");
        free(b.p);
        set_error(p, h, msg);
        return -1;
    }
    p->manifest = strdup(b.p);   // parse_manifest() cuts the text up
    int rc = parse_manifest(p, b.p, h);
    free(b.p);
    if (rc != 0) { set_error(p, h, "the manifest lists no files"); return -1; }
    say(h, "manifest: %d files%s%s%s%s", p->count, p->version[0] ? ", version " : "", p->version,
        p->built[0] ? ", built " : "", p->built);
    progress(h, p, -1);

    p->staged_for_boot = exists(UPDATE_PENDING_PATH, 0) == 1;
    compare_kernel(p, h);
    for (int i = 0; i < p->count; i++) {
        if (cancelled(h)) { set_error(p, h, "cancelled"); return -1; }
        struct upd_file *f = &p->files[i];
        if (f->flags & (UPD_F_KERNEL | UPD_F_KERNEL_GZ)) {
            // decided above
        } else {
            uint64_t size = 0;
            int e = exists(f->path, &size);
            uint32_t crc;
            if (e == 2) f->change = UPD_IGNORED;           // a directory where a file should be
            else if (!e) f->change = UPD_NEW;
            else if (f->flags & UPD_F_NEW_ONLY) f->change = UPD_SAME;   // the machine's own
            else if (size != f->size) f->change = UPD_CHANGED;
            else if (file_crc(f->path, &crc) != 0 || crc != f->crc) f->change = UPD_CHANGED;
            else f->change = UPD_SAME;
        }
        if (f->change == UPD_CHANGED || f->change == UPD_NEW) {
            p->changed++;
            p->bytes += f->size;
        }
        if ((i & 31) == 31) progress(h, p, -1);
    }
    say(h, "compared %d files: %d to fetch", p->count, p->changed);
    find_stale(p, h);
    // ALREADY AT THIS MANIFEST: it becomes the record, which is how a
    // machine with none (or an older one) gets its baseline.
    if (!p->changed && !p->removals && !p->staged_for_boot && !p->kernel_blocked)
        write_record(p, h);
    progress(h, p, -1);
    return 0;
}

// ---- fetching ---------------------------------------------------------------

struct fetch_ctx {
    int fd;
    struct upd_plan *p;
    struct upd_file *f;
    int idx;
    const struct upd_hooks *h;
    const struct uhash_alg *alg;
    union uhash_ctx crc;
    uint64_t next_report;
};

static int fetch_sink(void *ctx, const void *data, size_t len) {
    struct fetch_ctx *c = ctx;
    if (cancelled(c->h)) return -1;
    if (c->f->got + len > c->f->size) return -1;   // more than the manifest promised
    const char *s = data;
    size_t left = len;
    while (left) {
        long w = write(c->fd, s, left);
        if (w <= 0) return -1;
        s += w;
        left -= (size_t)w;
    }
    c->alg->update(&c->crc, data, len);
    c->f->got += len;
    c->p->done_bytes += len;
    if (c->f->got >= c->next_report || c->f->got == c->f->size) {
        c->next_report = c->f->got + 32768;
        progress(c->h, c->p, c->idx);
    }
    return 0;
}

static int fetch_one(struct upd_plan *p, int idx, const struct upd_hooks *h) {
    struct upd_file *f = &p->files[idx];
    char tmp[UPD_PATH_MAX + 8];
    staged_name(f, tmp, sizeof tmp);
    if (mkdir_parents(f->path) != 0) { say(h, "%s: cannot create its directory", f->path); return -1; }

    struct fetch_ctx c;
    memset(&c, 0, sizeof c);
    c.fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC);
    if (c.fd < 0) { say(h, "%s: cannot write %s", f->path, tmp); return -1; }
    c.p = p; c.f = f; c.idx = idx; c.h = h;
    c.alg = uhash_find("crc32");
    c.alg->init(&c.crc);
    f->got = 0;
    f->status = UPD_FETCHING;
    progress(h, p, idx);

    char url[UPD_URL_MAX + UPD_PATH_MAX + 8];
    snprintf(url, sizeof url, "%s/files%s", p->base, f->src);
    struct uhttp_request req;
    memset(&req, 0, sizeof req);
    req.url = url;
    req.sink = fetch_sink;
    req.sink_ctx = &c;
    int rc = uhttp_fetch(&req);
    close(c.fd);

    unsigned char d[UHASH_DIGEST_MAX];
    c.alg->final(&c.crc, d);
    uint32_t got_crc = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
    const char *why = 0;
    char buf[sizeof req.err + 32];
    if (cancelled(h)) why = "cancelled";
    else if (rc != 0) { snprintf(buf, sizeof buf, "fetch failed: %s", req.err); why = buf; }
    else if (req.status != 200) { snprintf(buf, sizeof buf, "server answered %d", req.status); why = buf; }
    else if (f->got != f->size) why = "size does not match the manifest";
    else if (got_crc != f->crc) why = "crc32 does not match the manifest";
    if (why) {
        unlink(tmp);
        f->status = UPD_FAILED;
        say(h, "%s: %s", f->path, why);
        progress(h, p, idx);
        return -1;
    }
    f->status = UPD_STAGED;
    say(h, "%s: %llu bytes, crc %u ok", f->path, (unsigned long long)f->size, got_crc);
    progress(h, p, idx);
    return 0;
}

// ---- committing -------------------------------------------------------------

static int is_kernel(const struct upd_file *f) {
    return (f->flags & (UPD_F_KERNEL | UPD_F_KERNEL_GZ)) != 0;
}

static int wanted(const struct upd_file *f) {
    return f->change == UPD_CHANGED || f->change == UPD_NEW;
}

static void discard_staged(struct upd_plan *p) {
    char tmp[UPD_PATH_MAX + 8];
    for (int i = 0; i < p->count; i++) {
        struct upd_file *f = &p->files[i];
        if (!wanted(f) || (f->status != UPD_STAGED && f->status != UPD_AT_BOOT)) continue;
        staged_name(f, tmp, sizeof tmp);
        unlink(tmp);
        f->status = UPD_QUEUED;
    }
}

// FAT32 has no atomic replace, so this is delete-then-rename, in the
// order that keeps a bootable kernel reachable at every step: the
// running kernel becomes kernel.old (the GRUB rescue entry) BEFORE the
// new one takes its name.
static int install_kernel(struct upd_file *f, const struct upd_hooks *h) {
    char tmp[UPD_PATH_MAX + 8];
    staged_name(f, tmp, sizeof tmp);
    unlink(KERNEL_OLD);
    if (sys_rename(KERNEL_PATH, KERNEL_OLD) != 0) {
        say(h, "kernel: could not keep the running kernel as %s -- not installing", KERNEL_OLD);
        return -1;
    }
    if (sys_rename(tmp, KERNEL_PATH) != 0) {
        sys_rename(KERNEL_OLD, KERNEL_PATH);   // put the running one back
        say(h, "kernel: could not rename %s into place", tmp);
        return -1;
    }
    say(h, "kernel: installed; the running kernel is kept as %s", KERNEL_OLD);
    return 0;
}

static int write_pending(struct upd_plan *p, const struct upd_hooks *h) {
    mkdir_parents(UPDATE_PENDING_PATH);
    int fd = open(UPDATE_PENDING_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { say(h, "cannot write %s", UPDATE_PENDING_PATH); return -1; }
    for (int i = 0; i < p->count; i++) {
        struct upd_file *f = &p->files[i];
        if ((!wanted(f) && f->change != UPD_REMOVE) || is_kernel(f)) continue;
        char minus = UPDATE_REMOVE_PREFIX;
        if ((f->change == UPD_REMOVE && write(fd, &minus, 1) != 1) ||
            write(fd, f->path, strlen(f->path)) < 0 || write(fd, "\n", 1) != 1) {
            close(fd);
            unlink(UPDATE_PENDING_PATH);
            say(h, "cannot write %s", UPDATE_PENDING_PATH);
            return -1;
        }
    }
    close(fd);
    return 0;
}

int upd_apply(struct upd_plan *p, const struct upd_hooks *h) {
    p->done_bytes = 0;
    p->at_boot = 0;
    p->kernel_installed = 0;
    p->failed = 0;
    p->error[0] = '\0';
    if (p->staged_for_boot) {
        set_error(p, h, "an update is already staged -- restart to finish it first");
        return -1;
    }
    if (p->kernel_blocked) {
        set_error(p, h, "the kernel cannot be updated on this machine (GRUB has no menu)");
        return -1;
    }

    // A LIBRARY OR THE KERNEL MEANS THE WHOLE SET WAITS FOR THE BOOT. A
    // running process pages a library in by PATH, so replacing one under
    // it mixes old and new code; and a new kernel may change an ABI the
    // new userland already assumes. docs/update-design.md.
    int kernel = -1, libs = 0;
    for (int i = 0; i < p->count; i++) {
        struct upd_file *f = &p->files[i];
        // A stale LIBRARY waits for the boot too: something may map it.
        if (f->change == UPD_REMOVE && !strncmp(f->path, "/lib/", 5)) libs++;
        if (!wanted(f)) continue;
        f->status = UPD_QUEUED;
        f->got = 0;
        if (is_kernel(f)) kernel = i;
        else if (!strncmp(f->path, "/lib/", 5)) libs++;
    }
    int at_boot = libs || kernel >= 0;
    if (kernel >= 0 && boot_mount(1) != 0) {
        set_error(p, h, "could not mount /boot read-write for the kernel");
        return -1;
    }

    say(h, "fetching %d files", p->changed);
    for (int i = 0; i < p->count; i++) {
        if (!wanted(&p->files[i])) continue;
        if (cancelled(h) || fetch_one(p, i, h) != 0) {
            p->failed = 1;
            discard_staged(p);
            if (kernel >= 0) boot_mount(0);
            set_error(p, h, cancelled(h) ? "cancelled -- nothing was changed"
                                         : "a download failed -- nothing was changed");
            return -1;
        }
    }

    // Everything is here and verified: only renames from now on.
    if (at_boot) {
        if (write_pending(p, h) != 0) {
            discard_staged(p);
            if (kernel >= 0) boot_mount(0);
            set_error(p, h, "could not record the update for the next boot");
            return -1;
        }
        for (int i = 0; i < p->count; i++)
            if ((wanted(&p->files[i]) || p->files[i].change == UPD_REMOVE) &&
                !is_kernel(&p->files[i])) p->files[i].status = UPD_AT_BOOT;
        p->at_boot = 1;
        say(h, "staged %d file%s for the next boot (%s)", p->changed - (kernel >= 0),
            p->changed - (kernel >= 0) == 1 ? "" : "s",
            libs ? "a library is in the set" : "the kernel is in the set");
    } else {
        char tmp[UPD_PATH_MAX + 8];
        for (int i = 0; i < p->count; i++) {
            struct upd_file *f = &p->files[i];
            if (!wanted(f)) continue;
            staged_name(f, tmp, sizeof tmp);
            if (sys_rename2(tmp, f->path, RENAME2_REPLACE) == 0) {
                f->status = UPD_INSTALLED;
            } else {
                f->status = UPD_FAILED;
                p->failed = 1;
                say(h, "%s: could not rename into place (%s kept)", f->path, tmp);
            }
            progress(h, p, i);
        }
        for (int i = 0; i < p->count; i++) {
            struct upd_file *f = &p->files[i];
            if (f->change != UPD_REMOVE) continue;
            if (unlink(f->path) == 0) {
                f->status = UPD_REMOVED;
                say(h, "%s: removed -- no longer shipped", f->path);
            } else {
                f->status = UPD_FAILED;
                p->failed = 1;
                say(h, "%s: could not remove", f->path);
            }
            progress(h, p, i);
        }
    }
    if (kernel >= 0) {
        struct upd_file *f = &p->files[kernel];
        if (install_kernel(f, h) == 0) {
            f->status = UPD_INSTALLED;
            p->kernel_installed = 1;
        } else {
            // THE STAGED USERLAND GOES TOO: applied at the next boot on
            // the OLD kernel, it is the mismatch the reboot exists to
            // avoid. install_kernel() has put the running one back.
            unlink(UPDATE_PENDING_PATH);
            discard_staged(p);
            p->at_boot = 0;
            f->status = UPD_FAILED;
            p->failed = 1;
            set_error(p, h, "the kernel could not be installed -- nothing was changed");
        }
        progress(h, p, kernel);
    }
    if (!p->failed) write_record(p, h);
    sys_sync();
    if (kernel >= 0) boot_mount(0);

    say(h, "done: %d file%s %s, %d stale %s%s", p->changed, p->changed == 1 ? "" : "s",
        p->at_boot ? "staged, restart to finish" : "updated", p->removals,
        p->at_boot ? "to remove at the restart" : "removed",
        p->failed ? ", with FAILURES (see above)" : "");
    progress(h, p, -1);
    if (g_log_fd >= 0) { close(g_log_fd); g_log_fd = -1; }
    return p->failed ? -1 : 0;
}

void upd_plan_free(struct upd_plan *p) {
    free(p->files);
    free(p->manifest);
    p->manifest = 0;
    p->files = 0;
    p->count = 0;
}

// ---- the server address -------------------------------------------------------

char *upd_server_get(char *out, size_t cap) {
    if (!usetting_get(UPD_SETTING, out, cap) || !out[0]) snprintf(out, cap, "%s", DEFAULT_SERVER);
    return out;
}

int upd_recent(char out[][UPD_URL_MAX], int max) {
    FILE *fp = fopen(UPD_RECENT_PATH, "r");
    if (!fp) return 0;
    int n = 0;
    while (n < max && fgets(out[n], UPD_URL_MAX, fp)) {
        size_t len = strlen(out[n]);
        while (len && (out[n][len - 1] == '\n' || out[n][len - 1] == '\r')) out[n][--len] = '\0';
        if (len) n++;
    }
    fclose(fp);
    return n;
}

int upd_server_set(const char *url) {
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) return -1;
    int r = usetting_set(UPD_SETTING, url);
    if (r != SETTING_SAVED && r != SETTING_UNSAVED) return -1;

    static char recent[UPD_RECENT_MAX][UPD_URL_MAX];
    int n = upd_recent(recent, UPD_RECENT_MAX);
    mkdir_parents(UPD_RECENT_PATH);
    FILE *fp = fopen(UPD_RECENT_PATH, "w");
    if (!fp) return 0;
    fprintf(fp, "%s\n", url);
    int kept = 1;
    for (int i = 0; i < n && kept < UPD_RECENT_MAX; i++) {
        if (!strcmp(recent[i], url)) continue;
        fprintf(fp, "%s\n", recent[i]);
        kept++;
    }
    fclose(fp);
    return 0;
}
