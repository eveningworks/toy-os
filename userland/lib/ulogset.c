// See ulogset.h.
#include "lib/ulogset.h"
#include "rt/sys.h"
#include "query_abi.h"
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

void ulogset_init(struct ulogset *s, struct ulog_line *lines, int *view, int cap) {
    memset(s, 0, sizeof *s);
    s->lines = lines;
    s->view = view;
    s->cap = cap;
}

void ulogset_clear(struct ulogset *s) {
    s->count = s->view_count = s->dropped = 0;
    s->max_cs = 0;
}

int ulog_severity(int level) {
    if (level >= 0 && level <= 3) return 1;
    if (level == 4) return 2;
    return 0;
}

const char *ulog_level_name(int level) {
    switch (level) {
    case 0: case 1: case 2: return "crit";
    case 3: return "err";
    case 4: return "warn";
    case 5: return "note";
    case 6: return "info";
    case 7: return "debug";
    default: return "";
    }
}

// Full: the OLDEST goes, which is what a reader of a log wants once it no
// longer fits. A file is read so this never runs (ulogset_read_file).
static struct ulog_line *next_line(struct ulogset *s) {
    if (s->count < s->cap) return &s->lines[s->count++];
    memmove(&s->lines[0], &s->lines[1], sizeof s->lines[0] * (size_t)(s->cap - 1));
    s->dropped++;
    return &s->lines[s->cap - 1];
}

// A PREFIX, NOT A WORD WITH A COLON IN IT: letters, digits, `_` and `-`,
// ending at ": " -- which rejects a later colon in `syscall: kill(pgid 2,
// SIGHUP)` and a message whose first colon is inside prose.
static void find_subsys(struct ulog_line *l) {
    l->subsys[0] = 0;
    for (int k = 0; k < ULOG_SUB_MAX - 1 && l->text[k]; k++) {
        char c = l->text[k];
        if (c == ':' && l->text[k + 1] == ' ' && k > 0) {
            memcpy(l->subsys, l->text, (size_t)k);
            l->subsys[k] = 0;
            return;
        }
        if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) return;
    }
}

static void put_text(struct ulog_line *l, const char *p, int n) {
    if (n > ULOG_TEXT_MAX - 1) n = ULOG_TEXT_MAX - 1;
    memcpy(l->text, p, (size_t)(n > 0 ? n : 0));
    l->text[n > 0 ? n : 0] = 0;
}

void ulogset_add(struct ulogset *s, const char *source, const char *raw, int len) {
    while (len > 0 && (raw[len - 1] == '\n' || raw[len - 1] == '\r')) len--;
    if (len <= 0) return;
    unsigned secs = 0, hund = 0;
    int i = 0, level = -1, stamped = 0;
    if (raw[0] == '[') {
        i = 1;
        while (i < len && raw[i] >= '0' && raw[i] <= '9') secs = secs * 10 + (unsigned)(raw[i++] - '0');
        if (i > 1 && i + 4 <= len && raw[i] == '.' && raw[i + 3] == ']' && raw[i + 4] == ' ') {
            hund = (unsigned)(raw[i + 1] - '0') * 10 + (unsigned)(raw[i + 2] - '0');
            i += 5;
            stamped = 1;
        } else {
            i = 0;   // not a stamp after all: the whole thing is the message
        }
    }
    if (i + 3 < len && raw[i] == '<' && raw[i + 2] == '>' && raw[i + 3] == ' ' &&
        raw[i + 1] >= '0' && raw[i + 1] <= '7') {
        level = raw[i + 1] - '0';
        i += 4;
    }
    struct ulog_line *l = next_line(s);
    memset(l, 0, sizeof *l);
    l->cs = secs * 100 + hund;
    l->stamped = (uint8_t)stamped;
    l->level = (int8_t)level;
    snprintf(l->source, sizeof l->source, "%s", source);
    put_text(l, raw + i, len - i);
    find_subsys(l);
    if (stamped && l->cs > s->max_cs) s->max_cs = l->cs;
}

