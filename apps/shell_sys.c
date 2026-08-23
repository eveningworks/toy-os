// System-info/settings shell commands: help/timezone/dmesg/apps/run/
// fontsize/keyboard/color/history, plus the kernel-introspection set
// (heap, kstack, ata, debug, ktest and the *test diagnostics). Most of
// what this file used to hold is /bin programs now -- see
// docs/conventions/shell.md. Split out of shell.c once it crossed 900 lines mixing every
// command category together -- see shell_internal.h's top comment for
// the split's own reasoning and the git history for the build this
// happened in. Shares `shell_fg`/history[]/history_count with shell.c
// (and shell_fs.c) via shell_internal.h.
#include "shell_internal.h"
#include "shell.h" // shell_path_find()
#include "apps.h"

// Prints `lines` one at a time (each expected to be one console line,
// i.e. end in '\n'), pausing with a "-- more --" prompt whenever a
// screenful has gone by, rather than printing everything at once and
// letting the console's own scroll-off-the-top behavior silently
// discard whatever didn't fit (which is what used to happen to the
// tail end of `help`'s ~44 lines against a 32-row screen at the
// default font size). Any key continues to the next page; 'q'/'Q'
// stops early. General enough for any future command whose output
// might outgrow one screen -- not just `help`.
static void console_page(const char *const *lines, uint32_t count) {
    // A sink means this might be running inside a non-blocking GUI
    // callback (see apps/terminal.c, phase 4) rather than the
    // interactive console loop's own blocking read -- keyboard_getchar()
    // below would hang whatever's driving that callback forever. Just
    // dump everything unpaginated instead; a GUI caller's own scrollback
    // widget (phase 3) is what handles "doesn't fit on one screen" in
    // that context, the same job this pagination does for the physical
    // console.
    if (vga_sink_active()) {
        for (uint32_t i = 0; i < count; i++) vga_write(lines[i]);
        return;
    }

    uint32_t rows = vga_rows();
    uint32_t page_rows = rows > 1 ? rows - 1 : rows; // reserve the bottom row for the prompt
    uint32_t shown = 0;

    for (uint32_t i = 0; i < count; i++) {
        vga_write(lines[i]);
        shown++;
        if (shown >= page_rows && i + 1 < count) {
            vga_write("-- more (press any key, 'q' to quit) --");
            int key = keyboard_getchar();
            vga_write("\n");
            if (key == 'q' || key == 'Q') return;
            shown = 0;
        }
    }
}

// Split in two (see the git history for the request/reasoning): HELP_LINES
// is what a day-to-day user actually needs, grouped under headers
// rather than one flat 50-line list. TEST_HELP_LINES holds the ring-3/
// syscall/scheduler diagnostic commands -- genuinely useful (they're
// how this kernel proves its own isolation/syscall/scheduler claims,
// see README), but not something you need in front of you to use the
// shell day to day, so they're one level down behind `help tests`
// rather than mixed into the main list. Both go through console_page()
// (see its own comment) for pagination, same as before the split.
static const char *const HELP_LINES[] = {
    "toy-os shell -- available commands:\n",
    "\n",
    "General:\n",
    "  help          - show this list ('help tests' for developer/\n",
    "                  diagnostic test commands)\n",
    "  clear         - clear the screen\n",
    "  beep          - a short test tone via the PC speaker\n",
    "  apps          - list all registered apps\n",
    "  <name> [args] - run an executable: a console app, or a binary\n",
    "                  found by searching PATH (see `path`). The `run`\n",
    "                  prefix below is optional -- `nx_test` and\n",
    "                  `run nx_test` do exactly the same thing.\n",
    "  run <app>     - launch an app by name (explicit form of the above)\n",
    "  path          - show the directories executables are searched in,\n",
    "                  in order (set PATH in /etc/toyos.conf)\n",
    "  gui           - graphics mode (Esc returns here)\n",
    "  history       - list past commands (arrows browse history)\n",
    "\n",
    "Editing the command line (bash/readline keys):\n",
    "  Left/Right    - move by a character; Ctrl+Left/Right by a word\n",
    "  Home/End      - start/end of line (also Ctrl-A / Ctrl-E)\n",
    "  Ctrl-B/Ctrl-F - back/forward one character\n",
    "  Alt-B/Alt-F   - back/forward one word\n",
    "  Ctrl-K/Ctrl-U - kill to end of line / to start of line\n",
    "  Ctrl-W        - kill the word before the cursor (to whitespace)\n",
    "  Alt-Backspace - same, but stopping at punctuation\n",
    "  Alt-D         - kill the word after the cursor\n",
    "  Ctrl-Y/Alt-Y  - yank the last kill / cycle to an older one\n",
    "  Ctrl-T        - swap the two characters around the cursor\n",
    "  Alt-U/L/C     - upper/lower/capitalize the word after the cursor\n",
    "  Ctrl-_        - undo (also Ctrl-X Ctrl-U)\n",
    "  Ctrl-R        - search history backwards; Ctrl-R again for the\n",
    "                  next match, Enter runs it, Esc edits it\n",
    "  Alt-.         - insert the last word of the previous command\n",
    "  Ctrl-P/Ctrl-N - previous/next history entry (same as Up/Down)\n",
    "  Ctrl-L        - clear the screen, keeping the line you're typing\n",
    "  Ctrl-C        - abandon the line and start a fresh one\n",
    "  (all of these work in the GUI Terminal too -- same editor)\n",

    "\n",
    "Programs (in /bin, run by name -- not builtins):\n",
    "  ls cat echo rm touch mkdir mv ln stat truncate sync less\n",
    "  df meminfo uptime time about random ps kill spawn reboot\n",
    "  lspci lscpu parttable heap ata kstack config tosh\n",
    "  (`<name> --help` where it has one; `path` shows where they are\n",
    "   found, and `rescue` is the kernel's own copy of the file ones)\n",
    "\n",
    "Files & filesystem:\n",
    "  (most of these are PROGRAMS in /bin, not builtins -- `path` shows\n",
    "   where they are found, and `rescue` is the kernel's own copy of\n",
    "   them for a disk whose /bin is damaged)\n",
    "  ls [flags] [d]- list a directory (default: cwd). -l type/size/\n",
    "                  mtime, -h human sizes, -1 one per line, -C in\n",
    "                  columns, -t newest first, -S largest first, -r\n",
    "                  reverse, -R recurse, --color=never|always;\n",
    "                  -a/-F accepted (no dotfiles or modes here)\n",
    "  cd [dir]      - change directory (default: /)\n",
    "  pwd           - print the current directory\n",
    "  mkdir <dir>   - create a directory\n",
    "  cat <f>       - print a file's contents\n",
    "  touch <f>     - create an empty file\n",
    "  write <f> <t> - overwrite file f with text t\n",
    "  append <f> <t>- append text t to file f\n",
    "  rm <f>        - delete a file, or an empty directory\n",
    "  mv <a> <b>    - rename or move a file/directory. Never\n",
    "                  overwrites: remove the destination first\n",
    "  truncate <f> <n> - set f's size to exactly n bytes. Growing is\n",
    "                  sparse (zeros, no blocks used); shrinking frees\n",
    "                  the blocks past the new end\n",
    "  stat <f>      - show a file/directory's type, size, and\n",
    "                  created/modified timestamps\n",
    "  edit <f>      - full-screen text editor (nano/pico-style); also\n",
    "                  `nano <f>`. Arrows/Home/End/Delete to navigate and\n",
    "                  edit, F2 to save, F3 to exit. Also works inside\n",
    "                  the GUI Terminal (same command).\n",
    "  (paths may be relative to cwd or absolute, e.g. /docs/todo.txt)\n",
    "\n",
    "System info:\n",
    "  timezone      - show/pick your timezone (interactive list)\n",
    "  timezone <c>  - set timezone directly, e.g. `timezone helsinki`\n",
    "  rescue        - the kernel's own copies of the file commands, for\n",
    "                  when /bin is missing or damaged. `rescue` alone\n",
    "                  lists them; they never shadow a real program\n",
    "  dmesg         - show the kernel log (boot messages, driver/\n",
    "                  syscall diagnostics, timestamped)\n",
    "\n",
    "Appearance:\n",
    "  color <name>  - change shell text color\n",
    "  cursor <s>    - console cursor style: translucent (the default --\n",
    "                  tints the cell so the character shows through),\n",
    "                  underline, beam, or reverse. `cursor` alone shows\n",
    "                  the current one. Persists across reboot.\n",
    "  fontsize <n>  - set font size in points: 8, 10, 12, 14, 16, 18,\n",
    "                  20, or 24 (`fontsize` alone shows the current one)\n",
    "  keyboard <l>  - set keyboard layout: us or se (Swedish/Finnish --\n",
    "                  Å/Ä/Ö at their real physical keys); `keyboard`\n",
    "                  alone shows the current one\n",
};
#define HELP_LINE_COUNT (sizeof(HELP_LINES) / sizeof(HELP_LINES[0]))

