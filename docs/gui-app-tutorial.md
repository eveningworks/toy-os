# Writing a GUI app in C

A walk through a small but complete toy-os desktop app: a window with a
label, a bar drawn by hand, and two buttons in a row. By the end it
builds, appears in the Start menu, answers the mouse and the keyboard,
and tells a test what it did.

**The example is real code: `userland/gui/demos/counter.c`.** It is in the
tree so it has to build, and it is what the snippets below are cut from.
Read it beside this page; if the two ever disagree, the file is right and
this page is the one to fix.

What it looks like after three clicks:

    ┌ Counter ──────────── – □ × ┐
    │ Clicked 3 times            │
    │ [██████░░░░░░░░░░░░░░░░░]  │
    │ [ +1 ] [ Reset ]           │
    └────────────────────────────┘

## What an app is here

A GUI app is an ordinary **ring-3 process**, dynamically linked against
`libuapp.so` (the toolkit) and `libc.so`. It asks the compositor, `toywm`,
for a window, and draws into a buffer the compositor shows. You never
talk to the window server directly: **`uapp_run()`** owns the event loop,
and your code is a set of callbacks in a `struct uapp_desc`
(`userland/ui/uapp.h`, whose comments are the API reference).

The pieces:

| Piece | Header | What it is |
|---|---|---|
| the app | `ui/uapp.h` | the window, the loop, the callbacks |
| widgets | `ui/uui.h` (+ `ui/uui_label.h`) | buttons, text boxes, lists, dropdowns... each with an **ops table** |
| layout | `ui/uui_layout.h` | rows, columns and grids that size and place widgets |
| drawing | `ui/ugfx.h` | rectangles, text, images, into the window's surface |
| theme | `ui/utheme.h` | the palette (`UTHEME_TEXT`, `UTHEME_ACCENT`, ...) and font-derived metrics |
| logging | `ui/ulog.h` | lines into the kernel log, which is how a test reads your app |

## 1. Where the file goes -- no Makefile edit

Put `counter.c` in one of `userland/gui/apps/` (applications),
`userland/gui/system/` (system tools) or `userland/gui/demos/`. Every `.c`
there becomes its own program, and **the directory is the install path**:
`userland/gui/demos/counter.c` becomes `/bin/wm/demos/counter`. `make all`
finds it by itself.

(A program made of several `.c` files, or one that needs another library,
is the exception: see `EXTRA_OBJS_*` and `ULIB_SO_*` in the Makefile.)

## 2. The widgets, as data

Widgets are plain structs you own. A **`struct uui_item`** pairs one with
its **ops table** -- the functions that measure, place, draw and hit-test
it -- plus an `id` for your callbacks and a `name` for tests:

```c
static char g_text[32] = "Clicked 0 times";
static struct uui_label g_label;
static struct uui_custom g_bar;          // a widget you draw yourself
static struct uui_button g_add, g_reset;

enum { ID_ADD = 1, ID_RESET };
```

**Layouts nest by being items themselves.** A row of the two buttons,
inside a column of label, bar and row:

```c
static struct uui_item g_row_items[] = {
    { .ops = &uui_button_ops, .widget = &g_add,   .id = ID_ADD,   .name = "add" },
    { .ops = &uui_button_ops, .widget = &g_reset, .id = ID_RESET, .name = "reset" },
};
static struct uui_layout g_row = {
    .dir = UUI_ROW, .items = g_row_items,
    .count = sizeof g_row_items / sizeof g_row_items[0],
};
static struct uui_item g_items[] = {
    { .ops = &uui_label_ops,  .widget = &g_label, .name = "label" },
    { .ops = &uui_custom_ops, .widget = &g_bar,   .name = "bar", .flags = UUI_FILL_W | UUI_FILL_H },
    { .ops = &uui_layout_ops, .widget = &g_row,   .name = "row" },
};
static struct uui_layout g_root = {
    .dir = UUI_COLUMN, .items = g_items,
    .count = sizeof g_items / sizeof g_items[0],
};
```

- **Derive counts with `sizeof`**, never write `.count = 3`: a literal
  that drifts from the array sends the toolkit past its end.
- `UUI_FILL_W` / `UUI_FILL_H` let the bar take the room left over when
  the window grows. A layout never SHRINKS a child below its natural
  size -- it overflows instead -- so a page that can grow goes in a
  `uui_scrollview` (`userland/CLAUDE.md`).
