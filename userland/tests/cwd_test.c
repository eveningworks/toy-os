// The current directory, and the filesystem syscalls that came with it.
//
// WHY THIS IS A /tests ELF AND NOT A KTEST. What is being claimed is
// that a RELATIVE path means the same thing to a program as to the shell
// that started it -- which is a property of the syscall boundary and of
// SPAWN INHERITANCE, neither of which is visible from inside the kernel.
// A KTEST could check that fs_mkdir() works, and it already did; the
// whole bug this exists to prevent is a correct fs_mkdir() reached with
// the wrong path.
//
// THE LOAD-BEARING CHECK IS THE INHERITANCE ONE, and it is shaped so a
// broken version cannot pass. It chdir()s into a subdirectory, spawns
// /bin/mkdir with a BARE NAME, and then asserts twice: that the
// directory appeared where the cwd pointed, AND that nothing of that
// name appeared at the root. Asserting only the first would stay green
// if the kernel resolved against "/" and the check happened to look
// there too; asserting only the second cannot tell a correct spawn from
// a spawn that failed outright.
//
// Prints one line per check and exits with the number of FAILURES.
#include <stdint.h>
#include "rt/sys.h"
#include <string.h>

#include "lib/utest.h"

static void check_errno(int got, int want, const char *what) {
    utest_checkf(got == want, "%s -- got %s%s%s", what, sys_strerror(got),
                 got != want ? ", wanted " : "", got != want ? sys_strerror(want) : "");
}

// Does `path` exist? Asked with stat rather than open so a directory
// answers yes too.
static int exists(const char *path) {
    struct sys_stat st;
    return sys_stat(path, &st) == 0;
}

#define BASE "/tmp/cwdt"
#define SUB  BASE "/sub"

