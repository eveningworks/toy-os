// See ucrash.h.
#include "lib/ucrash.h"
#include "lib/uelfsym.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define HEAD_CAP 16384
#define MARK "---- stack ----\n"
#define MODULES 8

const char *const ucrash_reg_names[15] = {
    "r15", "r14", "r13", "r12", "r11", "r10", "r9", "r8",
    "rbp", "rdi", "rsi", "rdx", "rcx", "rbx", "rax",
};

static uint64_t hex(const char *s) { return strtoull(s, 0, 16); }

static void parse_map(struct ucrash *r, char *l) {
    // "<kind> 0xA-0xB [prot N] [path]"
    if (r->nmap >= UCRASH_MAPS) return;
    struct ucrash_map *m = &r->map[r->nmap];
    memset(m, 0, sizeof *m);
    char *sp = strchr(l, ' ');
    if (!sp) return;
    *sp = '\0';
    strlcpy(m->kind, l, sizeof m->kind);
    char *p;
    m->a = strtoull(sp + 1, &p, 16);
    if (*p != '-') return;
    m->b = strtoull(p + 1, &p, 16);
    m->prot = !strcmp(m->kind, "image") ? 5 : 3;
    char *pr = strstr(p, "prot ");
    if (pr) { m->prot = (unsigned)strtoul(pr + 5, &p, 10); }
    while (*p == ' ') p++;
    strlcpy(m->path, !strcmp(m->kind, "image") ? r->program : p, sizeof m->path);
    if (m->b > m->a) r->nmap++;
}

int ucrash_load(struct ucrash *r, const char *path) {
    memset(r, 0, sizeof *r);
    static char buf[HEAD_CAP + 1];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    long got = 0, n;
    while (got < HEAD_CAP && (n = read(fd, buf + got, (size_t)(HEAD_CAP - got))) > 0) got += n;
    close(fd);
    struct stat st;   // stat, not fstat: fstat here does not report the mtime
    if (stat(path, &st) == 0) { r->when = st.st_mtime; r->size = (unsigned)st.st_size; }
    buf[got] = '\0';
    if (strncmp(buf, "toy-os crash report", 19)) return -1;
    char *mark = strstr(buf, "\n" MARK);
    if (mark) { r->stack_off = (long)(mark + 1 - buf) + (long)strlen(MARK); *mark = '\0'; }
    strlcpy(r->path, path, sizeof r->path);
    const char *b = strrchr(path, '/');
    strlcpy(r->file, b ? b + 1 : path, sizeof r->file);

    char *klog = strstr(buf, "\nklog:\n");
    if (klog) { *klog = '\0'; strlcpy(r->log, klog + 7, sizeof r->log); }
    // The program first: the image's map line names it.
    char *pl = strstr(buf, "\nprogram: ");
    if (pl) {
        char *e = strchr(pl + 10, '\n');
        size_t len = e ? (size_t)(e - (pl + 10)) : strlen(pl + 10);
        if (len >= sizeof r->program) len = sizeof r->program - 1;
        memcpy(r->program, pl + 10, len);
    }
    for (char *l = buf; l && *l; ) {
        char *nl = strchr(l, '\n');
        if (nl) *nl = '\0';
        if (!strncmp(l, "pid: ", 5)) r->pid = atoi(l + 5);
        else if (!strncmp(l, "fault: ", 7)) strlcpy(r->fault, l + 7, sizeof r->fault);
        else if (!strncmp(l, "kernel: ", 8)) strlcpy(r->kernel, l + 8, sizeof r->kernel);
        else if (!strncmp(l, "vector: ", 8)) {
            r->vector = (unsigned)atoi(l + 8);
            char *e = strstr(l, "error: ");
            if (e) r->err = hex(e + 7);
        } else if (!strncmp(l, "rip: ", 5)) {
            r->rip = hex(l + 5);
            char *c = strstr(l, "cs: "), *f = strstr(l, "rflags: ");
            if (c) r->cs = hex(c + 4);
            if (f) r->rflags = hex(f + 8);
        } else if (!strncmp(l, "rsp: ", 5)) {
            r->rsp = hex(l + 5);
            char *s = strstr(l, "ss: "), *c = strstr(l, "cr2: ");
            if (s) r->ss = hex(s + 4);
            if (c) r->cr2 = hex(c + 5);
        } else if (!strncmp(l, "map: ", 5)) parse_map(r, l + 5);
        else if (!strncmp(l, "stack: ", 7)) {
            char *p;
            r->stack_va = strtoull(l + 7, &p, 16);
            r->stack_len = (uint32_t)strtoul(p, 0, 10);
        } else {
            for (int i = 0; i < 15; i++) {
                size_t k = strlen(ucrash_reg_names[i]);
                if (!strncmp(l, ucrash_reg_names[i], k) && l[k] == ':' && l[k + 1] == ' ') {
                    r->reg[i] = hex(l + k + 2);
                    break;
                }
            }
        }
        l = nl ? nl + 1 : 0;
    }
    return r->program[0] ? 0 : -1;
}

