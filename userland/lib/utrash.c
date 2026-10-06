// The Recycle Bin -- see utrash.h.
#include "lib/utrash.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <caltime.h>
#include "rt/sys.h"
#include "kpath.h"
#include "query_abi.h"

#define HOME_BIN "/home/.Trash"
#define TOP_BIN  ".Trash-0"

// The mount `path` is on: the longest mount point that is a prefix of
// it, at a component boundary. 1 with its flags, or 0.
static int volume_of(const char *path, char *point, int cap, uint64_t *flags) {
    struct query_fsinfo fs;
    int best = -1;
    for (int i = 0; ; i++) {
        int n = sys_query_record(QUERY_FSINFO, (unsigned)i, &fs, sizeof fs);
        if (n <= 0) break;
        if ((unsigned)n < sizeof fs || !(fs.flags & QUERY_FS_MOUNTED)) continue;
        int len = (int)strlen(fs.point);
        int under = !strcmp(fs.point, "/") ||
                    (!strncmp(path, fs.point, (size_t)len) && (path[len] == '/' || !path[len]));
        if (!under || len <= best) continue;
        best = len;
        strlcpy(point, fs.point, (size_t)cap);
        *flags = fs.flags;
    }
    return best >= 0;
}

int utrash_bin_for(const char *path, char *out, int cap) {
    char point[64];
    uint64_t flags = 0;
    if (!path || path[0] != '/' || !volume_of(path, point, sizeof point, &flags)) return 0;
    if (!(flags & QUERY_FS_PERSISTENT) || (flags & QUERY_FS_RDONLY)) return 0;
    if (!strcmp(point, "/")) {
        if (strlcpy(out, HOME_BIN, (size_t)cap) >= (size_t)cap) return 0;
    } else if (!k_path_join(point, TOP_BIN, out, (size_t)cap)) {
        return 0;
    }
    // A bin, or something inside one, is not binned again.
    size_t bl = strlen(out);
    if (!strncmp(path, out, bl) && (path[bl] == '/' || !path[bl])) return 0;
    return 1;
}

// --- the info file ------------------------------------------------------

static int unreserved(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '/' || c == '-' || c == '_' || c == '.' || c == '~';
}

// The info file, written in pieces: the encoded path can be three
// times the path, and a ring-3 frame is 2 KiB.
static int write_info(int fd, const char *path, const char *date) {
    static const char hex[] = "0123456789ABCDEF";
    char chunk[96];
    int n = snprintf(chunk, sizeof chunk, "[Trash Info]\nPath=");
    for (const unsigned char *p = (const unsigned char *)path; ; p++) {
        if (!*p || n > (int)sizeof chunk - 4) {
            if (write(fd, chunk, (size_t)n) != n) return 0;
            n = 0;
            if (!*p) break;
        }
        if (unreserved(*p)) {
            chunk[n++] = (char)*p;
        } else {
            chunk[n++] = '%';
            chunk[n++] = hex[*p >> 4];
            chunk[n++] = hex[*p & 15];
        }
    }
    n = snprintf(chunk, sizeof chunk, "\nDeletionDate=%s\n", date);
    return write(fd, chunk, (size_t)n) == n;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int decode(const char *in, char *out, int cap) {
    int n = 0;
    for (const char *p = in; *p; p++) {
        int c = (unsigned char)*p;
        if (c == '%') {
            int hi = hexval(p[1]), lo = hi < 0 ? -1 : hexval(p[2]);
            if (lo < 0) return 0;
            c = hi * 16 + lo;
            p += 2;
        }
        if (!c || n + 1 >= cap) return 0;
        out[n++] = (char)c;
    }
    out[n] = '\0';
    return n > 0 && out[0] == '/';
}

// DeletionDate is LOCAL time per the spec; every timestamp in toy-os is
// UTC, so it is converted both ways here and nowhere else.
static void date_local(const struct rtc_time *utc, char *out, int cap) {
    time_t e = (time_t)cal_rtc_to_epoch(utc);
    struct tm tm;
    localtime_r(&e, &tm);
    strftime(out, (size_t)cap, "%Y-%m-%dT%H:%M:%S", &tm);
}

static int date_parse(const char *s, struct rtc_time *utc) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    time_t e = mktime(&tm);
    if (e == (time_t)-1) return 0;
    cal_epoch_to_rtc((uint64_t)e, utc);
    return 1;
}