void ulogset_read_klog(struct ulogset *s) {
    struct query_klog r;
    static char acc[512];          // a line spans slices; this reassembles
    int acc_len = 0, first = 1;
    // THE FIRST LINE OUT OF A WRAPPED RING IS HEADLESS -- the tail of a
    // line whose start is gone. Named `(cut)`: the bytes are real, and a
    // blank source would read as corruption.
    for (int i = 0; ; i++) {
        if (sys_query_record(QUERY_KLOG, (unsigned)i, &r, sizeof r) <= 0) break;
        for (unsigned j = 0; j < r.len; j++) {
            char c = (char)r.data[j];
            if (c == '\n') {
                ulogset_add(s, first && acc_len && acc[0] != '[' ? "(cut)" : "kernel", acc, acc_len);
                first = 0;
                acc_len = 0;
            } else if (acc_len < (int)sizeof acc) {
                acc[acc_len++] = c;
            }
        }
    }
    if (acc_len) ulogset_add(s, first && acc[0] != '[' ? "(cut)" : "kernel", acc, acc_len);
}

void ulogset_read_applog(struct ulogset *s) {
    struct query_applog a;
    if (sys_query_record(QUERY_APPLOG, 0, &a, sizeof a) <= 0) return;
    unsigned long long total = a.total, oldest = a.oldest;
    if (total < oldest) return;
    // ONE WRITE IS ONE RECORD and a line is often several (api/applog.h),
    // so fragments are joined PER TAG until one ends a line: two programs
    // writing at once interleave, and joining in arrival order would
    // splice one program's half-line onto another's.
    struct { char tag[ULOG_SRC_MAX]; char text[ULOG_TEXT_MAX]; int len; unsigned cs; } frag[6];
    memset(frag, 0, sizeof frag);
    for (unsigned long long seq = oldest; seq <= total; seq++) {
        if (sys_query_record(QUERY_APPLOG, (unsigned)(seq - oldest), &a, sizeof a) <= 0) break;
        int f = -1, free_slot = -1;
        for (int k = 0; k < 6; k++) {
            if (frag[k].len && strcmp(frag[k].tag, a.tag) == 0) { f = k; break; }
            if (!frag[k].len && free_slot < 0) free_slot = k;
        }
        if (f < 0) f = free_slot >= 0 ? free_slot : 0;
        if (!frag[f].len) {
            snprintf(frag[f].tag, sizeof frag[f].tag, "%s", a.tag);
            frag[f].cs = (unsigned)a.cs;
        }
        int room = (int)sizeof frag[f].text - 1 - frag[f].len;
        int take = (int)a.len < room ? (int)a.len : room;
        if (take > 0) {
            memcpy(frag[f].text + frag[f].len, a.text, (size_t)take);
            frag[f].len += take;
        }
        if (a.eol || take < (int)a.len) {
            struct ulog_line *l = next_line(s);
            memset(l, 0, sizeof *l);
            l->cs = frag[f].cs;
            l->stamped = 1;           // a record carries its own cs
            l->level = -1;            // a program declares none
            snprintf(l->source, sizeof l->source, "%s", frag[f].tag);
            put_text(l, frag[f].text, frag[f].len);
            find_subsys(l);
            if (l->cs > s->max_cs) s->max_cs = l->cs;
            frag[f].len = 0;
        }
    }
}

// "[tag   ] rest" (logd's boot file: the tag padded, the rest a ring line
// byte for byte), else a plain line whose "HH:MM:SS " lead, if any, is
// kept as its clock.
static void add_file_line(struct ulogset *s, const char *name, const char *p, int len) {
    while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r')) len--;
    if (len <= 0) return;
    if (p[0] == '[') {
        int j = 1;
        while (j < len && p[j] != ']') j++;
        // A tag is a word; "[12.34]" is a stamp, which a ring line starts with.
        if (j < len && j > 1 && !(p[1] >= '0' && p[1] <= '9')) {
            char src[ULOG_SRC_MAX];
            int n = j - 1;
            while (n > 0 && p[n] == ' ') n--;
            if (n > (int)sizeof src - 1) n = (int)sizeof src - 1;
            memcpy(src, p + 1, (size_t)n);
            src[n] = 0;
            int i = j + 1;
            if (i < len && p[i] == ' ') i++;
            ulogset_add(s, src, p + i, len - i);
            return;
        }
        ulogset_add(s, name, p, len);
        return;
    }
    struct ulog_line *l = next_line(s);
    memset(l, 0, sizeof *l);
    l->level = -1;
    snprintf(l->source, sizeof l->source, "%s", name);
    int i = 0;
    if (len >= 9 && isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]) && p[2] == ':' &&
        isdigit((unsigned char)p[3]) && isdigit((unsigned char)p[4]) && p[5] == ':' &&
        isdigit((unsigned char)p[6]) && isdigit((unsigned char)p[7]) && p[8] == ' ') {
        memcpy(l->clock, p, 8);
        l->clock[8] = 0;
        i = 9;
    }
    put_text(l, p + i, len - i);
    find_subsys(l);
}

