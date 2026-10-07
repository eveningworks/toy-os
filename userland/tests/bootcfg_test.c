// The editable grub.cfg model behind `bootcfg`, the Boot Manager and the
// Boot menu settings page (lib/ubootcfg.h), on fixture text and files in /tmp.
//
// WHAT A BROKEN VERSION WOULD STILL PASS, which is what shaped this:
//
//   - An edit that rewrote the whole file from the model would pass
//     every "the entry says X now" check and lose the comments and the
//     one-shot stanza; copy-then-remove must give back the ORIGINAL
//     BYTES, and a words edit must change exactly one line.
//   - A brace counter that ignored quotes would pass a plain fixture;
//     one title holds a `{` and an escaped quote.
//   - A refused two-step edit (a trial with a bad word) could leave its
//     first step behind and still report failure; the text is compared.
//   - A save checked only by reloading proves nothing about the .bak;
//     both files are read back directly, and undo is run twice.
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "lib/ubootcfg.h"
#include "lib/ubootwords.h"
#include "lib/utest.h"

#define CFG "/tmp/bootcfg_test.cfg"

static const char FIXTURE[] =
    "# toy-os grub.cfg\n"
    "set timeout=5\n"
    "set default=1\n"
    "if [ -s $prefix/grubenv ]; then\n"
    "    load_env\n"
    "fi\n"
    "if [ \"${next_entry}\" ]; then\n"
    "    set default=\"${next_entry}\"\n"
    "    set next_entry=\n"
    "    save_env next_entry\n"
    "fi\n"
    "menuentry \"rescue {old} \\\"x\\\"\" {\n"
    "    multiboot2 /boot/kernel.old\n"
    "    boot\n"
    "}\n"
    "\n"
    "menuentry \"toy-os\" --class os {\n"
    "    # Boot words: see docs/boot-flags.md\n"
    "    multiboot2 /boot/kernel.bin nokaslr video=1920x1080\n"
    "    boot\n"
    "}\n"
    "\n"
    "menuentry \"quoted\" {\n"
    "    multiboot2 /boot/kernel.bin \"loglevel=7\"\n"
    "    boot\n"
    "}\n";

static int g_minus, g_plus;
static char g_last_plus[256];
static void count_line(char sign, const char *s, int n, void *ctx) {
    (void)ctx;
    if (sign == '-') g_minus++;
    else {
        g_plus++;
        snprintf(g_last_plus, sizeof g_last_plus, "%.*s", n, s);
    }
}

static void diff(const struct ubootcfg *a, const struct ubootcfg *b) {
    g_minus = g_plus = 0;
    g_last_plus[0] = 0;
    ubootcfg_diff(a->text, a->len, b->text, b->len, count_line, 0);
}

static int get(const char *path, char *buf, int cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = 0;
    long r;
    while (n < cap - 1 && (r = read(fd, buf + n, (size_t)(cap - 1 - n))) > 0) n += (int)r;
    close(fd);
    buf[n] = 0;
    return n;
}

static int has(const struct ubootcfg_problem *p, int n, int level, const char *needle) {
    for (int i = 0; i < n; i++)
        if (p[i].level == level && strstr(p[i].msg, needle)) return 1;
    return 0;
}

static struct ubootcfg orig, c, other;
static struct ubootcfg_problem probs[16];

