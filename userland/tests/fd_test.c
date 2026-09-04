// The file-descriptor table, from ring 3: dup, dup2, inheritance and
// refcounted close.
//
// WHY A /tests ELF RATHER THAN A KTEST. Every property here is about
// what a PROCESS sees -- its own descriptor table, and what a child
// inherits from it. A KTEST runs in the kernel context, which has no
// address space of its own and therefore no descriptor table, so it
// could only test the helpers rather than the thing they are for.
//
// Prints one line per check and exits with the number of failures, so
// `run fd_test` reports 0 when everything holds.
#include <stdint.h>
#include "rt/sys.h"
#include <string.h>
#include <stdio.h>
#include "syscall_abi.h"

#include "lib/utest.h"

// Reads a whole file back through a fresh fd, so an assertion about
// what was written does not trust the fd that wrote it.
static int slurp(const char *path, char *out, int cap) {
    int fd = sys_open(path, 0);
    if (fd < 0) return -1;
    int64_t n = sys_read(fd, out, (uint64_t)(cap - 1));
    sys_close(fd);
    if (n < 0) n = 0;
    out[n] = '\0';
    return (int)n;
}

int main(void) {
    utest_begin("fd_test", "descriptors, dup and inheritance", 0);

    // --- dup gives a SECOND NAME, not a second stream ----------------
    int d = sys_dup(1);
    utest_check(d >= 3, "dup(1) returns a fresh descriptor");
    // Writing through it reaches the same console fd 1 does. Asserted
    // by the absence of an error rather than by pixels: what matters
    // here is that the descriptor is usable at all.
    utest_check(sys_write(d, "", 0) >= 0, "the duplicate is writable");
    utest_check(sys_close(d) == 0, "and closes cleanly");

    // Closing a duplicate must NOT close the original. If it did, every
    // line printed after this point would vanish -- which is exactly
    // the bug refcounting exists to prevent, and exactly why the check
    // below is a write to fd 1 rather than a flag.
    utest_check(sys_write(1, "", 0) >= 0, "closing the duplicate left fd 1 open");

    // --- dup2 redirects THIS process ---------------------------------
    const char *path = "/fdtest_out.txt";
    int f = sys_open(path, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    utest_check(f >= 3, "open a file for writing");

    // NOTHING MAY PRINT BETWEEN THE dup2 AND THE RESTORE -- including
    // utest_check() itself, which writes to fd 1. The first version of this
    // asserted "dup2 puts the file on fd 1" straight after the dup2 and
    // put its own `ok` line into the file, then failed the comparison
    // and blamed the kernel. Results are captured here and asserted
    // once fd 1 is back.
    int saved = sys_dup(1);
    int r_dup2 = sys_dup2(f, 1);

    sys_print("redirected\n");        // goes to the FILE, not the screen

    int r_back = sys_dup2(saved, 1);
    sys_close(saved);
    sys_close(f);

    utest_check(saved >= 3, "park fd 1 somewhere");
    utest_check(r_dup2 == 1, "dup2 puts the file on fd 1");
    utest_check(r_back == 1, "dup2 puts fd 1 back");

    char buf[128];
    int n = slurp(path, buf, sizeof buf);
    utest_check(n > 0 && strcmp(buf, "redirected\n") == 0,
          "what was printed landed in the file, not on the console");
    if (n <= 0 || strcmp(buf, "redirected\n") != 0) {
        utest_notef("(file held %d bytes: \"%s\")", n, buf);
    }

    // --- dup2(fd, fd) is a NO-OP, and must not close -----------------
    // POSIX is explicit about this, and getting it wrong destroys the
    // stream the call was asked to preserve. The check is that fd 1
    // still works AFTERWARDS -- if dup2 had closed it, this line and
    // every line after would be invisible.
    utest_check(sys_dup2(1, 1) == 1, "dup2(fd, fd) returns the fd");
    utest_check(sys_write(1, "", 0) >= 0, "...and did not close it");

    // --- a bad descriptor is refused ---------------------------------
    utest_check(sys_dup(99) < 0, "dup of a closed descriptor is refused");
    utest_check(sys_dup2(99, 1) < 0, "dup2 from a closed descriptor is refused");
    utest_check(sys_write(1, "", 0) >= 0, "...and a refused dup2 left fd 1 alone");

    // --- a CHILD inherits this table ---------------------------------
    // The whole point of the change: the parent redirects itself, the
    // child inherits, and no fork() is involved. /bin/hello writes a
    // line to fd 1 and exits -- chosen because it is non-interactive
    // and needs no arguments, unlike /tests/echo which reads keys.
    const char *child_path = "/fdtest_child.txt";
    int cf = sys_open(child_path, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    utest_check(cf >= 3, "open a file for the child's output");
    saved = sys_dup(1);
    sys_dup2(cf, 1);
    int pid = sys_spawn("/bin/hello", 0, -1); // -1 = inherit fd 1
    int code = -1;
    if (pid > 0) sys_waitpid(pid, &code);
    sys_dup2(saved, 1);
    sys_close(saved);
    sys_close(cf);

    utest_check(pid > 0, "the child spawned");
    n = slurp(child_path, buf, sizeof buf);
    // `echo` prints its argument; the assertion is that the bytes went
    // to the file this process redirected, which only inheritance can
    // achieve -- the child was never told about the file.
    utest_check(n > 0 && strstr(buf, "Hello") != 0,
          "the child's stdout followed the parent's redirection");
    if (n <= 0 || !strstr(buf, "Hello")) {
        utest_notef("(child file held %d bytes: \"%s\")", n, buf);
    }

    // --- a spawn can NAME the child's fd 0, not just its fd 1 --------
    //
    // The filter shape, with pipes standing in for the connection inetd
    // hands a handler: /bin/cat copies fd 0 to fd 1 and is told nothing
    // else, so bytes coming out the far pipe can only mean both ends
    // were installed. Same kernel path a socket takes.
    int pa[2], pb[2];
    utest_check(sys_pipe(pa) == 0 && sys_pipe(pb) == 0, "two pipes for the filter");

    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.stdin_fd = pa[0];
    o.stdout_fd = pb[1];
    int fpid = sys_spawn_opts("/bin/cat", &o);
    utest_check(fpid > 0, "spawn with stdin_fd and stdout_fd named");

    // OURS GO NOW, BOTH OF THEM. The child holds its own references;
    // keeping the write end of the far pipe would mean this process is
    // also a writer of the pipe it is reading, and EOF never arrives.
    sys_close(pa[0]);
    sys_close(pb[1]);
    sys_write(pa[1], "filter\n", 7);
    sys_close(pa[1]);              // EOF, so cat stops rather than parks

    char fbuf[64];
    int fn = 0;
    for (;;) {
        int64_t got = sys_read(pb[0], fbuf + fn, (uint64_t)(int)(sizeof fbuf - 1 - (size_t)fn));
        if (got <= 0) break;
        fn += (int)got;
    }
    fbuf[fn] = '\0';
    sys_close(pb[0]);
    if (fpid > 0) sys_waitpid(fpid, &code);
    utest_check(fn == 7 && !strcmp(fbuf, "filter\n"),
          "the child read fd 0 and wrote fd 1 -- a plain filter");
    if (fn != 7 || strcmp(fbuf, "filter\n")) {
        utest_notef("(got %d bytes: \"%s\")", fn, fbuf);
    }

    // A REFUSAL IS NOT A SILENT FALLBACK. An fd that is neither the
    // right pipe end nor a socket must fail the spawn, or the child
    // starts with the console on fd 0 and the parent never learns.
    int plain = sys_open(child_path, 0);
    sys_spawn_opts_init(&o);
    o.stdin_fd = plain;
    utest_check(sys_spawn_opts("/bin/hello", &o) < 0,
          "a spawn naming a plain file as stdin is refused");
    sys_close(plain);

    return utest_end();
}
