#!/usr/bin/env python3
"""tools/gui_regress.py -- run every GUI test tool, each on a fresh VM.

WHAT THIS IS
------------
The GUI test tools (`uidemo_test.py`, `gfxdemo_test.py`,
`notepad_client_test.py`, ...) each drive one app and assert on its log.
Running them is the standard check after touching `userland/ui/`,
`userland/`, or anything the window manager draws -- and doing that by
hand means retyping the same six commands, each with its own image copy
and VM lifecycle. This is that sequence, once, with a summary table.

    python3 tools/gui_regress.py                 # all of them, 4 at a time
    python3 tools/gui_regress.py -j1             # one at a time (the old behaviour)
    python3 tools/gui_regress.py -k uidemo -k gfxdemo    # a subset
    python3 tools/gui_regress.py --list
    echo $?                                      # 0 = every tool passed

WHY IT RUNS THEM IN PARALLEL
----------------------------
The tools are independent by construction (see below) and each spends
almost all of its wall clock waiting on an emulated machine, so running
them one after another wasted most of the host. Each concurrent tool
gets a VM SLOT -- `vm.py --instance N`, which derives that VM's
pidfile, serial socket, QMP port and VNC display from N, and which the
tool is pointed at with `--sock`/`--qmp-port`. Nothing is shared, so
the isolation the per-tool image and per-tool VM already provided is
unchanged; only the scheduling is.

Tools are STARTED longest-first (`COST_S`/`pick_order` below), because
a parallel run cannot finish before its slowest member does and
`forcequit` (71s of genuinely waiting out ping timeouts) previously sat
eleventh of fourteen and finished alone after everything else had
drained. The summary table is still printed in the declared dependency
order -- only the start order changed.

WHY EACH TOOL GETS ITS OWN IMAGE AND ITS OWN VM
-----------------------------------------------
This is the part that is easy to get wrong and expensive to debug.

  * **A fresh disk copy per tool.** Several of these write files
    (Notepad saves, the Terminal's shell creates things). A tool
    inheriting the previous one's filesystem hits "file already exists"
    paths its assertions never anticipated. The copy also keeps the
    real `disk.img` untouched, which matters if the user has their own
    QEMU open -- see CLAUDE.md on the write lock.
  * **A fresh VM per tool.** Each tool enters GUI mode itself and
    expects an empty desktop. Handing one a desktop with three windows
    already open fails in ways that look exactly like real widget bugs
    -- a click lands on the wrong window, and the log line that proves
    it is in a scrollback nobody reads.

`cp --reflink=auto` makes the copies nearly free on btrfs/XFS, and the
image is sparse besides (~3 MB actually allocated against a 9 GB
apparent size), so the per-tool copy costs far less than it looks.

WHAT THIS DOES NOT COVER
------------------------
`tools/damage_sweep.py` is deliberately NOT in the list: it takes a
different kind of run (`gui damage verify on` makes every frame render
twice, so the whole thing is several times slower) and it has its own
`--positive-control` protocol. Run it separately after touching
anything that draws, damages, focuses or changes window chrome.

Nor does this build anything. Run `make all && make iso` first -- these
tools drive whatever is already on the image.
"""

import argparse
import atexit
import concurrent.futures as cf
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import fresh_disk  # noqa: E402
from harness import copy_disk  # noqa: E402
from private_tmp import private_tmp  # noqa: E402