static const char *const TEST_HELP_LINES[] = {
    "toy-os shell -- developer/diagnostic test commands:\n",
    "(these exercise specific kernel subsystems -- see README for what\n",
    "each one actually proves; most are meant to be read via their\n",
    "diagnostic output, not used for everyday work)\n",
    "\n",
    "  ring3test     - proof-of-concept ring 3 + paging isolation\n",
    "                  (does not return; see README)\n",
    "  schedtest     - preemptive round-robin scheduler demo: two\n",
    "                  ring-3 processes run concurrently, neither\n",
    "                  ever yielding (returns once both exit)\n",
    "  fputest       - ring-3 floating point: a value check, then two\n",
    "                  processes racing with live XMM accumulators to\n",
    "                  prove the context switch saves FP state\n",
    "  stress <mb>   - real (non-sparse) write/read/verify pass over <mb>\n",
    "                  megabytes -- exercises direct/single/double/triple-\n",
    "                  indirect blocks with genuine data, not a sparse\n",
    "                  probe. Takes real minutes for multi-GB sizes.\n",
    "  dmatest [lba] - read-only proof of the non-blocking DMA start/poll\n",
    "                  pair (Phase 1 of the async-I/O roadmap item):\n",
    "                  reads a sector both the old blocking way and the\n",
    "                  new poll way, confirms they match, reports poll\n",
    "                  count. Default lba 0.\n",
    "  steptest <mb> - proof of the stepped write+read APIs (Phases 2\n",
    "                  and 4 of the async-I/O roadmap item): writes <mb>\n",
    "                  megabytes one block at a time via\n",
    "                  fs_write_range_begin/_step, reads it back the same\n",
    "                  way via fs_read_range_begin/_step, verifies\n",
    "                  byte-for-byte, reports both step counts. Keep small\n",
    "                  (1-5) -- see `stress` for a throughput test.\n",
    "  debug         - list per-subsystem debug-log switches (off by\n",
    "                  default)\n",
    "  debug <s> on|off - flip one on/off at runtime, no rebuild --\n",
    "                  subsystems: fs, wm, ata\n",
    "  ktest         - run the in-kernel test suite (kernel/test/ktest.c);\n",
    "                  `ktest <suite>` runs one (mm, fs, lib). Also\n",
    "                  runnable from the host: `make test`.\n",
    "  sync          - write out anything the disk cache is still holding,\n"
    "                  and report how many sectors it wrote. Runs\n"
    "                  automatically at shutdown and reboot.\n",
    "  fsck          - filesystem consistency check: walks every file's\n",
    "                  block tree and compares it against the free-block\n",
    "                  bitmap. Read-only, safe to run any time.\n",
    "  fsck repair   - the same pass, but also reclaims leaked blocks and\n",
    "                  fixes what can be fixed without guessing. Blocks\n",
    "                  claimed by two files are always reported, never\n",
    "                  repaired -- see fs.h's fs_check().\n",
    "  fsformat <fs> confirm - DESTROY everything on disk and reformat\n",
    "                  with the named filesystem (tfs2, tfs3), then\n",
    "                  remount it live. `df` shows which one is active.\n",
    "  ln <file> <new> - hardlink: a second name for the same file\n",
    "                  (tfs3 only -- tfs2's format has no link counts)\n",
    "\n",
    "Every other former *test command (elftest, syscalltest, writetest,\n",
    "ptrtest, guitest, echotest, wintest, filetest, newsyscalltest,\n",
    "crashtest, sockettest) is now a real disk-hosted binary under /bin\n",
    "instead of a dedicated command -- see `ls /bin` and `run <name>`\n",
    "(e.g. `run write_test`). See docs/decisions.md for why.\n",
    "\n",
    "Run `help` (no arguments) for everyday commands.\n",
};
#define TEST_HELP_LINE_COUNT (sizeof(TEST_HELP_LINES) / sizeof(TEST_HELP_LINES[0]))

void cmd_help(const char *args) {
    if (args && k_strcmp(args, "tests") == 0) {
        console_page(TEST_HELP_LINES, TEST_HELP_LINE_COUNT);
    } else {
        console_page(HELP_LINES, HELP_LINE_COUNT);
    }
}

// `timezone` alone: numbered list, prompts for a choice.
// `timezone <name>`: sets directly, matching tz_city_name() as a whole
// but ignoring ASCII case, so `timezone losangeles` and `timezone
// LosAngeles` both work. Still an exact match otherwise -- no prefixes,
// no fuzzy matching. `color <name>` remains case-sensitive; nothing has
// asked for it, and its names are typed lowercase.
// One place for the "...and it did not actually persist" tail every
// setting command needs, so the wording can't drift between four of
// them. Prints nothing on a clean save. The settings themselves are
// applied either way -- this is only about surviving a reboot, and
// saying so beats an unqualified success message that turns out to be
// half true only after the machine is restarted (see etc_config.h's
// enum setting_result for the bug that prompted it).
static void print_save_result(int r) {
    if (r == SETTING_UNSAVED) {
        vga_write(" (NOT saved -- /etc unwritable, see dmesg)");
    }
}

void cmd_timezone(const char *args) {
    if (args && k_strlen(args) > 0) {
        int idx = tz_find_by_name(args);
        if (idx < 0) {
            vga_write("Unknown timezone. Run `timezone` with no arguments to see the list.\n");
            return;
        }
        int r = tz_set_index(idx);
        vga_write("Timezone set to ");
        vga_write(tz_city_name(idx));
        print_save_result(r);
        vga_write(".\n");
        return;
    }

    // The no-args branch below blocks on keyboard_read_line() waiting
    // for a numbered choice -- fine for the interactive console loop
    // (shell_main() calls its own keyboard_getchar() in a loop already),
    // fatal for a non-blocking GUI callback driving this through a sink
    // (see shell_dispatch()'s comment). Print the list plus a pointer to
    // the direct-set form instead of blocking.
    if (vga_sink_active()) {
        vga_write("Interactive timezone picker isn't available here --\n");
        vga_write("use `timezone <city>` instead. Cities:\n");
        int n = tz_city_count();
        for (int i = 0; i < n; i++) {
            vga_write("  ");
            vga_write(tz_city_name(i));
            vga_putc('\n');
        }
        return;
    }

    int count = tz_city_count();
    int current = tz_current_index();
    for (int i = 0; i < count; i++) {
        vga_write_dec((uint32_t)(i + 1));
        vga_write(i == current ? ") * " : ")   ");
        vga_write(tz_city_name(i));
        vga_putc('\n');
    }
    vga_write("Enter a number (blank to cancel): ");

    char buf[8];
    keyboard_read_line(buf, sizeof(buf));
    if (k_strlen(buf) == 0) {
        vga_write("Cancelled.\n");
        return;
    }

    uint32_t parsed = 0;
    int choice = k_parse_u32(buf, &parsed) ? (int)parsed : -1;
    if (choice < 1 || choice > count) {
        vga_write("Not a valid choice.\n");
        return;
    }
    int r = tz_set_index(choice - 1);
    vga_write("Timezone set to ");
    vga_write(tz_city_name(choice - 1));
    print_save_result(r);
    vga_write(".\n");
}

