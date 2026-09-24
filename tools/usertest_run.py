#!/usr/bin/env python3
"""Run the self-checking ring-3 diagnostics in /tests, as one pass/fail table.

WHY THIS EXISTS. `make test` runs the in-kernel KTESTs; `gui_regress.py`
runs the GUI tools. Between them sat a gap: the `/tests` binaries are
real ring-3 programs that check real things, and nothing ever ran them
except a person typing `run <name>` at a shell. `libc_test` was written
into exactly that gap -- it covers what a KTEST structurally cannot
reach (the C-name headers and kfmt.o being LINKABLE from ring 3, not the
k_* logic underneath, which has KTESTs already and would pass either
way) -- and it would have been checked once and then never again.

WHAT IT ASSERTS. Each entry names an exit code AND the output that must
appear. Both halves matter: a program that dies before printing anything
can still exit 0 through a path that never ran the checks, and a program
that prints "all checks passed" while returning non-zero is equally
wrong. `exit_test` is in here for a related reason -- its expected code
is 42, so a kernel that lost the exit code entirely and reported 0 for
everything would redden it while every other test stayed green.

THE EXCLUSIONS ARE THE INTERESTING PART, so they are listed in EXCLUDED
below with a reason each rather than silently omitted. The short version:
a test that deliberately faults, one that blocks forever on the serial
port, one that needs a windowed desktop, and one whose result depends on
being spawned by a parent are all things a "just run everything" harness
would report as failures of the code under test rather than of its own
assumptions. That is not hypothetical -- `pipe_test` exits 3 under `run`
(its waitpid finds no parent) and passes perfectly well under the KTEST
that spawns it properly.

Usage:
    python3 tools/usertest_run.py                 # against a copy of disk.img
    python3 tools/usertest_run.py -k libc         # only matching tests
    python3 tools/usertest_run.py --list          # what's in it, and what isn't
    python3 tools/usertest_run.py --instance 2    # alongside another VM

Works against a COPY of disk.img by default (several of these write
files), so it never disturbs an image the user has a QEMU open on.
Exits 0 if every test passed, 1 otherwise, 2 if it could not run.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VM = os.path.join(REPO, "tools", "vm.py")

# name -> (expected exit code, [substrings that must appear],
#          [substrings that must NOT appear])
#
# AN EXIT CODE OF None MEANS "SPAWN IT, AND JUDGE IT BY WHAT IT PRINTS".
# `run` uses the legacy loader, which has no scheduler slot -- so a test
# needing anything that goes through the window server (ugfx_font_init()
# asks for the kernel's glyph tables through SYS_WIN_REQUEST) gets
# refused there and would measure nothing. Such a test is spawned as a
# real process instead, which costs the exit code, so its own printed
# verdict has to carry the whole assertion -- which is what
# UTEST_VERDICT_FILE is for (userland/lib/utest.h).
#
# `None` for the two string lists means the harness's own epilogue,
# which every migrated test prints in one shape. State them only where a
# test deviates.
#
# A FORBIDDEN substring is matched only against lines the test itself
# printed -- lines carrying its own name -- never against everything the
# serial console said during the run. The kernel logs into the same
# stream, and "FAIL"/"FAILED" are words it uses for its own reasons
# (`atac: FLUSH FAILED` among them), so an unscoped match turns unrelated
# kernel noise into a failed test. That is not hypothetical: CI reported
# newsyscalls_test as FAILED on a run whose captured output ended with
# "all phases passed" and exit code 0.
TESTS = [
    ("libc_test", 0, None, None),
    # A setting DECLARED by a file in /etc/settings.d, rather than
    # registered in kernel C (lib/usetting_schema.h). It runs here and
    # not as a KTEST because the library it covers is ring 3's -- the
    # kernel has no way to reach it -- and the check that matters is
    # that the generation MOVES when ring 3 writes /etc, which is the
    # only thing making the desktop notice a change.
    ("usetting_test", 0, None, None),
    # A process mapping a device's register file and READING it --
    # stage 1 of docs/umdf-design.md. The KTESTs beside dev_bar_check()
    # cover every refusal and cannot cover this one: they run on the
    # kernel context, which has no address space to map into.
    #
    # SPAWNED (exit code None), and it has to be: SYS_DEV_MAP_BAR
    # resolves the caller's address space the way sbrk and mmap do, and
    # the legacy `run` loader has no scheduler slot -- so every call
    # comes back EPERM and the test fails reporting three refusals that
    # never reached the code under test.
    ("devbar_test", None, None, None),
    # And stage 2: the claim, and the kernel letting go of a device it
    # was driving. SPAWNED for the same reason -- the claim is keyed to
    # the caller's address space, which the legacy `run` loader has no
    # scheduler slot for.
    #
    # Its HDA leg needs a controller the default headless boot has no
    # reason to attach, so here it exercises the claim on an unbound
    # device and NOTES the skip; tools/devclaim_test.py launches a
    # guest with one for the leg that proves the unbind.
    ("devclaim_test", None, None, None),
    # And stage 3's primitive: the DMA buffer, which is what makes a
    # claimed device able to reach memory at all. SPAWNED for the same
    # reason as the two above, and it is the only check that a released
    # buffer's MAPPING goes with its frames -- a KTEST cannot see that,
    # because the kernel context has no address space to map into.
    ("devdma_test", None, None, None),
    # The ring-3 half of termkey: the encoder is shared source compiled
    # twice, and the KTESTs would pass whether or not ring 3 linked it.
    ("termkey_test", 0, None, None),
    # shellsetting_test is NOT here: this runner starts a test through
    # the legacy `run` loader, and a process loaded that way has no
    # scheduler slot, so its system() does not reach the spawned shell
    # the way an ordinary process's does -- the marker never appears and
    # the test reports a hardcoded path that is not there. It is driven
    # by a KTEST instead (kernel/tty/tty_test.c), which spawns it
    # properly, the same arrangement pty_test and the dash tests have.
    # The argv VECTOR across a spawn (SPAWN_ARGV) and tosh's quoting on
    # top of it. SPAWNED (exit code None): it reads pipes its children
    # write, which the legacy `run` loader cannot block on.
    ("argv_test", None, None, None),
    # fork() and exec(): SPAWNED (exit code None) because a fork needs a
    # scheduler slot, which the legacy `run` loader has not got.
    ("fork_test", None, None, None),
    # A negative priority read back, and a new process's level being its
    # parent's rather than whatever its reused slot last held. SPAWNED:
    # the second needs a real slot to reuse.
    ("prio_test", None, None, None),
    # SPAWN_FD_LOG: a child's stdout landing in the application log,
    # tagged with the CHILD's name. SPAWNED (exit code None) because the
    # thing under test is a spawn -- it waits for two children, which the
    # legacy `run` loader has no scheduler slot to do.
    ("applog_test", None, None, None),
    # The UDP socket API, with no network at all: binding, the ephemeral
    # range, a port refused twice, and what each protocol will not
    # accept. What it CANNOT cover is a datagram reaching anything --
    # that needs a host at the far end, which is tools/net_test.py's
    # phase 6. Given three arguments it becomes that client instead.
    #
    # SPAWNED (exit code None), because two of its checks are about
    # BLOCKING: a receive that must really wait out its timeout, and a
    # reused descriptor that must not have inherited a non-blocking
    # flag. The legacy `run` loader has no scheduler slot, so it cannot
    # block at all -- the kernel correctly gives it the non-blocking
    # answer, and both checks measure nothing there. Same reason
    # cputime_test is spawned.
    ("udp_test", None, None, None),
    # Named shared memory across two processes, which is the only shape
    # that can see it working: one process mapping its own object twice
    # sees its own writes whether or not the frames are shared. SPAWNED
    # (exit code None) because it blocks in waitpid for /tests/shm_child,
    # which the legacy `run` loader has no scheduler slot to do.
    ("shm_test", None, None, None),
    # A futex parking and waking ACROSS two processes, through a shared
    # page. The KTESTs beside sys_futex_wait() cover key derivation and
    # every refusal and cannot cover this -- they run on the kernel
    # context, which has nobody to be woken by. SPAWNED (exit code None)
    # because both halves block: the child parks in the futex and the
    # parent in waitpid, neither of which the legacy `run` loader has a
    # scheduler slot to do.
    ("futex_test", None, None, None),
    # The message channel two ring-3 processes talk over, which is shm
    # plus a futex plus a wakeword and no kernel support of its own.
    # SPAWNED (exit code None) because the server half PARKS -- and a
    # message waking it out of that park is the check the whole file
    # exists for, which the legacy `run` loader has no slot to do.
    ("chan_test", None, None, None),
    # Tab completion's engine, built for ring 3. The KTESTs cover the
    # same source through the KERNEL shell's environment and would pass
    # whether or not a byte of it linked into libuapp.a -- this is the
    # link, plus userland/lib/ucomplete.c's own filesystem hooks.
    ("complete_test", 0,
     None, None),
    # The checksum table, in the ring its only caller runs in.
    # tools/hash_hostcheck.py sweeps the same source far harder against
    # hashlib and zlib, and kernel/lib/kcrc_test.c covers the CRC in ring
    # 0 -- neither can see whether uhash.c LINKS into libuapp.a, which is
    # what this is for. Same reason complete_test exists above.
    # SPAWNED (exit code None): it is a DYNAMIC binary -- the code it
    # checks is /lib/libhash.so -- and the legacy `run` loader refuses
    # one by name ("isn't a valid ELF64 executable"). Same reason
    # dyn_test and dynlibc_test are spawned.
    ("hash_test", None,
     None, None),
    # uui_label's word wrapping. Its load-bearing check is that a word
    # wider than the line is BROKEN rather than refused: refusing it
    # returns the same cursor and loops forever inside a draw call,
    # which hangs the compositor rather than drawing something wrong.
    ("wrap_test", None,
     None, None),
    # A focus indicator on every widget that accepts focus. Its
    # load-bearing checks are the ROW ones: a ring round the whole box
    # and a ring on the selected row both put accent pixels on the
    # surface, and only the height tells them apart. Each widget is also
    # asserted BOTH ways -- absent unfocused, present focused -- because
    # a one-sided check passes on a control that rings itself
    # unconditionally.
    ("focusring_test", None,
     None, None),
    # Type-ahead in uui_listbox and uui_table. Its load-bearing checks
    # are the SORTED ones: a table's rows are pulled and its app indices
    # are not the order on screen, so a search walking app order cycles
    # somewhere the user is not looking while every single-match check
    # still passes. The fixture is stored in reverse for that reason.
    ("typeahead_test", 0,
     None, None),
    # The audio decode path -- the half of lib/usnd.h that needs no
    # sound card. Playback is judged on the HOST instead
    # (tools/audio_test.py records what the device emitted), so these
    # two cover the library between them. Its fixtures are built byte by
    # byte rather than read from /usr/share/sounds: that is the only way
    # to construct a chunk between `fmt ` and `data`, a data chunk that
    # lies about its length, and a float WAV that must be refused as
    # UNPLAYABLE rather than as broken.
    ("usnd_test", 0,
     None, None),
    # Error codes reaching ring 3. Its load-bearing check is that a full
    # descriptor table and a missing file are DIFFERENT answers, which
    # needs a process that has really run out of fds -- see the file.
    ("errno_test", 0,
     ["errno_test: all checks passed",
      "ENOENT and EMFILE are distinct: yes"], ["FAIL"]),
    ("fpu_test", 0, None, None),
    # tolibc's <regex.h>. The engine's own correctness is checked
    # against the SAME case table on the host, and against glibc as an
    # independent oracle (tools/regex_hostcheck.py) -- what this run
    # adds is the second compilation: ring 3, tolibc's malloc and
    # ctype, a 2 KiB frame budget. Same gap libc_test and
    # klineedit_test cover for their own shared sources.
    ("regex_test", 0,
     ["regex_test: all"], ["FAIL"]),
    # The shared line editor's SECOND compilation. Same gap libc_test
    # covers: klineedit.c has KTESTs, and they would pass whether or not
    # ring 3 could link a byte of it.
    ("klineedit_test", 0, None, None),
    # The /etc parser's SECOND compilation, and the same gap again:
    # etc_config.c has KTESTs over the identical shared case table, and
    # every one would pass whether or not libuapp.a linked a byte of it
    # -- which is what /bin/netd and the File Manager read /etc through.
    ("etc_config_test", 0, None, None),
    # The SECOND BUILD of the formatter, not its logic -- the KTEST
    # beside it runs the identical cases in ring 0. What this pins is
    # that libc.a has the same kfmt at all, which is the gap a
    # kernel-only include in the shared half opens silently.
    ("kfmt_test", 0, None, None),
    # The TrueType rasterizer's SECOND compilation, and the same gap
    # klineedit_test covers: userland/lib/ttf.c has KTESTs, and every one
    # of them would pass whether or not ring 3 could link a byte of it.
    # SKIPS ITSELF on an image built with no fonts, so it must not
    # require a pass line that only appears when there is a font.
    ("ttf_test", 0,
     ["ttf_test:"], ["FAIL"]),
    # THE RING-3 ATLAS AGAINST THE KERNEL'S, slot by slot. This is what
    # makes moving font rasterisation out of ring 0 checkable rather than
    # hoped for: one implementation compiled twice has to produce the
    # same bytes on both sides, and QUERY_FONTGLYPH is the kernel's own
    # hash to compare against. SKIPS ITSELF when no face is loaded, since
    # the baked tables are not built by that code path at all.
    ("font_atlas_test", 0,
     ["font_atlas_test:"], ["FAIL"]),
    ("newsyscalls_test", 0,
     ["newsyscalls_test: all phases passed"], ["FAILED"]),
    ("file_test", 0,
     ["filetest: round trip OK"], []),
    ("write_test", 0,
     ["Hello from ring 3"], []),
    ("random_test", 0, None, None),
    # Takes every byte SYS_SBRK will give (~14 MiB), writes an
    # address-derived pattern and reads it back, so it also asserts the
    # heap bound holds -- "sbrk refused" is a REQUIRED line, not an
    # incidental one. Adds about a second.
    ("memtest", 0,
     ["memtest: PASSED", "sbrk refused as expected"], ["FAIL", "MISMATCH"]),
    # The stack GROWS: ~1.6 MiB of recursion, verified on the way back
    # out. The old kernel mapped four pages eagerly and died on the
    # guard below them, so the depth line is the assertion -- see the
    # file's note on why no KTEST can cover this.
    ("stackgrow_test", 0, None, None),
    # A 4 MiB image, four times the ceiling userland/rt/link.ld used to
    # ASSERT against, plus the aliasing check that ceiling existed to
    # make unnecessary. That it LINKS is half the test.
    ("bigimage_test", 0, None, None),
    ("guard_test", 0, None, None),
    # malloc/free in ring 3 -- the kernel's own allocator over sbrk.
    # Its coalescing and reuse checks are measured through sbrk(0), an
    # independent path from the allocator's own bookkeeping.
    ("malloc_test", 0, None, None),
    # mmap/munmap: the arena, file backing, MAP_FIXED, the split, and
    # the 16-region table bound. Address-derived patterns (memtest.c's
    # reason); the fault-fatal cases are deliberately absent here.
    # Spawned: SYS_MMAP needs a scheduler slot to own the mappings, and
    # `run` is the legacy loader, which has none.
    ("mmap_test", None, None, None),
    # The first DYNAMIC executable: /lib/ld-toy.so loads libhello.so,
    # applies every relocation class, and jumps to the real entry.
    # Spawned twice over: dynamic needs PT_INTERP, which the legacy
    # `run` loader refuses by design.
    ("dyn_test", None, None, None),
    # tolibc AS the shared library: no libc.a in the binary at all.
    ("dynlibc_test", None, None, None),
    ("fsgen_test", 0, None, None),
    # lseek/fstat/O_APPEND. Its pattern is POSITION-DERIVED, so a seek
    # landing at the wrong offset reads the wrong letter -- a file of
    # identical bytes cannot tell a working seek from a dead one.
    ("seek_test", None, None, None),
    # fsync(2) from ring 3. The KTESTs beside `storage.sync` cover the
    # half that commits; this covers the half a PROGRAM sees -- the
    # descriptor lookup, the kind check and the errno on a bad fd, none
    # of which the kernel's own tests go near because they call
    # fs_sync_path() directly and never touch a descriptor table.
    ("fsync_test", None, None, None),
    # The C library's stream layer. Two of its required lines are load-
    # bearing and neither is the verdict: "atexit:BA" can only appear if
    # exit() ran the handlers in LIFO order AND flushed an unterminated
    # line after main() returned, which no assertion inside main() can
    # reach. The buffering check inside the file is the other one -- see
    # the test's own header.
    # Stage 3 of the C library. Two of its checks needed a second try to
    # become real: "zz" cannot catch a strtol that returns the
    # post-sign position for a failed parse (both pointers are equal for
    # that input), and nothing else in the file can see whether longjmp
    # restored rsp, because it also restores rbp. See the test's header.
    ("libc3_test", 0, None, None),
    # Stage 4: %f/%e/%g, strtod and math.h. Asserted as formatted TEXT
    # against literals, since that is the only thing a printf caller can
    # observe -- and the accuracy limit is real (printf_float.c), so
    # nothing here asserts a 17th significant digit.
    # THE STAGE 6 PROOF: cJSON, vendored byte for byte from upstream
    # (userland/ports/cjson/), compiled against this C library. Every
    # other test here was written by somebody who knew what the library
    # supported; this one was written years before the OS existed. Its
    # value checks are load-bearing -- a broken strtod still produces
    # VALID JSON, so "it parsed" would measure nothing.
    ("cjson_test", 0, None, None),
    # tolibc's <math.h> transcendentals, against values generated by the
    # host's Python rather than by running toy-os. 405 checks, including
    # identities -- but only where they are WELL CONDITIONED, which cost
    # two rewrites to get right (see the file).
    # The environment, and the only check that matters is the one no
    # single process can make: that a CHILD inherits. Spawned rather
    # than `run` because the parent blocks reading the child's pipe and
    # the legacy loader has no scheduler slot to block on.
    ("env_test", None, None, None),
    ("libm_test", 0, None, None),
    ("libc4_test", 0, None, None),
    # Stage 5: time_t, struct tm, mktime, strftime. Every date is a
    # FIXED known one -- nothing asserts against the current clock, and
    # every expected value was checked against the host's Python
    # datetime rather than against the code under test.
    ("libc5_test", 0, None, None),
    # The timezone conversion, which is libc's since the city database
    # left ring 0 (api/tz.h): the offset, both DST rules and their
    # transition days. It WRITES `system.timezone` and puts it back,
    # including on a failed check -- a test that leaves a setting behind
    # changes the machine for every later tool.
    ("utz_test", 0, None, None),
    # The ten POSIX headers the dash port needed. Exit code is the
    # failure count. Run through the legacy loader, which has NO
    # scheduler slot -- which is why times() must not fail there.
    ("posixhdr_test", 0, None, None),
    ("stdio_test", 0,
     ["stdio_test: all checks passed", "atexit:BA"], ["FAIL"]),
    # SYS_QUERY from ring 3. Runs fine under `run`: it spawns nothing and
    # waits for nothing, so the legacy loader's missing scheduler slot
    # costs it nothing.
    ("query_test", 0, None, None),
    # The JPEG decoder in the ring it runs in. Its reference pixels are
    # LIBJPEG's, recorded by tools/gen_imgdata.py -- a decoder compared
    # against its own output is self-consistent, which a decoder with a
    # wrong IDCT constant also is. The breadth (182 images across every
    # subsampling and quality) is tools/uimg_hostcheck.py's job; this one
    # proves the same .c file works on this heap, in a real process.
    ("uimg_test", 0, None, None),
    # The screensaver option descriptors, against the ones this image
    # actually ships -- a saver's options are a contract between a data
    # file and two programs that never see each other, and a parser
    # tested against strings written beside it keeps agreeing with
    # itself while a shipped descriptor drifts. Runs fine under `run`:
    # it reads and writes files and waits for nothing.
    ("usaver_test", 0, None, None),
    # Not a self-checker: it exists to prove an exit code survives the
    # round trip out of ring 3, so the CODE is the whole assertion.
    ("exit_test", 42, [], []),
]

# Deliberately not run here. Each line is a reason, not an apology --
# adding one of these without solving the reason would produce a red
# table that says nothing about the code under test.
EXCLUDED = [
    # The two BENCHMARKS. Both are real programs under /tests and both
    # report a timing number, which would make this gate flap for
    # reasons that have nothing to do with the code under test. They are
    # listed here rather than left unnamed because a program no runner
    # names is run when somebody types it, which is never -- cjson_bench
    # sat outside every runner until stdio_bench was added beside it.
    ("cjson_bench",      "a benchmark, not a test: a timing number would make the "
                          "gate flap. Run it with `spawn /tests/cjson_bench`"),
    ("stdio_bench",      "a benchmark, not a test: it measures what BUFSIZ should be "
                          "and reports throughput, which varies with the host. Run it "
                          "with `spawn /tests/stdio_bench` and read its log with "
                          "`cat /var/tmp/stdio_bench.log` -- it outlives a console "
                          "capture, which is why it writes one"),
    ("fslat_bench",      "a benchmark, not a test: per-call fs latency, for "
                          "tools/fs_isolation.py -- a timing number would make the gate flap"),
    ("crash_test",       "faults on purpose; the point is the kernel's recovery"),
    ("nx_test",          "faults on purpose (jumps into a data page) -- see faulttest_run.py"),
    ("stack_smash_test", "faults on purpose (trips the stack canary)"),
    ("stackovf_test",    "runs off the stack on purpose; the assertion is the KERNEL's "
                          "report, not an exit code -- see its own top comment"),
    ("write_bad_test",   "hands the kernel a bad pointer on purpose"),
    ("thread_test",      "SYS_THREAD_CREATE needs the caller to hold a scheduler "
                          "slot, and `run` is the legacy loader, which has none -- "
                          "so every check would fail against a correct kernel; "
                          "kernel/proc/thread_test.c spawns it"),
    ("waitany_test",     "SYS_WAITPID(-1) needs the caller to BE a scheduled process; "
                          "`run` uses the legacy loader, which has no pid, so nothing it "
                          "spawns has a parent and wait-any has nobody to ask about. "
                          "Drive it with `spawn /tests/waitany_test` instead"),
    ("orphan_test",      "abandons children on purpose and exits at once; the "
                          "assertion is whether INIT reaps them, which is the process "
                          "table before and after -- tools/init_test.py"),
    ("sleep_test",       "SYS_SLEEP refuses a caller with no scheduler slot, and `run` "
                          "is the legacy loader, which has none. Driven by "
                          "tools/init_test.py through `spawn` instead"),
    ("pipefull_test",    "the writer must BLOCK on a full pipe, and the legacy "
                          "`run` loader has no scheduler slot to park in -- so the "
                          "write reports 0 there and the test fails against a "
                          "correct kernel; kernel/proc/fd_test.c spawns it"),
    ("notready",         "never exits and never says anything -- it is init's "
                          "readiness-timeout fixture, not a test; "
                          "tools/init_test.py stages it as a service"),
    ("pipedrain",        "the reading half of pipefull_test; on its own it waits "
                          "on a console that never reaches EOF"),
    ("catin",            "copies stdin to stdout; without a `<` it waits on the "
                          "console, which has no EOF"),
    ("cwd_test",         "its load-bearing check spawns /bin/mkdir with a bare name "
                          "and waits for it, which the legacy `run` loader cannot do "
                          "(no scheduler slot, so waitpid returns before the child has "
                          "created anything and inheritance reads as broken); "
                          "kernel/fs/cwd_test.c's KTEST spawns it properly"),
    ("fd_test",          "spawns a child and waits for it, which the legacy `run` "
                          "loader cannot do (no scheduler slot, so waitpid returns "
                          "at once and it reads the child's file too early); "
                          "kernel/proc/fd_test.c's KTEST spawns it properly"),
    ("pipe_test",        "needs a parent to spawn it and reap it; exits 3 under `run`, "
                          "and kernel/proc/pipe_test.c's KTEST covers it properly"),
    ("posix_test",       "needs a procs[] slot of its own for the same reasons "
                          "signal_test does -- it spawns children, waits for them and "
                          "reads its own process group, none of which the legacy `run` "
                          "loader has. kernel/proc/signal_test.c spawns it, beside "
                          "signal_test, since it is the same syscall surface wearing "
                          "POSIX's names"),
    ("signal_test",      "needs a procs[] slot of its own: under `run` it has no pid, "
                          "no process group and no pending mask, so every check would "
                          "measure the absence of a process rather than the behaviour "
                          "of one. kernel/proc/signal_test.c spawns it properly"),
    ("trace_test",       "needs a procs[] slot of its own: it SPAWNS a child and waits "
                          "for it, and the legacy `run` loader has neither a pid to "
                          "spawn from nor a waitpid that means anything. "
                          "kernel/proc/strace_test.c spawns it properly"),
    ("cputime_test",     "must be SCHEDULER-spawned to have a procs[] slot at all; "
                          "under `run` (the legacy process_run_ring3 path) it has none, "
                          "so it cannot find itself and nothing is billed to it either. "
                          "kernel/proc/cputime_test.c's KTEST spawns it properly"),
    ("echo_test",        "blocks forever reading a serial port with nothing on the far end"),
    ("winclient",        "windowed TWP client -- tools/winclient_test.py"),
    ("uiclient",         "windowed ugfx client -- tools/uiclient_test.py"),
    ("hangclient",       "wedges on purpose -- tools/forcequit_test.py"),
    ("event_test",       "waits on window events that only a desktop delivers"),
    ("counter_a",        "runs until descheduled; a scheduler fixture, not a checker"),
    ("counter_b",        "as counter_a"),
    ("spin_test",        "spins forever on purpose"),
    ("fpu_race",         "a scheduler fixture -- kernel/proc/sched_test.c asserts on it"),
    ("socket_test",      "needs a network peer"),
]

# A spawned test is waited for by READING ITS VERDICT, not by sleeping.
# Every utest program ends with exactly one of these two lines, so the
# artifact is guaranteed to arrive -- and asking for it repeatedly costs
# nothing when it is already there. The fixed 3-second sleep this
# replaced was a coin flip the moment a build changed the timing: under a
# TRAP gate (idt.c) it lost every time, and focusring_test was reported
# as failing while its own file said `all checks passed`.
VERDICT_DONE = ("all checks passed", "FAILED --")
# GENEROUS, because polling costs nothing when the answer is already
# there and this bound is a HANG GUARD rather than a budget. 15 s was
# too tight: argv_test spawns /bin/tosh several times and was still on
# its third check when the poll gave up, which reads exactly like a
# truncated verdict.
VERDICT_TIMEOUT_S = 90.0
VERDICT_POLL_S = 0.5

EXIT_RE = re.compile(r"Exit code:\s*(-?\d+)")


def vm(args, *argv, check=True):
    cmd = [sys.executable, VM, "--disk", args.disk]
    if args.instance:
        cmd += ["--instance", str(args.instance)]
    cmd += list(argv)
    r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    if check and r.returncode != 0:
        print(r.stdout + r.stderr, file=sys.stderr)
    return r


def verdict_file(args, name):
    """The test's own verdict, polled until it terminates or the deadline.

    Waits on an ARTIFACT that must come to exist: every utest program
    ends with exactly one of VERDICT_DONE. There is deliberately no
    "the file is missing" shortcut -- the obvious one looked for "not
    found" in the reply and matched unrelated console text, which made
    the poll return on its FIRST read and reported passing tests as
    truncated. The deadline is the only way out.
    """
    deadline = time.time() + VERDICT_TIMEOUT_S
    text = ""
    while True:
        b = vm(args, "exec", f"cat /tmp/{name}.out", "--label", check=False)
        text = b.stdout + b.stderr
        if any(d in text for d in VERDICT_DONE) or time.time() >= deadline:
            return text
        time.sleep(VERDICT_POLL_S)


def run_one(args, name, spawned=False):
    if spawned:
        # Spawned, then dmesg'd: `spawn` returns as soon as the child
        # exists, so the child's own output is collected on the second
        # command rather than from the first. There is no exit code to
        # read -- see the TESTS table comment.
        a = vm(args, "exec", f"spawn /tests/{name}", "--label", check=False)
        # THE VERDICT IS READ BACK FROM A FILE, not from the console.
        # `spawn` returns as soon as the child exists, so its output
        # arrives while this harness is between commands -- where it is
        # dropped. Chaining `dmesg` onto the same exec collects the log
        # from BEFORE the test ran; sleeping and then asking collects it
        # from after the output was already discarded. Both were tried.
        # The test writes its own verdict to /tmp, which can be asked for
        # at any time -- waiting on the artifact rather than the timing.
        return None, a.stdout + a.stderr + verdict_file(args, name)
    r = vm(args, "exec", f"run {name}", "--label", check=False)
    out = r.stdout + r.stderr
    m = EXIT_RE.search(out)
    # NOT extended to this path, and that was MEASURED rather than
    # assumed: reading the verdict file here as well took the suite from
    # 1 failure in 4 runs to 3, because a `run` test's file is still
    # being written while the poll reads it and the partial content then
    # replaces a console line that was complete.
    return (m.group(1) if m else None), out


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disk", default=None,
                    help="image to boot (default: a temporary copy of disk.img)")
    ap.add_argument("--instance", type=int, default=0,
                    help="vm.py slot, for running alongside another VM")
    ap.add_argument("-k", metavar="SUBSTR", default="",
                    help="only tests whose name contains SUBSTR")
    ap.add_argument("--list", action="store_true",
                    help="print what is run and what is deliberately not")
    args = ap.parse_args()

    if args.list:
        print("Run by this tool:")
        for name, code, want, _ in TESTS:
            note = f"exit {code}" + (f", expects {want[0]!r}" if want else "")
            print(f"  {name:<18} {note}")
        print("\nDeliberately NOT run:")
        for name, why in EXCLUDED:
            print(f"  {name:<18} {why}")
        return 0

    selected = [t for t in TESTS if args.k in t[0]]
    if not selected:
        print(f"usertest_run: no test matches -k {args.k!r}")
        return 2

    tmp = None
    if not args.disk:
        src = os.path.join(REPO, "disk.img")
        if not os.path.exists(src):
            print("usertest_run: no disk.img -- run `make iso` first")
            return 2
        # A copy, not the real image: several of these write files, and
        # the user may have their own QEMU holding a write lock on it.
        tmp = tempfile.NamedTemporaryFile(suffix=".img", delete=False)
        tmp.close()
        subprocess.run(["cp", "--reflink=auto", src, tmp.name], check=True)
        args.disk = tmp.name

    try:
        if vm(args, "start").returncode != 0:
            print("usertest_run: could not start the VM")
            return 2
        results = []
        for name, want_code, want, forbid in selected:
            spawned = want_code is None
            # `None` means the harness's own epilogue (userland/lib/utest.h),
            # which every migrated test prints in one shape. A row states
            # its strings only where it deviates -- a test that reports
            # something else, or that must be judged on a second line as
            # well. That is what took this table from a per-test string
            # each to a handful of exceptions.
            if want is None:
                want = [f"{name}: all checks passed"]
            if forbid is None:
                forbid = ["FAIL"]
            code, out = run_one(args, name, spawned)
            problems = []
            if spawned:
                pass  # judged by its printed verdict alone
            elif code is None:
                problems.append("no exit code (did it hang?)")
            elif int(code) != want_code:
                problems.append(f"exit {code}, wanted {want_code}")
            for s in want:
                if s not in out:
                    problems.append(f"missing {s!r}")
            # Only the test's OWN lines -- see the TESTS table comment.
            mine = [l for l in out.splitlines() if name.split("_")[0] in l]
            # A scoped check that found no lines to scope to cannot fail,
            # which is indistinguishable from passing. Say so.
            if forbid and not mine:
                problems.append("no output lines carry this test's name -- "
                                "its forbidden-substring checks could not run")
            for s in forbid:
                hits = [l for l in mine if s in l]
                if hits:
                    # Quote the offending line. Printing only the tail of
                    # the output (below) hid it completely the one time
                    # this fired, which turned a one-line diagnosis into
                    # an investigation.
                    problems.append(f"saw {s!r} in: {hits[0].strip()[:80]}")
            # **ASK THE GUEST WHILE IT IS STILL THERE.** A failure here
            # used to be diagnosed afterwards, from the output alone,
            # against a machine that no longer existed -- which sent one
            # session chasing a hang that was never happening. The three
            # questions worth having are whether the test is still a
            # process, what the verdict file really holds, and what the
            # kernel said; all three are one command each, and only on a
            # failure.
            if problems:
                post = vm(args, "exec", "ps", "--label", check=False)
                whole = vm(args, "exec", f"cat /tmp/{name}.out", "--label",
                           check=False)
                log = vm(args, "exec", "dmesg", "--label", check=False)
                out += ("\n--- after the failure ---\n"
                        + post.stdout + post.stderr
                        + whole.stdout + whole.stderr
                        + "\n".join((log.stdout + log.stderr).splitlines()[-150:]))
            results.append((name, problems, out))
    finally:
        vm(args, "stop", check=False)
        if tmp:
            os.unlink(tmp.name)

    failed = 0
    print()
    for name, problems, out in results:
        if problems:
            failed += 1
            print(f"  FAIL  {name:<18} {'; '.join(problems)}")
            # The test's OWN lines first, all of them: a self-checking
            # binary says which phase failed, and that line is usually
            # nowhere near the end of the output. Printing only the tail
            # hid it completely on a CI failure and turned "which phase?"
            # into a second round trip.
            own = [l for l in out.splitlines() if name.split("_")[0] in l]
            for line in own[-20:]:
                print(f"          | {line}")
            # **AND THE DIAGNOSTICS, WHICH USED TO BE COLLECTED AND THEN
            # THROWN AWAY.** The block above filters to lines carrying
            # the test's NAME, which is right for its own verdict and
            # wrong for everything the failure path just asked the guest:
            # `ps`, the verdict file and dmesg mention the test's name
            # hardly at all, so a stall that needed the PROCESS TABLE to
            # diagnose printed four lines of "exit() called by ring-3
            # process" instead. The whole point of asking was to keep
            # the evidence from a machine that is about to be torn down.
            marker = "--- after the failure ---"
            if marker in out:
                for line in out[out.index(marker):].splitlines():
                    print(f"          . {line}")
            else:
                tail = [l for l in out.strip().splitlines()[-4:] if l not in own]
                for line in tail:
                    print(f"          . {line}")
        else:
            print(f"  ok    {name}")
    total = len(results)
    print(f"\nusertest_run: {total - failed}/{total} passed"
          f"{'' if not failed else f', {failed} FAILED'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
