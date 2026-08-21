# config

**a `/bin` program.**

**Category:** Configuration

## Synopsis

    config list

## Description

Every registered setting, its value, and **the file it lives in**. Flags any whose file no longer matches what is live.

Settings live as plain `name=value` text under `/etc` and can be edited
in `edit` — this is the index over those files, which is the part a
directory of text cannot provide about itself. See
`docs/decisions.md`'s settings-registry entry.

| Command | Notes |
|---|---|
| `config list` | Every registered setting, its value, and **the file it lives in**. Flags any whose file no longer matches what is live. |
| `config get <name>` | One value. Falls back to the FACT registry when no setting has that name, so `config get mem.frame_free` answers — **labelled as read-only kernel state**, since a setting survives a reboot and a fact does not exist between them. A list-shaped fact says so and names a tool that can show it. |
| `config set <name> <value>` | Validate, apply and persist. `name=value` works too. A refusal says what WOULD be accepted — the named choices for an enum, the range and step for a number (`try 25..300 % (in steps of 25)`); a value that applied but did **not** save says so rather than reporting success. |
| `config unset <name>` | Removes the key, so the built-in default applies at the next boot. |
| `config where <name>` | Just the owning file's path — scriptable. |
| `config diff` | Settings whose file differs from what is in effect, i.e. exactly what a hand edit changed and what `reload` would apply. |
| `config reload` | Re-read every file after editing by hand. Reports how many values were **refused**. |
| `config files` | Every known config file, its path, its description, and whether it is built in or declared in `/etc/config.d`. |
| `config show <name\|path>` | Print one config file verbatim. |
| `config find <text>` | Search key names **and** values across every registered config file, `file:key=value` per hit. Case-insensitive. |
| `config register <name> <path> [description]` | Declare a new config file by writing a descriptor into `/etc/config.d`. Picked up live. |
| `config unregister <name>` | Remove that descriptor. A built-in cannot be unregistered. |

**System Settings is GENERATED from the same registry** — it holds no
list of settings and no list of categories, it asks
(`SETTING_OP_COUNT`/`INFO`/`CHOICE`) and draws what comes back. A
setting registered anywhere in the kernel gains a sidebar home, a page
and a `config` entry with no edit to any of them. It deliberately shows
settings only, not facts: it is the "what can I change" screen, and
read-only counters would bury the settings. (It was *Control Panel*
until 2026-08-19 — that is Windows' name.)

**Settings are named `<namespace>.<name>`** — the namespace being the
registered name of the file the setting lives in, so `font_size` in
`/etc/toyos.conf` is `system.font_size`. A bare name still works when
only one setting has it; when several do, `config` lists them and
refuses rather than picking. `config set` and `config unset` refuse an
ambiguous name outright, because writing the wrong setting changes
something you did not mean to change. `config list` prints the qualified
form, which is the one that always works.