// Milestone 25 (docs/roadmap.md): "the simplest possible output" -- a
// fixed tone, not a freq/duration-adjustable command, by explicit
// request. 800Hz/200ms is just an audible, unremarkable beep, no
// particular significance to the exact numbers.
void cmd_beep(void) {
    vga_write("beep!\n");
    speaker_beep(800, 200);
}

// `df` -- disk usage, meminfo's sibling for fs.c's data blocks instead
// of pmm.c's physical frames. Divides to KB (not MB) before narrowing
// to uint32_t (vga_write_dec() takes one): a real byte count can exceed
// 32 bits on a multi-gigabyte disk, but KB doesn't need to -- even
// FS_DISK_TOTAL_BYTES's full 9GiB is a bit over 9.4 million KB, well
// under uint32_t's ~4.29 billion ceiling. MB looked tempting (smaller
// numbers) but rounds everything under 1MB down to a flat, useless "0"
// -- today's whole seeded /bin + /etc content totals under 1MB, so an
// MB-only df would always claim 0 used regardless of what's actually
// on disk. Found live testing this command, not by review.
void cmd_df(void) {
    uint64_t used_bytes = 0, total_bytes = 0;
    fs_disk_usage(&used_bytes, &total_bytes);
    uint64_t free_bytes = total_bytes - used_bytes;

    vga_write("Filesystem: ");
    vga_write(fs_backend_name());
    vga_write(" (");
    vga_write(fs_is_persistent() ? "persistent, on disk" : "RAM-only -- won't survive reboot");
    vga_write(")\n");
    vga_write("  total: "); vga_write_dec((uint32_t)(total_bytes / 1024));
    vga_write(" KB\n");
    vga_write("  used:  "); vga_write_dec((uint32_t)(used_bytes / 1024));
    vga_write(" KB\n");
    vga_write("  free:  "); vga_write_dec((uint32_t)(free_bytes / 1024));
    vga_write(" KB\n");
}

// `ktest [suite]` -- runs the in-kernel test suite (kernel/test/ktest.c)
// and reports. Note the tests run inside the live, booted kernel with
// full access to the real heap, allocator and filesystem, which is the
// point of them existing here rather than as host-side unit tests --
// but it also means a test that corrupts something corrupts the running
// system, not a sandbox. See kernel/include/kernel/ktest.h.
void cmd_ktest(const char *args) {
    ktest_run_all(args && k_strlen(args) > 0 ? args : 0);
}

// `fputest` -- proves ring-3 floating point is not just enabled but
// SAFE under preemption.
//
// Two things get checked, and the second is the one that matters.
// /tests/fpu_test is a single process doing double/float arithmetic
// with exactly-representable values: that only proves CR4.OSFXSR is
// set. /tests/fpu_race runs TWICE, CONCURRENTLY, with different seeds,
// each holding eight live accumulators in XMM registers across
// thousands of 100Hz preemptions. Without the FXSAVE/FXRSTOR pair in
// scheduler.c the two share one physical register file and both come
// back with the other's numbers.
//
// Deliberately spawned through the public scheduler_spawn()/
// scheduler_poll() API rather than reaching into the scheduler, so this
// exercises the same path Terminal's async spawn uses. Busy-waits on
// `hlt` like `schedtest` does -- blocking the shell for the duration is
// fine (and necessary) for a test command.
void cmd_fputest(void) {
    vga_write("Single-process float check (/tests/fpu_test):\n");
    if (!shell_exec_name("fpu_test", 0, 1)) {
        vga_write("  could not run /tests/fpu_test -- is it seeded onto\n");
        vga_write("  disk.img? (see the Makefile's `seed` target)\n");
        return;
    }
    vga_write("\n");

    vga_write("Concurrent FP state check: two /tests/fpu_race processes,\n");
    vga_write("preempted against each other with eight live XMM accumulators\n");
    vga_write("each. This is what proves the context switch saves FP state.\n");

    int a = scheduler_spawn("/tests/fpu_race", "1");
    int b = scheduler_spawn("/tests/fpu_race", "2");
    if (a <= 0 || b <= 0) {
        vga_write("fputest: failed to spawn both racers -- is /tests/fpu_race\n");
        vga_write("seeded onto disk.img? (see the Makefile's `seed` target)\n");
        return;
    }

    int a_code = 0, b_code = 0, a_done = 0, b_done = 0;
    while (!a_done || !b_done) {
        __asm__ volatile ("hlt");
        if (!a_done && scheduler_poll(a, &a_code) == SCHED_POLL_EXITED) a_done = 1;
        if (!b_done && scheduler_poll(b, &b_code) == SCHED_POLL_EXITED) b_done = 1;
    }

    if (a_code == 0 && b_code == 0) {
        vga_write("\n  ok -- both processes' accumulators survived intact.\n");
    } else {
        vga_printf("\n  FAILED -- exit codes %d and %d (a non-zero code is the\n",
                    a_code, b_code);
        vga_write("  1-based index of the first corrupted accumulator).\n");
    }
}

// `fsck` / `fsck repair` -- filesystem consistency check, and the
// reclaim half of it. See fs.h's fs_check() for what a repair pass will
// and won't fix; the interesting asymmetry is that leaked blocks are
// reclaimed automatically while double-allocated ones are only ever
// reported (picking which of two files keeps a shared block is a
// data-destroying guess). Report-only by default on purpose: this walks
// every record's block tree and, in repair mode, writes to the bitmap,
// so "see what it would do" should not require committing to it.
// Writes out everything the disk cache is still holding, and SAYS WHAT
// IT WROTE. The count is the point: a `sync` that prints nothing is
// indistinguishable from one that did nothing, and with a write-back
// cache underneath the difference is somebody's file.
void cmd_sync(const char *args) {
    (void)args;
    if (!ata_cache_active()) {
        vga_write("sync: no disk cache active -- writes already go straight to the disk.\n");
        return;
    }

    uint32_t wrote = 0, pending = 0;
    int ok = ata_sync(&wrote, &pending);

    vga_write("sync: wrote "); vga_write_dec(wrote);
    vga_write(wrote == 1 ? " sector" : " sectors");
    vga_write(" back to disk");
    if (wrote == 0 && ok) vga_write(" (nothing was pending)");
    vga_write(".\n");

    if (!ok) {
        // Loud, and specific about what to do: the data still exists,
        // in RAM only, and powering off is what would lose it.
        vga_write("sync: FAILED -- "); vga_write_dec(pending);
        vga_write(" sector(s) could NOT be written and are still pending.\n");
        vga_write("      Do not power off yet; that data is in memory only.\n");
    }
}

void cmd_fsck(const char *args) {
    int repair = (args && k_strcmp(args, "repair") == 0);
    if (args && k_strlen(args) > 0 && !repair) {
        vga_write("usage: fsck [repair]\n");
        vga_write("  fsck         check and report only, touches nothing\n");
        vga_write("  fsck repair  also reclaim leaked blocks and fix what's safely fixable\n");
        return;
    }

    struct fs_check_result r;
    vga_write(repair ? "fsck: checking and repairing " : "fsck: checking (read-only) ");
    vga_write(fs_backend_name());
    vga_write(" ...\n");
    if (!fs_check(repair, &r)) {
        vga_write("fsck: filesystem is RAM-only -- nothing on disk to check.\n");
        return;
    }

    vga_write("  records in use:        "); vga_write_dec(r.records_used); vga_putc('\n');
    vga_write("  blocks referenced:     "); vga_write_dec(r.blocks_referenced); vga_putc('\n');
    vga_write("  leaked (unreferenced): "); vga_write_dec(r.leaked); vga_putc('\n');
    vga_write("  referenced but free:   "); vga_write_dec(r.referenced_but_free); vga_putc('\n');
    vga_write("  double-allocated:      "); vga_write_dec(r.double_allocated); vga_putc('\n');
    vga_write("  out-of-range pointers: "); vga_write_dec(r.out_of_range); vga_putc('\n');

    if (repair) {
        vga_write("  -- repaired --\n");
        vga_write("  blocks reclaimed:      "); vga_write_dec(r.reclaimed);
        vga_write(" ("); vga_write_dec(r.reclaimed * 4); vga_write(" KB)\n");
        vga_write("  marked allocated:      "); vga_write_dec(r.marked_allocated); vga_putc('\n');
        vga_write("  pointers cleared:      "); vga_write_dec(r.pointers_cleared); vga_putc('\n');
    } else if (r.leaked || r.referenced_but_free || r.out_of_range) {
        vga_write("Run `fsck repair` to reclaim ");
        vga_write_dec(r.leaked); vga_write(" leaked block(s) (");
        vga_write_dec(r.leaked * 4); vga_write(" KB) and fix the rest.\n");
    }

    if (r.double_allocated) {
        // Deliberately not repaired -- see fs.h. Say what to do instead
        // of leaving the number sitting there unexplained.
        vga_write("WARNING: blocks claimed by more than one file. `fsck repair` will NOT\n");
        vga_write("fix this -- choosing which file keeps a shared block would destroy the\n");
        vga_write("other's data. Delete one of the affected files to resolve it.\n");
    }

    if (!r.leaked && !r.referenced_but_free && !r.double_allocated && !r.out_of_range) {
        vga_write("fsck: clean.\n");
    }
}