// TWO PASSES, THE FIRST ONLY COUNTING: next_line() keeps the newest by
// shifting the array, which per line of a long file is quadratic -- a
// 183 KiB boot once hung the window. Skipping to the last `cap` lines
// means the shift never runs.
int ulogset_read_file(struct ulogset *s, const char *path, const char *name) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    static char buf[2048];
    static char line[512];
    int total = 0, partial = 0, got;
    while ((got = (int)read(fd, buf, sizeof buf)) > 0)
        for (int i = 0; i < got; i++) { if (buf[i] == '\n') { total++; partial = 0; } else partial = 1; }
    total += partial;
    int room = s->cap - s->count;
    int skip = total > room ? total - room : 0;
    s->dropped += skip;
    if (lseek(fd, 0, SEEK_SET) < 0) { close(fd); return -1; }
    int len = 0, index = 0;
    while ((got = (int)read(fd, buf, sizeof buf)) > 0) {
        for (int i = 0; i < got; i++) {
            if (buf[i] == '\n') {
                if (index++ >= skip) add_file_line(s, name, line, len);
                len = 0;
            } else if (len < (int)sizeof line - 1) {
                line[len++] = buf[i];
            }
        }
    }
    if (len && index >= skip) add_file_line(s, name, line, len);
    close(fd);
    return 0;
}

// --- merging and repeats -------------------------------------------------

// Unstamped lines keep their place AHEAD of the stamped ones: a ring's cut
// head is the oldest thing it holds, and a file without stamps has only
// its own order.
static int earlier(const struct ulog_line *a, const struct ulog_line *b) {
    if (a->stamped != b->stamped) return !a->stamped;
    return a->cs < b->cs;
}

static uint32_t key_of(const struct ulog_line *l) {
    uint32_t h = 2166136261u;
    for (const char *p = l->source; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    h = (h ^ 0xff) * 16777619u;
    for (const char *p = l->text; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return h;
}

static int same_message(const struct ulog_line *a, const struct ulog_line *b) {
    return !strcmp(a->text, b->text) && !strcmp(a->source, b->source);
}

void ulogset_finish(struct ulogset *s) {
    int n = s->count;
    if (n <= 0) return;
    // A STABLE merge sort on indices, then one permuting copy. Each source
    // is already in its own order, so stability is what keeps it.
    int *idx = malloc(sizeof(int) * (size_t)n * 2);
    struct ulog_line *tmp = malloc(sizeof *tmp * (size_t)n);
    if (idx && tmp) {
        int *a = idx, *b = idx + n;
        for (int i = 0; i < n; i++) a[i] = i;
        for (int w = 1; w < n; w *= 2) {
            for (int lo = 0; lo < n; lo += 2 * w) {
                int mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
                int i = lo, j = mid, k = lo;
                while (i < mid && j < hi)
                    b[k++] = earlier(&s->lines[a[j]], &s->lines[a[i]]) ? a[j++] : a[i++];
                while (i < mid) b[k++] = a[i++];
                while (j < hi) b[k++] = a[j++];
            }
            int *t = a; a = b; b = t;
        }
        for (int i = 0; i < n; i++) tmp[i] = s->lines[a[i]];
        memcpy(s->lines, tmp, sizeof *tmp * (size_t)n);
    }
    free(tmp);

    // REPEATS: an open-addressed table of each message's first line. Every
    // line ends up knowing how many share its message and where the first
    // and last are, which is all the details pane and "only this" need.
    int size = 1;
    while (size < n * 2) size <<= 1;
    int *slot = idx ? realloc(idx, sizeof(int) * (size_t)size) : malloc(sizeof(int) * (size_t)size);
    if (!slot) {
        for (int i = 0; i < n; i++) { s->lines[i].first = s->lines[i].last = i; s->lines[i].count = 1; }
        return;
    }
    for (int i = 0; i < size; i++) slot[i] = -1;
    for (int i = 0; i < n; i++) {
        struct ulog_line *l = &s->lines[i];
        unsigned h = key_of(l) & (unsigned)(size - 1);
        while (slot[h] >= 0 && !same_message(&s->lines[slot[h]], l)) h = (h + 1) & (unsigned)(size - 1);
        if (slot[h] < 0) { slot[h] = i; l->first = i; l->count = 1; l->last = i; }
        else {
            struct ulog_line *f = &s->lines[slot[h]];
            l->first = slot[h];
            f->count++;
            f->last = i;
        }
    }
    for (int i = 0; i < n; i++) {
        const struct ulog_line *f = &s->lines[s->lines[i].first];
        s->lines[i].count = f->count;
        s->lines[i].last = f->last;
    }
    free(slot);
}

// --- the view ------------------------------------------------------------

const char *ulogset_who(const struct ulogset *s, int i) {
    const struct ulog_line *l = &s->lines[i];
    return l->subsys[0] ? l->subsys : l->source;
}

static int is_kernel(const struct ulog_line *l) {
    return !strcmp(l->source, "kernel") || !strcmp(l->source, "(cut)");
}

static int contains_nocase(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return 1;
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n)) return 1;
    return 0;
}