# How many tools run at once by default. Each one is a QEMU with 256 MB
# of guest RAM under TCG, so this is bounded by host cores far more than
# by memory. `-j1` restores the original serial behaviour exactly.
#
# DERIVED from the host rather than fixed at 4: the tool-seconds in a
# full run total roughly 460s, so the ceiling is the slowest single tool
# (notepad, ~41s -- forcequit dropped to ~35s in the 663d63b pass, so it
# is no longer the straggler) and everything between 4 and that is just
# how many cores are free. HALF the cores (`cpu_count() // 2`), because
# `cpu_count()` counts hardware THREADS and a TCG guest is a busy CPU
# thread plus its I/O -- so half the threads is roughly one guest per
# physical core, and going past that oversubscribes and turns wall-clock
# into settle flakes rather than speed (measurable: at five concurrent
# guests on an 8-core box both gfxdemo and notepad failed comparisons
# they pass alone).
#
# The CAP is 12, raised from 8 in the pass that converted menubar and
# taskmgr to observable waits (so the whole suite waits on something the
# app SAYS, not a sleep). The cap only bites on a big host: on an 8-core
# / 16-thread machine `//2` is already 8, so nothing changes there; a
# 24-thread box now gets 12. It is a ceiling against oversubscription,
# not a target -- do not raise it above `//2` for a given host.
DEFAULT_JOBS = min(12, max(2, (os.cpu_count() or 4) // 2))

# In rough dependency order: the widget toolkit first, so a toolkit
# regression is reported before the apps built on it start failing for
# what looks like their own reasons.
TOOLS = [
    ("uidemo", "uidemo_test.py", "userland/ui/ widgets, in a ring-3 client"),
    ("uiclient", "uiclient_test.py", "the ported widgets, ring 3"),
    ("winclient", "winclient_test.py", "the ring-3 window protocol"),
    ("gfxdemo", "gfxdemo_test.py", "geometry primitives + the canvas widget"),
    ("calculator", "calculator_client_test.py", "Calculator in ring 3"),
    ("notepad", "notepad_client_test.py", "Notepad in ring 3"),
    ("clipboard", "clipboard_test.py", "the system text clipboard, across two apps"),
    ("editmenu", "editmenu_test.py", "clipboard keys + the right-click edit menu in every text field"),
    ("uterm", "uterm_test.py", "Terminal + the ring-3 shell"),
    ("crt", "crt_test.py", "the Terminal's screen effect: toggle, Options, the curve's pointer"),
    ("crashview", "crashview_test.py", "the crash report viewer: a real crash, its backtrace against the host's"),
    ("uapp", "uapp_test.py", "the TWP resize handshake"),
    ("fullscreen", "fullscreen_test.py", "the fullscreen state and the display lease, on virtio-gpu"),
    ("stride", "resize_stride_test.py", "a resized window's buffers agree with its size"),
    ("resizeedges", "resize_edges_test.py", "all eight resize edges, and their cursors"),
    ("keyup", "keyup_test.py", "key RELEASES reaching a ring-3 client"),
    ("mousebtn", "mouse_buttons_test.py", "five buttons, and a SHORT press, reaching a client"),
    ("hover", "hover_test.py", "a hover change REPAINTS, not just damages"),
    ("scrollbar", "scrollbar_test.py", "scrollbar behaviour, per the guidelines"),
    ("pager", "pager_test.py", "the shared pager under /bin/less and /bin/doc"),
    ("menubar", "menubar_test.py", "menu bar, submenus and the status bar"),
    ("popup", "popup_test.py", "a menu leaves its window: popup surfaces"),
    ("startmenu", "start_menu_test.py", "the Start menu: folders, search, keyboard"),
    ("startset", "start_settings_test.py", "the Start menu's settings: list style, power, folders, hover"),
    ("filedialog", "filedialog_test.py", "the shared file chooser, as an owned window"),
    ("trashgui",   "trash_gui_test.py",  "the Recycle Bin: File Manager and desktop Delete, trash:/, Restore"),
    ("undogui",    "undo_gui_test.py",   "Undo/Redo: the toast's button, Ctrl+Z/Y, rename and new folder"),
    ("searchgui",  "search_gui_test.py", "search: typing filters, Enter walks subfolders, results open in place"),
    ("sizesgui",   "sizes_gui_test.py",  "folder sizes: each folder counted, subfolders included"),
    ("tabsgui",    "tabs_gui_test.py",   "title-bar tabs: new, label, switch, own history, close, last closes window"),
    ("pinsgui",    "pins_gui_test.py",   "Pin to Places: the file, the Pinned row, going there, Unpin, a deleted pin"),
    ("fmthumb",    "fm_thumb_drag_test.py", "icons view: a thumb drag after a click scrolls, carries no file"),
    ("zipgui",     "zip_gui_test.py",    "a .zip as a folder: list, open in place, Up, Extract all, open a member"),
    ("renamegui",  "rename_gui_test.py", "Rename many: F2 on a set, a chain through temp names, one Undo, Esc"),
    ("recentgui",  "recent_gui_test.py", "Recent: open records the app, recent:/ in day bands, Delete forgets, Open folder, Clear"),
    ("forcequit", "forcequit_test.py", "not-responding detection and force quit"),
    ("dialog", "dialog_test.py", "the confirm dialog, by pixel value"),
    ("leave", "leave_test.py", "the Leave page: Restart/Shut down/Exit to shell, apps asked to close, Restart into"),
    ("sched", "sched_gui_test.py", "the desktop stays live while a process runs"),
    ("blank", "blank_window_test.py", "no app opens a blank window"),
    ("wingeom", "window_geometry_test.py", "windows come back where you left them"),
    ("compositor", "compositor_test.py", "raw input to a ring-3 compositor"),
    ("screen", "screen_surface_test.py", "a ring-3 compositor's screen surface"),
    ("compdeath", "compositor_death_test.py", "the compositor death path (R7)"),
    ("cursor", "cursor_theme_test.py", "cursor themes: shapes as data files, size, fallback"),
    ("cursorshp", "cursor_shapes_test.py", "hand over a link, move, not-allowed on a refused drop"),
    ("saver", "screensaver_test.py", "the idle clock, and the savers it spawns"),
    ("shot", "screenshot_test.py", "screen capture: the command, the app, the region band"),
    ("shotopts", "screenshot_options_test.py", "Screenshot's options: naming, card/flash, Shift/Alt+PrtSc, popover, keys"),
    ("launch",   "launch_test.py", "Running programs/scripts: the card, Terminal -e, Always, x bit, menu, open"),
    ("doomdata", "doom_data_test.py", "DOOM's game-data card, a refused tampered download, the launcher and F1's key sheet"),
    ("crash", "crashtest_test.py", "fault paths: ring-3 crashes, and the gate on kernel panics"),
    ("hoversweep", "hover_sweep_test.py", "a hover over any widget of any app opens nothing"),
    ("entries", "desktop_entries_test.py", "ShowIn= and live .desktop reload"),
    ("taskmgr", "taskmgr_test.py", "the table widget, resize reflow, ending a process"),
    ("devmgr", "devmgr_test.py", "the device tree and its icons, disable/enable through the dialog"),
    ("netadapter", "netadapter_test.py", "a network card's adapter settings: netctl link, Settings > Adapters, saved and restored"),
    ("sndformat", "sndformat_test.py", "a sound card's Format panel, in Settings and Device Manager"),
    ("bootmgr", "bootmgr_test.py", "Boot Manager and Settings > Boot menu write grub.cfg; a broken file is refused"),
    ("logview", "logview_test.py", "Log Viewer: severity colours, Errors, repeats, Only this, a kept Mute"),
    ("properties", "properties_test.py", "Properties: hero by pixel, sections, chmod/rename/opens-with/SHA-256 checked outside the app"),
    ("sysupdate", "sysupdate_test.py", "System Update: finds, shows and installs a change, by pixel and by sum"),
    ("help", "help_test.py", "Help: contents, links, history and full-text search"),
    ("singleinst", "single_instance_test.py", "one copy of an app, and relaunch raises it"),
    ("osk", "osk_test.py", "the on-screen keyboard types, floats, drags, docks and closes"),
    ("settings", "settings_test.py", "the settings registry, in ring 3"),
    ("galleryset", "settings_gallery_test.py", "System Settings' cursor theme gallery: cards, preview, stage, apply"),
    ("smooth", "smooth_scroll_test.py", "smooth scrolling glides, and desktop.smooth_scroll turns it off"),
    ("shadow", "shadow_test.py", "drop shadows under windows and menus, and desktop.shadows turns them off"),
    ("glass", "glass_test.py", "transparency: clear/frosted/wallpaper glass on the taskbar, Start, menus and windows"),
    ("occlusion", "occlusion_test.py", "a window under an opaque one is not drawn; under glass it still shows"),
    ("anim", "animation_test.py", "open/close/minimize/restore ghosts, and desktop.animations turns them off"),
    ("idle", "idle_desktop_test.py", "nothing paints over an idle desktop"),
    ("halfframe", "half_frame_test.py", "no half-painted window frame while scrolling"),
    ("caret", "caret_blink_test.py", "the text caret blinks, stops solid, and never spins"),
    ("imgview", "imgview_test.py", "JPEG decoding, the viewer, the wallpaper, GIF playback, BMP"),
    ("icons", "icons_test.py", "app icons: QOI, alpha compositing, three draw sites"),
    ("desktopmenu", "desktop_menu_test.py", "desktop menus, glass, rename, properties, popup corners"),
    ("taskbarmenu", "taskbar_menu_test.py", "the taskbar strip's and the Start button's right-click menus"),
    ("player", "player_test.py", "the Audio Player on a machine with NO sound device"),
    ("video", "video_test.py", "the Video Player: drawn, moving, pausing, seeking, both codecs, full screen"),
    ("calendar", "calendar_test.py", "the tray clock's calendar popup: grid, week start, week numbers, Settings link"),
    ("clock", "clock_settings_test.py", "Settings' Date & time: Change... steps the kernel clock, time -s, the NTP lock"),
    ("taskbar_style", "taskbar_style_test.py", "the taskbar's buttons, alignment, Start position, floating and themes, drawn and reported"),
    ("taskbar_peek", "taskbar_peek_test.py", "the taskbar's window preview: opens, shows the window, acts, highlights, switches off"),
    ("taskbar_drag", "taskbar_drag_test.py", "dragging taskbar buttons to reorder, the glide, click on release, drag-over raise"),
    ("mines", "mines_test.py", "Minesweeper, and a secondary click reaching a client"),
    ("charmap", "charmap_test.py", "Character Map: search, copy and paste back, the text line, blocks, the Fonts page"),
    ("volume", "volume_test.py", "the tray volume flyout: slider, mute, wheel, devices"),
    ("traypress", "tray_press_test.py", "the tray's hover and pressed fills, and that neither latches"),
    ("brightness", "brightness_test.py", "the tray brightness flyout, and its answer with no backlight"),
    ("network", "network_tray_test.py", "the tray network item: its state, card, switch (netctl down/up) and visibility"),
    ("kblayouts", "keyboard_layouts_test.py", "several keyboard layouts: the list, the tray item, Super+Space, the Settings page and Try it"),
    ("modeset", "modeset_test.py", "a runtime resolution change: device, desktop and setting agree"),
    ("wallpaper", "wallpaper_mode_test.py", "fit vs fill, at a mode where they differ, and when the picture is decoded"),
    ("livewall", "live_wallpaper_test.py", "live wallpapers: the background client, moving, paused, crash fallback, GIF, plain colour"),
    ("thumbcache", "thumbcache_test.py", "thumbnail decode rate, and the disk cache under it"),
    ("shortcut", "shortcut_test.py", "global keyboard shortcuts, and rebinding them"),
    ("identity", "window_identity_test.py", "a held window survives a close or raise that renumbers windows[]"),
    ("focusstate", "focus_state_test.py", "clients hear the WM's focus; minimize/restore keeps maximized"),
    ("badpresent", "bad_present_test.py", "a refused buffer replacement keeps the last frame and the WM alive"),
    ("npoptions", "notepad_options_test.py", "Notepad Options: saved, applied, remembered across runs"),
]

# Roughly how long each tool takes, in seconds, used ONLY to decide what
# order to START them in (longest first -- see pick_order below). These
# are measured wall-clock times from one run on one machine, so treat
# them as a hint and nothing more: a stale or wrong number costs some
# scheduling efficiency and can never affect a result, because the tools
# are independent and each gets its own image and VM regardless.
#
# A tool with no entry here is assumed SLOW rather than fast. Being
# wrong in that direction costs nothing (a quick tool started early
# finishes early), while assuming a new tool is quick would risk making
# it the straggler everything else waits behind -- which is the exact
# problem this table exists to fix.
# Extra `vm.py` flags for the few tools whose machine has to differ from
# the default one. Deliberately a small dict rather than a field on
# every row: the default -- no sound card, PS/2 input, one IDE disk -- is
# what nearly every tool wants and what an unusual one should have to
# state.
EXTRA_VM_ARGS = {
    # The volume flyout's device list is empty on a machine with no
    # sound hardware, so its device rows would be untestable exactly
    # where they matter. `both` gives it an AC'97 and a USB card with no
    # recording attached. player_test.py deliberately keeps the default,
    # since a machine with NO device is its premise.
    "volume": ["--audio", "both"],
    # A card's Format panel exists only for a card: QEMU's HDA, whose
    # seven rates the test expects by name.
    "sndformat": ["--audio", "hda"],
    # The Start menu's favourites have to SURVIVE A REBOOT, and the
    # default launch answers a reboot by ending QEMU (-no-reboot, so a
    # triple-faulting guest stops rather than looping). This tool is the
    # one that deliberately reboots, so it gets a guest that can.
    "startmenu": ["--reboot"],
    # A USB MOUSE, BECAUSE THE PS/2 ONE CANNOT LOSE A CLICK HERE. The
    # stage that dropped short presses is the HID drain -- it takes every
    # queued report in one pass, so a press and its release cancel before
    # anything looks. The PS/2 path interrupts per packet and the idle
    # loop wakes on those interrupts, so it gets a look in between no
    # matter how fast the tap: with the fix reverted, the PS/2 guest
    # still passed every check. MEASURED, not assumed.
    "mousebtn": ["--usb", "xhci+mouse"],
    # THE ONE QEMU DISPLAY WITH A CURSOR PLANE, which the lease under test
    # needs. The tool asks for it too, but the runner's guest is already
    # on the slot by then -- `vm.py start` answers "already running" and
    # exits 0 -- so without this it tested a standard VGA and failed the
    # cursor-plane pair on every suite run while passing alone.
    "fullscreen": ["--vga", "virtio"],
}

COST_S = {
    # Re-measured 2026-08-18, against the ring-3 desktop (which is the
    # only desktop now). Three of these moved a long way when their
    # tools were reshaped: `compositor` no longer spawns a stand-in
    # compositor and waits for it, `screen` drives its client in `auto`
    # mode instead of one keystroke at a time, and `compdeath` kills the
    # desktop directly. A stale cost is not a correctness problem -- it
    # only makes the start order slightly wrong -- but a tool whose real
    # cost has tripled would quietly become the straggler.
    # Re-measured 2026-08-21 from two full runs: the 663d63b pass cut
    # forcequit from ~71s to ~35s (it waits on observable client death
    # now, not a real ping timeout), so notepad (~41s) is the straggler.
    "notepad": 41,     # the current ceiling -- slowest single tool
    "forcequit": 35,
    "menubar": 32,
    "popup": 30,       # a resize, a drag, five settled frames
    "idle": 10,        # eight captures a third of a second apart
    "caret": 30,       # two seconds of blink, the ten-second stop, a CPU window
    "imgview": 30,     # two decodes, several settled frames, a wallpaper hop
    "wallpaper": 30,   # a resolution change out and back, plus five settled frames
    "icons": 25,       # three draw sites, each a settled frame
    "desktopmenu": 30, # eight groups, four settled frames, two folder polls
    "gfxdemo": 24,
    "cursor": 21,
    "saver": 22,
    "uapp": 19,
    "taskmgr": 19,
    "devmgr": 25,      # a row-by-row select twice, a disable and an enable
    "bootmgr": 25,     # two apps, a save, a refused save, four bootcfg reads
    "properties": 25,  # two windows, a screenshot, five edits each read back
    "help": 15,        # a dozen settled clicks and keys, one screenshot
    "uidemo": 17,
    "blank": 17,
    "wingeom": 70,
    "uterm": 16,
    "crt": 30,         # three effect frames under TCG, an Options round trip
    "crashview": 30,   # a crash, three viewer opens, readelf on the host
    "hover": 5,        # eight injected moves and two counter reads
    "osk": 20,         # ~40 keycap clicks, each confirmed against the WM
    "singleinst": 16,  # six launches, each waiting out a client's first frame
    "scrollbar": 15,
    "settings": 20,   # +4 scroll checks, incl. a resize and a wheel
    "galleryset": 14, # one page, one settled frame, an Apply
    "smooth": 10,      # three glides waited out, a drag, and a fixture of 40 files
    "shadow": 8,       # two apps, two drags, four screenshots
    "anim": 12,        # eight state changes waited out, three frames each
    "calculator": 14,
    "entries": 13,
    "sched": 12,
    "winclient": 12,
    "compdeath": 11,
    "screen": 9,
    "crash": 9,
    "uiclient": 9,
    "dialog": 9,
    "compositor": 8,
}
COST_UNKNOWN_S = 90


def pick_order(picked):
    """Longest job first -- the standard fix for a parallel makespan.

    Order matters here for one reason: with `-j4`, the run cannot finish
    before its slowest tool does, so a slow tool that STARTS late adds
    its whole duration to the tail. `forcequit` (71s) sat eleventh of
    fourteen in dependency order and finished alone, well after the
    other thirteen had drained -- 339s of tool-time took 116s of wall
    clock when the theoretical floor was ~85s.

    This is LPT scheduling (Graham 1969), which is within 4/3 of optimal
    and costs one sort. Deliberately applied to the START order ONLY:
    the summary table is still rebuilt in the declared dependency order,
    so a toolkit regression still reads before the apps built on it.

    Ties keep declaration order (`sorted` is stable), so the ordering is
    deterministic and a run is reproducible.
    """
    return sorted(picked, key=lambda t: -COST_S.get(t[0], COST_UNKNOWN_S))


def run_one(name, script, disk_src, timeout, keep_logs, slot, kvm=False):
    """Run one tool against its own image, in VM slot `slot`.

    Everything that could collide between two concurrently running
    tools is derived from `slot`: vm.py's pidfile/serial socket/QMP
    port/VNC display (`--instance`), and the `--sock`/`--qmp-port` the
    tool itself connects with. The disk copy is already per-tool.

    Note the `stop` below is also slot-scoped -- it used to be a bare
    `vm.py stop`, which under parallelism would have killed a sibling's
    VM rather than a leftover of its own.
    """
    tool = os.path.join(HERE, script)
    if not os.path.exists(tool):
        return ("SKIP", 0.0, f"no such tool: {script}")

    img = os.path.join(tempfile.gettempdir(), f"gui_regress_{name}.img")
    vm = os.path.join(HERE, "vm.py")
    inst = ["--instance", str(slot)]

    subprocess.run([sys.executable, vm] + inst + ["stop"], cwd=REPO,
                   capture_output=True)
    # --reflink=auto: a copy-on-write clone where the filesystem
    # supports it, a plain copy where it doesn't. Never --reflink=always,
    # which fails outright on ext4. --sparse=always because disk.img is
    # ~4 MB of data in a 9 GB sparse file, and the destination is
    # usually /tmp on a tmpfs -- filling the holes in would cost 9 GB of
    # RAM per tool (`cp` defaults to --sparse=auto and gets this right
    # already; stating it means a future edit cannot quietly lose it,
    # which is exactly how damage_hunt.py's shutil.copyfile did).
    copy_disk(disk_src, img, cwd=REPO)

    # KVM is a per-run choice, passed straight to vm.py. Opt-in, never
    # the default -- see launch_qemu_cmd()'s comment on why a gate
    # quietly switched to KVM stops covering the emulated path.
    kvm_args = ["--kvm"] if kvm else []

    started = time.time()
    try:
        # CHECK vm.py's exit code. It used to be ignored, so a guest
        # that never started -- a refused port, a stale ISO, a QEMU
        # that died -- showed up only as the TOOL failing with
        # "could not connect to QMP", which says nothing about why and
        # points at the tool rather than at the launch. Eighteen tools
        # reported that at once before this existed, and the actual
        # reason was one line on vm.py's stderr that nobody read.
        boot = subprocess.run([sys.executable, vm] + inst + kvm_args +
                              EXTRA_VM_ARGS.get(name, []) +
                              ["--disk", img, "start"],
                              cwd=REPO, capture_output=True, text=True,
                              timeout=120)
        if boot.returncode != 0:
            why = (boot.stderr or boot.stdout or "").strip().splitlines()
            return ("FAIL", time.time() - started,
                    "the guest never started: "
                    + (why[0] if why else f"vm.py exited {boot.returncode}"))
        # -u, because the partial output below is the whole diagnosis
        # and without it there is none. A captured child's stdout is a
        # pipe, so Python block-buffers it: a tool killed by the guard
        # has flushed nothing, and the timeout branch reports "NO
        # output at all" for a tool that had printed thirty checks.
        # A PRIVATE TMPDIR, removed after: tools leave disk copies and
        # screenshots under tempfile, and /tmp is a tmpfs (private_tmp.py).
        with private_tmp(name) as env:
            r = subprocess.run([sys.executable, "-u", tool,
                                "--instance", str(slot)],
                               cwd=REPO, capture_output=True, text=True,
                               timeout=timeout, env=env)
        out = r.stdout + r.stderr
        rc = r.returncode
    except subprocess.TimeoutExpired as e:
        # KEEP WHAT IT MANAGED TO SAY. A timed-out tool used to report
        # the bare word TIMEOUT, so a run that hit the guard showed zero
        # checks and no hint of WHERE -- indistinguishable from a tool
        # that crashed on line one, and the reason a `files` that merely
        # grew past the guard read as a hang. The partial output names
        # the last check that passed, which is the whole diagnosis.
        partial = ""
        for chunk in (e.stdout, e.stderr):
            if not chunk:
                continue
            partial += chunk if isinstance(chunk, str) else chunk.decode(
                "utf-8", errors="replace")
        tail = partial.strip().splitlines()[-25:]
        out = (f"TIMEOUT after {timeout}s -- raise --timeout if the tool "
               f"merely grew; the last checks it printed were:\n"
               + "\n".join(tail) if tail else f"TIMEOUT after {timeout}s "
               f"with NO output at all (it hung before its first check)")
        rc = 124
    finally:
        subprocess.run([sys.executable, vm] + inst + ["stop"], cwd=REPO,
                       capture_output=True)
        # The per-tool image has served its purpose once the VM is
        # stopped. Left behind, a full run leaves thirteen of them in
        # /tmp -- sparse, so cheap on disk, but that is RAM on a tmpfs
        # and they are stale the moment `make iso` reseeds disk.img.
        try:
            os.unlink(img)
        except OSError:
            pass

    if keep_logs:
        # The slot goes in the LOG, not only on the console. An
        # intermittent failure is diagnosed after the fact from these
        # files, and "which slot was it on" is the first question asked
        # of one -- it was unanswerable once already, which is how a
        # slot correlation ended up recorded in docs/roadmap.md on no
        # evidence and had to be withdrawn.
        with open(os.path.join(keep_logs, f"{name}.log"), "w") as f:
            f.write(f"# tool={name} slot={slot} qmp={4445 + slot} "
                    f"serial={'.vm.serial' if slot == 0 else f'.vm.{slot}.serial'}\n")
            f.write(out)

    # The tools all print a "<name>: N passed, M failed" summary line;
    # pull it out for the table, but fall back to the last line rather
    # than to nothing if a tool died before printing one.
    summary = ""
    for line in out.splitlines():
        if "passed," in line and "failed" in line:
            summary = line.strip()
    if not summary:
        tail = [l for l in out.splitlines() if l.strip()]
        summary = tail[-1].strip() if tail else "(no output)"

    if rc == EXIT_SKIP:
        summary = "nothing judged -- " + summary
    return (status_of(rc), time.time() - started, summary)


# automake's SKIP convention, which tools/harness.py's finish() follows:
# a run that judged NOTHING (every check skipped) is neither a pass nor
# a fail, and is counted on its own line.
EXIT_SKIP = 77


def status_of(rc):
    return "PASS" if rc == 0 else "SKIP" if rc == EXIT_SKIP else "FAIL"


def report_skips(results):
    skipped = [r[0] for r in results if r[1] == "SKIP"]
    if skipped:
        print(f"\ngui_regress: {len(skipped)} tool(s) skipped, judging nothing: "
              f"{', '.join(skipped)}")


def all_clear(results, where=""):
    """Never a bare "all clear" while anything skipped: the skips are named."""
    skipped = [r for r in results if r[1] == "SKIP"]
    line = "gui_regress: all clear" + where
    if skipped:
        line += f", {len(skipped)} skipped: " + ", ".join(
            f"{r[0]} ({r[3].split(' -- ')[0]})" for r in skipped)
    return line


def run_remote_suite(picked, args):
    """The whole suite against one bare-metal machine, SERIALLY.

    There is one laptop: no slots, no parallelism, and nothing to copy.
    That also means this can never be a gate -- it is a spot-check tier
    like `qemu_matrix.py`, run when a change is hardware-shaped or
    before a release.

    ASK THE MACHINE FIRST, and say which one did not answer: the Lenovo
    is not always switched on (local_info.txt), and silence from it is a
    machine that is off rather than fifty broken tools.
    """
    sys.path.insert(0, HERE)
    import remote_gui

    host = args.host
    if not remote_gui.machine_is_up(host):
        print(f"gui_regress: {host} is not answering on telnet -- is it "
              f"powered on and booted? (local_info.txt lists the machines)")
        return 2
    con = remote_gui.RemoteConsole(host=host, quiet=True)
    try:
        if not remote_gui.desktop_is_up(con):
            print(f"gui_regress: {host} answers, but no desktop is running "
                  f"-- `guictl state` says nothing. It may be at a shell "
                  f"prompt or mid-boot.")
            return 2
    finally:
        # THE SESSION, not just this object: inetd serves four at a time
        # and the parent holding one leaves the children three. The
        # check above is the only thing this process does itself.
        remote_gui.close_session()

    print(f"gui_regress: {len(picked)} tool(s) against {host} (bare metal), "
          f"one at a time\n")
    results = []
    for name, script, what in picked:
        print(f"=== {name}: {what}  [{host}]")
        status, secs, summary = run_one_remote(name, script, host,
                                               args.timeout, args.logs)
        print(f"    {status}  ({secs:.0f}s)  {summary}\n", end="\n")
        results.append((name, status, secs, summary))

    print("gui_regress: summary")
    for name, status, secs, summary in results:
        print(f"  {status:<5} {name:<12} {secs:5.0f}s  {summary}")

    report_skips(results)
    na = [r[0] for r in results if r[1] == "N/A"]
    if na:
        print(f"\ngui_regress: {len(na)} tool(s) cannot run on hardware: "
              f"{', '.join(na)}")
    failed = [r[0] for r in results if r[1] == "FAIL"]
    if failed:
        print(f"\ngui_regress: FAILED on {host} -- {', '.join(failed)}")
        if not args.logs:
            print("  re-run with --logs DIR to keep each tool's full output")
        return 1
    print("\n" + all_clear(results, f" on {host}"))
    return 0


def run_one_remote(name, script, host, timeout, keep_logs):
    """Run one tool against the BARE-METAL machine.

    No guest to launch, no disk to copy, no slot: there is ONE machine
    and it is already running. `TOYOS_REMOTE_HOST` is what redirects the
    tool -- see tools/remote_gui.py for why that lands in the two
    constructors rather than in a flag each tool has to grow.

    A tool that asks for something hardware cannot do raises
    `RemoteUnsupported`, and that is reported as **N/A with the reason**
    rather than as a failure: "this check needs an emulator" and "the
    desktop is broken" are different answers, and a run that conflates
    them teaches a reader to ignore red.
    """
    tool = os.path.join(HERE, script)
    if not os.path.exists(tool):
        return ("SKIP", 0.0, f"no such tool: {script}")

    env = dict(os.environ)
    env["TOYOS_REMOTE_HOST"] = host
    started = time.time()
    try:
        with private_tmp(name, env) as env:
            r = subprocess.run([sys.executable, "-u", tool],
                               cwd=REPO, capture_output=True, text=True,
                               timeout=timeout, env=env)
        out = r.stdout + r.stderr
        rc = r.returncode
    except subprocess.TimeoutExpired as e:
        partial = ""
        for chunk in (e.stdout, e.stderr):
            if not chunk:
                continue
            partial += chunk if isinstance(chunk, str) else chunk.decode(
                "utf-8", errors="replace")
        tail = partial.strip().splitlines()[-25:]
        out = (f"TIMEOUT after {timeout}s on {host}; last output:\n"
               + "\n".join(tail)) if tail else f"TIMEOUT after {timeout}s with no output"
        rc = 124

    if keep_logs:
        with open(os.path.join(keep_logs, f"{name}.log"), "w") as f:
            f.write(f"# tool={name} host={host} (bare metal)\n")
            f.write(out)

    unsupported = [ln.strip() for ln in out.splitlines()
                   if "RemoteUnsupported" in ln]
    summary = ""
    for line in out.splitlines():
        if "passed," in line and "failed" in line:
            summary = line.strip()
    if not summary:
        tail = [ln for ln in out.splitlines() if ln.strip()]
        summary = tail[-1].strip() if tail else "(no output)"
    if rc != 0 and unsupported and not summary.startswith(name):
        return ("N/A", time.time() - started, unsupported[-1][:160])
    if rc == EXIT_SKIP:
        summary = "nothing judged -- " + summary
    return (status_of(rc), time.time() - started, summary)


def acquire_run_lock():
    """Refuse a SECOND concurrent suite run. Returns (handle, holder).

    Every tool here takes a VM SLOT, and the slots start at 0 for every
    run -- so two suites started at once fight over the same pidfiles,
    serial sockets and QMP ports. It does not fail as a port clash:
    `port_guard` refuses some of the second run's launches, and the rest
    surface MINUTES later as `BrokenPipeError` or a screenshot that was
    never written, in whichever tool happened to be mid-command. Seven
    tools "failed" that way in one run here, none of them at fault.

    An flock, so a run killed with -9 leaves nothing to clean up: the
    lock dies with the process that held it. The pid in the file is for
    the message only.
    """
    import fcntl
    path = os.path.join(REPO, "build", ".gui_regress.lock")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fh = open(path, "a+")
    try:
        fcntl.flock(fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        fh.seek(0)
        holder = fh.read().strip() or "unknown pid"
        fh.close()
        return None, holder
    fh.seek(0)
    fh.truncate()
    fh.write(f"{os.getpid()}\n")
    fh.flush()
    return fh, None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-k", "--only", action="append", metavar="NAME",
                    help="run only these tools (repeatable); matches on the short name")
    ap.add_argument("--disk", default=None,
                    help="image to copy for each run (default: a FRESH image of this "
                         "build, made by tools/fresh_disk.py -- not disk.img)")
    ap.add_argument("--no-fresh", action="store_true",
                    help="copy disk.img as it is instead of a fresh image: what a "
                         "test or `make run` left on it reaches every tool")
    ap.add_argument("--timeout", type=int, default=600,
                    help="per-tool timeout in seconds (default: 600). This is a "
                         "HANG GUARD, not a budget: it wants a wide margin over "
                         "the slowest real tool, which is `files` at ~5min (it "
                         "drives real file operations through spawned children "
                         "and waits on each). At 360 it began failing HEALTHY "
                         "runs as that tool grew -- and a timed-out tool prints "
                         "NOTHING, so the symptom was zero checks and no clue "
                         "which one hung, the worst possible failure for a "
                         "guard. Re-check the margin whenever `files` grows. "
                         "It was 180 while the ceiling was "
                         "~41s; a guard with no margin fails healthy runs, which "
                         "is worse than no guard.")
    ap.add_argument("--logs", metavar="DIR",
                    help="write each tool's full output to DIR/<name>.log")
    ap.add_argument("--host", metavar="IP",
                    help="run against the BARE-METAL machine at this address "
                         "instead of QEMU guests (serial, one machine; see "
                         "tools/remote_gui.py). local_info.txt has the addresses")
    ap.add_argument("--list", action="store_true", help="list the tools and exit")
    ap.add_argument("--kvm", action="store_true",
                    help="run the guests under KVM instead of TCG -- much "
                         "faster, and deliberately NOT the default: every "
                         "automated test here runs TCG, and the difference is "
                         "load-bearing (see tools/kvm_soak.py). Use it to "
                         "iterate, not to judge a release.")
    ap.add_argument("-j", "--jobs", type=int, default=DEFAULT_JOBS, metavar="N",
                    help=f"run N tools concurrently, each in its own VM slot "
                         f"(default: {DEFAULT_JOBS}). -j1 is the old serial "
                         f"behaviour, one VM at a time in slot 0.")
    args = ap.parse_args()

    if args.list:
        for name, script, what in TOOLS:
            print(f"  {name:<12} {script:<28} {what}")
        return 0

    picked = [t for t in TOOLS
              if not args.only or any(k in t[0] for k in args.only)]
    if not picked:
        print(f"gui_regress: nothing matched {args.only}")
        return 2

    if args.logs:
        os.makedirs(args.logs, exist_ok=True)
    import tree_lock
    tree_lock.hold()   # the fresh disk and every tool read this build

    if args.host:
        return run_remote_suite(picked, args)

    # A FRESH IMAGE BY DEFAULT. `make iso` syncs disk.img rather than
    # reformatting it, so remembered window positions, a changed setting
    # or a stray launcher left by an earlier test or a `make run` reach
    # every copy -- four tools failed that way on 2026-09-30, and
    # predates.py agreed they were pre-existing because it boots the same
    # image. Twenty seconds buys a fixture that is the build and nothing
    # else (tools/fresh_disk.py).
    fresh_dir = None
    if args.disk is None and not args.no_fresh:
        fresh_dir = tempfile.mkdtemp(prefix="gui_regress_fresh.")
        atexit.register(shutil.rmtree, fresh_dir, True)   # every return path, not just the last
        disk = os.path.join(fresh_dir, "fresh.img")
        print("gui_regress: making a fresh image of this build (tools/fresh_disk.py)")
        fresh_disk.make_fresh(disk)
    else:
        disk = args.disk or "disk.img"
        disk = disk if os.path.isabs(disk) else os.path.join(REPO, disk)
        if not os.path.exists(disk):
            print(f"gui_regress: no {disk} -- run `make iso` first")
            return 2
        found = fresh_disk.drift(disk)
        if found:
            print("gui_regress: " + "\ngui_regress: ".join(fresh_disk.describe(disk, found)) + "\n")

    if shutil.which("qemu-system-x86_64") is None:
        print("gui_regress: qemu-system-x86_64 not on PATH")
        return 2

    lock, holder = acquire_run_lock()
    if lock is None:
        print(f"gui_regress: another run is already going (pid {holder}).\n"
              f"gui_regress: two suites share VM slot 0 upwards and would "
              f"kill each other's guests -- wait for it, or `kill {holder}`.")
        return 2

    jobs = max(1, min(args.jobs, len(picked)))
    print(f"gui_regress: {len(picked)} tool(s), each on its own copy of "
          f"{'a fresh image' if fresh_dir else os.path.basename(disk)}, {jobs} at a time\n")

    # Each concurrent tool gets a VM slot, and the slot is what keeps
    # two of them from sharing a pidfile, a serial socket or a QMP port
    # (see vm.py's _apply_instance and run_one above).
    #
    # Slots are LEASED from a pool, not derived from the tool's position
    # in the list. Position looks equivalent and is not: with `-j4` and
    # seven tools, task 4 also maps to slot 0, but it starts as soon as
    # ANY worker frees up -- which is routinely while task 0 is still
    # running on slot 0. Written that way first, and the symptom was
    # ugly and misleading: the fifth tool's `vm.py --instance 0 stop`
    # killed the first tool's VM out from under it, so the FIRST tool
    # died on a broken pipe and the fifth died on a screenshot that was
    # never written. Neither traceback pointed anywhere near the
    # scheduling. A lease is held for exactly as long as the VM exists.
    free_slots = queue.Queue()
    for s in range(jobs):
        free_slots.put(s)

    def work(name, script, what):
        slot = free_slots.get()
        try:
            status, secs, summary = run_one(name, script, disk, args.timeout,
                                            args.logs, slot, args.kvm)
        finally:
            free_slots.put(slot)
        return (name, what, slot, status, secs, summary)

    done = {}
    with cf.ThreadPoolExecutor(max_workers=jobs) as pool:
        # Submitted longest-first (pick_order), not in declaration
        # order: a ThreadPoolExecutor starts tasks in submission order,
        # so this is what keeps the slowest tool from being the tail.
        futures = [pool.submit(work, *t) for t in pick_order(picked)]
        # as_completed, not map: results print the moment each tool
        # finishes rather than in submission order, so one slow tool
        # doesn't hold up everything behind it. The summary table below
        # is rebuilt in the original (dependency) order regardless.
        for fut in cf.as_completed(futures):
            name, what, slot, status, secs, summary = fut.result()
            print(f"=== {name}: {what}  [slot {slot}]\n"
                  f"    {status}  ({secs:.0f}s)  {summary}\n", end="\n")
            done[name] = (status, secs, summary)

    results = [(name, *done[name]) for name, _s, _w in picked if name in done]

    print("gui_regress: summary")
    for name, status, secs, summary in results:
        print(f"  {status:<5} {name:<12} {secs:5.0f}s  {summary}")

    report_skips(results)
    failed = [r[0] for r in results if r[1] == "FAIL"]
    if failed:
        print(f"\ngui_regress: FAILED -- {', '.join(failed)}")
        if not args.logs:
            print("  re-run with --logs DIR to keep each tool's full output")
        return 1
    print("\n" + all_clear(results))
    return 0


if __name__ == "__main__":
    sys.exit(main())