// Real (non-sparse) multi-GB write/read/verify stress test over
// fs_write_range()/fs_read_range() -- built to answer docs/roadmap.md's
// long-standing "full end-to-end multi-GB write/read pass hasn't been
// run yet" item. tfs_selftest() (kernel/fs/tfs.c, runs on every
// disk-backed boot) already proves triple-indirect *addressing* --
// that the pointer chain can be built and walked -- but it only writes
// 64 bytes at a ~4.6GB offset, not real content filling that space.
// This command actually writes `<mb>` megabytes of a verifiable,
// per-chunk-varying pattern (so a corrupted or swapped chunk is
// detectable, not just "did every byte come back nonzero"), reads it
// all back, and confirms it matches -- exercising direct, single-,
// double-, AND triple-indirect blocks with genuine data, not a sparse
// probe. Deliberately a manual/on-demand command, not part of the boot
// self-test: a real multi-GB pass over this kernel's PIO/DMA ATA path
// takes real wall-clock time (minutes, not the sub-second boot self-
// test), unsuitable for every boot.
//
// One static 1MB chunk buffer (not a stack array -- see kernel/core's
// existing convention of static/heap buffers for anything this size,
// e.g. dmesg's paging state above) reused for every chunk, both
// directions, keeping this O(1) in RAM regardless of `<mb>`.
#define STRESS_CHUNK_BYTES (1024u * 1024u)
static uint8_t g_stress_chunk[STRESS_CHUNK_BYTES];
// In /tmp rather than a dotfile at the root of the filesystem, which
// is where it used to live -- scratch space is exactly what /tmp is
// for, and a multi-gigabyte temp file sitting in / was the single
// biggest argument for having the directory at all.
#define STRESS_TEST_PATH "/tmp/stress_test"

// Fills g_stress_chunk with a pattern that varies both by chunk index
// and byte offset, so two different chunks (or a chunk read back from
// the wrong offset) don't accidentally look identical.
static void stress_fill_pattern(uint32_t chunk_index) {
    for (uint32_t i = 0; i < STRESS_CHUNK_BYTES; i++) {
        g_stress_chunk[i] = (uint8_t)((chunk_index * 31 + i) ^ 0xA5);
    }
}

// Redraws "<verb> [####----] <pct>% <done>/<total>MB <speed>MB/s" IN
// PLACE on one line, `\r`-style, instead of scrolling a new line per
// update -- both console backends (kernel/drivers/vga.c's legacy
// 0xB8000 path and the framebuffer path) already treat '\r' as "column
// 0, same row, no scroll" and draw characters in-place, so this needed
// no kernel/driver changes, just not using '\n' between updates.
// Trailing PAD_SPACES blanks out whatever a longer previous line left
// behind -- total_mb's width is fixed for a whole run, but done_mb's/
// pct's grow monotonically and speed's tenths digit can occasionally
// shrink by one as the running average settles, so the line's total
// length isn't perfectly monotonic even though it's close. A real '\n'
// only gets emitted once the bar reaches 100% (the caller relies on
// this -- the next thing printed, e.g. "reading back...", must start
// on its own fresh line, not overwrite the finished bar).
// speed as a running average (bytes done so far / time since the phase
// -- write or read -- started, not just since the last update). One
// decimal place, computed in tenths to avoid needing float on this
// freestanding target: MB/s*10 == done_mb * 1000 / phase_ticks (PIT
// runs at 100Hz -- ticks/100 == seconds -- see timer.h). phase_ticks
// is clamped to at least 1 so a sub-tick-resolution phase (only
// possible for a tiny `mb`) can't divide by zero.
#define STRESS_BAR_WIDTH 20
#define STRESS_BAR_PAD_SPACES 6

static void stress_print_progress(const char *verb, uint32_t done_mb,
                                   uint32_t total_mb, uint64_t phase_ticks) {
    uint32_t pct = (done_mb * 100) / total_mb;
    if (phase_ticks == 0) phase_ticks = 1;
    uint32_t speed_x10 = (uint32_t)((uint64_t)done_mb * 1000 / phase_ticks);
    uint32_t filled = (pct * STRESS_BAR_WIDTH) / 100;

    vga_putc('\r');
    vga_write("  "); vga_write(verb); vga_write(" [");
    for (uint32_t i = 0; i < STRESS_BAR_WIDTH; i++) vga_putc(i < filled ? '#' : '-');
    vga_write("] "); vga_write_dec(pct); vga_write("% ");
    vga_write_dec(done_mb); vga_write("/"); vga_write_dec(total_mb); vga_write("MB ");
    vga_write_dec(speed_x10 / 10); vga_write("."); vga_write_dec(speed_x10 % 10); vga_write("MB/s");
    for (uint32_t i = 0; i < STRESS_BAR_PAD_SPACES; i++) vga_putc(' ');
    // Suppress the cursor block vga_putc() just repainted at end-of-
    // line -- this loop never calls vga_cursor_tick() between updates,
    // so left alone it would sit there solid (not blinking) instead of
    // reappearing where a real prompt cursor belongs. See
    // vga_cursor_hide()'s own comment (vga.h) for the full reasoning.
    vga_cursor_hide();
    if (pct >= 100) vga_putc('\n');
}

// This file's own parse_decimal() became knum.h's k_parse_u32() -- the
// "reject rather than guess" contract it established is the whole
// toolkit's parsing rule now, and it gained overflow checking the
// hand-rolled version didn't have. Kept as a one-line alias rather than
// renaming three call sites for no behavioral reason.
#define parse_decimal(s, out) k_parse_u32((s), (out))