int main(void) {
    utest_begin("bootcfg_test", "the editable grub.cfg model", 0);
    int flen = (int)sizeof FIXTURE - 1;

    utest_check(ubootcfg_parse(&orig, FIXTURE, flen) == 0, "the fixture parses");
    utest_checkf(orig.count == 3, "three entries (got %d)", orig.count);
    utest_check(!strcmp(orig.entry[0].title, "rescue {old} \"x\""),
                "a title with a brace and escaped quotes is read whole");
    utest_check(orig.open_line < 0 && orig.stray_line < 0 && orig.quote_line < 0,
                "and its brace does not unbalance the file");
    utest_checkf(orig.def == 1 && orig.timeout == 5 && orig.oneshot,
                 "default 1, timeout 5, the one-shot stanza (got %d, %d, %d)",
                 orig.def, orig.timeout, orig.oneshot);
    utest_check(orig.entry[1].plain && orig.entry[1].nwords == 2 &&
                !strcmp(orig.entry[1].kernel, "/boot/kernel.bin") &&
                !strcmp(orig.entry[1].word[1], "video=1920x1080"), "kernel and words are read");
    utest_check(!orig.entry[2].plain, "a quoted word makes a boot line text-only");

    // Words: exactly one line changes, a key replaces its old value.
    c = orig;
    const char *ops[] = { "+video=1280x720", "-nokaslr", "+loglevel=4" };
    utest_check(ubootcfg_edit_words(&c, 1, ops, 3) == 0, "a words edit is accepted");
    diff(&orig, &c);
    utest_checkf(g_minus == 1 && g_plus == 1 && !strcmp(g_last_plus,
                 "    multiboot2 /boot/kernel.bin video=1280x720 loglevel=4"),
                 "one line changed, to the replaced words (-%d +%d: \"%s\")", g_minus, g_plus, g_last_plus);
    utest_check(strstr(c.text, "# Boot words: see docs/boot-flags.md") != 0, "the comment survives");
    const char *qw[] = { "nokaslr" };
    other = c;
    utest_check(ubootcfg_set_words(&c, 2, qw, 1) < 0 && c.why && !strcmp(c.text, other.text),
                "a text-only boot line is refused, unchanged");

    // Copy then remove: the original bytes back.
    c = orig;
    int t = ubootcfg_copy(&c, 0, "rescue copy");
    utest_checkf(t == 1 && c.count == 4 && !strcmp(c.entry[1].title, "rescue copy") &&
                 !strcmp(c.entry[1].kernel, "/boot/kernel.old"),
                 "a copy lands after its source with the body (t=%d)", t);
    utest_checkf(c.def == 2 && !strcmp(c.entry[c.def].title, "toy-os"),
                 "the numeric default follows its entry past the copy (def=%d)", c.def);
    utest_check(ubootcfg_remove(&c, 1) == 0 && c.len == orig.len && !memcmp(c.text, orig.text, (size_t)c.len),
                "copy then remove gives back the original bytes");
    utest_check(ubootcfg_remove(&c, 1) < 0 && c.len == orig.len, "the default entry cannot be removed");

    // Title, default, timeout.
    c = orig;
    utest_check(ubootcfg_set_title(&c, 1, "toy-os main") == 0 &&
                strstr(c.text, "menuentry \"toy-os main\" --class os {\n") != 0,
                "a rename keeps the entry's options");
    utest_check(ubootcfg_set_title(&c, 0, "toy-os main") < 0 && ubootcfg_set_title(&c, 0, "a\"b") < 0 &&
                ubootcfg_set_title(&c, 0, "12") < 0, "a duplicate, quoted or numeric title is refused");
    utest_check(ubootcfg_set_default(&c, 0) == 0 && c.def == 0 && strstr(c.text, "\nset default=0\n"),
                "the default is rewritten in place");
    utest_check(ubootcfg_set_timeout(&c, 0) == 0 && c.timeout == 0, "a timeout is rewritten");
    int n = ubootcfg_check(&c, &orig, 0, probs, 16);
    utest_check(has(probs, n, UBOOTCFG_RISKY, "timeout 0") && !ubootcfg_has(probs, n, UBOOTCFG_BROKEN),
                "timeout 0 is risky, not broken");

    // Problems.
    static const char broken[] =
        "set timeout=soon\n"
        "menuentry \"a\" {\n"
        "    multiboot2 /boot/kernel.bin vidoe=1280x720 frobnicate\n"
        "    boot\n";
    ubootcfg_parse(&c, broken, (int)sizeof broken - 1);
    n = ubootcfg_check(&c, &orig, 0, probs, 16);
    utest_check(has(probs, n, UBOOTCFG_BROKEN, "never closed") && probs[0].level == UBOOTCFG_BROKEN,
                "an unclosed entry is broken, and broken comes first");
    utest_check(has(probs, n, UBOOTCFG_BROKEN, "not a number"), "a malformed timeout is broken");
    utest_check(has(probs, n, UBOOTCFG_RISKY, "did you mean \"video=\""), "a misspelt word gets a suggestion");
    utest_check(has(probs, n, UBOOTCFG_RISKY, "\"frobnicate\" is not a boot word"), "an unknown word is named");
    utest_check(has(probs, n, UBOOTCFG_RISKY, "next_entry stanza is gone"), "losing the stanza is noticed");
    utest_check(ubootword_find("video=1920x1080") && !ubootword_find("video=") &&
                ubootword_find("debugcon") && ubootword_find("nokaslr") && !ubootword_find("nokaslrx"),
                "the generated word table matches keys and values");

    // Trials.
    c = orig;
    const char *tw[] = { "nokaslr", "clocksource=tsc" };
    t = ubootcfg_try(&c, 1, tw, 2);
    utest_checkf(t == 2 && !strcmp(c.entry[2].title, "toy-os (trial)") && c.entry[2].nwords == 3 &&
                 !strcmp(c.entry[2].word[2], UBOOTCFG_MARK),
                 "a trial is a marked copy after its source (t=%d)", t);
    utest_check(ubootcfg_trial_find(&c) == 2 && ubootcfg_trial_source(&c, 2) == 1, "and is found as one");
    const char *tw2[] = { "nohz=off" };
    utest_check(ubootcfg_try(&c, 1, tw2, 1) == 2 && c.count == 4 && c.entry[2].nwords == 2,
                "trying again replaces the trial rather than adding one");
    other = c;
    const char *bad[] = { "a b" };
    utest_check(ubootcfg_try(&c, 1, bad, 1) < 0 && !strcmp(c.text, other.text),
                "a refused trial changes nothing");
    utest_check(ubootcfg_keep(&c, 2) == 0 && c.count == 3 && c.entry[1].nwords == 1 &&  // mark stripped
                !strcmp(c.entry[1].word[0], "nohz=off") && ubootcfg_trial_find(&c) < 0,
                "keep moves the words to the source and removes the trial");

    // A trial boot is told by its mark, not by matching words.
    utest_check(ubootcfg_trial_booted("debugcon " UBOOTCFG_MARK) && ubootcfg_trial_booted(UBOOTCFG_MARK) &&
                !ubootcfg_trial_booted("x" UBOOTCFG_MARK) && !ubootcfg_trial_booted(UBOOTCFG_MARK "x") &&
                !ubootcfg_trial_booted("debugcon"), "a trial boot is the mark as a whole word");

    // Save and undo, on /tmp.
    const char *why = 0;
    static char back[UBOOTCFG_MAX];
    ubootcfg_parse(&c, FIXTURE, flen);
    unlink(CFG); unlink(CFG ".bak");
    int fd = open(CFG, O_WRONLY | O_CREAT | O_TRUNC);
    write(fd, "old\n", 4);
    close(fd);
    utest_check(ubootcfg_save(&c, CFG, &why) == 0, "a save succeeds");
    utest_check(get(CFG, back, sizeof back) == flen && !memcmp(back, FIXTURE, (size_t)flen),
                "the file holds the new text");
    utest_check(get(CFG ".bak", back, sizeof back) == 4 && !strcmp(back, "old\n"), "the old one is the .bak");
    utest_check(get(CFG ".new", back, sizeof back) < 0, "no .new is left behind");
    utest_check(ubootcfg_undo(CFG, &why) == 0 && get(CFG, back, sizeof back) == 4, "undo puts the old file back");
    utest_check(ubootcfg_undo(CFG, &why) == 0 && get(CFG, back, sizeof back) == flen, "and a second undo redoes");

    unlink(CFG); unlink(CFG ".bak");

    // bootpart=: ONE final `$name` keeps a line editable, and survives
    // an edit of its neighbours; anything more stays text-only.
    static const char VARS[] =
        "menuentry \"a\" {\n    multiboot2 /boot/kernel.bin bootpart=$bootpart nokaslr\n    boot\n}\n"
        "menuentry \"b\" {\n    multiboot2 /boot/kernel.bin x=${y}\n    boot\n}\n"
        "menuentry \"c\" {\n    multiboot2 /boot/kernel.bin x=$y-z\n    boot\n}\n";
    utest_check(ubootcfg_parse(&c, VARS, (int)sizeof VARS - 1) == 0, "the variables fixture parses");
    utest_check(c.entry[0].plain && c.entry[0].nwords == 2 &&
                !strcmp(c.entry[0].word[0], "bootpart=$bootpart"), "a final $name is a plain word");
    utest_check(!c.entry[1].plain && !c.entry[2].plain, "${y} and $y-z are text-only");
    const char *nk[] = { "-nokaslr", "+video=1280x720" };
    utest_check(ubootcfg_edit_words(&c, 0, nk, 2) == 0 &&
                strstr(c.text, "kernel.bin bootpart=$bootpart video=1280x720\n"),
                "an edit keeps bootpart=$bootpart verbatim");
    utest_check(ubootcfg_bootpart_missing(&c) == 0, "an entry with the word is not missing it");

    // Adding it: the two editable multiboot2 entries gain the probe above
    // the boot line and the word on it; the quoted one and a `linux` one
    // are left as they are; a second pass changes nothing.
    static char withlinux[sizeof FIXTURE + 128];
    int wl = snprintf(withlinux, sizeof withlinux, "%s%s", FIXTURE,
                      "menuentry \"other\" {\n    linux /vmlinuz root=/dev/sda2\n}\n");
    utest_check(ubootcfg_parse(&orig, withlinux, wl) == 0, "the fixture with a linux entry parses");
    c = orig;
    utest_checkf(ubootcfg_bootpart_missing(&c) == 2, "two entries lack bootpart= (got %d)",
                 ubootcfg_bootpart_missing(&c));
    utest_checkf(ubootcfg_add_bootpart(&c) == 2, "both gain it (%s)", c.why ? c.why : "");
    utest_check(strstr(c.text,
                "    set bootpart=\n"
                "    if probe --part-uuid --set=bootpart $root; then true; fi\n"
                "    multiboot2 /boot/kernel.bin nokaslr video=1920x1080 bootpart=$bootpart\n") != 0,
                "the probe sits above the boot line, the word at its end");
    utest_check(strstr(c.text, "multiboot2 /boot/kernel.old bootpart=$bootpart\n") != 0,
                "the rescue entry gains it too");
    utest_check(strstr(c.text, "\"loglevel=7\"\n") && strstr(c.text, "linux /vmlinuz root=/dev/sda2\n"),
                "the quoted and the linux entries are untouched");
    utest_checkf(c.count == 4 && c.def == 1, "entries and the default are unchanged (%d, %d)",
                 c.count, c.def);
    int np = ubootcfg_check(&c, &orig, 0, probs, 16);
    utest_check(!ubootcfg_has(probs, np, UBOOTCFG_BROKEN), "and the result checks out");
    other = c;
    utest_check(ubootcfg_add_bootpart(&c) == 0 && !strcmp(c.text, other.text),
                "a second pass changes nothing");
    return utest_end();
}
