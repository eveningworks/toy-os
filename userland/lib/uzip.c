// See lib/uzip.h.
#include "lib/uzip.h"
#include "lib/uinflate.h"
#include "lib/ubytes.h"
#include <kcrc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define EOCD_SIG  0x06054b50u
#define CDIR_SIG  0x02014b50u
#define LOCAL_SIG 0x04034b50u
#define EOCD_LEN  22
#define CDIR_LEN  46
#define LOCAL_LEN 30
// The end record sits behind a comment of at most 64 KiB.
#define EOCD_SEARCH (EOCD_LEN + 0xFFFF)
// A directory bigger than this is not an archive anything here asks for.
#define CDIR_MAX (4u * 1024u * 1024u)

static int failf(char *err, int cap, int rc, const char *fmt, ...) {
    if (err && cap > 0) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, (size_t)cap, fmt, ap);
        va_end(ap);
    }
    return rc;
}

struct member {
    unsigned method, flags;
    uint32_t crc, csize, usize, local;
};

// Finds `name` in the central directory. 0, or a negative errno.
static int find_member(int fd, unsigned long fsize, const char *name, struct member *m,
                       char *err, int cap) {
    unsigned long tail = fsize < EOCD_SEARCH ? fsize : EOCD_SEARCH;
    if (tail < EOCD_LEN) return failf(err, cap, -EINVAL, "not a zip archive (too short)");
    unsigned char *t = malloc(tail);
    if (!t) return failf(err, cap, -ENOMEM, "out of memory for the archive's end");
    if (ub_read_at(fd, fsize - tail, t, tail) < 0) {
        free(t);
        return failf(err, cap, -EIO, "could not read the archive's end");
    }
    // The LAST signature whose comment length lands exactly on the end:
    // a signature inside the comment would otherwise be believed.
    long at = -1;
    for (long i = (long)tail - EOCD_LEN; i >= 0; i--)
        if (ub_le32(t + i) == EOCD_SIG && (unsigned long)i + EOCD_LEN + ub_le16(t + i + 20) == tail) {
            at = i;
            break;
        }
    if (at < 0) { free(t); return failf(err, cap, -EINVAL, "not a zip archive (no end record)"); }
    uint32_t cd_size = ub_le32(t + at + 12), cd_off = ub_le32(t + at + 16);
    unsigned entries = ub_le16(t + at + 10);
    free(t);
    if (cd_off == 0xFFFFFFFFu || cd_size == 0xFFFFFFFFu || entries == 0xFFFF)
        return failf(err, cap, -ENOTSUP, "a zip64 archive, which this does not read");
    if (cd_size > CDIR_MAX || (unsigned long)cd_off + cd_size > fsize)
        return failf(err, cap, -EINVAL, "the archive's directory does not fit the file");

    unsigned char *cd = malloc(cd_size ? cd_size : 1);
    if (!cd) return failf(err, cap, -ENOMEM, "out of memory for the archive's directory");
    if (ub_read_at(fd, cd_off, cd, cd_size) < 0) {
        free(cd);
        return failf(err, cap, -EIO, "could not read the archive's directory");
    }
    size_t nlen = strlen(name);
    uint32_t p = 0;
    int rc = failf(err, cap, -ENOENT, "the archive has no %s", name);
    for (unsigned e = 0; e < entries; e++) {
        if (p + CDIR_LEN > cd_size || ub_le32(cd + p) != CDIR_SIG) {
            rc = failf(err, cap, -EINVAL, "the archive's directory is damaged");
            break;
        }
        unsigned fl = ub_le16(cd + p + 28), xl = ub_le16(cd + p + 30), cl = ub_le16(cd + p + 32);
        if ((unsigned long)p + CDIR_LEN + fl + xl + cl > cd_size) {
            rc = failf(err, cap, -EINVAL, "the archive's directory is damaged");
            break;
        }
        if (fl == nlen && !memcmp(cd + p + CDIR_LEN, name, nlen)) {
            m->flags = ub_le16(cd + p + 8);
            m->method = ub_le16(cd + p + 10);
            m->crc = ub_le32(cd + p + 16);
            m->csize = ub_le32(cd + p + 20);
            m->usize = ub_le32(cd + p + 24);
            m->local = ub_le32(cd + p + 42);
            rc = 0;
            break;
        }
        p += CDIR_LEN + fl + xl + cl;
    }
    free(cd);
    return rc;
}