// --- the backtrace -------------------------------------------------------

struct module { char path[64]; uint64_t delta; struct uelfsym e; int ok; };

// The binary an address is in, opened once per backtrace.
static struct module *module_of(struct ucrash *r, struct module *mods, int *nmods, uint64_t addr) {
    const struct ucrash_map *hit = 0;
    for (int i = 0; i < r->nmap; i++)
        if (addr >= r->map[i].a && addr < r->map[i].b &&
            (!strcmp(r->map[i].kind, "image") || !strcmp(r->map[i].kind, "file")) && r->map[i].path[0]) {
            hit = &r->map[i];
            break;
        }
    if (!hit) return 0;
    for (int i = 0; i < *nmods; i++) if (!strcmp(mods[i].path, hit->path)) return mods[i].ok ? &mods[i] : 0;
    if (*nmods >= MODULES) return 0;
    struct module *m = &mods[(*nmods)++];
    strlcpy(m->path, hit->path, sizeof m->path);
    m->ok = uelfsym_open(&m->e, m->path) == 0;
    if (!m->ok) return 0;
    // The file's first mapping holds its lowest page.
    uint64_t lo = hit->a;
    for (int i = 0; i < r->nmap; i++)
        if (!strcmp(r->map[i].path, hit->path) && r->map[i].a < lo) lo = r->map[i].a;
    m->delta = lo - uelfsym_base(&m->e);
    struct stat st;
    if (stat(m->path, &st) == 0 && r->when && st.st_mtime > r->when) r->stale = 1;
    return m;
}

// The bytes before a return address are a call: E8 rel32, or FF /2 in
// its 2-, 3-, 6- and 7-byte forms (register, [reg+disp8], [rip+disp32],
// [reg+disp32]/SIB).
static int after_call(const struct module *m, uint64_t va) {
    unsigned char c[7];
    if (!uelfsym_read(&m->e, va - 7, c, 7)) return 0;
    if (c[2] == 0xE8) return 1;
    static const int back[] = { 2, 3, 6, 7 };
    for (int i = 0; i < 4; i++) {
        int k = 7 - back[i];
        if (c[k] == 0xFF && ((c[k + 1] >> 3) & 7) == 2) return 1;
    }
    return 0;
}

static void add_frame(struct ucrash *r, struct module *m, uint64_t addr, uint64_t slot) {
    struct ucrash_frame *f = &r->frame[r->nframe++];
    memset(f, 0, sizeof *f);
    f->addr = addr;
    f->slot = slot;
    const char *b = strrchr(m->path, '/');
    strlcpy(f->module, b ? b + 1 : m->path, sizeof f->module);
    uint64_t va = addr - m->delta;
    if (!uelfsym_name(&m->e, va, f->func, sizeof f->func, &f->off)) f->off = va - uelfsym_base(&m->e);
}

int ucrash_backtrace(struct ucrash *r) {
    r->nframe = 0;
    r->stale = 0;
    static struct module mods[MODULES];   // static, as ucrash_load()'s buffer is: one caller at a time
    int nmods = 0;
    struct module *m = module_of(r, mods, &nmods, r->rip);
    if (m) add_frame(r, m, r->rip, 0);

    if (r->stack_off && r->stack_len && r->rsp >= r->stack_va && r->rsp < r->stack_va + r->stack_len) {
        int fd = open(r->path, O_RDONLY);
        uint64_t from = r->rsp - r->stack_va;
        if (fd >= 0 && lseek(fd, r->stack_off + (long)from, SEEK_SET) >= 0) {
            static uint64_t buf[512];
            uint64_t slot = r->rsp;
            long n;
            while (r->nframe < UCRASH_FRAMES && slot < r->stack_va + r->stack_len &&
                   (n = read(fd, buf, sizeof buf)) >= 8) {
                for (long i = 0; i < n / 8 && r->nframe < UCRASH_FRAMES; i++, slot += 8) {
                    uint64_t v = buf[i];
                    struct module *mm = module_of(r, mods, &nmods, v);
                    if (!mm) continue;
                    uint64_t va = v - mm->delta;
                    char name[4];
                    uint64_t off = 1;
                    // A function's own first byte is not a return address.
                    if (!uelfsym_is_code(&mm->e, va) || !after_call(mm, va)) continue;
                    if (uelfsym_name(&mm->e, va, name, sizeof name, &off) && off == 0) continue;
                    add_frame(r, mm, v, slot);
                }
            }
        }
        if (fd >= 0) close(fd);
    }
    for (int i = 0; i < nmods; i++) if (mods[i].ok) uelfsym_close(&mods[i].e);
    return r->nframe;
}

