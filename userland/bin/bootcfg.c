// bootcfg -- read and change GRUB's grub.cfg safely: one edit, checked,
// shown as a diff, saved with the old file kept as grub.cfg.bak.
//
// The shape is XP's bootcfg.exe and Fedora's grubby -- subcommands that
// each change one thing in place -- plus `edit` (visudo: a copy in
// /bin/edit, checked before it can replace the real file) and `try`
// (grub-reboot: a trial entry booted ONCE, then kept or dropped). The
// model, the checks and the save are lib/ubootcfg.h, shared with the
// Boot Manager and the Startup settings page.
//
// A problem the edit INTRODUCES refuses the save: BROKEN always, RISKY
// unless --force. One the file already had is reported and does not
// block an unrelated edit.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include "lib/ubootcfg.h"
#include "lib/ubootwords.h"
#include "lib/cmd.h"
#include "lib/uargs.h"
#include "rt/sys.h"

static const char *g_path = UBOOTMENU_CFG;
static int g_force, g_keep, g_drop;

static const struct uargs_opt OPTS[] = {
    { "file",  0, "CFG", "edit CFG instead of /boot/boot/grub/grub.cfg", 0, &g_path },
    { "force", 0, 0,     "save despite a risky problem (never a broken one)", &g_force, 0 },
    { "keep",  0, 0,     "with try: move the trial's words to its entry", &g_keep, 0 },
    { "drop",  0, 0,     "with try: remove the trial", &g_drop, 0 },
    { 0 },
};

static const struct uargs_cmd CMDS[] = {
    { "list",    0,                    "the menu, default marked (also a bare bootcfg)" },
    { "words",   "ENTRY +WORD|-KEY...", "add, replace or remove boot words" },
    { "default", "ENTRY",              "boot ENTRY when nobody chooses" },
    { "timeout", "SECONDS",            "how long GRUB shows the menu, 0 to 3600" },
    { "copy",    "ENTRY TITLE [WORDS]", "a new entry right after ENTRY" },
    { "rename",  "ENTRY TITLE",        "retitle an entry" },
    { "remove",  "ENTRY",              "delete an entry (never the default)" },
    { "try",     "ENTRY [WORDS]",      "boot a changed copy ONCE, then --keep or --drop" },
    { "edit",    0,                    "the file in /bin/edit, checked before it is saved" },
    { "check",   0,                    "report the file's problems" },
    { "undo",    0,                    "swap grub.cfg and grub.cfg.bak" },
    { "known",   0,                    "every boot word this accepts" },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "bootcfg",
    .usage = "[OPTION]... [COMMAND [ARG]...]",
    .summary = "Read and change GRUB's boot menu, /boot/boot/grub/grub.cfg. Every change\n"
               "is checked, shown as a diff, and saved with the old file as grub.cfg.bak.",
    .opts = OPTS,
    .cmds = CMDS,
    .notes = "ENTRY is a number from `bootcfg list`, or an exact title. WORDS are\n"
             "+WORD (add, replacing a word of the same key) and -KEY (remove).\n"
             "Exit status: 0 done, 1 refused or failed, 2 bad usage; check exits\n"
             "1 for a risky file and 3 for one GRUB would stop at.",
};

// The command's own argument shape, from the table.
static int wrong(const char *cmd) {
    for (const struct uargs_cmd *c = CMDS; c->name; c++)
        if (!strcmp(c->name, cmd))
            return c->args ? uargs_error(&PROG, "'%s' takes %s", cmd, c->args)
                           : uargs_error(&PROG, "'%s' takes no arguments", cmd);
    return uargs_error(&PROG, "unknown command '%s'", cmd);
}

#define C_DIM   "\x1b[90m"
#define C_RED   "\x1b[31m"
#define C_GREEN "\x1b[32m"
#define C_WARN  "\x1b[33m"
#define C_BOLD  "\x1b[1m"
#define C_OFF   "\x1b[0m"

static struct ubootcfg g_orig, g_cfg;
static struct ubootcfg_problem g_now[24], g_was[24];

static int load(void) {
    int rc = ubootcfg_load(&g_orig, g_path);
    if (rc == -1) { printf("bootcfg: cannot read %s -- no boot menu on this boot\n", g_path); return -1; }
    if (rc == -2) { printf("bootcfg: %s is too big to edit here (%d KiB at most)\n", g_path, UBOOTCFG_MAX / 1024); return -1; }
    g_cfg = g_orig;
    return 0;
}

static int entry_arg(const char *spec) {
    int e = ubootcfg_find(&g_cfg, spec);
    if (e < 0) {
        printf("bootcfg: no boot entry \"%s\"; the entries are:\n", spec);
        for (int i = 0; i < g_cfg.count; i++) printf("  %d  %s\n", i, g_cfg.entry[i].title);
    }
    return e;
}