void cmd_stress(const char *args) {
    uint32_t mb;
    if (!parse_decimal(args, &mb) || mb == 0) {
        vga_write("usage: stress <mb>  -- real write/read/verify pass over\n");
        vga_write("  <mb> megabytes (e.g. `stress 4200` to cross the ~4004MB\n");
        vga_write("  triple-indirect boundary, `stress 8192` for the full 8GB\n");
        vga_write("  target). Takes real minutes for large sizes -- see `help tests`.\n");
        return;
    }
    if (!fs_is_persistent()) {
        vga_write("stress: filesystem is RAM-only -- this test needs a real disk\n");
        vga_write("  backend (RAM_ONLY_MAX_BLOCKS is far smaller than any useful\n");
        vga_write("  stress size). See `df`.\n");
        return;
    }

    fs_delete(STRESS_TEST_PATH); // clean slate if a previous run left it behind
    if (!fs_touch(STRESS_TEST_PATH)) {
        vga_write("stress: FAILED (couldn't create test file)\n");
        return;
    }

    uint32_t chunks = mb; // 1 chunk == 1MB by construction
    uint64_t start_ticks = pit_ticks();

    vga_write("stress: writing "); vga_write_dec(mb); vga_write(" MB to ");
    vga_write(STRESS_TEST_PATH); vga_write(" ...\n");
    uint64_t write_start_ticks = pit_ticks();
    uint32_t last_pct_printed = 0;
    for (uint32_t c = 0; c < chunks; c++) {
        stress_fill_pattern(c);
        uint64_t offset = (uint64_t)c * STRESS_CHUNK_BYTES;
        if (!fs_write_range(STRESS_TEST_PATH, offset, g_stress_chunk, STRESS_CHUNK_BYTES)) {
            vga_write("stress: FAILED (write failed at chunk ");
            vga_write_dec(c); vga_write(" / offset ");
            vga_write_dec((uint32_t)(offset / (1024 * 1024))); vga_write(" MB)\n");
            fs_delete(STRESS_TEST_PATH);
            return;
        }
        // One line per percentage point crossed -- not time-based
        // (100 PIT ticks) anymore: after the free_all_blocks() batching
        // fix sped up a typical run, a 1x/sec cadence was skipping from
        // ~6% straight to ~13% on a fast disk, never landing on a clean
        // 1%..100% sequence. Percent-based naturally caps at ~100 lines
        // total regardless of <mb> or disk speed, so it can't flood for
        // a huge `mb` either.
        uint32_t pct = ((c + 1) * 100) / mb;
        if (pct > last_pct_printed) {
            stress_print_progress("wrote", c + 1, mb, pit_ticks() - write_start_ticks);
            last_pct_printed = pct;
        }
    }
    uint64_t write_ticks = pit_ticks() - write_start_ticks;

    vga_write("stress: reading back and verifying ...\n");
    static uint8_t readback[STRESS_CHUNK_BYTES];
    uint64_t read_start_ticks = pit_ticks();
    last_pct_printed = 0;
    for (uint32_t c = 0; c < chunks; c++) {
        uint64_t offset = (uint64_t)c * STRESS_CHUNK_BYTES;
        uint32_t got = fs_read_range(STRESS_TEST_PATH, offset, readback, STRESS_CHUNK_BYTES);
        if (got != STRESS_CHUNK_BYTES) {
            vga_write("stress: FAILED (short read at chunk ");
            vga_write_dec(c); vga_write(", got "); vga_write_dec(got); vga_write(" bytes)\n");
            fs_delete(STRESS_TEST_PATH);
            return;
        }
        stress_fill_pattern(c); // recompute expected into g_stress_chunk
        int mismatch = 0;
        for (uint32_t i = 0; i < STRESS_CHUNK_BYTES; i++) {
            if (g_stress_chunk[i] != readback[i]) { mismatch = 1; break; }
        }
        if (mismatch) {
            vga_write("stress: FAILED (data mismatch at chunk ");
            vga_write_dec(c); vga_write(" / offset ");
            vga_write_dec((uint32_t)(offset / (1024 * 1024))); vga_write(" MB)\n");
            fs_delete(STRESS_TEST_PATH);
            return;
        }
        uint32_t pct = ((c + 1) * 100) / mb;
        if (pct > last_pct_printed) {
            stress_print_progress("verified", c + 1, mb, pit_ticks() - read_start_ticks);
            last_pct_printed = pct;
        }
    }
    uint64_t read_ticks = pit_ticks() - read_start_ticks;

    if (!fs_delete(STRESS_TEST_PATH)) {
        vga_write("stress: WARNING -- test passed but couldn't delete ");
        vga_write(STRESS_TEST_PATH); vga_write(" (clean up manually)\n");
    }

    uint64_t elapsed_ticks = pit_ticks() - start_ticks; // 100Hz PIT -- see timer.h
    uint32_t write_speed_x10 = (uint32_t)((uint64_t)mb * 1000 / (write_ticks ? write_ticks : 1));
    uint32_t read_speed_x10 = (uint32_t)((uint64_t)mb * 1000 / (read_ticks ? read_ticks : 1));
    vga_write("stress: PASSED -- "); vga_write_dec(mb);
    vga_write(" MB written, read back, and verified byte-for-byte in ");
    vga_write_dec((uint32_t)(elapsed_ticks / 100)); vga_write(" s (");
    vga_write_dec(write_speed_x10 / 10); vga_write(".");
    vga_write_dec(write_speed_x10 % 10); vga_write(" MB/s write, ");
    vga_write_dec(read_speed_x10 / 10); vga_write(".");
    vga_write_dec(read_speed_x10 % 10); vga_write(" MB/s read)\n");
}

// Proves ata_dma_nonblocking_selftest() (Phase 1 of the async-I/O
// roadmap item -- see docs/roadmap.md and kernel/drivers/ata.c) from
// the shell: read-only, so it's always safe to run, and reports how
// many dma_transfer_poll() calls the non-blocking read needed to
// complete, not just pass/fail. Defaults to LBA 0 (the very first
// sector -- always readable if a disk is present at all) if no
// argument is given.
void cmd_dmatest(const char *args) {
    if (!ata_dma_active()) {
        vga_write("dmatest: DMA path not active on this machine (PIO fallback\n");
        vga_write("  in use, or no drive present) -- nothing to test. See\n");
        vga_write("  `lspci` / `dmesg` for why.\n");
        return;
    }

    uint32_t lba = 0;
    if (args && *args && !parse_decimal(args, &lba)) {
        vga_write("usage: dmatest [lba]  -- read-only proof of the non-blocking\n");
        vga_write("  DMA start/poll pair against a real sector (default lba 0).\n");
        return;
    }

    uint32_t polls = 0;
    int ok = ata_dma_nonblocking_selftest(lba, &polls);
    if (!ok) {
        vga_write("dmatest: FAILED (lba "); vga_write_dec(lba);
        vga_write(") -- see `debug ata on` + `dmesg` for detail\n");
        return;
    }

    vga_write("dmatest: PASSED -- lba "); vga_write_dec(lba);
    vga_write(" read identically via the blocking path and the new\n");
    vga_write("  non-blocking start/poll pair ("); vga_write_dec(polls);
    vga_write(" poll call"); vga_write(polls == 1 ? "" : "s");
    vga_write(" before completion)\n");
}

#define STEPTEST_TEST_PATH "/.steptest_tmp"

