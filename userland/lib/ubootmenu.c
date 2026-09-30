// See ubootmenu.h.
#include "lib/ubootmenu.h"
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include "rt/sys.h"

#define CFG_MAX   (32 * 1024)
// GRUB's environment block: exactly this many bytes, this header first,
// `name=value` lines, and '#' to the end. GRUB rewrites it IN PLACE and
// refuses a file of any other size.
#define ENV_SIZE  1024
#define ENV_HEAD  "# GRUB Environment Block\n"

static int read_file(const char *path, char *buf, int cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = 0;
    for (;;) {
        long r = read(fd, buf + n, (size_t)(cap - 1 - n));
        if (r <= 0) break;
        n += (int)r;
        if (n >= cap - 1) break;
    }
    close(fd);
    buf[n] = 0;
    return n;
}

// One GRUB word at *pp: 'single quoted', "double quoted" (backslash
// escapes), or bare up to whitespace, `{` or `;`. 0 when it does not
// fit `cap` -- refused, never truncated into another title.
static int word(const char **pp, char *out, int cap) {
    const char *p = *pp;
    int n = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '{' && *p != ';') {
        char q = *p;
        if (q == '\'' || q == '"') {
            p++;
            while (*p && *p != q) {
                if (q == '"' && *p == '\\' && p[1]) p++;
                if (n >= cap - 1) return 0;
                out[n++] = *p++;
            }
            if (*p != q) return 0;
            p++;
        } else {
            if (n >= cap - 1) return 0;
            out[n++] = *p++;
        }
    }
    out[n] = 0;
    *pp = p;
    return 1;
}

