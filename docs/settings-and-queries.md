# Settings and queries

How to read and change what the system knows about itself. Two
registries, one addressing scheme.

## The vocabulary

**These three words are the project's terms for these three things. Use
them; do not invent a fourth.** A codebase with "facts", "properties",
"metrics" and "readings" for one concept is one where nobody can grep
for the thing they mean.

| Term | What it is | Registry | Writable | Survives a reboot |
|---|---|---|---|---|
| **Fact** | Read-only, computed fresh on every read — `mem_free`, the process list, a PCI device | query | no | it does not EXIST between boots |
| **Setting** | Read/write, persisted to `/etc` — `font_size`, `timezone`, `PATH` | setting | yes | yes |
| **Tunable** | A setting whose `apply` also writes a live kernel variable — a scheduler slice, a cache depth | setting | yes | optionally |

Two distinctions the table is making precise, both of which were live
questions while this was being designed and both of which have an
obvious-looking wrong answer:

- **A tunable is a KIND OF SETTING, not a third registry.** It differs
  only in what its `apply` touches. The word exists because "setting"
  alone does not tell you whether changing it does anything before the
  next boot.

  **"Survives a reboot: optionally" is expressed by the FILE, not by a
  flag.** A tunable that should not persist declares
  `CONFIG_PATH_RUNTIME` as its `file`; one that should names a real
  `/etc` file and needs nothing else. That is the sysctl/sysctl.conf
  split — a runtime knob by default, persistence opted into — and it is
  why `setting_persists()` is a predicate over the file rather than a
  bit somebody has to remember to set.

  A tunable still needs a NAMESPACE, because identity is
  `(namespace, name)` and a bare name is refused when ambiguous. The
  sentinel path resolves to the registered name `kernel`, so these read
  as `kernel.heap_debug` — the same word sysctl uses for the same kind
  of thing (`kernel.printk`).

  **A tunable MUST have an `apply`.** The persisted-only flavour
  (`apply == NULL`) works because the registry writes the file itself;
  with no file and no apply, a value would be neither held nor stored,
  and `set` would report success having changed nothing.
  `setting_register()` refuses it.
- **A fact is not a setting with the write refused.** It has no stored
  form at all, so "reset it to the default" is meaningless and
  `config diff` has nothing to compare. That is why the two live in
  separate registries rather than one with a read-only flag.

> Both registries are LIVE. The query registry's first stage shipped
> 2026-08-19; **tunables shipped 2026-08-20** — `kernel.heap_debug`,
> `kernel.ata_nodma` and `kernel.kstack_track`, the first three, each
> the write half of a shell command. See
> [query-design.md](query-design.md) for what is left.