// Proves fs_write_range_begin()/fs_write_range_step() (Phase 2) AND
// fs_read_range_begin()/fs_read_range_step() (Phase 4) of the async-I/O
// roadmap item (kernel/fs/tfs.c) -- writes <mb> megabytes through
// the stepped write API instead of fs_write_range(), one block at a
// time via an explicit step loop this command drives itself (standing
// in for what wm_run() would eventually do once per frame, which Phase
// 3 wired up for real), then reads it back through the stepped read
// API the same way and verifies byte-for-byte, same pattern/
// verification `stress` already uses. Reports both step counts so a
// genuinely multi-block operation is visibly proven, not just a
// trivial single-block case.
void cmd_steptest(const char *args) {
    uint32_t mb;
    if (!parse_decimal(args, &mb) || mb == 0) {
        vga_write("usage: steptest <mb>  -- write/read/verify <mb> megabytes\n");
        vga_write("  through the new stepped write API (fs_write_range_begin/\n");
        vga_write("  _step) instead of fs_write_range(), reporting step count.\n");
        vga_write("  Keep this small (1-5) -- it's a primitive proof, not a\n");
        vga_write("  throughput test (see `stress` for that).\n");
        return;
    }
    if (!fs_is_persistent()) {
        vga_write("steptest: filesystem is RAM-only -- this test needs a real\n");
        vga_write("  disk backend. See `df`.\n");
        return;
    }

    fs_delete(STEPTEST_TEST_PATH); // clean slate if a previous run left it behind
    if (!fs_touch(STEPTEST_TEST_PATH)) {
        vga_write("steptest: FAILED (couldn't create test file)\n");
        return;
    }

    uint32_t total_steps = 0;
    vga_write("steptest: writing "); vga_write_dec(mb); vga_write(" MB to ");
    vga_write(STEPTEST_TEST_PATH); vga_write(" via the stepped API ...\n");
    for (uint32_t c = 0; c < mb; c++) {
        stress_fill_pattern(c);
        uint64_t offset = (uint64_t)c * STRESS_CHUNK_BYTES;
        void *step = fs_write_range_begin(STEPTEST_TEST_PATH, offset, g_stress_chunk, STRESS_CHUNK_BYTES);
        if (!step) {
            vga_write("steptest: FAILED (begin() failed at chunk "); vga_write_dec(c); vga_write(")\n");
            fs_delete(STEPTEST_TEST_PATH);
            return;
        }
        enum fs_step_result r;
        while ((r = fs_write_range_step(step)) == FS_STEP_PENDING) total_steps++;
        total_steps++; // the terminal step() call itself
        if (r != FS_STEP_DONE) {
            vga_write("steptest: FAILED (step() failed at chunk "); vga_write_dec(c); vga_write(")\n");
            fs_delete(STEPTEST_TEST_PATH);
            return;
        }
    }

    uint32_t total_read_steps = 0;
    vga_write("steptest: reading back via the stepped read API and verifying ...\n");
    static uint8_t readback[STRESS_CHUNK_BYTES];
    for (uint32_t c = 0; c < mb; c++) {
        uint64_t offset = (uint64_t)c * STRESS_CHUNK_BYTES;
        void *step = fs_read_range_begin(STEPTEST_TEST_PATH, offset, readback, STRESS_CHUNK_BYTES);
        if (!step) {
            vga_write("steptest: FAILED (read begin() failed at chunk "); vga_write_dec(c); vga_write(")\n");
            fs_delete(STEPTEST_TEST_PATH);
            return;
        }
        enum fs_step_result r;
        uint32_t got = 0;
        while ((r = fs_read_range_step(step, &got)) == FS_STEP_PENDING) total_read_steps++;
        total_read_steps++; // the terminal step() call itself
        if (r != FS_STEP_DONE || got != STRESS_CHUNK_BYTES) {
            vga_write("steptest: FAILED (short/failed read at chunk "); vga_write_dec(c); vga_write(")\n");
            fs_delete(STEPTEST_TEST_PATH);
            return;
        }
        stress_fill_pattern(c);
        int mismatch = 0;
        for (uint32_t i = 0; i < STRESS_CHUNK_BYTES; i++) {
            if (g_stress_chunk[i] != readback[i]) { mismatch = 1; break; }
        }
        if (mismatch) {
            vga_write("steptest: FAILED (data mismatch at chunk "); vga_write_dec(c); vga_write(")\n");
            fs_delete(STEPTEST_TEST_PATH);
            return;
        }
    }

    if (!fs_delete(STEPTEST_TEST_PATH)) {
        vga_write("steptest: WARNING -- test passed but couldn't delete ");
        vga_write(STEPTEST_TEST_PATH); vga_write(" (clean up manually)\n");
    }

    vga_write("steptest: PASSED -- "); vga_write_dec(mb);
    vga_write(" MB written via "); vga_write_dec(total_steps);
    vga_write(" step() calls, read back via "); vga_write_dec(total_read_steps);
    vga_write(" step() calls and verified byte-for-byte\n");
}

// dmesg scratch state -- klog_dump() (klog.h) takes a plain
// void(*)(char) callback with no userdata slot, same pattern fs_list()
// uses (see syscall.c's "SYS_LISTDIR scratch state" comment), so
// pagination state that needs to survive across callback invocations
// lives in file-scope statics here instead of being threaded through
// the callback itself.
static uint32_t dmesg_rows_shown;
static uint32_t dmesg_page_rows;
static int dmesg_quit;

static void dmesg_putc_cb(char c) {
    // klog_dump() is an unconditional walk of the ring buffer with no
    // way to signal "stop" back into it mid-stream (see its own doc
    // comment) -- so quitting early doesn't stop the dump itself, it
    // just makes this callback stop actually drawing anything for the
    // remainder of that walk. Cheap and correct: klog_dump() still
    // touches every remaining byte, this just becomes a no-op for them.
    if (dmesg_quit) return;

    vga_putc(c);
    if (c != '\n') return;
    dmesg_rows_shown++;
    if (dmesg_rows_shown < dmesg_page_rows) return;
    vga_write("-- more (press any key, 'q' to quit) --");
    int key = keyboard_getchar();
    vga_write("\n");
    dmesg_rows_shown = 0;
    if (key == 'q' || key == 'Q') dmesg_quit = 1;
}

void cmd_dmesg(void) {
    dmesg_rows_shown = 0;
    dmesg_quit = 0;
    // A sink means this might be running inside a non-blocking GUI
    // callback (see console_page()'s identical check) -- pagination's
    // keyboard_getchar() would hang whatever's driving that. Dump
    // everything unpaginated in that case; Terminal's own scrollback
    // widget handles "doesn't fit one screen" there, same as it does
    // for every other command's output.
    dmesg_page_rows = vga_sink_active() ? 0xFFFFFFFFu
                                         : (vga_rows() > 1 ? vga_rows() - 1 : vga_rows());
    klog_dump(dmesg_putc_cb);
}

static void apps_list_cb(const char *name, const char *description) {
    vga_write("  ");
    vga_write(name);
    vga_write("  - ");
    vga_write(description);
    vga_putc('\n');
}

void cmd_apps(void) {
    vga_write("Registered apps:\n");
    app_list(apps_list_cb);
}

void cmd_run(const char *name_and_args) {
    if (!name_and_args || k_strlen(name_and_args) == 0) {
        vga_write("usage: run <app> [args...]  (see 'apps' for the list)\n");
        vga_write("note: the `run` prefix is optional -- typing the name alone\n");
        vga_write("works too, searching PATH (see `path`).\n");
        return;
    }

    // Split the binary's own name from whatever trailing arguments it
    // should receive -- the same first-word/rest split dispatch() does,
    // needed again here because `run`'s argument arrives as one string.
    char name[LINE_MAX];
    k_strcpy(name, name_and_args);
    char *bin_args = name;
    while (*bin_args && *bin_args != ' ') bin_args++;
    if (*bin_args == ' ') {
        *bin_args = '\0';
        bin_args++;
        while (*bin_args == ' ') bin_args++;
    } else {
        bin_args = 0;
    }

    // Everything past this point is shell_path.c's shell_exec_name() --
    // the same resolver a bare typed name goes through, so `run foo` and
    // `foo` can never resolve differently.
    if (!shell_exec_name(name, bin_args, 1)) {
        vga_write("run: no such app or executable: ");
        vga_write(name);
        vga_write("\n(`path` shows where executables are searched for)\n");
    }
}

// `cursor` / `cursor <style>` -- how the console draws its cursor.
// Reads VGA_CURSOR_STYLE_NAMES generically (vga.h), so adding a style
// there needs no change here, same as `debug` and its subsystem list.
void cmd_cursor(const char *args) {
    if (!args || k_strlen(args) == 0) {
        vga_write("cursor style: ");
        vga_write(VGA_CURSOR_STYLE_NAMES[vga_cursor_style()]);
        vga_write("\navailable: ");
        for (int i = 0; i < VGA_CURSOR_STYLE_COUNT; i++) {
            if (i) vga_write(", ");
            vga_write(VGA_CURSOR_STYLE_NAMES[i]);
        }
        vga_write("\n(set with `cursor <style>`; persists across reboot)\n");
        return;
    }

    enum vga_cursor_style want;
    if (!vga_cursor_style_parse(args, &want)) {
        vga_write("cursor: not a style: ");
        vga_write(args);
        vga_write("\n(`cursor` alone lists them)\n");
        return;
    }

    vga_set_cursor_style(want);
    int r = cursor_config_save(want);
    vga_write("cursor style set to ");
    vga_write(VGA_CURSOR_STYLE_NAMES[want]);
    print_save_result(r);
    vga_write("\n");
}

static enum vga_color color_from_name(const char *s) {
    if (k_strcmp(s, "black") == 0) return VGA_BLACK;
    if (k_strcmp(s, "blue") == 0) return VGA_BLUE;
    if (k_strcmp(s, "green") == 0) return VGA_GREEN;
    if (k_strcmp(s, "cyan") == 0) return VGA_CYAN;
    if (k_strcmp(s, "red") == 0) return VGA_RED;
    if (k_strcmp(s, "magenta") == 0) return VGA_MAGENTA;
    if (k_strcmp(s, "brown") == 0) return VGA_BROWN;
    if (k_strcmp(s, "lightgrey") == 0) return VGA_LIGHT_GREY;
    if (k_strcmp(s, "darkgrey") == 0) return VGA_DARK_GREY;
    if (k_strcmp(s, "lightblue") == 0) return VGA_LIGHT_BLUE;
    if (k_strcmp(s, "lightgreen") == 0) return VGA_LIGHT_GREEN;
    if (k_strcmp(s, "lightcyan") == 0) return VGA_LIGHT_CYAN;
    if (k_strcmp(s, "lightred") == 0) return VGA_LIGHT_RED;
    if (k_strcmp(s, "lightmagenta") == 0) return VGA_LIGHT_MAGENTA;
    if (k_strcmp(s, "yellow") == 0) return VGA_LIGHT_BROWN;
    if (k_strcmp(s, "white") == 0) return VGA_WHITE;
    return VGA_LIGHT_GREY;
}

