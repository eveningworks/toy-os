// `rescue <cmd> [args...]` -- the kernel's own copies of the file
// commands, for when /bin is damaged.
//
// WHY THIS EXISTS AT ALL. Every ordinary file command at this prompt is
// now a ring-3 program: `rm` runs /bin/rm, `cat` runs /bin/cat. That is
// what a Unix shell does, and it is what stops one command having two
// implementations that drift. But it means a disk whose /bin is empty
// or damaged leaves the shell unable to so much as LIST a directory --
// and a mounted filesystem with no working tools is a bad place to
// stand.
//
// WHY IT IS ONE COMMAND RATHER THAN ELEVEN. The precedent is `sash`
// (the stand-alone shell, which spells its built-in copies `-ls`,
// `-rm`) and busybox (one binary, many applets). Both make the same two
// guarantees this does: a rescue copy can NEVER shadow the real program
// -- you always know which one ran -- and the rescue set is visible AS
// A SET. The second is why one command won over eleven `-` names here:
// the whole point of moving these out of ring 0 is that kernel-side
// file code should be a small bounded thing, and a table in one file is
// something you can look at and ask "is this still small?". Eleven
// entries scattered through dispatch()'s chain is not.
//
// WHAT IT DOES NOT DO. It cannot REPAIR a missing /bin -- there is no
// way to write an ELF back onto the disk from a shell. Recovery is
// booting toy-os-live.iso (which carries a TFS3 image as a GRUB module)
// or re-seeding from the host, the same answer a real system gives. Its
// job is DIAGNOSIS -- see what is on the disk, and make the small
// changes that let a boot get further -- plus `sync`, so anything it
// does change actually reaches the platters.
//
// AND IT IS A TABLE, not a chain, because that is this project's rule
// for dispatch (tools/check_dispatch.py) and because the table IS the
// documentation: adding a row is the only way to grow the rescue set,
// which is exactly the friction wanted here.
#include "shell.h"
#include "shell_internal.h"

// fs_list() takes a plain callback with no context argument, so the
// column state has to be file-scope. Only ever touched by rescue_ls()
// and its callback, which cannot run concurrently -- the shell is one
// thread and fs_list() does not yield to it.
static int g_ls_count;

static void rescue_ls_cb(const char *name, uint32_t size, int is_dir) {
    g_ls_count++;
    vga_write("  ");
    vga_write(name);
    if (is_dir) {
        vga_write("/\n");
        return;
    }
    // Deliberately plainer than /bin/ls: no columns, no sort, no flags.
    // A rescue listing that reimplemented ls would be the duplicate
    // this whole change exists to delete -- name and size is what
    // answers "is the file there, and is it empty?", which is the
    // question you have in this situation.
    vga_write("  ");
    vga_write_dec(size);
    vga_write(" bytes\n");
}

// Not a copy of /bin/ls. This is the one rescue command with no ring-0
// implementation left to reuse: the kernel shell has not listed a
// directory itself since `ls` became a /bin program, and the builtin
// wrapper that used to invoke that program is gone too. So this is
// written here as the minimum that answers the question -- and it is
// now the ONLY way to see a directory when /bin is damaged, which
// raises its stakes rather than lowering them.
static void rescue_ls(const char *args) {
    char path[FS_PATH_MAX];
    if (!resolve_path((args && k_strlen(args) > 0) ? args : 0, path)) {
        vga_write("rescue ls: path too long\n");
        return;
    }
    if (!fs_is_dir(path)) {
        vga_write("rescue ls: not a directory: ");
        vga_write(path);
        vga_putc('\n');
        return;
    }
    vga_write(path);
    vga_write(":\n");
    g_ls_count = 0;
    fs_list(path, rescue_ls_cb);
    // An empty directory and a missing one read identically otherwise,
    // and telling them apart is most of the point of running this.
    if (g_ls_count == 0) vga_write("  (empty)\n");
}

// cmd_df() takes no arguments; every other entry takes the argument
// string. One adapter rather than two shapes in the table, so the
// dispatch below has no special case in it.
static void rescue_df(const char *args) {
    (void)args;
    cmd_df();
}

struct rescue_cmd {
    const char *name;
    void (*fn)(const char *args);
    const char *usage;
};