struct out {
    int fd;
    uint32_t crc;
    unsigned long done, total;
    uzip_progress progress;
    void *ctx;
    int stopped, write_failed;
};

static int out_write(void *vctx, const uint8_t *data, size_t n) {
    struct out *o = vctx;
    size_t put = 0;
    while (put < n) {
        long w = write(o->fd, data + put, n - put);
        if (w <= 0) { o->write_failed = 1; return -1; }
        put += (size_t)w;
    }
    o->crc = kcrc32_update(o->crc, data, n);
    o->done += n;
    if (o->progress && o->progress(o->ctx, o->done, o->total)) { o->stopped = 1; return -1; }
    return 0;
}

int uzip_extract(const char *zip_path, const char *name, const char *out_path,
                 uzip_progress progress, void *ctx, char *err, int errcap) {
    if (err && errcap > 0) err[0] = 0;
    int fd = open(zip_path, O_RDONLY);
    if (fd < 0) return failf(err, errcap, -ENOENT, "cannot open %s", zip_path);
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return failf(err, errcap, -EIO, "cannot stat %s", zip_path); }
    unsigned long fsize = (unsigned long)st.st_size;

    struct member m;
    int rc = find_member(fd, fsize, name, &m, err, errcap);
    if (rc < 0) { close(fd); return rc; }
    if (m.flags & 1) { close(fd); return failf(err, errcap, -ENOTSUP, "%s is encrypted", name); }
    if (m.method != 0 && m.method != 8) {
        close(fd);
        return failf(err, errcap, -ENOTSUP, "%s uses compression method %u", name, m.method);
    }
    if (m.method == 0 && m.csize != m.usize) {
        close(fd);
        return failf(err, errcap, -EINVAL, "%s is stored but its sizes differ", name);
    }

    unsigned char lh[LOCAL_LEN];
    if ((unsigned long)m.local + LOCAL_LEN > fsize || ub_read_at(fd, m.local, lh, sizeof lh) < 0 ||
        ub_le32(lh) != LOCAL_SIG) {
        close(fd);
        return failf(err, errcap, -EINVAL, "%s's local header is damaged", name);
    }
    unsigned long data = (unsigned long)m.local + LOCAL_LEN + ub_le16(lh + 26) + ub_le16(lh + 28);
    if (data + m.csize > fsize) {
        close(fd);
        return failf(err, errcap, -EINVAL, "%s runs past the end of the archive", name);
    }
    unsigned char *src = malloc(m.csize ? m.csize : 1);
    if (!src) { close(fd); return failf(err, errcap, -ENOMEM, "out of memory for %s", name); }
    if (ub_read_at(fd, data, src, m.csize) < 0) {
        free(src);
        close(fd);
        return failf(err, errcap, -EIO, "could not read %s", name);
    }
    close(fd);

    char part[512];
    if (snprintf(part, sizeof part, "%s.part", out_path) >= (int)sizeof part) {
        free(src);
        return failf(err, errcap, -ENAMETOOLONG, "%s: name too long", out_path);
    }
    struct out o = { .crc = KCRC32_INIT, .total = m.usize, .progress = progress, .ctx = ctx };
    o.fd = open(part, O_WRONLY | O_CREAT | O_TRUNC);
    if (o.fd < 0) { free(src); return failf(err, errcap, -EACCES, "cannot write %s", part); }

    if (m.method == 0) {
        rc = out_write(&o, src, m.csize) ? -1 : 0;
    } else {
        struct uinflate_scratch *sc = malloc(sizeof *sc);
        size_t outlen = 0;
        rc = sc ? uinflate(src, m.csize, UINFLATE_RAW, sc, out_write, &o, &outlen) : -ENOMEM;
        free(sc);
    }
    free(src);
    if (fsync(o.fd) != 0) o.write_failed = 1;
    close(o.fd);

    if (o.stopped) rc = failf(err, errcap, -EINTR, "stopped");
    else if (o.write_failed) rc = failf(err, errcap, -EIO, "writing %s failed", part);
    else if (rc < 0) rc = failf(err, errcap, -EINVAL, "%s: %s", name, uinflate_error());
    else if (o.done != m.usize || KCRC32_FINAL(o.crc) != m.crc)
        rc = failf(err, errcap, -EINVAL, "%s does not match its CRC-32 and size", name);
    if (rc < 0) { unlink(part); return rc; }
    if (rename(part, out_path) != 0) {
        unlink(part);
        return failf(err, errcap, -EIO, "cannot rename %s into place", part);
    }
    return 0;
}