void ucrash_where(const struct ucrash *r, char *out, int cap) {
    if (r->nframe && r->frame[0].slot == 0) {
        const struct ucrash_frame *f = &r->frame[0];
        if (f->func[0]) snprintf(out, (size_t)cap, "%s +0x%llx in %s", f->func, (unsigned long long)f->off, f->module);
        else snprintf(out, (size_t)cap, "%s +0x%llx", f->module, (unsigned long long)f->off);
        return;
    }
    for (int i = 0; i < r->nmap; i++) {
        const struct ucrash_map *m = &r->map[i];
        if (r->rip < m->a || r->rip >= m->b) continue;
        if (m->path[0]) {
            uint64_t lo = m->a;
            for (int j = 0; j < r->nmap; j++) if (!strcmp(r->map[j].path, m->path) && r->map[j].a < lo) lo = r->map[j].a;
            const char *b = strrchr(m->path, '/');
            snprintf(out, (size_t)cap, "%s +0x%llx", b ? b + 1 : m->path, (unsigned long long)(r->rip - lo));
        } else {
            snprintf(out, (size_t)cap, "%s memory, 0x%llx", m->kind, (unsigned long long)r->rip);
        }
        return;
    }
    snprintf(out, (size_t)cap, "0x%llx, outside every mapping", (unsigned long long)r->rip);
}

int ucrash_log_lines(const struct ucrash *r, char *out, int cap) {
    char pidw[24], name[40];
    snprintf(pidw, sizeof pidw, "pid %d", r->pid);
    const char *b = strrchr(r->program, '/');
    snprintf(name, sizeof name, "%s:", b ? b + 1 : r->program);
    int n = 0, at = 0;
    out[0] = '\0';
    for (const char *l = r->log; *l; ) {
        const char *e = strchr(l, '\n');
        int len = e ? (int)(e - l) : (int)strlen(l);
        char line[256];
        int k = len < (int)sizeof line - 1 ? len : (int)sizeof line - 1;
        memcpy(line, l, (size_t)k);
        line[k] = '\0';
        // "pid 18" must not match "pid 180".
        char *p = strstr(line, pidw);
        int pid_hit = p && (p[strlen(pidw)] < '0' || p[strlen(pidw)] > '9');
        if ((pid_hit || strstr(line, name)) && at + k + 2 < cap) {
            memcpy(out + at, line, (size_t)k);
            at += k;
            out[at++] = '\n';
            out[at] = '\0';
            n++;
        }
        l = e ? e + 1 : l + len;
    }
    return n;
}

void ucrash_explain(const struct ucrash *r, char *out, int cap) {
    char by[24];
    ucrash_sender(r, by, sizeof by);
    if (!strncmp(r->fault, "Killed by ", 10)) {
        snprintf(out, (size_t)cap, "It was killed by %s%s%s.", r->fault + 10, by[0] ? ", sent by " : "", by);
        return;
    }
    if (r->vector == 14) {
        // #PF error code: bit 0 present, 1 write, 4 instruction fetch.
        const char *what = r->err & 0x10 ? "run code at" : r->err & 2 ? "write to" : "read";
        snprintf(out, (size_t)cap, "It tried to %s 0x%llx, which %s.", what, (unsigned long long)r->cr2,
                 !(r->err & 1) ? "is not mapped" : r->err & 0x10 ? "is not executable"
                 : r->err & 2 ? "is read-only" : "it may not read");
        return;
    }
    snprintf(out, (size_t)cap, "A %s stopped it.", r->fault);
    if (out[2] >= 'A' && out[2] <= 'Z') out[2] = (char)(out[2] + 32);
}

void ucrash_sender(const struct ucrash *r, char *out, int cap) {
    out[0] = '\0';
    char want[32];
    snprintf(want, sizeof want, "kill(pid %d,", r->pid);
    const char *k = strstr(r->log, want);
    if (!k) return;
    const char *by = strstr(k, ") by pid ");
    const char *eol = strchr(k, '\n');
    if (!by || (eol && by > eol)) return;
    snprintf(out, (size_t)cap, "pid %d", atoi(by + 9));
}