// THE RESCUE SET. Everything here is a kernel-side implementation that
// also exists as a /bin program -- except `ls`, whose ring-0 copy lives
// above. Keep it short: a row here is ring-0 code that has to be
// maintained forever, and the /bin version is the one that should be
// getting the features.
// cmd_dmesg() takes no arguments and every rescue entry is called with
// them, so this is the adapter and nothing more. Arguments are IGNORED
// rather than refused: `rescue dmesg -n 40` should show the log, not a
// usage error, because the flags belong to /bin/dmesg and this is the
// copy you reach for when /bin is what is broken.
static void rescue_dmesg(const char *args) {
    (void)args;
    cmd_dmesg();
}

static const struct rescue_cmd RESCUE_CMDS[] = {
    { "ls",       rescue_ls,     "rescue ls [dir]" },
    { "cat",      cmd_cat,       "rescue cat <file>" },
    { "stat",     cmd_stat,      "rescue stat <path>" },
    { "df",       rescue_df,     "rescue df" },
    { "rm",       cmd_rm,        "rescue rm <path>" },
    { "touch",    cmd_touch,     "rescue touch <file>" },
    { "mkdir",    cmd_mkdir,     "rescue mkdir <dir>" },
    { "mv",       cmd_mv,        "rescue mv <from> <to>" },
    { "ln",       cmd_ln,        "rescue ln <file> <new>" },
    { "truncate", cmd_truncate,  "rescue truncate <file> <bytes>" },
    // sync is in the set for a reason the others are not: without it a
    // rescue edit sits in the write-back cache and a reset loses it.
    // `rescue rm` that does not survive the reboot is not a rescue.
    { "sync",     cmd_sync,      "rescue sync" },
    // dmesg is here for a different reason from the file commands, and
    // it is worth being explicit about the stretch. The set is nominally
    // "what you need to put /bin BACK" -- which is why `strace` went to
    // /bin and not here -- but this file's own opening says its job is
    // DIAGNOSIS, and the kernel log is the first thing you want when a
    // boot is going wrong. A machine whose /bin will not load is exactly
    // a machine that cannot run /bin/dmesg to find out why.
    //
    // It costs nothing to keep: the implementation is klog_dump() plus a
    // pager that already existed, so this is a table row rather than a
    // second copy of anything.
    { "dmesg",    rescue_dmesg,  "rescue dmesg" },
};
#define RESCUE_COUNT (sizeof(RESCUE_CMDS) / sizeof(RESCUE_CMDS[0]))

// dispatch() asks this when a name resolved to nothing, so a missing
// /bin/<name> is reported as a missing PROGRAM rather than as an
// unknown command. `df` is in the table below, so `df` with no /bin/df
// is told about `rescue df`; `meminfo` is not, and gets the ordinary
// unknown-command message -- which is correct, since there is no kernel
// copy of it to point at any more.
int shell_rescue_has(const char *name) {
    if (!name) return 0;
    for (unsigned i = 0; i < RESCUE_COUNT; i++) {
        if (k_strcmp(RESCUE_CMDS[i].name, name) == 0) return 1;
    }
    return 0;
}

static void rescue_list(void) {
    vga_write("rescue -- the kernel's own copies of the file commands,\n");
    vga_write("for when /bin is missing or damaged. These never shadow a\n");
    vga_write("real program: plain `rm` always runs /bin/rm.\n\n");
    for (unsigned i = 0; i < RESCUE_COUNT; i++) {
        vga_write("  ");
        vga_write(RESCUE_CMDS[i].usage);
        vga_putc('\n');
    }
    vga_write("\nThey diagnose; they cannot put /bin back. For that, boot\n");
    vga_write("toy-os-live.iso or re-seed the image from the host.\n");
}

void cmd_rescue(const char *args) {
    if (!args || k_strlen(args) == 0) {
        rescue_list();
        return;
    }

    // Same first-word/rest split dispatch() does, needed again because
    // `rescue`'s arguments arrive as one string.
    char name[LINE_MAX];
    k_strlcpy(name, args, sizeof name);
    char *rest = name;
    while (*rest && *rest != ' ') rest++;
    if (*rest == ' ') {
        *rest = '\0';
        rest++;
        while (*rest == ' ') rest++;
        if (*rest == '\0') rest = 0;
    } else {
        rest = 0;
    }

    for (unsigned i = 0; i < RESCUE_COUNT; i++) {
        if (k_strcmp(RESCUE_CMDS[i].name, name) != 0) continue;
        RESCUE_CMDS[i].fn(rest);
        return;
    }

    vga_write("rescue: no such rescue command: ");
    vga_write(name);
    vga_write("\n(`rescue` alone lists them)\n");
}