// Prints every baked size's name (point size, see font_ttf.h), comma-
// separated -- shared by cmd_fontsize()'s usage line and its "unknown
// size" error, so the list shown to the user can't drift out of sync
// with what font_ttf.c actually has baked in.
static void print_fontsize_choices(void) {
    for (enum font_size i = 0; i < FONT_SIZE_COUNT; i++) {
        if (i > 0) vga_write(", ");
        vga_write(gfx_font_size_name(i));
    }
}

// Changes the font size, in points/pixels.
//
// **Any size is legal once a face is loaded from /usr/share/fonts** --
// gfx_set_font_px() rasterizes it on demand (font_face.h). With no face
// loaded the eight sizes tools/genttf.py baked into the kernel are all
// there is, so the request SNAPS to the nearest one and this reports
// what it snapped to rather than the number that was typed. Only
// affects the framebuffer console; the legacy 80x25 text-mode fallback
// has one fixed cell size and cannot resize. Persists to
// /etc/toyos.conf (font_config.h) so it survives a reboot -- the same
// pattern as `timezone` and `keyboard`.
void cmd_fontsize(const char *args) {
    if (!args || k_strlen(args) == 0) {
        vga_printf("usage: fontsize <points>  (currently: %d", gfx_font_px());
        if (font_face_active()[0]) vga_printf(", %s", font_face_active());
        else {
            vga_write(", built-in sizes only: ");
            print_fontsize_choices();
        }
        vga_write(")\n");
        return;
    }
    uint32_t px = 0;
    if (!k_parse_u32(args, &px) || px == 0) {
        vga_write("fontsize: '");
        vga_write(args);
        vga_write("' is not a size -- give a number of points\n");
        return;
    }
    int before = gfx_font_px();
    int r = font_config_apply_px((int)px);
    if (r == SETTING_INVALID) {
        vga_printf("fontsize: %u could not be rasterized -- still %d\n",
                   (unsigned)px, before);
        return;
    }
    vga_reflow(); // recompute console_cols/rows for the new cell and clear
    vga_printf("Font size set to %d", gfx_font_px());
    if ((int)px != gfx_font_px()) {
        vga_printf(" (nearest built-in size to %u -- no font loaded)", (unsigned)px);
    }
    print_save_result(r);
    vga_write(".\n");
}

// Lists the faces in /usr/share/fonts and switches between them.
//
// `fontface` alone lists; `fontface <name>` selects; `fontface builtin`
// goes back to the glyphs baked into the kernel image. A face is named
// by its filename without the extension, the same way a cursor theme is
// named by its directory -- so what this lists IS a directory listing,
// and nothing has to parse a font's internal name table to answer it.
void cmd_fontface(const char *args) {
    if (!args || k_strlen(args) == 0) {
        int n = font_face_count();
        const char *active = font_face_active();
        vga_printf("Font faces in /usr/share/fonts (%d):\n", n);
        vga_printf("  %-24s %s%s\n", "builtin",
                   "baked into the kernel image",
                   active[0] ? "" : "  <- active");
        for (int i = 0; i < n; i++) {
            struct font_face_info info;
            if (!font_face_info(i, &info)) continue;
            vga_printf("  %-24s %uKB%s\n", info.name,
                       (unsigned)(info.size / 1024),
                       k_strcmp(info.name, active) == 0 ? "  <- active" : "");
        }
        if (n == 0) vga_write("  (none -- the built-in font is the only one)\n");
        return;
    }

    int builtin = k_strcmp(args, "builtin") == 0;
    int r = font_config_apply_face(args);
    if (r == SETTING_INVALID) {
        vga_write("fontface: cannot use '");
        vga_write(args);
        vga_write("' -- no such face, or not a TrueType outline font\n");
        return;
    }
    vga_reflow();
    vga_printf("Font face set to %s", builtin ? "builtin" : args);
    print_save_result(r);
    vga_write(".\n");
}

// Changes the active keyboard scancode layout -- any name with a
// matching /etc/kbs/<name> file (see kernel/lib/keyboard_layout.c and
// tools/gen_kbs.py; `us` and `se`/Finnish ship by default). `keyboard`
// alone shows the current layout. Persists via keyboard_config_save()
// so it survives a reboot -- same pattern as `fontsize`/`timezone`.
void cmd_keyboard(const char *args) {
    if (!args || k_strlen(args) == 0) {
        vga_write("usage: keyboard <name>  (currently: ");
        vga_write(keyboard_layout_current());
        vga_write(")\n");
        return;
    }
    int found = keyboard_layout_load(args);
    int r = keyboard_config_save(keyboard_layout_current());
    if (!found) {
        vga_write("keyboard: '");
        vga_write(args);
        vga_write("' not found in /etc/kbs -- reverted to ");
        vga_write(keyboard_layout_current());
        vga_write("\n");
        return;
    }
    vga_write("Keyboard layout set to ");
    vga_write(keyboard_layout_current());
    print_save_result(r);
    vga_write(".\n");
}

void cmd_color(const char *args) {
    if (!args || k_strlen(args) == 0) {
        vga_write("usage: color <name> (green, lightcyan, white, red)\n");
        return;
    }
    shell_fg = color_from_name(args);
    vga_set_color(shell_fg, VGA_BLACK);
    vga_write("Color set.\n");
}

void cmd_history(void) {
    if (history_count == 0) {
        vga_write("(no commands yet)\n");
        return;
    }
    for (int i = 0; i < history_count; i++) {
        vga_write_dec((uint32_t)(i + 1));
        vga_write("  ");
        vga_write(history[i]);
        vga_putc('\n');
    }
}

// Runs /bin/lspci rather than listing the devices itself -- same
// precedent (and same one-line implementation) as cmd_ls_bin() above
// running /bin/ls.
//
// It used to print the list directly from pci_device_at(), which meant
// two implementations of the same command: this one, and the userland
// ELF. They already carried duplicate copies of the class-name table,
// and userland/lspci.c's top comment flagged the drift risk that
// creates. Deferring collapses them to one, and the one that survives
// is the userland program -- which is also where a real OS puts lspci,
// since resolving a vendor id to a name is a database lookup in a file,
// not something a kernel should know how to do.
//
// The device list itself still comes from the kernel, through
// SYS_PCI_COUNT/SYS_PCI_INFO; only the formatting and the pci.ids
// lookup moved out.

// `debug` (no args): lists every subsystem and its current on/off
// state. `debug <subsys> on|off`: flips one. Backed by
// kernel/include/api/debugflags.h's dbgflag_*() -- see its top comment for
// why this exists (a permanent, named, off-by-default alternative to
// hand-rolled temporary klog_write() calls added and removed each
// debugging session).
void cmd_debug(const char *args) {
    if (!args || !*args) {
        vga_write("debug subsystems (off by default -- `debug <name> on|off`):\n");
        for (int i = 0; i < DBGFLAG_SUBSYS_COUNT; i++) {
            vga_write("  ");
            vga_write(DBGFLAG_NAMES[i]);
            vga_write(" -- ");
            vga_write(dbgflag_enabled((enum dbgflag_subsys)i) ? "on" : "off");
            vga_putc('\n');
        }
        return;
    }

    // Split "<name> on|off" the same way shell.c's dispatch() splits
    // "<cmd> <args>" -- first word vs rest.
    char name[32];
    const char *p = args;
    size_t n = 0;
    while (*p && *p != ' ' && n + 1 < sizeof(name)) name[n++] = *p++;
    name[n] = '\0';
    while (*p == ' ') p++;

    enum dbgflag_subsys s;
    if (!dbgflag_parse(name, &s)) {
        vga_write("debug: unknown subsystem '"); vga_write(name);
        vga_write("' -- run `debug` with no arguments to list them\n");
        return;
    }
    if (k_strcmp(p, "on") == 0) {
        dbgflag_set(s, 1);
        vga_write("debug: "); vga_write(name); vga_write(" on\n");
    } else if (k_strcmp(p, "off") == 0) {
        dbgflag_set(s, 0);
        vga_write("debug: "); vga_write(name); vga_write(" off\n");
    } else {
        vga_write("usage: debug [<subsystem> on|off]\n");
    }
}