int ulog_filter_muted(const struct ulog_filter *f, const char *name) {
    for (int k = 0; k < f->nmute; k++)
        if (!strcmp(f->mute[k], name)) return 1;
    return 0;
}

int ulog_filter_mute(struct ulog_filter *f, const char *name) {
    if (ulog_filter_muted(f, name)) return 0;
    if (f->nmute >= ULOG_MUTE_MAX || !name[0]) return -1;
    snprintf(f->mute[f->nmute++], ULOG_SUB_MAX, "%s", name);
    return 0;
}

int ulog_filter_unmute(struct ulog_filter *f, const char *name) {
    for (int k = 0; k < f->nmute; k++) {
        if (strcmp(f->mute[k], name)) continue;
        memmove(f->mute[k], f->mute[k + 1], (size_t)(f->nmute - k - 1) * ULOG_SUB_MAX);
        f->nmute--;
        return 0;
    }
    return -1;
}

void ulogset_filter(struct ulogset *s, const struct ulog_filter *f) {
    s->view_count = 0;
    int only_first = f->only >= 0 && f->only < s->count ? s->lines[f->only].first : -1;
    for (int i = 0; i < s->count; i++) {
        const struct ulog_line *l = &s->lines[i];
        int sev = ulog_severity(l->level);
        if (f->show == ULOG_SHOW_ERRORS && sev != 1) continue;
        if (f->show == ULOG_SHOW_WARNINGS && sev != 2) continue;
        if (f->from == ULOG_FROM_KERNEL && !is_kernel(l)) continue;
        if (f->from == ULOG_FROM_PROGRAMS && is_kernel(l)) continue;
        if (only_first >= 0 && l->first != only_first) continue;
        if ((f->range_lo || f->range_hi) && l->stamped && (l->cs < f->range_lo || l->cs > f->range_hi)) continue;
        if (f->nmute && ulog_filter_muted(f, ulogset_who(s, i))) continue;
        if (f->search[0] && !contains_nocase(l->text, f->search) && !contains_nocase(l->source, f->search)) continue;
        s->view[s->view_count++] = i;
    }
}

void ulogset_counts(const struct ulogset *s, struct ulog_counts *out) {
    memset(out, 0, sizeof *out);
    for (int i = 0; i < s->count; i++) {
        int sev = ulog_severity(s->lines[i].level);
        if (sev == 1) out->errors++;
        if (sev == 2) out->warnings++;
        if (is_kernel(&s->lines[i])) out->kernel++; else out->programs++;
    }
}

int ulogset_count_who(const struct ulogset *s, const char *name) {
    int n = 0;
    for (int i = 0; i < s->count; i++)
        if (!strcmp(ulogset_who(s, i), name)) n++;
    return n;
}

int ulogset_format(const struct ulogset *s, int i, char *out, int cap) {
    const struct ulog_line *l = &s->lines[i];
    char lv[8] = "";
    if (l->level >= 0) snprintf(lv, sizeof lv, "<%d> ", l->level);
    if (l->stamped)
        return snprintf(out, (size_t)cap, "[%-6s] [%u.%02u] %s%s\n", l->source,
                        l->cs / 100, l->cs % 100, lv, l->text);
    if (l->clock[0]) return snprintf(out, (size_t)cap, "%s %s\n", l->clock, l->text);
    return snprintf(out, (size_t)cap, "%s\n", l->text);
}
