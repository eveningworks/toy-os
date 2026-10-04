# `/etc/settings.d` — what a setting IS, and what it says

One file per setting, named by its **qualified name** —
`<namespace>.<name>`, e.g. `system.mouse_speed` — because a setting's
identity is that pair and two programs may each own a `theme`.

A file here does one of two jobs, told apart by a single key:

- **It DESCRIBES a setting the kernel registered** (`api/setting.h`) —
  its description, its choices' display names, how a UI should present
  it. That is what every file here used to do.
- **It DECLARES a setting outright**, when it carries `Type=`. Nothing
  in the kernel knows about that setting; `lib/usetting_schema.h` reads
  this file, validates values against it and writes them to the `File=`
  it names. That is how every desktop setting works — the wallpaper, the
  taskbar height, the screensaver, the shortcuts.

One directory and two roles rather than two directories, because the
difference is one key and the second directory's only distinction would
be who reads it.

The format is `etc_config`'s: `name=value` lines, `#` comments,
whitespace trimmed. These files use **no section**, though the parser
has them now — one file per setting is what removes the need, and it
matches `/etc/services.d`, `/etc/config.d` and `/usr/wm/applications`, which
are all directories of small descriptors.

```
# /etc/settings.d/desktop.icon_size  -- a DECLARATION
Type=enum
File=/etc/desktop.conf
Label=Size
Category=Desktop
Group=Icons
Default=medium
Choices=small,medium,large
Description=How big the desktop icons are drawn
Choice.small=Small

# /etc/settings.d/system.mouse_speed -- only TEXT; the kernel owns it
Description=How far the pointer moves for a given hand movement
Widget=radio
Choice.slow=Slow
```

## The text keys — for either kind of file

| Key | Meaning |
|---|---|
| `Description` | One line, shown under the setting's label. Not a paragraph. |
| `Widget` | `auto` (default), `radio`, `dropdown`, `slider`. A **hint** — a client with no such control still shows the setting some other way. `slider` suits an ORDERED enum (off/low/medium/high), where a radio list says nothing about the order. System Settings honours `dropdown` and `slider`; `auto` and `radio` both let it pick from the values — a switch for an on/off pair, side-by-side buttons for a few short names, a list for a few long ones, a dropdown for many. |
| `Choice.<value>` | The display name for one choice. The stored value is still `<value>`; only what is shown differs. |
| `Applies` | `now` (default) or `reboot` — whether the change takes effect immediately. |
| `Advanced` | `1` keeps it behind a disclosure in a UI that has one. |
| `Order` | An integer, lower first, for where the setting sits on its page. Unordered settings follow. |
| `Sort` | `label` lists an enum's choices A to Z by display name -- for an UNORDERED set whose registry order means nothing (`system.keyboard_layout`, whose order is `/etc/kbs`'s directory order). An ordered enum (off/low/high) never carries it. |
| `Label` | **Group files only** (`group.<category>.<group>`), which carry a `Label` and a `Description` for the PAGE rather than for a setting. |

A file named `group.<category>.<group>` describes the page itself —
`group.Appearance.Windows` names and explains the Windows page, and
carries no `Choice.` lines. It declares nothing, having no `Type=`.
A page with no group of its own is keyed by its setting's LABEL, so its
file is `group.System.Startup target`, spaces and all.

A file named `category.<category>` carries that category's `Order=`.

## Debugging pages — `Debug=1`

`Debug=1` in `category.<category>` marks every page of that category as
a debugging one, and in `group.<category>.<group>` marks one page.
System Settings leaves those pages out of its sidebar, and out of its
search, until **Kernel and debugging settings** is ticked on System
Information -- Android's Developer options, rather than Windows' always-
listed For developers page. The tick is the app's own furniture, kept in
`/etc/settings.conf` as `show_debug`; opening a hidden page by name
(`settings kernel.heap_debug`) still works. `category.Kernel` and
`group.System.Diagnostics` carry it.

## Where a page sits — `Order=`

The sidebar is sorted by a DECLARED weight, lower first: `Order=` in
`category.<category>` places the category's whole run, and `Order=` in
`group.<category>.<group>` places a page within it. Anything without
one is 0 and keeps its first-seen position among its peers, so a
machine whose `/etc` says nothing looks as it always did.

It is data because it used to be an ACCIDENT: the sidebar was built in
the order settings happened to be registered, which was the kernel's
boot sequence — so moving a setting between files, or out of the kernel
entirely, silently rearranged a list people navigate by muscle memory.
KDE and GNOME both give a panel an explicit weight for the same reason.
Leave gaps (10, 20, 30) so a new page can be slotted between two
existing ones without renumbering.

## The declaration keys — `Type=` and what comes with it

| Key | Meaning |
|---|---|
| `Type` | `enum`, `int`, `string` or `keycombo`. **Its presence is what makes the file a declaration.** An unrecognised word reads as `string`, so a client that has never heard of a newer type still shows a text box. |
| `File` | The `/etc` file the value persists to. Its registered name (`config files`) is the setting's NAMESPACE, so this must agree with the file name here. A declaration with no `File` is refused: there would be nowhere to put the value. |
| `Label` | What a UI calls it. Defaults to the key. |
| `Category` / `Group` | Where it lands in System Settings: the sidebar section, then the page. `General` when no category is named. |
| `Default` | What it reads as when the key is absent from `File`. |
| `Choices` | `enum`: a comma-separated list of legal values. |
| `ChoiceDir` | `enum`: the options are the entries of this directory — so dropping a screensaver in gives it a row with no edit anywhere. |
| `ChoiceDirMode` | `name` (default), `stem` (the filename without its extension — the wallpapers) or `subdir` (directories only — the cursor themes). |
| `Min` / `Max` / `Step` / `Unit` | `int`: the inclusive range, the stepper increment, and what the number means (`px`, `min`, `%`). |
| `Requires` | `<qualified name>=<value>`: the setting only applies while that OTHER DECLARED setting has that value. Meanwhile it is UNAVAILABLE -- a UI greys it and shows `RequiresReason`, and a write is refused (an unset is not). Naming a kernel setting does nothing: `/etc` cannot disable what the kernel owns. |
| `RequiresReason` | The sentence shown in place of the description while `Requires` is unmet. |
| `Otherwise` | The value the setting READS as while `Requires` is unmet -- what GET answers and a UI shows. The file keeps the stored choice, so meeting the requirement again brings it back. `desktop.taskbar_align` is the example: centred while Start is. |

`Choices` and `ChoiceDir` **compose**, in that order: the
wallpaper declares `Choices=none` beside a `ChoiceDir`, which is how
"No wallpaper" sits ahead of a directory the declaration knows nothing
about.

**A value is validated against the declaration**, exactly as the kernel
registry validates its own: an `int` must parse and fall inside
`Min`..`Max`, an `enum` must be one of its choices. An enum whose choice
list comes back EMPTY accepts anything — an empty list means a directory
nothing has been put in yet, not a setting with no legal values.

## Two things worth knowing

**The label is the floor.** A missing or malformed TEXT file costs that
one setting its extra text and nothing else. A missing DECLARATION file
is different in kind: the setting stops existing. That is the trade for
settings being data — they can be added one at a time, and they can be
removed by deleting a file.

**These are shipped metadata, not configuration.** The tracked masters
are in `data/etc/settings.d`; `make iso` content-syncs them onto the
image, and `tools/remote.py flash` overwrites them on a real machine —
unlike the rest of `/etc`, which is new-files-only so a machine's own
values survive. Editing one here changes what a setting is everywhere;
editing `/etc/desktop.conf` changes what it is set to on one machine.