static void diff_line(char sign, const char *s, int n, void *ctx) {
    (void)ctx;
    printf("%s  %c %.*s" C_OFF "\n", sign == '-' ? C_RED : C_GREEN, sign, n, s);
}

static int already(const struct ubootcfg_problem *p, const struct ubootcfg_problem *was, int nwas) {
    for (int i = 0; i < nwas; i++)
        if (was[i].level == p->level && !strcmp(was[i].msg, p->msg)) return 1;
    return 0;
}

// Print the problems `c` has; returns 1 when they block the save.
static int report(const struct ubootcfg *c, const struct ubootcfg *before, const char *where) {
    int nwas = ubootcfg_check(before, 0, UBOOTCFG_CHECK_FILES, g_was, 24);
    int n = ubootcfg_check(c, before, UBOOTCFG_CHECK_FILES, g_now, 24);
    int block = 0, risky = 0;
    for (int i = 0; i < n; i++) {
        int old = already(&g_now[i], g_was, nwas);
        if (g_now[i].line >= 0) printf(C_WARN "bootcfg: %s line %d: %s", where, g_now[i].line + 1, g_now[i].msg);
        else printf(C_WARN "bootcfg: %s", g_now[i].msg);
        printf("%s" C_OFF "\n", old ? C_DIM " (already in the file)" : "");
        if (old) continue;
        if (g_now[i].level == UBOOTCFG_BROKEN) block = 1;
        else risky = 1;
    }
    if (!block && risky && !g_force) {
        printf(C_WARN "         see docs/boot-flags.md; --force writes it anyway" C_OFF "\n");
        block = 1;
    }
    return block;
}

static int save(void) {
    if (g_cfg.len == g_orig.len && !memcmp(g_cfg.text, g_orig.text, (size_t)g_cfg.len)) {
        printf("bootcfg: no change\n");
        return 0;
    }
    if (report(&g_cfg, &g_orig, "grub.cfg")) return 1;
    ubootcfg_diff(g_orig.text, g_orig.len, g_cfg.text, g_cfg.len, diff_line, 0);
    const char *why = 0;
    if (ubootcfg_save(&g_cfg, g_path, &why) < 0) {
        printf("bootcfg: not saved: %s\n", why);
        return 1;
    }
    printf("  saved    %s " C_DIM "(previous copy: %s.bak)" C_OFF "\n", g_path, g_path);
    return 0;
}

static int refused(void) {
    printf("bootcfg: %s\n", g_cfg.why ? g_cfg.why : "refused");
    return 1;
}

static void words_of(const struct ubootcfg_entry *e, char *out, int cap) {
    int n = 0;
    out[0] = 0;
    for (int k = 0; k < e->nwords && n < cap; k++)
        n += snprintf(out + n, (size_t)(cap - n), "%s%s", k ? " " : "", e->word[k]);
    if (!e->plain) snprintf(out, (size_t)cap, "(as text: not plain words)");
    else if (!e->nwords) snprintf(out, (size_t)cap, "-");
}

static int cmd_list(void) {
    static struct ubootmenu m;
    ubootmenu_read(&m, g_path);
    static char line[UBOOTCFG_WORDS * UBOOTCFG_WORD], running[UBOOTCFG_WORDS * UBOOTCFG_WORD];
    int have_running = ubootcfg_cmdline(running, sizeof running) >= 0;
    int t = ubootcfg_trial_find(&g_cfg);
    int booted = t >= 0 && have_running && ubootcfg_trial_booted(running) ? t : -1;
    int tw = 5;
    for (int i = 0; i < g_cfg.count; i++) {
        int l = (int)strlen(g_cfg.entry[i].title);
        if (l > tw) tw = l;
    }
    printf(C_BOLD "  #  %-*s  %-8s %-18s %s" C_OFF "\n", tw, "Entry", "", "Kernel", "Boot words");
    for (int i = 0; i < g_cfg.count; i++) {
        const struct ubootcfg_entry *e = &g_cfg.entry[i];
        const char *mark = i == booted ? "booted" : i == m.next ? "next" : i == g_cfg.def ? "*" : "";
        words_of(e, line, sizeof line);
        printf("  %d  %-*s  %-8s %-18s %s\n", i, tw, e->title, mark, e->kernel[0] ? e->kernel : "-", line);
    }
    printf(C_DIM "  ");
    if (g_cfg.timeout >= 0) printf("timeout %d s    ", g_cfg.timeout);
    printf("* default    one-shot boot: %s" C_OFF "\n", m.oneshot ? "yes (grubenv)" : m.why_not ? m.why_not : "no");
    printf(C_DIM "  this boot's words: %s" C_OFF "\n", have_running ? running : "(none)");
    if (t >= 0 && t == booted)
        printf("  This boot came from the trial.  Keep it:  bootcfg try --keep\n"
               "                                  Drop it:  bootcfg try --drop\n");
    else if (t >= 0)
        printf("  A trial entry is waiting: bootcfg try --keep or --drop\n");
    return 0;
}

