# Kernel state without a filesystem: the query registry

A staged plan, in the shape `docs/init-design.md` used. It answers the
question `docs/init-design.md` left open as **R10** -- how the kernel
shell's ~15 introspection commands (`meminfo`, `heap`, `kstack`,
`dmesg`, `ata`, `parttable`, ...) become ring-3 programs -- and it
answers it **differently from that document's assumption**, which was
Linux's `/proc`.

**Status: the mechanism is BUILT.** Stage 0 shipped 2026-08-19 --
`kernel/lib/query.c`, `SYS_QUERY`, the self-describing
`QUERY_PROVIDERS` class and `/bin/meminfo` as its first consumer --
and stage 2 is under way (see its section below for what has moved).
This line used to read "nothing here is built yet" while the staging
section below it already marked stage 0 done, which is the shape of
staleness worth naming: a status sentence at the TOP of a staged plan
has to be re-read every time a stage lands, and nothing makes anyone
do that. The per-stage markers are the authority.

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

Three kinds, ONE addressing scheme, two registries. **The terms are
defined once, in
[settings-and-queries.md](settings-and-queries.md#the-vocabulary)** --
a fact is read-only and computed, a setting is read/write and
persisted, a tunable is a setting whose `apply` also writes a live
kernel variable. They are the project's words for these three things;
repeating the table here would be a second copy to keep true.

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

### Stage 0 -- the mechanism, and one real provider -- DONE 2026-08-19

`SYS_QUERY` (`abi/query_abi.h`), `struct query_provider`, the registry
(`api/query.h`, `kernel/lib/query.c`), and **memory** as the first class
(`kernel/mm/mem_query.c`), with `/bin/meminfo` over it.

**Exit criterion: met.** `meminfo` at the kernel shell and
`/bin/meminfo` report the same numbers -- `total: 524159` and a 4096-byte
frame from both. They agree because they are ONE READER: `cmd_meminfo()`
calls `query_read()`, and so does the syscall. `used`/`free` legitimately
differ between the two runs by ~14 frames, because the ring-3 one is
itself a process that consumed some; a fact is live, and a check that
demanded byte-identical output would be asserting the wrong thing.

Four things came out of building it that the plan did not have:

- **Two classes, not one.** `QUERY_PROVIDERS` (class 0, the registry
  describing itself) ships beside `QUERY_MEMINFO`. Without it the LIST
  half of the mechanism -- `count()`, `index`, the walk -- would have had
  no caller and therefore no test, which by this project's own rule is a
  half nobody has validated. It also means a program needs to know
  exactly ONE number to discover every other class.
- **`config get` needed named fields.** A record has several fields, so
  `config get mem.frame_free` cannot work without a way to name one.
  `struct query_field` (name, type, offset) declares them, `QUERY_FIELD()`
  derives the offset from the member so nobody maintains a number, and
  the FIELD ops carry names and values across the syscall boundary while
  offsets stay kernel-side -- so a record can still grow append-only.
- **A LIST class is not addressable as a value, and says so.** `config
  get providers.anything` answers `-ENOTSUP`, deliberately distinct from
  `-ENOENT`: "that fact is a table, use a tool that can show one" versus
  "no such fact". The first sends a reader somewhere useful; the second
  sends them hunting for a typo they did not make. Linux keeps processes
  out of sysctl for the same reason.
- **`SYS_SYSINFO` folded in early** (stage 1's first half). It computed
  `mem_free_kb` from `pmm_free_frames() * 4` -- a second reader of the
  same numbers, and a hardcoded frame size. It reads through the
  registry now, so there is one reader rather than three that agree.

**The positive controls both fired, and one of them found a bug in the
test.** Making the provider cache `frame_free` reddened exactly the
liveness check. Making the syscall ignore the caller's `len` reddened
the truncation check -- but NOT the sentinel beside it, because the
first version of that fixture handed the kernel a full-size buffer and
only a short length, so a kernel writing the whole record still fit. The
buffer is genuinely short now and both fire. That is the control earning
its keep on the test rather than on the code.

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

**Done 2026-08-20: `df` and `meminfo` fully.** Both had already been
`/bin` programs for their arithmetic, and both still had a kernel
builtin in front of them holding the part ring 3 could not ask for.
Three classes closed that gap and the builtins were deleted:

- `QUERY_FSINFO` (scalar) -- the mounted backend's NAME and whether it
  persists, beside the usage numbers. It is what `df` was a builtin
  for, and it also restores the filesystem line
  `userland/gui/system/about.c` had been omitting with a comment
  saying ring 3 could not ask.
- `QUERY_MEMMAP` (list) -- the firmware memory map, one record per
  region, over the `multiboot_mmap_foreach()` walk that already
  existed. The first LIST provider outside `QUERY_PROVIDERS` itself.
- `QUERY_MMAUDIT` (list) -- one record per DANGLING mapping, so zero
  records means healthy. A list rather than a count because the
  addresses are the diagnostic: a space with two violations is exactly
  when "how many" stops being enough. It needed
  `vmm_audit_space_cb()`, since the summary records only the first.

The shape worth copying: **the question to ask of a builtin is not "is
this a file command" but "can ring 3 ask?"** -- and when the answer is
no, the fix is a provider, not a syscall and not keeping the builtin.

**Done 2026-08-20: `about`, `time`, `random`, `reboot`, `kill`, `spawn`
and `parttable`.** Four needed nothing new (`sys_gettime`, `sys_kill`,
`sys_spawn`, `sys_poweroff` all existed). Three more classes covered
the rest:

- `QUERY_RANDOM` (scalar) -- the entropy SOURCE. Deliberately not part
  of `SYS_GETRANDOM`, whose ABI comment refuses to report quality on
  the grounds that a caller checking it per draw "would mostly use it
  to decide to carry on anyway". That reasoning is about a guarantee
  attached to bytes. "What is this machine's entropy source?" is a
  different question with a human audience, and it was the entire point
  of the `random` command -- the numbers look equally random whatever
  produced them. A fact answers it without putting a promise on the
  syscall that this kernel cannot keep.
- `QUERY_PARTTABLE` (scalar) + `QUERY_PARTITION` (list) -- **two
  classes for one command, and the split is the point.** A list alone
  cannot distinguish "a table with no partitions" from "no partition
  table at all": both are zero records, and the second is what this
  repo's own `disk.img` is. The table class carries the kind.

**What did NOT move, and why it is the same reason three times.**
`heap`, `ata` and `kstack` each have a READ half and a WRITE half
(`heap debug on|off`, `heap check`, `ata nodma on|off`, `kstack track
on|off`). Moving only the read half would put one command in two rings
-- the exact shape the `ls` wrapper had, and the thing this whole
sequence exists to delete. They are stage 3 work: the write halves want
to be TUNABLES, which `struct setting` already carries across the ring
boundary in both directions, so `/bin/heap` would read a provider and
flip a tunable with no new syscall at all. That is now three tunables
somebody actually wants, which is the trigger stage 3 was waiting for.

### Stage 3 -- tunables -- DONE 2026-08-20

The non-persisting flavour of `struct setting`, and the first three real
kernel tunables: `kernel.heap_debug`, `kernel.ata_nodma` and
`kernel.kstack_track` (`kernel/lib/tunables.c`). It had been waiting for
"a tunable somebody actually wants" so the flavour would not ship
without users; three arrived at once, each the write half of a command
whose read half was ready to move.

`struct setting` gained NOTHING. A tunable names `CONFIG_PATH_RUNTIME`
as its file, which both marks it as unpersisted and gives it the
`kernel` namespace -- see `docs/decisions.md` for why that is a sentinel
path rather than a flag or an empty string.

**`heap check` did NOT need a write-triggered action after all.** It
became `QUERY_HEAPCHECK`, a class whose READ performs the scan -- which
is what a fact already is here ("computed fresh on every read, no
stored form"), and the shape `QUERY_MMAUDIT` had been using since the
day before. The drop_caches precedent would have worked; it was simply
the wrong tool for something that returns a RESULT rather than
performing a side effect.

The one care it needs: that class declares NO named fields, so
`config get heapcheck.damaged` cannot exist. A field is what `config`
and any generated UI enumerate, and a full heap scan running as a side
effect of something that reads like an inspection would be a trap.

### Stage 2, continued -- `heap`, `ata` and `kstack` -- DONE 2026-08-20

The last three introspection commands, and the ones that needed BOTH
registries: a query class for the read half and a tunable for the
write half. `QUERY_HEAP` + `QUERY_HEAPCHECK`, `QUERY_ATA`,
`QUERY_KSTACK` + `QUERY_KSTACK_SYSCALL`.

Two things worth carrying:

- **A list can be legitimately EMPTY, and that is not the same as a
  table of zeroes.** `QUERY_KSTACK_SYSCALL` has no records at all while
  `kernel.kstack_track` is off, which is why `/bin/kstack syscalls`
  says "tracking is off" rather than printing a heading with nothing
  under it.
- **Check the OLD command for modes before deleting it.** `/bin/kstack`
  shipped without the builtin's `slots` view, and
  `compositor_death_test.py` caught it -- that view is how the test
  proves a declining client is still alive. A dropped mode looks
  exactly like a broken feature from the outside.

### After the staging -- a class that is a LOG, not a snapshot -- 2026-08-24

`QUERY_KBDTAP` (`/bin/kbd`) is the first class whose records describe
things that HAPPENED rather than the state of something that IS, and it
answers open question 1 below for its own case without settling it in
general.

**It is also the first class that is OFF by default.** `kernel.kbdtap`
gates the ring, and a query provider whose `count()` legitimately
returns 0 is already a shape the registry supports (`QUERY_KSTACK_SYSCALL`
while `kernel.kstack_track` is off). What is new is the reason: not "the
feature is not collecting" but "collecting is a privacy decision the
machine's owner has to make" -- `SYS_QUERY` performs no privilege check
of any kind, so anything a provider exposes is exposed to every ring-3
process. **That is worth carrying forward as a rule for the registry:
before adding a class, ask what it discloses, because there is no
mechanism here that will ask for you.**

**A fixed-size record works because a key event is fixed-size.** The
question was posed about `dmesg`, where a record is a line of arbitrary
length; a keypress has four numbers and at most two produced codes, so
it fits in a struct and needs none of the variable-length machinery.

**What a log needs that a snapshot does not is a SEQUENCE NUMBER.** The
registry's contract is `count()` plus `fill(index)`, and the ring behind
this one is fed from an interrupt handler -- so a reader walking
`0..count-1` may legitimately see a record twice or miss one under a
burst. `seq` is monotonic and never reused, which makes a repeat
recognisable and a gap countable; `kbd` prints "12 events lost" rather
than skipping silently. **Draining on read was the alternative and is
wrong here**: it makes the log readable exactly once and breaks the
second reader, which is not a property the registry has anywhere else.

**And it is a genuine LIST, so it declares no named fields.** Not for
the usual reason (an index in a name meaning a different record a second
later) but for a stronger one: `kbdtap.keycode` would be a different
keypress on every read by design.

The trap it added to the pile is in the other direction -- see
`docs/tools.md` on `kbd_test.py`, and note here that **reading class 0
to get a count is expensive**: filling a `query_provider_info` calls
that provider's `count()`, and `QUERY_HEAPCHECK`'s scans the heap. Cheap
metadata is only cheap when it is stored rather than computed.

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
