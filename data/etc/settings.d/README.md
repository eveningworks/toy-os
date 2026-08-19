# `/etc/settings.d` — the human-facing text for a setting

One file per setting, named by its **qualified name** —
`<namespace>.<name>`, e.g. `system.mouse_speed` — because a setting's
identity is that pair and two programs may each own a `theme`.

The format is `etc_config`'s, unchanged: `name=value` lines, `#`
comments, whitespace trimmed. No sections and no new parser — one file
per setting is what removes the need for them, and it matches
`/etc/services.d`, `/etc/config.d` and `/usr/wm/desktop`, which are all
directories of small descriptors.

```
# /etc/settings.d/system.mouse_speed
Description=How far the pointer moves for a given hand movement
Widget=radio
Choice.slow=Slow
Choice.normal=Normal
Choice.veryfast=Very fast
```

| Key | Meaning |
|---|---|
| `Description` | One line, shown under the setting's label. Not a paragraph. |
| `Widget` | `auto` (default), `radio`, `dropdown`, `slider`. A **hint** — a client with no such control still shows the setting some other way. `slider` suits an ORDERED enum (off/low/medium/high), where a radio list says nothing about the order. |
| `Choice.<value>` | The display name for one choice. The stored value is still `<value>`; only what is shown differs. |

**The compiled-in label is the floor.** A missing or malformed file costs
that one setting its extra text and nothing else: no description, and
choices shown as their raw values. So these can be added one at a time,
and a machine with an empty `/etc/settings.d` still has a completely
usable settings UI — the same containment a cursor theme has when its
files fail to load.

**Why prose lives here rather than in `struct setting`.** `label` is
compiled in because a setting without one cannot be presented at all.
Prose is the part somebody rewords, and eventually translates, and
neither should need a kernel rebuild. It is also the part that can be
absent without breaking anything, which is what makes a file safe for it.
