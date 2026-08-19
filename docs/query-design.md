# Kernel state without a filesystem: the query registry

A staged plan, in the shape `docs/init-design.md` used. It answers the
question `docs/init-design.md` left open as **R10** -- how the kernel
shell's ~15 introspection commands (`meminfo`, `heap`, `kstack`,
`dmesg`, `ata`, `parttable`, ...) become ring-3 programs -- and it
answers it **differently from that document's assumption**, which was
Linux's `/proc`.

Nothing here is built yet.

## Why not `/proc`

`/proc` is the obvious answer and it is the wrong first one here, for
three reasons that get worse in order.

1. **It needs a mount table that does not exist.** `kernel/fs/vfs.c`
   holds `static const struct fs_ops *g_fs` -- ONE backend, globally,
   chosen by probing the disk. A `/proc` is a second filesystem mounted
   at a path, so it needs `docs/roadmap.md`'s **Real mount points**
   milestone first. That is a large change to the most safety-critical
   code in the tree, taken on in order to print `mem_free`.
2. **It inverts a dependency.** Diagnostics would sit ON TOP of storage:
   a machine whose disk failed to mount would lose its introspection at
   exactly the moment somebody needs it. `dmesg` explaining why the
   filesystem did not mount must not itself be a file.
3. **The output would be TEXT.** `/proc/meminfo` is prose, and every
   consumer re-parses it. This project already has the rule
   (`uui_table` cannot sort the text it draws) and it costs real
   features: a Task Manager that graphs memory needs a number, not a
   line to scrape.

Linux is the only mainstream system that made this a filesystem.
Windows uses one syscall with an information class --
`NtQuerySystemInformation(class, buf, len, &returned)` -- and macOS/BSD
uses `sysctl(mib[], ...)`, a named tree, still a syscall. The
filesystem is the outlier, and it is the one shape toy-os cannot
cheaply afford.

## What already exists, and what is wrong with it

toy-os is most of the way to the NT shape without having named it:

| syscall | what it reports |
|---|---|
| `SYS_PROC_INFO` | one process-table slot |
| `SYS_PCI_INFO` / `SYS_PCI_COUNT` | one PCI device |
| `SYS_CPU_INFO` | CPU identity |
| `SYS_SYSINFO` | memory and disk usage |

Four syscalls that are each **one information class**, each with its own
number, struct and handler. The number grows by one per fact, which is
the growth `tools/check_dispatch.py` exists to notice, and there is
nothing that can answer "what facts exist?".

## The shape

**One syscall, an information class, a typed struct, and a REGISTRY of
providers.**

```
    /bin/meminfo   /bin/df   /bin/query   /bin/config   Control Panel
          \           \         |            /   \           /
           +-----------+--------+-----------+     \         /
                          |                        \       /
                    SYS_QUERY                    SYS_SETTING
                    (read-only)                  (read/write)
                          |                            |
                 query provider registry        setting registry
                  mm / proc / drivers / fs        /etc/*.conf
                          |                            |
                   live kernel state           files + live variables
```

`SYS_QUERY(class, index, buf, len)`:

- **`class`** names the fact (`QUERY_MEMINFO`, `QUERY_PROCESS`, ...).
- **`index`** selects one element for a class that is a LIST, and is 0
  for a scalar class. Same convention `SYS_PROC_INFO` already uses,
  including its rule that an empty slot is a SUCCESSFUL report of an
  empty record -- enumeration SKIPS rather than stopping.
- **`buf`/`len`** receive the class's struct. `len` is the caller's, so
  a struct that grows does not break an old binary: the kernel copies
  `min(len, sizeof)`, exactly what `NtQuerySystemInformation` does with
  its returned-length argument.
- Returns the number of BYTES written, or a negative errno.

A provider REGISTERS itself, the way `display_driver`, `block_device`,
`clocksource` and `struct setting` already do:

```c
struct query_provider {
    uint32_t    cls;                 // QUERY_*
    const char *name;                // "meminfo" -- what /bin/query takes
    uint32_t    size;                // sizeof the class's struct
    int       (*count)(void);        // 1 for a scalar; the list length otherwise
    int       (*fill)(int index, void *out);
};
```

So adding a fact is **a struct in `abi/`, a provider, and a
registration** in the subsystem that owns it -- `kernel/mm/` owns
memory, `kernel/proc/` owns processes, a driver owns its own device.
There is no central table to edit and nothing to forget, which is the
same property that makes adding a syscall three edits and no registry.

## What this is NOT

- **Not writable.** A fact is computed on every read and never
  persisted. Anything settable is a SETTING (below), and that
  separation is deliberate -- see "Facts, settings and tunables".
- **Not a replacement for `/proc` forever.** If a mount table lands
  later, a `/proc` becomes a thin READ-ONLY VIEW over this registry --
  the same way Windows grew a WMI face over the same query calls. The
  registry stays the source of truth; the filesystem, if it ever
  exists, is a rendering of it.
- **Not a text protocol.** Formatting is `/bin/meminfo`'s job.