- **`ui/uui.h` is the umbrella header but does not include
  `ui/uui_label.h`** -- include it yourself.

## 3. Drawing your own widget

A `uui_custom` is a rectangle the layout places and your function paints.
Draw **inside `c->x, c->y, c->w, c->h`** and nowhere else:

```c
static void draw_bar(struct ugfx_surface *s, const struct uui_custom *c) {
    ugfx_fill_rect(s, c->x, c->y, c->w, c->h, UTHEME_OUTLINE);
    ugfx_fill_rect(s, c->x + 1, c->y + 1, c->w - 2, c->h - 2, UTHEME_PANEL_BG);
    int n = g_count > GOAL ? GOAL : g_count;
    int fill = (c->w - 4) * n / GOAL;
    if (fill > 0) ugfx_fill_rect(s, c->x + 2, c->y + 2, fill, c->h - 4, UTHEME_ACCENT);
}
```

**Colours come from the theme**, not from `ugfx_rgb()` constants, so the
app follows the desktop's palette. For free-form drawing of a whole
window there is also `on_draw` (paints under the widgets) and
`on_draw_over` (on top), and `uui_canvas` for lines, circles and polygons
in local coordinates -- `userland/gui/demos/gfxdemo.c` uses both.

## 4. Behaviour: commit on RELEASE

With `.widgets` set, the library hit-tests, routes and grabs the mouse
for you. **A button's click is a command**, and arrives as `on_action`
with the code you gave the button:

```c
static void on_action(struct uapp *a, int code) {
    if (code == ID_ADD) set_count(a, g_count + 1);
    else if (code == ID_RESET) set_count(a, 0);
}
```

**A button acts when it is released over itself**, never on the press --
a press can still be cancelled by dragging off, which is the rule in
`docs/gui-guidelines.md` and what every button here does. The library
enforces it: `on_action` fires only for a completed click (or Space/Enter
on a focused button), and a press or a hover never reaches the app.

**Every other widget reports through `on_widget(a, id, reason)`** -- a
checkbox toggled, a list's selection moved, a slider dragged. The value
is read from the widget struct itself inside that callback. The two are
kept apart on purpose: when commands and widget changes shared one
callback, a table whose id equalled a command's code ran that command on
every hover. Ids must also be unique -- an app declaring one twice is
refused at startup.

Changing state means changing the widget and asking for a repaint:

```c
static void set_count(struct uapp *a, int n) {
    g_count = n;
    snprintf(g_text, sizeof g_text, "Clicked %d time%s", n, n == 1 ? "" : "s");
    uui_label_set_text(&g_label, g_text);
    ulogf("counter: count %d\n", n);     // what a test reads
    uapp_redraw(a);
}
```

Keys the app owns go to `on_key`. **Esc closes nothing**: closing is the
title bar's × or Alt+F4, handled by the window manager (`on_close` can
refuse it, e.g. for unsaved work).

```c
static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    if (key == '+' || key == ' ') set_count(a, g_count + 1);
    else if (key == 'r' || key == 'R') set_count(a, 0);
}
```

(A widget that takes keys itself -- a text box -- needs a **focus ring**,
`.focus` in the descriptor; `userland/gui/system/about.c` shows one. The
build checks that such a widget is reachable: `check_key_routing.py`.)

## 5. `main()`: the font first, then describe the app

```c
int main(void) {
    if (!ugfx_font_init()) return 2;   // metrics come from the font

    uui_label_init(&g_label, g_text);
    uui_button_init(&g_add, 0, 0, 0, 0, "+1", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_ADD);
    uui_button_init(&g_reset, 0, 0, 0, 0, "Reset", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_RESET);
    g_add.outlined = g_reset.outlined = 1;

    g_bar.w = ugfx_text_width("Clicked 10 times") * 2;   // font-derived
    g_bar.h = ugfx_char_h() * 2;
    g_bar.draw = draw_bar;

    struct uapp_desc desc = {
        .title = "Counter",
        .app_id = "counter",
        .layout = &g_root,          // sizes and draws the window
        .widgets = g_items,         // routes the mouse, nested rows included
        .widget_count = sizeof g_items / sizeof g_items[0],
        .on_action = on_action,
        .on_key = on_key,
        .flags = UAPP_RESIZABLE,
    };
    return uapp_run(&desc);
}
```