int main(void) {
    utest_begin("cwd_test", "the current directory is the kernel's", 0);

    // Left behind by an earlier run -- these tests run in the LIVE
    // filesystem, so establish the precondition rather than inherit it.
    sys_unlink(SUB "/made");
    sys_unlink(SUB "/inherited");
    sys_unlink(SUB);
    sys_unlink(BASE "/renamed");
    sys_unlink(BASE "/linked");
    sys_unlink(BASE "/file");
    sys_unlink(BASE);
    sys_unlink("/inherited");

    // --- it starts at the root ----------------------------------------
    char here[64];
    int n = sys_getcwd(here, sizeof here);
    utest_check(n == 1 && strcmp(here, "/") == 0, "a fresh process starts at /");

    // --- chdir refuses what is not a directory ------------------------
    utest_check(sys_mkdir(BASE) == 0, "mkdir " BASE);
    utest_check(sys_mkdir(SUB) == 0, "mkdir " SUB);
    int fd = sys_open(BASE "/file", SYS_O_WRITE | SYS_O_CREAT);
    utest_check(fd >= 0, "create " BASE "/file");
    if (fd >= 0) { sys_write(fd, "0123456789", 10); sys_close(fd); }

    utest_check(sys_chdir(BASE "/file") < 0, "chdir onto a FILE is refused");
    check_errno(sys_errno(), ENOTDIR, "chdir onto a file says why");
    utest_check(sys_chdir("/no/such/place") < 0, "chdir onto nothing is refused");
    check_errno(sys_errno(), ENOENT, "chdir onto nothing says why");

    // --- and a refused chdir must not have MOVED anything -------------
    // The failure that would otherwise be silent: a cd that reports an
    // error and moves anyway leaves every later relative path wrong,
    // with nothing pointing back at the cd.
    sys_getcwd(here, sizeof here);
    utest_check(strcmp(here, "/") == 0, "a refused chdir leaves the cwd alone");

    // --- relative paths resolve against it ----------------------------
    utest_check(sys_chdir(SUB) == 0, "chdir " SUB);
    sys_getcwd(here, sizeof here);
    utest_check(strcmp(here, SUB) == 0, "getcwd reports where chdir went");
    utest_check(sys_mkdir("made") == 0, "mkdir with a BARE name");
    utest_check(exists(SUB "/made"), "the bare name landed in the cwd");
    utest_check(!exists("/made"), "and NOT at the root");

    // --- ".." is the kernel's job now ---------------------------------
    utest_check(sys_chdir("..") == 0, "chdir ..");
    sys_getcwd(here, sizeof here);
    utest_check(strcmp(here, BASE) == 0, "..  went up exactly one level");

    // --- getcwd refuses to truncate -----------------------------------
    char tiny[4];
    utest_check(sys_getcwd(tiny, sizeof tiny) < 0, "getcwd into too small a buffer fails");
    check_errno(sys_errno(), ERANGE, "getcwd says the buffer was too small");

    // --- a CHILD inherits it ------------------------------------------
    // The whole point. A bare name handed to a spawned program must mean
    // the same directory it means here.
    utest_check(sys_chdir(SUB) == 0, "chdir back into " SUB);
    int pid = sys_spawn("/bin/mkdir", "inherited", -1);
    utest_check(pid > 0, "spawn /bin/mkdir with a bare-name argument");
    if (pid > 0) {
        int code = -1;
        sys_waitpid(pid, &code);
        utest_check(code == 0, "the child succeeded");
    }
    utest_check(exists(SUB "/inherited"), "the child created it in OUR cwd");
    utest_check(!exists("/inherited"), "and NOT at the root");

    // --- the rest of the new calls ------------------------------------
    utest_check(sys_chdir(BASE) == 0, "chdir " BASE);
    struct sys_stat st;
    utest_check(sys_stat("file", &st) == 0, "stat with a relative path");
    utest_check(st.size == 10 && !st.is_dir, "stat reports the size and the type");
    utest_check(sys_truncate("file", 4) == 0, "truncate");
    utest_check(sys_stat("file", &st) == 0 && st.size == 4, "truncate changed the size");
    utest_check(sys_truncate("file", 9) == 0, "truncate can also GROW");
    utest_check(sys_stat("file", &st) == 0 && st.size == 9, "growing changed the size");

    utest_check(sys_rename("file", "renamed") == 0, "rename");
    utest_check(exists(BASE "/renamed") && !exists(BASE "/file"), "rename moved the name");

    if (sys_link("renamed", "linked") == 0) {
        utest_check(exists(BASE "/linked"), "link made a second name");
        struct sys_stat a, b;
        utest_check(sys_stat("renamed", &a) == 0 && sys_stat("linked", &b) == 0 &&
              a.ino == b.ino, "both names are one inode");
        // Deleting one name must leave the other's DATA intact -- the
        // property that makes it a hardlink rather than a copy.
        utest_check(sys_unlink("renamed") == 0, "unlink one of the two names");
        utest_check(sys_stat("linked", &b) == 0 && b.size == 9, "the other name still has the data");
    } else {
        check_errno(sys_errno(), EPERM, "link refused: this format has no hardlinks");
    }

    // --- sync reports a count, and 0 is a real answer -----------------
    int wrote = sys_sync();
    utest_check(wrote >= 0, "sync did not fail");

    // LEAVE NOTHING BEHIND. tools/check_layout.py compares the built
    // image against docs/filesystem-layout.md and fails on a path no row
    // describes -- which is how it caught the first version of this test
    // seeding four directories into /tmp on every `make iso`. Children
    // before parents, since a non-empty directory is refused. Back at
    // the root first, because the cwd is this process's and unlinking
    // the directory it is standing in is not a thing to leave to luck.
    sys_chdir("/");
    sys_unlink(SUB "/made");
    sys_unlink(SUB "/inherited");
    sys_unlink(SUB);
    sys_unlink(BASE "/renamed");
    sys_unlink(BASE "/linked");
    sys_unlink(BASE "/file");
    sys_unlink(BASE);
    sys_unlink("/inherited");

    return utest_end();
}