## Facts, settings and tunables

Three kinds, ONE addressing scheme, two registries.

| kind | example | registry | writable | persisted |
|---|---|---|---|---|
| **fact** | `mem_free`, the process list | query | no | never |
| **config setting** | `font_size`, `timezone` | setting | yes | yes |
| **tunable** | a scheduler slice, a cache depth | setting | yes | optionally |

**A tunable is a setting, not a third thing.** `struct setting` already
carries a namespace derived from its file and one `apply` that
validates, applies AND persists; a kernel tunable is a setting whose
`apply` writes a live kernel variable, in a namespace like `kernel`
from its own `/etc/sysctl.conf`. `config set kernel.foo` then works
with no new mechanism, Control Panel grows a row for free, and the
value is restored at boot exactly as `font_size` is. This is sysctl's
split precisely: `/etc/sysctl.conf` persists, `sysctl -w` writes live,
and readings live somewhere else entirely.

**One thing settings cannot express today, and must:** a knob you want
applied LIVE and NOT written -- a debug tunable for one boot.
`setting_set()` always persists. That needs a flag on `struct setting`
and one branch, and it makes `config diff` (live vs stored) meaningful
for tunables rather than always empty. `SETTING_UNSAVED` stops being a
warning there and becomes the intended outcome.

**Why not one registry with a read-only flag.** The setting registry's
whole value is answering "what can I change?" -- that is what generates
Control Panel. Two hundred read-only counters in that list bury the ten
real settings, and Control Panel would have to start filtering a list
it currently trusts wholesale.

## `config` reads facts, and SAYS SO

`config get system.mem_free` resolves against the setting registry
first and falls back to the query registry -- so one command reads
both, while only settings can be written (`config set` on a fact fails
because no setting has that name, not through a special case).

**The fallback must LABEL its answer.** A fact prints as read-only
kernel state with its class named, never in the shape a `/etc` key
prints in. Two reasons, and the second is the important one: a person
must never be left unsure whether they just read a file or the running
kernel, and the two have different lifetimes -- a setting survives a
reboot and a fact does not exist between them.

Nothing about `config` is privileged. It is one client of `SYS_QUERY`
among several, and an app that wants a number calls the syscall itself.

## Tools

**A command per fact, plus one dumper.** `/bin/meminfo`, `/bin/dmesg`,
`/bin/kstack` each format their own class -- which is what `free`, `ps`
and `df` are on Linux, and what `/bin/ps`, `/bin/lspci`, `/bin/lscpu`
and `/bin/df` already are here. Plus one generic `query <class>` that
dumps any class raw, for bringing a new provider up before its command
exists, and for looking at a class that has no command yet.

## Staging

Each stage builds, boots and passes the existing suites on its own.

### Stage 0 -- the mechanism, and one real provider

`SYS_QUERY`, `struct query_provider`, the registry, and **memory** as
the first class, with `/bin/meminfo` over it. One provider rather than
none, for this project's own bar: a registry with no users is a
framework, and its first real caller is what finds the shape wrong.

**Exit criterion:** `meminfo` at the kernel shell and `/bin/meminfo`
report the same numbers, and a positive control that makes the provider
return stale values reddens the ring-3 one.

### Stage 1 -- fold the four existing syscalls in

`SYS_PROC_INFO`, `SYS_PCI_INFO`, `SYS_CPU_INFO` and `SYS_SYSINFO`
become classes. The old numbers stay and forward to the registry, so
`/bin/ps`, `/bin/lspci`, `/bin/lscpu`, `/bin/df`, Task Manager and
Control Panel are untouched; they are removed only once nothing calls
them.

### Stage 2 -- the introspection commands move

One at a time, each becoming a `/bin` program over its own class:
`meminfo` (done in stage 0), `dmesg`, `kstack`, `heap`, `ata`,
`parttable`, `lspci` (already a program, now over a class). Each is
independently verifiable and the kernel shell keeps working throughout.

### Stage 3 -- tunables

The non-persisting flavour of `struct setting`, plus the first real
kernel tunable and its `/etc` file. Needs a tunable somebody actually
wants; adding the flavour without one is the framework-with-no-users
mistake again.

### Stage 4 -- `config` reads facts

The labelled fallback. Last, because it is the only piece that needs
BOTH registries to be settled.

## Open questions

1. **Does a class get to be variable-length?** `dmesg` is a buffer, not
   a struct, and forcing it into fixed records means an index that is a
   line number and a re-read per line. The alternative is a class whose
   `fill` writes bytes and reports a total, which is a second shape.
   Needed at stage 2, not before.
2. **Does the kernel shell keep its copies?** The session that started
   this chose "keep a rescue shell", so probably yes for the ones worth
   having when userland is broken (`meminfo`, `dmesg`) and no for the
   rest. Decide per command as each moves, not up front.
3. **Is `index` enough for a list that CHANGES while it is walked?**
   The process table already has this and answers it by reporting empty
   slots rather than compacting. A class whose list is generated fresh
   per call (open files, say) does not have that luxury.
