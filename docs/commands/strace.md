# strace

**a `/bin` program.**

**Category:** Processes and programs

## Synopsis

    strace <program> [args...]

## Description

Linux-style syscall tracing: runs `<program>` with tracing on and prints one decoded line per syscall it makes — `open("notes.txt", O_WRITE\ | O_CREAT) = 3` — plus a `+++ N syscalls traced +++` summary when it exits. Every line is also written to the kernel log, so `dmesg` has the whole trace afterwards and `python3 tools/vm.py exec "strace <program>"` returns text you can assert on rather than a screenshot you have to read.

**It was a kernel shell builtin until 2026-08-23**, and moving it out is why the trace can reach you at all: a ring-0 command could only be typed at the physical console and could only print there, so tracing anything from a Terminal window put the output on a screen nobody was looking at. The program is a spawn and a wait and nothing else — the tracing itself is the kernel's, because every ring-3 syscall funnels through one dispatcher and there is nothing for a tracer to instrument.

**The trace goes to the terminal your fds name, not into any of them.** So `strace foo | grep x` greps `foo`'s output and never the trace, and `strace foo > out.txt` puts `foo`'s output in the file and leaves the trace on your terminal — the same separation real strace gets by writing its stderr. This OS reaches it the other way round: fd 2 here is the kernel log rather than a second terminal stream (`userland/lib/cmd.h` documents that trap at length), so the terminal is found from fd 1, and from fd 0 when fd 1 has been redirected. Only with both ends redirected is there no terminal to name, and the trace falls back to the physical console; `dmesg` always has it either way.

**One traced process at a time**, and tracing is a property of the spawn (`SPAWN_TRACE` on `SYS_SPAWN`'s message) rather than a mode you switch on. That is what makes it race-free: there is no window between "arm" and "start" for somebody else's spawn to fall into.

**What it cannot do:** attach to a program that is already running, trace a program's own children, or filter by syscall. There is no `ptrace` here — a tracer names the child it creates, and that is the whole interface.

**Not reachable from the kernel shell with a bare name.** `strace` needs a scheduler slot of its own (it spawns and waits), which the legacy `run` loader does not have — so at a `#` prompt use `spawn /bin/strace <program>`. At a `$` prompt, in `/bin/tosh` or a Terminal window, it is an ordinary command.