- **`ugfx_font_init()` before any measurement.** Without it every
  text width and line height is 0 and the window collapses silently.
- **Geometry `0, 0, 0, 0`**: the layout assigns it. The window's size is
  the layout's natural size -- nothing here is a pixel constant, so the
  app reflows with the session font. An app with no layout sizes itself
  with `on_size`, from `ugfx_char_h()` and `ugfx_text_width()`.
- `app_id` is the app's identity to the desktop (taskbar grouping, single
  instance with `UAPP_SINGLE_INSTANCE`).
- Everything else is optional: `on_open`, `on_tick` (a timer, every
  `tick_ms`, 33 ms if you name none), `on_resize`, `on_close`, raw `on_press`/`on_release`/`on_motion`
  for canvas-style apps. `uapp.h` documents each.

## 6. Into the Start menu

One file in `data/wm/applications/` (the format is that directory's
`README.md`):

```
# Counter -- the worked example of docs/gui-app-tutorial.md.
Name=Counter
Exec=/bin/wm/demos/counter
Category=development
Comment=The GUI tutorial's example app
Icon=#
ShowIn=startmenu
```

- `Category` picks the Start menu folder: `utility`, `graphics`,
  `multimedia`, `games`, `system` or `development`.
- `Icon=` is a name from `data/icons/` (generated by `tools/gen_icons.py`
  -- add a `def icon_<name>()` and an entry in its `ICONS` table), or a
  single character, which draws a letter tile.
- `ShowIn=startmenu` leaves it off the desktop; the default is both.
- **Never write into `seed/sync/`** -- it is build staging. The source is
  `data/`, and `git status` should show the new file.

## 7. Build it and run it

```
make iso          # builds everything and puts it on disk.img
make run          # boots it in a window
```

Then **Start → Development → Counter**, or type `/bin/wm/demos/counter`
in the Terminal. From a test or the serial debug console, `gui open
Counter` (the `Name=`, exactly). What `ulogf()` printed is in `dmesg`:

```
[4.85] counter: count 1
[5.21] counter: count 2
```

## 8. Rules that bite

`docs/gui-guidelines.md` is binding for anything drawn; the ones that
catch a first app:

- **Text in a fixed box is drawn with `ugfx_draw_string_clipped()`** --
  plain `ugfx_draw_string()` does not clip, and a long label runs over
  whatever is next to it.
- **Sizes are font-derived** (`ugfx_char_h()`, `ugfx_text_width()`,
  `utheme_pad()`, `utheme_control_h()`), never pixel constants.
- **Act on release**, and never on a hover.
- **An app cannot draw outside its window**, and a clip rect with a zero
  or negative size draws NOTHING.
- **A widget you write yourself** fills its ops table against
  `ui/uui_widget.h`: `draw` needs `natural_size` and `set_geometry`,
  `press` needs `release`, `hit` returns a BOOLEAN. `make verify` runs
  `check_widget_ops.py` and `check_key_routing.py`, which fail on these.
- **Before inventing a widget, check `userland/ui/`** -- and ask whether
  what you are building should be a reusable one.

## 9. Testing it

Log what a test needs to know -- the app's state, and where its widgets
are -- and have the test ask rather than guess:

```python
con = DebugConsole(sock_path=".vm.0.serial")    # tools/gui_debug.py
con.open_app("Counter")
x, y = con.widget_center("add")                 # the item's .name
con.click(x, y)                                 # exact, from the WM's own geometry
```

Then assert on the `counter: count` line, and read PIXELS for anything
visual (`tools/pixel_probe.py`) -- "it responded" is not "it is drawn".
A new test tool joins `tools/gui_regress.py`'s list, and
`docs/testing.md` has the rest, including the ways a GUI test passes
without testing anything.

## Where to read next

- `userland/gui/system/about.c` -- free drawing with `on_draw`, footer
  buttons placed by hand, a focus ring.
- `userland/gui/apps/calculator.c` -- a grid layout and a button group.
- `userland/gui/demos/uidemo.c` -- every stock widget in one window.
- `userland/ui/uapp.h` -- every callback and flag, with the reason for each.
- `docs/gui-guidelines.md` and `docs/conventions/gui.md` -- how things
  must look and behave, and why.