static int info_path(const char *bin, const char *name, char *out, int cap) {
    return snprintf(out, (size_t)cap, "%s/info/%s.trashinfo", bin, name) < cap;
}

int utrash_item_path(const struct utrash_item *it, char *out, int cap) {
    return snprintf(out, (size_t)cap, "%s/files/%s", it->bin, it->name) < cap;
}

static int read_info(const char *bin, const char *name, struct utrash_item *it) {
    char p[UTRASH_PATH + 80], buf[UTRASH_PATH * 3 + 128];
    if (!info_path(bin, name, p, sizeof p)) return 0;
    int fd = open(p, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    int have_path = 0, have_date = 0, header = 0;
    for (char *line = buf, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = '\0';
        size_t l = strlen(line);
        if (l && line[l - 1] == '\r') line[l - 1] = '\0';
        if (!strcmp(line, "[Trash Info]")) header = 1;
        else if (!strncmp(line, "Path=", 5))
            have_path = decode(line + 5, it->orig, sizeof it->orig);
        else if (!strncmp(line, "DeletionDate=", 13))
            have_date = date_parse(line + 13, &it->deleted);
    }
    if (!header || !have_path) return 0;
    if (!have_date) memset(&it->deleted, 0, sizeof it->deleted);
    strlcpy(it->bin, bin, sizeof it->bin);
    strlcpy(it->name, name, sizeof it->name);
    char fp[UTRASH_PATH + 80];
    struct sys_stat st;
    if (!utrash_item_path(it, fp, sizeof fp) || sys_stat(fp, &st) != 0) return 0;
    it->is_dir = (int)st.is_dir;
    it->size = st.is_dir ? 0 : st.size;
    return 1;
}

// --- put ------------------------------------------------------------------

static int ensure_dir(const char *p) {
    struct sys_stat st;
    if (sys_stat(p, &st) == 0) return st.is_dir ? 0 : -ENOTDIR;
    return sys_mkdir(p) == 0 ? 0 : -sys_errno();
}

// Claim a unique name: "dusk.jpg", then "dusk.2.jpg", "dusk.3.jpg" --
// the extension kept, so the bin's own listing still knows the type.
static int claim(const char *bin, const char *base, char *name, int cap, int *fd_out) {
    const char *dot = strrchr(base, '.');
    int stem = (dot && dot != base) ? (int)(dot - base) : (int)strlen(base);
    for (int n = 1; n < 1000; n++) {
        if (n == 1) {
            if ((int)strlcpy(name, base, (size_t)cap) >= cap) return -ENAMETOOLONG;
        } else if (snprintf(name, (size_t)cap, "%.*s.%d%s", stem, base, n,
                            base + stem) >= cap) {
            return -ENAMETOOLONG;
        }
        char ip[UTRASH_PATH + 80], fp[UTRASH_PATH + 80];
        struct sys_stat st;
        if (!info_path(bin, name, ip, sizeof ip) ||
            snprintf(fp, sizeof fp, "%s/files/%s", bin, name) >= (int)sizeof fp)
            return -ENAMETOOLONG;
        if (sys_stat(fp, &st) == 0) continue;   // an orphan holds it
        int fd = open(ip, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) { *fd_out = fd; return 0; }
        if (errno != EEXIST) return -errno;
    }
    return -EEXIST;
}

int utrash_put(const char *path, struct utrash_item *out) {
    struct utrash_item local;
    struct utrash_item *it = out ? out : &local;
    memset(it, 0, sizeof *it);
    char bin[UTRASH_PATH];
    if (!utrash_bin_for(path, bin, sizeof bin)) return -ENOTSUP;
    struct sys_stat st;
    if (sys_stat(path, &st) != 0) return -sys_errno();

    char sub[UTRASH_PATH + 8];
    int rc = ensure_dir(bin);
    snprintf(sub, sizeof sub, "%s/files", bin);
    if (!rc) rc = ensure_dir(sub);
    snprintf(sub, sizeof sub, "%s/info", bin);
    if (!rc) rc = ensure_dir(sub);
    if (rc) return rc;

    char date[32];
    struct rtc_time now;
    sys_gettime(&now);
    date_local(&now, date, sizeof date);
    int fd = -1;
    rc = claim(bin, k_path_basename(path), it->name, sizeof it->name, &fd);
    if (rc) return rc;
    int wrote = write_info(fd, path, date);
    close(fd);

    char ip[UTRASH_PATH + 80], fp[UTRASH_PATH + 80];
    info_path(bin, it->name, ip, sizeof ip);
    strlcpy(it->bin, bin, sizeof it->bin);
    utrash_item_path(it, fp, sizeof fp);
    if (!wrote) { unlink(ip); return -EIO; }
    if (sys_rename(path, fp) != 0) {
        rc = -sys_errno();
        unlink(ip);
        return rc;
    }
    strlcpy(it->orig, path, sizeof it->orig);
    it->deleted = now;
    it->is_dir = (int)st.is_dir;
    it->size = st.is_dir ? 0 : st.size;
    return 0;
}

// --- list -----------------------------------------------------------------

static int list_bin(const char *bin, struct utrash_item *out, int n, int cap) {
    static struct sys_dirent ents[64];   // static: a ring-3 frame is 2 KiB
    char dir[UTRASH_PATH + 8];
    snprintf(dir, sizeof dir, "%s/info", bin);
    for (int start = 0; n < cap; ) {
        int got = sys_listdir_at(dir, ents, 64, start);
        if (got <= 0) break;
        for (int i = 0; i < got && n < cap; i++) {
            char name[64];
            size_t l = strlen(ents[i].name);
            static const char ext[] = ".trashinfo";
            if (ents[i].is_dir || l <= sizeof ext - 1 ||
                strcmp(ents[i].name + l - (sizeof ext - 1), ext)) continue;
            strlcpy(name, ents[i].name, sizeof name);
            name[l - (sizeof ext - 1)] = '\0';
            if (read_info(bin, name, &out[n])) n++;
        }
        start += got;
        if (got < 64) break;
    }
    return n;
}

static int newest_first(const void *a, const void *b) {
    uint64_t ta = cal_rtc_to_epoch(&((const struct utrash_item *)a)->deleted);
    uint64_t tb = cal_rtc_to_epoch(&((const struct utrash_item *)b)->deleted);
    return ta < tb ? 1 : ta > tb ? -1 : 0;
}

int utrash_list(struct utrash_item *out, int cap) {
    int n = list_bin(HOME_BIN, out, 0, cap);
    struct query_fsinfo fs;
    for (int i = 0; n < cap; i++) {
        int r = sys_query_record(QUERY_FSINFO, (unsigned)i, &fs, sizeof fs);
        if (r <= 0) break;
        if ((unsigned)r < sizeof fs || !(fs.flags & QUERY_FS_MOUNTED) ||
            !(fs.flags & QUERY_FS_PERSISTENT) || !strcmp(fs.point, "/")) continue;
        char bin[UTRASH_PATH];
        if (k_path_join(fs.point, TOP_BIN, bin, sizeof bin)) n = list_bin(bin, out, n, cap);
    }
    qsort(out, (size_t)n, sizeof *out, newest_first);
    return n;
}

// --- restore and forget ---------------------------------------------------

int utrash_restore(const struct utrash_item *it) {
    char fp[UTRASH_PATH + 80], parent[UTRASH_PATH];
    struct sys_stat st;
    if (!utrash_item_path(it, fp, sizeof fp)) return -ENAMETOOLONG;
    if (sys_stat(it->orig, &st) == 0) return -EEXIST;
    if (!k_path_dirname(it->orig, parent, sizeof parent) || sys_stat(parent, &st) != 0 || !st.is_dir)
        return -ENOENT;
    if (sys_rename(fp, it->orig) != 0) return -sys_errno();
    return utrash_forget(it);
}

int utrash_forget_path(const char *files_path) {
    struct utrash_item it;
    char files[UTRASH_PATH];
    if (!k_path_dirname(files_path, files, sizeof files) ||
        strcmp(k_path_basename(files), "files") ||
        !k_path_dirname(files, it.bin, sizeof it.bin)) return -EINVAL;
    strlcpy(it.name, k_path_basename(files_path), sizeof it.name);
    return utrash_forget(&it);
}

int utrash_forget(const struct utrash_item *it) {
    char ip[UTRASH_PATH + 80];
    if (!info_path(it->bin, it->name, ip, sizeof ip)) return -ENAMETOOLONG;
    return sys_unlink(ip) == 0 ? 0 : -sys_errno();
}
