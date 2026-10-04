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

`kernel.intel_cycle` is the same shape with four words: `pipe`, `link`
and `native` each run one mechanism of the Intel display's modeset (see
`docs/conventions/kernel.md`'s EDID entry) and log every readback --
on gen8 the eDP panel's, on gen9 the HDMI port's (`link` there cycles
the DDI buffer, `native` the PLL too);
`off` does nothing, and the value always reads back as `off`.

`kernel.crash` is the destructive one: `config set kernel.crash gp-fault`
panics the machine the named way -- Linux's sysrq `c` -- with the kinds
the Crash Test app lists (`kernel/core/crashtest.c`), and is refused,
with the reason, unless `faultinject` is on the GRUB line.
`kernel.panic_record=clear` acknowledges the previous boot's panic record
(`QUERY_PANIC`, `kernel/panic_store.h`); logd sets it once it has
appended the record to the dead boot's log, which is what deleting a file
under Linux's `/sys/fs/pstore` does.

`kernel.usb_reset` takes a PORT NUMBER and forces that root port
through a real reset and re-enumeration, without anyone touching the
cable. It exists to settle one question: the intermittent USB
enumeration failure in `docs/bugs.md` is cured by REPLUGGING the
device, and a replug does two things at once -- it gives the port a
genuine connect and it power-cycles the device. This does only the
first, so whichever half matters becomes a measurement rather than an
argument. The result is in the kernel log, not in the setting's answer,
because "the port was refused" and "the device would not come back" are
both "we tried" and only one of them is an invalid value.

`kernel.usb_replug` is the OTHER half of that replug, added
2026-09-17 when the question above got its answer: the reset half has
never rescued a wedged device, and the power half is what a replug does
that nothing in software could. It takes a port number and takes that
port's whole SOCKET away from the device for 100 ms -- both halves,
since they are one connector -- then gives it back and lets the
ordinary scan notice the connect. Which lever it uses depends on the
machine: port power where HCCPARAMS1 says software controls it, and the
Intel port mux (un-route and re-route) where it does not, which is the
case on the ASUS. The driver now runs this by itself when a port's
enumeration gives up and the device is still sitting there; the knob
exists so the MECHANISM can be checked on a device that currently works,
which is a question any boot can answer, unlike the intermittent
failure. Refused, in the log, on a controller whose HCCPARAMS1 says port
power is not software-controllable -- the boot line says which kind a
machine is (`port power software-controlled (PPC)` or `always on`).

`kernel.usb_attach_delay` is the third of that family and the odd one
out, because it READS BACK: it is a state rather than an action. It is
the USB2 attach debounce (TATTDB) in milliseconds -- how long a freshly
connected port is left alone before it is reset -- and it defaults to
100. Resetting a port whose connection has not settled is the documented
way to lose the high-speed chirp, and a device that loses it comes up
FULL-speed, which is the signature of the enumeration failure in
`docs/bugs.md`. Settable to 0 so that can be provoked instead of waited
for at one boot in fifty. **It was the instrument that REFUTED that
theory** (ten software replugs at 0 ms, ten high-speed results), which
is what a diagnostic is for; it is kept because the same question comes
back whenever the retry timing changes.

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

Settings go through `usetting_dispatch()` (`userland/lib/usetting.h`),
which serves `struct setting_msg` against the MERGED registry — the
kernel's settings and the declared ones as one list:

```c
struct setting_msg m = { .op = SETTING_OP_SET };
strlcpy(m.name,  "system.font_size", sizeof m.name);
strlcpy(m.value, "16",               sizeof m.value);
usetting_dispatch(&m);
// m.result: SETTING_INVALID / SETTING_SAVED / SETTING_UNSAVED
```

**Not `sys_setting()` directly.** The syscall answers for the kernel's
half only, so a client calling it sees the machine's settings with the
desktop's missing — and two clients each merging for themselves is the
second source of truth the registry exists to remove.

Read the `result`, not just the return value — the call SUCCEEDED in
asking; whether it applied and persisted is separate.

For one setting by name, `userland/lib/usetting.h` builds that message
for you: `usetting_get(name, buf, cap)`, `usetting_set(name, value)`
(returning the `result`, or -1 when the call itself failed), the
`_int` variants, and `usetting_find(name, &msg)` for the INFO record.

Enumeration is `SETTING_OP_COUNT` → `SETTING_OP_INFO` → `SETTING_OP_CHOICE`,
and `SETTING_OP_GET` reads one. That is all System Settings does: it is
GENERATED from the registry, which is why **a setting registered
anywhere in the kernel — or declared by a file in `/etc/settings.d` —
gets a System Settings row and a `config` entry with no edit to
either.**

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