**A tunable can also be a DIAGNOSTIC ACTION: `kernel.hda_tone`** reads
as `off` and, set `on`, plays three seconds of a kernel-generated tone
through the HD Audio controller with no app in the loop (see
`docs/conventions/kernel.md`'s HDA entry). Write-only and never
persisted; it is the write half of a probe, the same shape as the
others.

**A tunable can also be a CONSENT switch, and `kernel.kbdtap` is the
first.** The others trade performance against diagnostics — turning one
on costs cycles and tells you more. That one gates whether the kernel
keeps a log of recent keystrokes at all (`kbd`), and its default is off
because of what the machine would otherwise be holding, not because of
what it would cost: there is no privilege model here, so `SYS_QUERY`
exposes anything a provider knows to every ring-3 process. Two rules
fall out of it and are worth applying to the next one of its kind.
**Turning such a setting off must ERASE what it collected**, or "off" is
decorative. And **a tool must not consult the switch instead of looking
at the data** — one that returns early on `off` cannot tell you when
`off` is a lie.

## Names

Every name is `namespace.name`. The namespace is the registered name of
the file the setting lives in, so `font_size` in `/etc/toyos.conf` is
`system.font_size`. It is derived, never declared.

A **bare name works when exactly one setting has it** and is REFUSED
when several do — never resolved by registration order, which would make
the answer depend on boot sequence. A read reports the ambiguity; a
write refuses it.

## At the shell

```
config list                  # every setting, its value and its file
config get system.font_size  # one value
config set system.font_size 16
config unset system.timezone # back to the default
config diff                  # live vs what the files say
config reload                # re-read /etc after a hand edit
config files                 # which config files are registered
```

`config set` reports three outcomes, not two: applied and saved,
applied but NOT saved, or invalid. **"Applied" is not "saved"** — a
setting that could not be written is gone at the next boot, and saying
so is the whole reason the third answer exists.

The files stay hand-editable. `config` is an INDEX over them, which is
the half `/etc` cannot provide about itself.

`config get` falls back to the query registry for facts, and **labels
the answer** so it cannot be mistaken for an `/etc` key:

```
config get mem.frame_free
514887
  (kernel fact, read-only -- a count, not stored in any file)

config get providers.anything
config: 'providers.anything' is a list of records, not a single value
  read it with a command built for it (e.g. `meminfo --list`)
```

A setting wins over a fact of the same name, so `get` and `set` can
never be talking about different objects. `config set` on a fact fails
because no setting has that name — no special case needed.

## From an app

Settings go through `SYS_SETTING` (`sys_setting()`, `struct setting_msg`):

```c
struct setting_msg m = { .op = SETTING_OP_SET };
strlcpy(m.name,  "system.font_size", sizeof m.name);
strlcpy(m.value, "16",               sizeof m.value);
sys_setting(&m);
// m.result: SETTING_INVALID / SETTING_SAVED / SETTING_UNSAVED
```

Read the `result`, not just the return value — the call SUCCEEDED in
asking; whether it applied and persisted is separate.

Enumeration is `SETTING_OP_COUNT` → `SETTING_OP_INFO` → `SETTING_OP_CHOICE`,
and `SETTING_OP_GET` reads one. That is all Control Panel does: it is
GENERATED from the registry, which is why **a setting registered
anywhere in the kernel gets a Control Panel row and a `config` entry
with no edit to either.**

Facts go through `SYS_QUERY` (`struct query_msg`), wrapped by libsys:

```c
struct query_meminfo m;
sys_query_record(QUERY_MEMINFO, 0, &m, sizeof m);   // a whole record

unsigned long long v; unsigned type;
sys_query_field_get("mem.frame_free", &v, &type);   // one named field
```

**Class 0 is the registry describing itself**, so a program that knows
only the number 0 can walk `struct query_provider_info` records and find
every other class by name — which is how a generic tool works, while a
purpose-built one (`/bin/meminfo`) asks for its class directly.

Two rules worth knowing. **`len` is version tolerance**: the kernel
writes `min(len, record)` and reports how much in `returned`, so a
struct that GAINS a field does not break an older binary — growth is
append-only and existing fields never move. And **`count` is a hint, not
a bound**: a list's length is itself a fact and can change between
calls, so an enumerator ends on the record that is not there (`-ERANGE`)
rather than on a count it read earlier.

Nothing about `config` is privileged; it is one client of that call
among several.

## Adding a setting

Register a `struct setting` (`api/setting.h`) from `settings_init()`:
a name, a label, a type, its file, a **category**, a choice enumerator,
a getter, and one `apply` that validates, applies AND persists.

The `category` is a free string — `"Appearance"`, `"Input"`,
`"Startup"` — and it is what files the setting under a heading in System
Settings' sidebar. Omit it and the setting lands under `"General"`; a
ring-3 program declaring its own config file may name a section the
kernel has never heard of. It is declared here rather than mapped by
the app for the same reason the app holds no list of settings: a table
in the UI drifts the moment a subsystem adds a key.

### Which type

`SETTING_TYPE_ENUM` needs a `choice` enumerator and gets radio buttons,
a dropdown or a slider depending on how many options there are (and on
what `/etc/settings.d` says). `SETTING_TYPE_STRING` is free text with no
enumerator. `SETTING_TYPE_INT` is a bounded number: declare `min`, `max`
and `step`, optionally a `unit` (`"%"`, `"px"`, `"ms"`), and System
Settings gives it a spinbox.

**The registry enforces an INT's range**, so `config set`, a hand-edited
`/etc` file and a widget are all checked the same way — a value outside
it is REFUSED, not clamped. `/etc/settings.d` can override which widget
a setting uses, but not its bounds: presentation is the file's business
and a constraint is not.

**Pick ENUM over INT when the names are doing real work.**
`mouse_accel`'s values are thresholds where lower means more
acceleration, so `high` is a better name than `3` — the numbers run
backwards from the effect. `mouse_speed` is a percentage and reads
better as one, which is why it stopped being four named levels.

**Do not add a setting as a bare `etc_config_get`/`_set` pair.** That is
the shape the registry replaced, and it leaves nothing able to answer
"what settings exist" — which is what Control Panel and `config list`
both depend on.

Put the key in `/etc/toyos.conf` by default. A feature with enough keys
to be unwieldy there gets its own `/etc/<name>.conf` and an
`/etc/config.d` descriptor, which also gives it its own namespace.

## The text and hints in `/etc/settings.d`

The kernel says what a setting IS; `/etc/settings.d` says how it reads
and looks. One file per setting, named by its qualified name, in
`etc_config`'s ordinary `name=value` format — no sections, no new parser.

```
# /etc/settings.d/system.mouse_accel
Description=Move faster, and the pointer travels further still
Widget=slider
Applies=now
Order=40
Choice.off=Off
Choice.low=Low
Choice.high=High
```

| Key | Meaning |
|---|---|
| `Description` | One line, shown under the label. Not a paragraph — `uui_label` does not wrap yet, so a long one is clipped. |
| `Widget` | `auto` (default), `radio`, `dropdown`, `slider`. A **hint**: a client without that control still shows the setting some other way. |
| `Applies` | `now` (default) or `reboot`. `reboot` is the thing `SETTING_OP_SET`'s result cannot say — `system.default_target` persists perfectly and visibly does nothing until you restart. |
| `Advanced` | `1` keeps it off the page behind a "Show advanced settings" toggle. |
| `Order` | Lower first within a page; ties keep registration order. |
| `Choice.<value>` | Display name for one choice. The stored value is still `<value>`. |

A **page** gets its own file, `group.<category>.<group>`, carrying
`Label` and `Description` — the text belongs to the group, not to any
setting in it.

**The compiled-in label is the floor.** A missing or malformed file
costs that one setting its extra text and nothing else: no description,
and choices shown as raw values. So these are added one at a time, and
an empty `/etc/settings.d` still leaves a completely usable UI.

## Where a setting appears

`category` and `group` on `struct setting` steer the sidebar:
`category` is a heading, `group` is a page. Several settings sharing a
group land on one page — cursor theme, cursor size, pointer speed and
pointer acceleration all declare `group = "Mouse"`, from two different
kernel files.

A setting with no `group` gets a page of its own, named by its label,
which is what every setting did before groups existed.

**Grouping never touches the config file.** Both are registry metadata;
each setting still writes its own `name=value` line to its own file, so
moving a setting between groups migrates no data.

## Adding a fact

Three things, in the subsystem that owns the numbers — there is no
central table to edit and no init call to forget:

1. A record struct and a class number in `abi/query_abi.h`.
2. A `struct query_provider` (`api/query.h`) with `count`/`fill`, plus a
   `query_field` table if the fields should be addressable by name.
   Use `QUERY_FIELD()` so the offset is derived from the member rather
   than written out.
3. A `query_register()` call at boot.

`kernel/mm/mem_query.c` is the worked example, at about 60 lines.

Two rules. A provider is stored **by pointer**, so it must have static
storage duration — a stack local leaves the registry holding a dangling
pointer that reads as plausible garbage. And a **LIST class gets no
field table**: an index baked into a name means a different record a
second later, which is sysctl's worst corner.

## The UI

**System Settings** (`userland/gui/system/settings.c`, `/bin/wm/system/settings`)
— a navigation tree on the left, one page on the right, a status line
underneath. KDE System Settings' shape.

It contains **no list of settings and no list of categories**. Both come
from the registry, so a setting registered anywhere in the kernel
appears under a heading, with its legal values, with no edit to the app.
Selecting a heading opens its first setting rather than an empty pane.

Renamed from *Control Panel* on 2026-08-19: that is Windows' name, and
this shows exactly the setting registry.

## Related

- [query-design.md](query-design.md) — the query registry, why it is not `/proc`, and its staging
- `kernel/include/api/setting.h` — the setting registry
- `kernel/include/api/config_file.h` — how a config file declares itself
- `kernel/include/abi/setting_abi.h` — the `SYS_SETTING` message