static const char *skip_blank(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

int ubootmenu_find(const struct ubootmenu *m, const char *spec) {
    if (!spec || !*spec) return -1;
    int digits = 1;
    for (const char *s = spec; *s; s++) if (*s < '0' || *s > '9') digits = 0;
    if (digits) {
        int n = atoi(spec);
        return n < m->count ? n : -1;
    }
    for (int i = 0; i < m->count; i++)
        if (!strcmp(m->title[i], spec)) return i;
    return -1;
}

// `next_entry` from the environment block, or "" when none is pending.
static void pending(char *out, int cap) {
    static char env[ENV_SIZE + 1];
    out[0] = 0;
    if (read_file(UBOOTMENU_ENV, env, sizeof env) < (int)sizeof ENV_HEAD - 1) return;
    if (strncmp(env, ENV_HEAD, sizeof ENV_HEAD - 1)) return;
    for (char *l = env; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : 0) {
        if (strncmp(l, "next_entry=", 11)) continue;
        const char *v = l + 11;
        int n = 0;
        while (v[n] && v[n] != '\n' && n < cap - 1) { out[n] = v[n]; n++; }
        out[n] = 0;
        return;
    }
}

int ubootmenu_read(struct ubootmenu *m, const char *cfg) {
    static char buf[CFG_MAX];
    memset(m, 0, sizeof *m);
    m->next = -1;
    if (read_file(cfg ? cfg : UBOOTMENU_CFG, buf, sizeof buf) < 0) return -1;

    char defspec[UBOOTMENU_TITLE] = "";
    int depth = 0;
    for (const char *line = buf; *line; ) {
        const char *end = strchr(line, '\n');
        const char *p = skip_blank(line);
        if (depth == 0 && !strncmp(p, "menuentry", 9) && (p[9] == ' ' || p[9] == '\t')) {
            p = skip_blank(p + 9);
            if (m->count < UBOOTMENU_MAX && word(&p, m->title[m->count], UBOOTMENU_TITLE))
                m->count++;
        } else if (depth == 0 && !strncmp(p, "set default=", 12)) {
            // A value computed at boot -- the one-shot stanza's own
            // `set default="${next_entry}"` -- says nothing about the
            // default, and must not replace the line that does.
            char v[UBOOTMENU_TITLE];
            p += 12;
            if (word(&p, v, sizeof v) && !strchr(v, '$')) strcpy(defspec, v);
        }
        // Braces outside quotes and comments move the depth -- which
        // is what keeps a `submenu`'s inner entries out of the count.
        char q = 0;
        for (const char *c = line; c < (end ? end : line + strlen(line)); c++) {
            if (q) { if (*c == q) q = 0; continue; }
            if (*c == '#') break;
            if (*c == '\'' || *c == '"') q = *c;
            else if (*c == '{') depth++;
            else if (*c == '}' && depth > 0) depth--;
        }
        if (!end) break;
        line = end + 1;
    }

    // The stanza reads next_entry; a cfg that never names it cannot
    // be the one that clears it.
    m->oneshot = strstr(buf, "next_entry") != 0;
    if (!m->oneshot) m->why_not = "grub.cfg has no one-shot stanza (see the repo's grub.cfg)";
    // No stamp is not a no: a disk set up from the host has none, and
    // its core image comes from the same CORE_MODULES list.
    static char stamp[512];
    if (m->oneshot && read_file(UBOOTMENU_CORE_STAMP, stamp, sizeof stamp) >= 0 &&
        !strstr(stamp, "loadenv")) {
        m->oneshot = 0;
        m->why_not = "this machine's GRUB has no loadenv -- run `install --bootloader confirm`";
    }

    int d = ubootmenu_find(m, defspec);
    m->def = d >= 0 ? d : 0;
    char next[UBOOTMENU_TITLE];
    pending(next, sizeof next);
    if (next[0]) m->next = ubootmenu_find(m, next);
    return m->count;
}

int ubootmenu_set_next_at(const char *env_path, const char *title) {
    static char old[ENV_SIZE + 1];
    char blk[ENV_SIZE];
    int n = (int)sizeof ENV_HEAD - 1;
    memcpy(blk, ENV_HEAD, (size_t)n);

    // Every OTHER variable is kept: the block is GRUB's too.
    if (read_file(env_path, old, sizeof old) > n && !strncmp(old, ENV_HEAD, (size_t)n)) {
        for (char *l = old + n; *l && *l != '#'; ) {
            char *e = strchr(l, '\n');
            if (!e) break;
            int len = (int)(e - l) + 1;
            if (strncmp(l, "next_entry=", 11)) {
                if (n + len > ENV_SIZE) return -1;
                memcpy(blk + n, l, (size_t)len);
                n += len;
            }
            l = e + 1;
        }
    }
    if (title) {
        // GRUB escapes these two inside a value; a title needing them
        // is refused rather than stored in a form GRUB reads back
        // differently.
        if (strchr(title, '\n') || strchr(title, '\\')) return -1;
        int len = (int)strlen(title);
        if (n + 11 + len + 1 > ENV_SIZE) return -1;
        memcpy(blk + n, "next_entry=", 11);
        memcpy(blk + n + 11, title, (size_t)len);
        blk[n + 11 + len] = '\n';
        n += 11 + len + 1;
    }
    memset(blk + n, '#', (size_t)(ENV_SIZE - n));

    int fd = open(env_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    long w = write(fd, blk, ENV_SIZE);
    close(fd);
    return w == ENV_SIZE ? 0 : -1;
}

// /boot IS READ-ONLY BY DEFAULT (kernel/fs/mount.c), so a write there
// remounts it read-write for the one write and puts it back: the same
// umount-then-mount `remote.py flash` does, and Linux's shape for an ESP
// that is `ro` until something needs it. Returns the device to restore
// from in `dev`, or "" when /boot was writable already.
static int boot_writable(char *dev, int cap) {
    struct query_fsinfo fs;
    dev[0] = 0;
    for (int i = 0; ; i++) {
        if (sys_query_record(QUERY_FSINFO, i, &fs, sizeof fs) <= 0) return -1;
        if (!(fs.flags & QUERY_FS_MOUNTED) || strcmp(fs.point, "/boot")) continue;
        if (!(fs.flags & QUERY_FS_RDONLY)) return 0;
        break;
    }
    struct mount_request req;
    memset(&req, 0, sizeof req);
    snprintf(req.source, sizeof req.source, "%s", fs.device);
    snprintf(req.point, sizeof req.point, "/boot");
    if (sys_umount("/boot") < 0) return -1;
    if (sys_mount(&req) < 0) {
        req.flags = SYS_MNT_RDONLY;       // put back what was there
        sys_mount(&req);
        return -1;
    }
    snprintf(dev, (size_t)cap, "%s", fs.device);
    return 0;
}

static void boot_restore(const char *dev) {
    if (!dev[0]) return;
    struct mount_request req;
    memset(&req, 0, sizeof req);
    snprintf(req.source, sizeof req.source, "%s", dev);
    snprintf(req.point, sizeof req.point, "/boot");
    req.flags = SYS_MNT_RDONLY;
    if (sys_umount("/boot") == 0) sys_mount(&req);
}

int ubootmenu_set_next(const char *title) {
    char dev[64];
    if (boot_writable(dev, sizeof dev) < 0) return -1;
    int rc = ubootmenu_set_next_at(UBOOTMENU_ENV, title);
    boot_restore(dev);
    return rc;
}