**First: which half owns it?** There are two, and one question separates
them — *does ring 0 do anything with the value?*

| | Declared by a file | Registered in the kernel |
|---|---|---|
| Where | `/etc/settings.d/<ns>.<name>`, with `Type=` | a `struct setting` from `settings_init()` |
| Who validates and persists | `lib/usetting_schema.h`, in ring 3 | the registry, in ring 0 |
| Who applies it | nobody — the owning process notices | the setting's own `apply` |
| Use it when | the value is read by a ring-3 program (the desktop, the toolkit, an app) | the kernel itself acts on the value |
| Examples | `desktop.wallpaper`, `desktop.taskbar_height`, `shortcuts.terminal` | `system.font_size`, `system.mouse_speed`, `storage.*`, `kernel.*` |

Most new settings are the first kind, and adding one is **writing a
file** — no C, no rebuild. `data/etc/settings.d/README.md` is the key
reference. A declaration names its type, the `/etc` file it persists to,
a label, a category and group, a default, and either its choices or its
bounds:

```
# data/etc/settings.d/desktop.icon_size
Type=enum
File=/etc/desktop.conf
Label=Size
Category=Desktop
Group=Icons
Default=medium
Choices=small,medium,large
```

An enum's options may also be computed: `ChoiceDir=` reads a directory
(`ChoiceDirMode=stem` for the wallpapers, `subdir` for the cursor
themes) and `ChoiceFile=` reads a file's lines, so dropping a
screensaver into `/bin/wm/savers` gives it a row with no edit anywhere.

**Do not add a persist-only `struct setting`** — one whose `apply` is 0.
That is what a declaration replaced, and twelve files of them were
deleted at once.

### Registering one in the kernel

For the second kind: register a `struct setting` (`api/setting.h`) from
`settings_init()` — a name, a label, a type, its file, a **category**, a
choice enumerator, a getter, and one `apply` that validates, applies AND
persists.

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
enumerator, and System Settings gives it a **text field** — it used to
draw an empty control that could only be changed with `config set`.
`SETTING_TYPE_INT` is a bounded number: declare `min`, `max`
and `step`, optionally a `unit` (`"%"`, `"px"`, `"ms"`), and System
Settings gives it a spinbox.

**A string setting validates in its own `apply`**, because the registry
range-checks an INT and enumerates an ENUM's choices but has nothing to
check free text against. `system.ntp_server` refuses an empty value and
anything carrying whitespace — the `/etc` parser reads `key=value` to
end of line, so an embedded space would be stored and read back as a
different string than was typed.

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

For a setting the kernel registered, `/etc/settings.d` says how it reads
and looks (for a DECLARED one the same file says both, since it is the
same file). One file per setting, named by its qualified name, in
`etc_config`'s ordinary `name=value` format. No section: the parser has
them (`docs/decisions/storage.md`), and a directory of small descriptors
is what removes the need for one here.

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
| `Widget` | `auto` (default), `radio`, `dropdown`, `slider`, `gallery`. A **hint**: a client without that control still shows the setting some other way. |
| `Preview` | With `gallery`: what each card's picture shows -- System Settings knows `cursor` (the theme's shapes). Read by the client, not carried in the ABI. |
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

**Debugging pages start hidden.** `Debug=1` in `category.<category>` or
`group.<category>.<group>` keeps those pages out of System Settings'
sidebar and search until **Kernel and debugging settings** is ticked on
System Information -- the whole Kernel category and System › Diagnostics
carry it. `data/etc/settings.d/README.md` has the details.

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

### The control a setting gets

From its TYPE, and from nothing the app decides: an ENUM gets radio
buttons or a dropdown (by count, or by what `/etc/settings.d` asks for),
an INT gets a spinbox with the registry's own bounds, a STRING gets a
text field, and a **KEYCOMBO gets a capture control** — click it, press
the keys, and it records what you pressed (`userland/ui/uui_keycapture.c`).

That last one is why KEYCOMBO is a type rather than a flag on STRING: a
key combination is storable as text but not *spellable* by most people
on the first try, and a text box makes a typo silent until the key does
not work. KDE and GNOME both put a capture control in front of a
shortcut. While it is listening the app holds
`uapp_inhibit_shortcuts()`, or the compositor would spend the very keys
being recorded — see `docs/decisions.md`.

## Related

- [query-design.md](query-design.md) — the query registry, why it is not `/proc`, and its staging
- `kernel/include/api/setting.h` — the setting registry
- `kernel/include/api/config_file.h` — how a config file declares itself
- `kernel/include/abi/setting_abi.h` — the `SYS_SETTING` message
