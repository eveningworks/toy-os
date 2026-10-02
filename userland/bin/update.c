// update -- bring this machine up to date from an update server.
//
// A FRONT END over userland/update/upd.c, which the System Update window
// shares; what it prints IS the log (/var/log/update.log), so running it
// over `remote.py exec` shows exactly what the window would have.
// docs/commands/update.md has the usage, docs/update-design.md the why.
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/human.h"
#include "lib/umd.h"
#include "lib/upager.h"
#include "update/upd.h"

#define USAGE "update [--check] [-v] [--from <url>] | --server [<url>] | --log"

struct cli {
    int tty;          // redraw a live line for the file being fetched
    int drawn;        // that line is on screen and must be cleared first
    int verbose;
    long last_ms;     // when it was last redrawn: a few times a second is plenty
};

static long now_ms(void) { return (long)(sys_monotonic_ns() / 1000000ULL); }

static void clear_live(struct cli *c) {
    if (c->drawn) { fputs("\r\033[K", stdout); c->drawn = 0; }
}

static void on_log(void *ctx, const char *line) {
    struct cli *c = ctx;
    clear_live(c);
    printf("%s\n", line);
    fflush(stdout);
}

static void on_progress(void *ctx, const struct upd_plan *p, int idx) {
    struct cli *c = ctx;
    if (!c->tty || idx < 0) return;
    const struct upd_file *f = &p->files[idx];
    if (f->status != UPD_FETCHING) return;
    long t = now_ms();
    if (c->drawn && t - c->last_ms < 250 && f->got != f->size) return;
    c->last_ms = t;
    char got[16], all[16];
    human_size(got, sizeof got, p->done_bytes);
    human_size(all, sizeof all, p->bytes);
    int pct = f->size ? (int)(f->got * 100 / f->size) : 100;
    printf("\r\033[K  %s  %d%%   (%s of %s)", f->path, pct, got, all);
    fflush(stdout);
    c->drawn = 1;
}

static void list_changes(const struct upd_plan *p, int verbose) {
    int shown = 0;
    for (int i = 0; i < p->count; i++) {
        const struct upd_file *f = &p->files[i];
        if (f->change != UPD_CHANGED && f->change != UPD_NEW && f->change != UPD_REMOVE) continue;
        if (!verbose && shown == 10) {
            printf("  ... %d more (update --check -v)\n", p->changed + p->removals - shown);
            return;
        }
        if (f->change == UPD_REMOVE) {
            printf("  %-34s %7s  remove (no longer shipped)\n", f->path, "");
            shown++;
            continue;
        }
        char sz[16];
        human_size(sz, sizeof sz, f->size);
        printf("  %-34s %7s%s%s\n", f->path, sz, f->change == UPD_NEW ? "  new" : "",
               (f->flags & (UPD_F_KERNEL | UPD_F_KERNEL_GZ)) ? "  (restart needed)" : "");
        shown++;
    }
}

// The engine's notes are Markdown; umd wraps them to this terminal, as
// /bin/doc does a page.
static void show_notes(const struct upd_plan *p, int color) {
    if (!p->notes) return;
    int cols = 80;
    upager_term(NULL, &cols);
    static char buf[65536];
    struct umd_opts mo = { .cols = cols, .color = color, .indent = 2 };
    struct umd_out o = { buf, (int)sizeof buf, 0, 0 };
    umd_render(p->notes, (int)strlen(p->notes), &mo, &o);
    printf("\nWhat's new\n");
    fwrite(buf, 1, (size_t)o.len, stdout);
    if (o.overflow) printf("  ... (cut short)\n");
    printf("\n");
}

static int show_log(void) {
    int fd = open(UPD_LOG_PATH, O_RDONLY);
    if (fd < 0) { printf("update: no log yet (%s)\n", UPD_LOG_PATH); return 1; }
    char buf[1024];
    long n;
    while ((n = read(fd, buf, sizeof buf)) > 0) fwrite(buf, 1, (size_t)n, stdout);
    close(fd);
    return 0;
}

static int server_cmd(const char *url) {
    char cur[UPD_URL_MAX];
    if (!url) {
        printf("%s\n", upd_server_get(cur, sizeof cur));
        static char recent[UPD_RECENT_MAX][UPD_URL_MAX];
        int n = upd_recent(recent, UPD_RECENT_MAX);
        for (int i = 0; i < n; i++) printf("  recent: %s\n", recent[i]);
        return 0;
    }
    if (upd_server_set(url) != 0) {
        printf("update: refused %s -- an http:// address, under 64 characters\n", url);
        return 1;
    }
    printf("update: server is now %s\n", url);
    return 0;
}

int main(int argc, char **argv) {
    int check_only = 0, verbose = 0;
    const char *from = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--check") || !strcmp(argv[i], "-n")) check_only = 1;
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "--from") && i + 1 < argc) from = argv[++i];
        else if (!strcmp(argv[i], "--server")) return server_cmd(i + 1 < argc ? argv[i + 1] : 0);
        else if (!strcmp(argv[i], "--log")) return show_log();
        else { cmd_usage(USAGE); return 1; }
    }

    char server[UPD_URL_MAX];
    if (from) snprintf(server, sizeof server, "%s", from);
    else upd_server_get(server, sizeof server);

    struct cli c = { .tty = isatty(1), .verbose = verbose };
    struct upd_hooks h = { .ctx = &c, .log = on_log, .progress = on_progress };
    struct upd_plan p;
    if (upd_check(server, &p, &h) != 0) { upd_plan_free(&p); return 1; }

    if (p.staged_for_boot)
        printf("update: an update is already staged -- restart to finish it\n");
    if (p.kernel_blocked)
        printf("update: nothing can be installed until GRUB shows a menu "
               "(docs/commands/update.md)\n");
    if (!p.changed && !p.removals) {
        printf("update: up to date\n");
        upd_plan_free(&p);
        return 0;
    }
    show_notes(&p, c.tty);
    char total[16];
    human_size(total, sizeof total, p.bytes);
    printf("%d file%s to fetch, %s", p.changed, p.changed == 1 ? "" : "s", total);
    if (p.removals) printf("; %d stale to remove", p.removals);
    printf(":\n");
    list_changes(&p, verbose || !check_only);
    if (check_only || p.staged_for_boot) { upd_plan_free(&p); return 0; }

    int rc = upd_apply(&p, &h);
    clear_live(&c);
    if (p.at_boot || p.kernel_installed)
        printf("update: restart to finish (`reboot`)\n");
    upd_plan_free(&p);
    return rc == 0 ? 0 : 1;
}