static int cmd_check(void) {
    int n = ubootcfg_check(&g_cfg, 0, UBOOTCFG_CHECK_FILES, g_now, 24);
    for (int i = 0; i < n; i++) {
        if (g_now[i].line >= 0) printf("line %d: ", g_now[i].line + 1);
        printf("%s: %s\n", g_now[i].level == UBOOTCFG_BROKEN ? "broken" : "risky", g_now[i].msg);
    }
    if (!n) printf("bootcfg: %s checks out (%d entries)\n", g_path, g_cfg.count);
    return ubootcfg_has(g_now, n, UBOOTCFG_BROKEN) ? 3 : n ? 1 : 0;
}

static int cmd_known(void) {
    for (int i = 0; i < ubootword_count(); i++) {
        const struct ubootword *w = ubootword_at(i);
        printf("  %-14s %-22.22s " C_DIM "%s" C_OFF "\n", w->key, w->hint, w->desc);
    }
    return 0;
}

static int copy_file(const char *from, const char *to) {
    static char buf[UBOOTCFG_MAX];
    int in = open(from, O_RDONLY);
    if (in < 0) return -1;
    long n = read(in, buf, sizeof buf);
    close(in);
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0 || n < 0) { if (out >= 0) close(out); return -1; }
    long w = write(out, buf, (size_t)n);
    close(out);
    return w == n ? 0 : -1;
}

static int ask(const char *prompt) {
    char reply[16];
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(reply, sizeof reply, stdin)) return 0;
    return reply[0];
}

// visudo: edit a copy, check it, show the change, ask.
static int cmd_edit(void) {
    char tmp[64];
    snprintf(tmp, sizeof tmp, "/tmp/grub.cfg.%d", sys_getpid());
    if (copy_file(g_path, tmp) < 0) { printf("bootcfg: cannot copy %s to %s\n", g_path, tmp); return 1; }
    printf(C_DIM "  editing a copy: %s -- the live file is untouched until it checks out" C_OFF "\n", tmp);
    for (;;) {
        char *argv[] = { "edit", tmp, 0 };
        struct sys_spawn_opts o;
        sys_spawn_opts_init(&o);
        o.argv = argv;
        int pid = sys_spawn_opts("/bin/edit", &o);
        int status = 0;
        if (pid < 0 || sys_waitpid(pid, &status) < 0) { printf("bootcfg: cannot run /bin/edit\n"); unlink(tmp); return 1; }
        if (ubootcfg_load(&g_cfg, tmp) < 0) { printf("bootcfg: the copy is unreadable or too big\n"); unlink(tmp); return 1; }
        if (g_cfg.len == g_orig.len && !memcmp(g_cfg.text, g_orig.text, (size_t)g_cfg.len)) {
            printf("bootcfg: no change\n");
            unlink(tmp);
            return 0;
        }
        if (report(&g_cfg, &g_orig, tmp)) {
            int a = ask("What now?  (e) edit again   (q) quit, discard the copy   ");
            if (a == 'e' || a == 'E') continue;
            unlink(tmp);
            return 1;
        }
        ubootcfg_diff(g_orig.text, g_orig.len, g_cfg.text, g_cfg.len, diff_line, 0);
        int a = ask("Save this to the real grub.cfg?  [y/N] ");
        unlink(tmp);
        if (a != 'y' && a != 'Y') { printf("bootcfg: not saved\n"); return 1; }
        // Already checked and shown: write it without a second report.
        const char *why = 0;
        if (ubootcfg_save(&g_cfg, g_path, &why) < 0) { printf("bootcfg: not saved: %s\n", why); return 1; }
        printf("  saved    %s " C_DIM "(previous copy: %s.bak)" C_OFF "\n", g_path, g_path);
        return 0;
    }
}

