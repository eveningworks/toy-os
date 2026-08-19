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
- **A fact is not a setting with the write refused.** It has no stored
  form at all, so "reset it to the default" is meaningless and
  `config diff` has nothing to compare. That is why the two live in
  separate registries rather than one with a read-only flag.

> The query registry is DESIGNED, not built — see
> [query-design.md](query-design.md). Everything about settings below is
> live today.

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

Once the query registry lands, `config get` falls back to it for facts —
`config get system.mem_free` will answer, **labelled as read-only kernel
state** so it cannot be mistaken for an `/etc` key. `config set` on a
fact fails because no setting has that name.

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

Facts will be `SYS_QUERY(class, index, buf, len)` — one syscall, an
information class, a typed struct back. Nothing about `config` is
privileged; it is one client of that call among several.

## Adding a setting

Register a `struct setting` (`api/setting.h`) from `settings_init()`:
a name, a label, a type, its file, a choice enumerator, a getter, and
one `apply` that validates, applies AND persists.

**Do not add a setting as a bare `etc_config_get`/`_set` pair.** That is
the shape the registry replaced, and it leaves nothing able to answer
"what settings exist" — which is what Control Panel and `config list`
both depend on.

Put the key in `/etc/toyos.conf` by default. A feature with enough keys
to be unwieldy there gets its own `/etc/<name>.conf` and an
`/etc/config.d` descriptor, which also gives it its own namespace.

## Related

- [query-design.md](query-design.md) — the query registry, why it is not `/proc`, and its staging
- `kernel/include/api/setting.h` — the setting registry
- `kernel/include/api/config_file.h` — how a config file declares itself
- `kernel/include/abi/setting_abi.h` — the `SYS_SETTING` message
