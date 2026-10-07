#include "lib/ulaunch.h"
#include "lib/uappentry.h"
#include "lib/uconf.h"
#include "lib/uopen.h"
#include "rt/sys.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ELF_ET_EXEC 2
#define ELF_ET_DYN  3

static uint64_t le64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

// A library is ET_DYN with no entry point; a position-independent
// program is ET_DYN with one.
static int elf_is_program(const unsigned char *b, size_t n) {
    if (n < 32 || b[4] != 2) return 0;   // ELFCLASS64
    unsigned type = (unsigned)b[16] | ((unsigned)b[17] << 8);
    return type == ELF_ET_EXEC || (type == ELF_ET_DYN && le64(b + 24) != 0);
}

static int is_app(const char *path, struct ulaunch_info *out) {
    struct uappentry e;
    if (uappentry_find_exec(path, &e)) {
        snprintf(out->app, sizeof out->app, "%s", e.name);
        return 1;
    }
    return strncmp(path, "/bin/wm/", 8) == 0;
}

// The script's first lines, for a person to read before running it.
static void take_head(const unsigned char *b, size_t n, struct ulaunch_info *out) {
    size_t i = 0;
    while (i < n && out->head_lines < ULAUNCH_HEAD_LINES) {
        char *line = out->head[out->head_lines];
        int c = 0;
        while (i < n && b[i] != '\n') {
            unsigned char ch = b[i++];
            if (ch == '\t') ch = ' ';
            if (ch == '\r') continue;
            if (ch < 0x20 || ch >= 0x7f) ch = '?';
            if (c < ULAUNCH_HEAD_COLS) line[c++] = (char)ch;
        }
        line[c] = '\0';
        // A line cut by the 512-byte read is not shown half-way.
        if (i >= n && n == 512) break;
        i++;
        out->head_lines++;
    }
}

int ulaunch_classify(const char *path, struct ulaunch_info *out) {
    memset(out, 0, sizeof *out);
    struct sys_stat st;
    if (!path || sys_stat(path, &st) != 0 || st.is_dir) return 0;
    out->runnable = (st.mode & 0111) != 0;

    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char b[512];
    size_t n = fread(b, 1, sizeof b, f);
    fclose(f);

    if (n >= 4 && b[0] == 0x7f && b[1] == 'E' && b[2] == 'L' && b[3] == 'F') {
        if (!elf_is_program(b, n)) return 1;
        out->kind = is_app(path, out) ? ULAUNCH_APP : ULAUNCH_PROGRAM;
        out->interp_found = 1;
        return 1;
    }
    if (n >= 3 && b[0] == '#' && b[1] == '!') {
        // `#!/bin/dash -e`: the program is the first word, which is all
        // the loader needs to exist (kernel/proc/sched_fork.c).
        size_t i = 2, k = 0;
        while (i < n && (b[i] == ' ' || b[i] == '\t')) i++;
        while (i < n && b[i] != '\n' && b[i] != ' ' && b[i] != '\t' && k + 1 < sizeof out->interp)
            out->interp[k++] = (char)b[i++];
        out->interp[k] = '\0';
        if (!out->interp[0]) return 1;
        out->kind = ULAUNCH_SCRIPT;
        struct sys_stat is;
        out->interp_found = sys_stat(out->interp, &is) == 0 && !is.is_dir;
        take_head(b, n, out);
    }
    return 1;
}

static const char *const ACT[] = { "ask", "terminal", "run", "edit" };

static const char *mime_of(int kind) {
    return kind == ULAUNCH_SCRIPT ? ULAUNCH_MIME_SCRIPT : ULAUNCH_MIME_PROGRAM;
}

int ulaunch_policy(int kind) {
    if (kind == ULAUNCH_APP) return ULAUNCH_RUN;
    if (kind != ULAUNCH_SCRIPT && kind != ULAUNCH_PROGRAM) return ULAUNCH_ASK;
    char v[16];
    if (!uconf_get(UOPEN_CONF, mime_of(kind), v, sizeof v)) return ULAUNCH_ASK;
    for (int i = 1; i < (int)(sizeof ACT / sizeof ACT[0]); i++)
        if (!strcmp(v, ACT[i])) return i == ULAUNCH_EDIT && kind != ULAUNCH_SCRIPT ? ULAUNCH_ASK : i;
    return ULAUNCH_ASK;
}

int ulaunch_set_policy(int kind, int act) {
    if (kind != ULAUNCH_SCRIPT && kind != ULAUNCH_PROGRAM) return 0;
    if (act <= ULAUNCH_ASK || act > ULAUNCH_EDIT) return uconf_unset(UOPEN_CONF, mime_of(kind));
    return uconf_set(UOPEN_CONF, mime_of(kind), ACT[act]);
}

int ulaunch_allow(const char *path) {
    struct sys_stat st;
    if (sys_stat(path, &st) != 0) return 0;
    unsigned m = st.mode & 07777;
    m |= (m & 0444) >> 2;   // r -> x, per class
    if (sys_chmod(path, m) != 0) return 0;
    // FAT32 accepts the call and keeps nothing: ask again.
    return sys_stat(path, &st) == 0 && (st.mode & 0111);
}

int ulaunch_argv(const char *path, int kind, int act, char **argv) {
    int n = 0;
    switch (act) {
    case ULAUNCH_TERMINAL:
        if (kind != ULAUNCH_SCRIPT && kind != ULAUNCH_PROGRAM) return 0;
        argv[n++] = ULAUNCH_TERMINAL_EXEC;
        argv[n++] = "-e";
        break;
    case ULAUNCH_RUN:
        if (kind == ULAUNCH_NONE) return 0;
        break;
    case ULAUNCH_EDIT:
        if (kind != ULAUNCH_SCRIPT) return 0;
        argv[n++] = ULAUNCH_EDIT_EXEC;
        break;
    default:
        return 0;
    }
    argv[n++] = (char *)path;
    argv[n] = 0;
    return 1;
}