static int cmd_try(int argc, char **argv) {
    if (g_keep || g_drop) {
        if (argc || (g_keep && g_drop)) return uargs_error(&PROG, "try --keep or try --drop takes nothing else");
        int t = ubootcfg_trial_find(&g_cfg);
        if (t < 0) { printf("bootcfg: there is no trial entry\n"); return 1; }
        int rc = g_keep ? ubootcfg_keep(&g_cfg, t) : ubootcfg_remove(&g_cfg, t);
        return rc < 0 ? refused() : save();
    }
    if (argc < 1) return wrong("try");
    int e = entry_arg(argv[0]);
    if (e < 0) return 1;
    // The trial starts from the entry's own words, edited like `words`.
    static struct ubootcfg scratch;
    scratch = g_cfg;
    if (ubootcfg_edit_words(&scratch, e, (const char *const *)argv + 1, argc - 1) < 0) {
        g_cfg.why = scratch.why;
        return refused();
    }
    const char *w[UBOOTCFG_WORDS];
    for (int i = 0; i < scratch.entry[e].nwords; i++) w[i] = scratch.entry[e].word[i];
    int t = ubootcfg_try(&g_cfg, e, w, scratch.entry[e].nwords);
    if (t < 0) return refused();
    static struct ubootmenu m;
    ubootmenu_read(&m, g_path);
    if (!m.oneshot) {
        printf("bootcfg: this machine cannot boot an entry once: %s\n", m.why_not);
        return 1;
    }
    if (save()) return 1;
    if (ubootmenu_set_next(g_cfg.entry[t].title) < 0) {
        printf("bootcfg: could not choose the next boot -- the trial entry is saved but unchosen\n");
        return 1;
    }
    printf("  next boot only: \"%s\" " C_DIM "-- the boot after is the default again" C_OFF "\n",
           g_cfg.entry[t].title);
    int a = ask("  Restart now?  [y/N] ");
    if (a == 'y' || a == 'Y') return system("/bin/reboot") != 0;
    return 0;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    const char *cmd = a.argc ? a.argv[0] : "list";
    char **rest = a.argv + 1;
    int nrest = a.argc ? a.argc - 1 : 0;
    if ((g_keep || g_drop) && strcmp(cmd, "try")) return uargs_error(&PROG, "--keep and --drop go with try");

    if (!strcmp(cmd, "known")) return nrest ? wrong(cmd) : cmd_known();
    if (!strcmp(cmd, "undo")) {
        if (nrest) return wrong(cmd);
        const char *why = 0;
        static struct ubootcfg before;
        if (ubootcfg_load(&before, g_path) < 0) before.len = 0;
        if (ubootcfg_undo(g_path, &why) < 0) { printf("bootcfg: %s\n", why); return 1; }
        if (load() < 0) return 1;
        ubootcfg_diff(before.text, before.len, g_cfg.text, g_cfg.len, diff_line, 0);
        printf("  restored %s.bak " C_DIM "(the replaced file is the .bak now: undo again to redo)" C_OFF "\n", g_path);
        return 0;
    }
    if (load() < 0) return 1;

    if (!strcmp(cmd, "list"))  return nrest ? wrong(cmd) : cmd_list();
    if (!strcmp(cmd, "check")) return nrest ? wrong(cmd) : cmd_check();
    if (!strcmp(cmd, "edit"))  return nrest ? wrong(cmd) : cmd_edit();
    if (!strcmp(cmd, "try"))   return cmd_try(nrest, rest);

    int rc;
    if (!strcmp(cmd, "words") && nrest >= 2) {
        int e = entry_arg(rest[0]);
        if (e < 0) return 1;
        rc = ubootcfg_edit_words(&g_cfg, e, (const char *const *)rest + 1, nrest - 1);
    } else if (!strcmp(cmd, "default") && nrest == 1) {
        int e = entry_arg(rest[0]);
        if (e < 0) return 1;
        rc = ubootcfg_set_default(&g_cfg, e);
    } else if (!strcmp(cmd, "timeout") && nrest == 1) {
        unsigned long long secs;
        if (!cmd_parse_count(rest[0], &secs) || secs > 3600) {
            printf("bootcfg: \"%s\" is not a number of seconds (0 to 3600)\n", rest[0]);
            return 1;
        }
        rc = ubootcfg_set_timeout(&g_cfg, (int)secs);
    } else if (!strcmp(cmd, "copy") && nrest >= 2) {
        int e = entry_arg(rest[0]);
        if (e < 0) return 1;
        int t = ubootcfg_copy(&g_cfg, e, rest[1]);
        rc = t < 0 ? -1 : nrest > 2 ? ubootcfg_edit_words(&g_cfg, t, (const char *const *)rest + 2, nrest - 2) : 0;
    } else if (!strcmp(cmd, "rename") && nrest == 2) {
        int e = entry_arg(rest[0]);
        if (e < 0) return 1;
        rc = ubootcfg_set_title(&g_cfg, e, rest[1]);
    } else if (!strcmp(cmd, "remove") && nrest == 1) {
        int e = entry_arg(rest[0]);
        if (e < 0) return 1;
        rc = ubootcfg_remove(&g_cfg, e);
    } else {
        return wrong(cmd);
    }
    return rc < 0 ? refused() : save();
}