// `gfxbench [iterations]` -- how fast can we actually get pixels onto
// the screen, and by which mechanism.
//
// This exists because the answer differs by orders of magnitude between
// QEMU and real hardware while NOTHING on screen reveals which you are
// getting. A framebuffer left uncached makes every store a bus
// transaction the CPU waits on; write-combining gathers them into
// bursts. Under QEMU the framebuffer is cached host RAM, so this reports
// an implausibly good number there and is only meaningful on a real
// machine -- which is also the machine with no serial port, so the
// output is shaped to be readable in a photograph of the screen.
//
// It leaves the screen filled, deliberately in the console background
// colour so it reads as a clear rather than as damage.
void cmd_gfxbench(const char *args) {
    while (*args == ' ') args++;

    int iterations = 10;
    if (*args) {
        uint32_t n = 0;
        if (!k_parse_u32(args, &n) || n == 0 || n > 1000) {
            vga_write("usage: gfxbench [iterations]   (1-1000, default 10)\n");
            return;
        }
        iterations = (int)n;
    }

    int w = gfx_width(), h = gfx_height(), depth = gfx_bpp();
    if (w <= 0 || h <= 0 || depth <= 0) {
        vga_write("gfxbench: no display surface\n");
        return;
    }

    struct cpu_info info;
    cpu_info_get(&info);

    uint64_t cycles = gfx_bench_fill(0x000000, iterations);

    // Bytes actually pushed: visible pixels, not the whole pitch -- the
    // loop skips any padding at the end of each scanline.
    uint64_t bytes = (uint64_t)w * h * (depth / 8) * (uint64_t)iterations;

    vga_write("gfxbench: "); vga_write_dec((uint32_t)w);
    vga_putc('x'); vga_write_dec((uint32_t)h);
    vga_write(" x"); vga_write_dec((uint32_t)depth);
    vga_write(", "); vga_write_dec((uint32_t)iterations);
    vga_write(" full-screen fills\n");

    vga_write("  write-combining: ");
    vga_write(gfx_write_combining_name());
    vga_putc('\n');

    if (info.mhz == 0) {
        // No usable clock: report raw cycles rather than a wrong
        // millisecond figure. A number in the wrong unit is worse than
        // none, because it invites comparison.
        vga_write("  total: "); vga_write_dec((uint32_t)(cycles / 1000000));
        vga_write(" Mcycles (CPU MHz unknown -- no ms/MB-s figure)\n");
        return;
    }

    uint64_t total_us = cycles / (uint64_t)info.mhz;
    uint64_t per_us = total_us / (uint64_t)iterations;

    vga_write("  per frame: "); vga_write_dec((uint32_t)(per_us / 1000));
    vga_putc('.'); vga_write_dec((uint32_t)((per_us % 1000) / 100));
    vga_write(" ms (");
    if (per_us > 0) vga_write_dec((uint32_t)(1000000 / per_us));
    else            vga_write("inf");
    vga_write(" fps ceiling)\n");

    vga_write("  throughput: ");
    if (total_us > 0) {
        vga_write_dec((uint32_t)(bytes / total_us));   // bytes/us == MB/s
        vga_write(" MB/s\n");
    } else {
        vga_write("(too fast to measure)\n");
    }

    // The console's own hot path, measured separately -- a full-screen
    // fill and a scroll cost wildly different amounts once the surface
    // is write-combined, because only the scroll reads it back. One
    // text line per scroll, which is what console output actually does.
    uint64_t scroll_cycles = gfx_bench_scroll(gfx_char_h(), iterations);

    vga_write("  scroll: ");
    if (scroll_cycles == 0) {
        vga_write("(not measurable on this surface)\n");
        return;
    }
    uint64_t scroll_us = (scroll_cycles / (uint64_t)info.mhz) / (uint64_t)iterations;
    vga_write_dec((uint32_t)(scroll_us / 1000));
    vga_putc('.');
    vga_write_dec((uint32_t)((scroll_us % 1000) / 100));
    vga_write(" ms per text line, ");
    vga_write(gfx_double_buffered() ? "buffered" : "DIRECT (reads the framebuffer)");
    vga_putc('\n');
}

// `hwcursor [demo [x y] | off]` -- the display adapter's own cursor
// plane, from the shell.
//
// IT EXISTS BECAUSE NOTHING ELSE CALLS IT. The hardware cursor has been
// in `struct display_driver` since vmsvga, and no caller ever appeared:
// the ring-3 compositor draws a software sprite, so gfx_hw_cursor_*()
// was reachable code with zero users -- which means an adapter could
// claim DISPLAY_CAP_CURSOR, implement it wrongly, and nothing would
// ever notice. virtio-gpu's cursor queue is real hardware work (its own
// virtqueue, its own resource, its own transfer), so it gets a caller
// that can be pointed at and a test that can read the pixels.
//
// This is a diagnostic, not the feature: the compositor using the plane
// for the real pointer is a roadmap item, and it needs the WM's damage
// bookkeeping to stand down rather than a shell command.
static uint32_t g_hwcursor_img[32 * 32];   // static: the ring-0 frame budget is 1 KiB

void cmd_hwcursor(const char *args) {
    if (!gfx_hw_cursor_available()) {
        vga_write("hwcursor: this display has no cursor plane\n");
        vga_write("  (`-vga virtio` has one; plain `-vga std` does not)\n");
        return;
    }

    if (!args || !args[0]) {
        vga_write("hwcursor: available on this display\n");
        vga_write("usage: hwcursor demo [x y] | hwcursor off\n");
        return;
    }

    if (k_strncmp(args, "off", 3) == 0) {
        gfx_hw_cursor_show(0);
        vga_write("hwcursor: hidden\n");
        return;
    }

    if (k_strncmp(args, "demo", 4) != 0) {
        vga_write("usage: hwcursor demo [x y] | hwcursor off\n");
        return;
    }

    // Optional position, defaulting to somewhere unambiguously inside
    // any mode the ladder can produce.
    uint32_t x = 400, y = 300;
    const char *p = args + 4;
    while (*p == ' ') p++;
    if (*p) {
        const char *sp = p;
        while (*sp && *sp != ' ') sp++;
        char first[16];
        unsigned n = (unsigned)(sp - p);
        if (n >= sizeof first) n = sizeof first - 1;
        for (unsigned i = 0; i < n; i++) first[i] = p[i];
        first[n] = 0;
        while (*sp == ' ') sp++;
        if (!k_parse_u32(first, &x) || !*sp || !k_parse_u32(sp, &y)) {
            vga_write("usage: hwcursor demo [x y] | hwcursor off\n");
            return;
        }
    }

    // A solid magenta square with an opaque alpha. Deliberately a colour
    // this desktop uses nowhere, so a pixel probe reading it cannot be
    // reading anything else -- the whole point of the test being a
    // number rather than a look at a screenshot.
    for (int i = 0; i < 32 * 32; i++) g_hwcursor_img[i] = 0xFFFF00FFu;

    if (!gfx_hw_cursor_define(g_hwcursor_img, 32, 32, 0, 0)) {
        vga_write("hwcursor: the adapter refused the image\n");
        return;
    }
    gfx_hw_cursor_move((int)x, (int)y);
    gfx_hw_cursor_show(1);

    vga_write("hwcursor: 32x32 magenta square at ");
    char buf[16];
    k_utoa(x, buf, sizeof buf); vga_write(buf); vga_write(",");
    k_utoa(y, buf, sizeof buf); vga_write(buf);
    vga_write(" (hwcursor off to hide)\n");
}
