#!/usr/bin/env python3
"""Run the tools nothing else runs, and report which ones have rotted.

WHY THIS EXISTS
---------------
`preflight.sh` and `gui_regress.py` between them cover most of tools/.
About twenty test tools are covered by NEITHER: they need a particular
boot target, absent hardware, a second QEMU, an IWAD nobody fetched, or
simply take too long for a per-change gate. They run when somebody types
them, which is to say almost never.

**Two of them were found red by accident in one session**, having failed
for an unknown period: `fs_switch_test.py` (10 of its checks, because
`df` and `stat` became /bin programs and it still parsed the ring-0
output) and `init_test.py` (16 of 31, same class). Neither failure was a
regression anybody introduced; both were rot, and the repo had no way to
notice. That is the gap.

WHAT IT IS NOT. Not a gate, and it must never become one -- several of
these need hardware or a fetched IWAD, and a check that cannot pass on a
clean checkout is a check people learn to ignore. It is the thing you
run before a release, or when you want to know whether the on-demand
half of this directory still works.

A SKIP IS A RESULT, NOT A PASS. A tool that cannot run here says so and
is counted separately, because "19 passed" over a table with eight skips
in it is a number that means something different from what it looks
like.

    python3 tools/ondemand_sweep.py               # everything runnable
    python3 tools/ondemand_sweep.py --only init   # one, by name fragment
    python3 tools/ondemand_sweep.py --list        # what it would run
"""
import argparse
import concurrent.futures as cf
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# Each entry: name, script, what it covers, and how it must be run.
#
# `serial` means it needs the one shared VM slot / the physical console
# / its own boot, and cannot share the machine with another run. Those
# are run one at a time even under -j; everything else fans out.
#
# `needs` is a precondition the sweep checks BEFORE running, so a
# missing IWAD reports as a skip with a reason rather than as a failure.
#
# `vm` means the tool ATTACHES to an already-running `vm.py` guest
# rather than launching its own QEMU. Those need one started for them,
# and the sweep does it -- otherwise they fail with "could not connect
# to QMP", which is the sweep's fault and not theirs.
#
# THE HAZARD THIS FIXES IS WORSE THAN A FAILURE. Tools that launch
# their own guest sometimes leave it running, so an attaching tool
# further down the list can find somebody else's VM and PASS on it.
# That is a green line that means nothing, and which one you get
# depends on the order the list happens to be in.
TOOLS = [
    # name          script                     what it covers                        serial  needs                  wants_vm
    # --- host-side decoder oracles ----------------------------------
    # No guest at all: the same .c compiled with the host gcc, judged by
    # a decoder that shares no code with it.
    ("usnd_host",   "usnd_hostcheck.py",       "the MP3 decoder against ffmpeg",     False,
     ("host_audio", "needs gcc, lame and ffmpeg on PATH"),                                   False),
    # Needs only gcc and the Python standard library -- hashlib and zlib
    # are the oracle -- so it has no `needs` gate at all.
    ("hash_host",   "hash_hostcheck.py",       "crc32/sha256 against hashlib and zlib", False,
     None,                                                                                   False),
    # The HID report-descriptor walker against descriptors CAPTURED off
    # real devices -- a Logitech G305 receiver and QEMU's pair. Host-only
    # for the same reason: the bytes are fixed, and the failure it exists
    # to catch (an axis read from the wrong bits) is a pointer flying
    # across somebody's screen with a person as the only oracle.
    ("hid_parse",   "hid_parse_hostcheck.py",  "the HID descriptor parser vs real captures", False,
     None,                                                                                   False),
    # The easing tween's invariants, on the host: it takes its clock as
    # an argument, so nothing here needs a guest.
    ("utween_host", "utween_hostcheck.py",     "the easing tween lands, is monotonic, eases out", False,
     None,                                                                                   False),
    # The two image harnesses, both host-only. The codec one runs the QOI
    # and PNG codecs BOTH WAYS against Pillow and zlib; the other is the
    # JPEG decoder against libjpeg.
    ("uimg_codec",  "uimg_codec_hostcheck.py", "QOI and PNG both ways vs Pillow/zlib", False,
     ("host_pillow", "needs gcc and Pillow"),                                                 False),
    ("uimg_jpeg",   "uimg_hostcheck.py",       "the JPEG decoder against libjpeg",   False,
     ("host_pillow", "needs gcc and Pillow"),                                                 False),
    # Compiles the vendored dash against tolibc with -nostdinc and
    # reports what the compiler still refuses. Needs gcc and nothing
    # else -- no guest, since nothing links or runs.
    ("dash_gap",    "dash_gap.py",             "what the dash port still needs from tolibc", False,
     ("host_cc", "needs gcc on PATH"),                                                       False),
    # tolibc's formatter and parsers against glibc, ~8,900 cases. Host
    # gcc only; exits non-zero when anything differs.
    ("libc_diff",   "libc_diff.py",            "tolibc's printf and strtol against glibc", False,
     ("host_cc", "needs gcc on PATH"),                                                       False),
    # A REPORT, not a gate: it always exits 0, because whether a
    # duplicated block is worth extracting is a judgement call (see the
    # tool's docstring for the bar) and a check that failed a build on it
    # would be wrong most of the time. It is in the sweep so the number
    # stays visible and the tool keeps being exercised.
    ("dup_scan",    "dup_scan.py",             "copy-paste across userland -- a report, never a gate", False,
     None,                                                                                   False),
    # Boots its OWN guest from a copy of disk.img with the scripts
    # written in, so wants_vm is False -- handing it one would point it
    # at an image with no test scripts on it.
    ("dash_shell",  "dash_test.py",            "dash behaves like a shell: 18 constructs", False,
     None,                                                                                   False),
    # Renders every docs/commands page through the same umd.c the guest
    # runs, at three widths. gcc and the standard library only.
    ("umd_host",    "umd_hostcheck.py",        "the Markdown renderer over every page", False,
     None,                                                                                   False),
    # filemanager_test.py's own wait/toolbar logic against a scripted
    # console: a missing item, a split report, an expired wait. Standard
    # library only.
    ("fm_harness",  "filemanager_harness_hostcheck.py", "the File Manager harness's own logic", False,
     None,                                                                                   False),
    # settings_test.py's own geometry and waits against a scripted
    # console: a row aimed at where it is, a scroll position waited for,
    # a page confirmed before its controls. Standard library only.
    ("set_harness", "settings_harness_hostcheck.py", "the Settings harness's own logic", False,
     None,                                                                               False),
    # The 128-bit division helpers, against Python's arbitrary-precision
    # integers. gcc and the standard library only.
    ("divti3_host", "divti3_hostcheck.py",     "__udivti3 and friends against bignums", False,
     None,                                                                                   False),
    # utext's sparse wrap index against the naive scan it replaced, over
    # this repo's own Markdown and pci.ids. gcc and the standard library.
    ("utext_host",  "utext_hostcheck.py",      "the editor's wrap index vs a naive scan", False,
     None,                                                                                   False),
    # The toolkit's text measurement and uui_textbox's caret/window/hit
    # geometry against a synthetic HOSTILE proportional face -- narrow
    # 'i' against wide 'W', with kerning on. gcc and the standard
    # library. Carries a --positive-control that must go red.
    ("ugfx_text",   "ugfx_text_hostcheck.py",  "text measurement + textbox geometry", False,
     None,                                                                                   False),
    # The Terminal's shipped colour schemes against the ANSI -> VGA
    # permutation the loader applies, reimplemented here from the
    # parser's table rather than shared with it.
    ("term_scheme", "term_scheme_hostcheck.py", "Terminal colour schemes vs the ANSI order", False,
     None,                                                                                   False),
    # --- networking -------------------------------------------------
    # Launches its own guests against a COPY of disk.img and serves TLS
    # from the host, so wants_vm is False and nothing leaves the machine.
    # Needs openssl to make the certificates and SKIPS cleanly without it.
    ("https",       "https_test.py",           "wget over TLS, and what it refuses", False,
     ("openssl",    "needs openssl on PATH to make test certificates"),                       False),
    # Same arrangement, no certificates needed: a plain http server on
    # the host, so this too leaves nothing.
    ("hwdata",      "hwdata_test.py",          "the id databases refuse a bad download", False, None,               False),
    # --- storage and boot -------------------------------------------
    ("partition",   "partition_test.py",       "mounting from an MBR/GPT partition", True,  None,                   False),
    ("fs_switch",   "fs_switch_test.py",       "format, remount, reboot persistence", True, None,                   False),
    # Two guests: one to install from, then the installed disk booted
    # alone. It makes its own images and never touches disk.img, so
    # wants_vm is False.
    ("install",     "install_test.py",         "self-hosted install, then boot it",  False, None,                   False),
    ("tfs3_v1",     "tfs3_v1_test.py",         "the older TFS3 on-disk version",     True,  None,                   False),
    # FAT32 and the mount table, cross-checked on the HOST with mtools
    # and fsck.fat. It launches its own guest against a COPY of
    # disk.img, so wants_vm is False; it SKIPS cleanly without mtools,
    # which is a skip rather than a pass -- see run_one().
    ("fat32",       "fat32_test.py",           "FAT32 and /boot, against mtools",    True,  None,                   False),
    ("live_boot",   "live_boot_test.py",       "the Live CD's RAM image",            True,
     ("live_iso", "no toy-os-live.iso -- run `make live-iso` first"),                        False),
    # Two disks on two drivers -- the one configuration no other tool
    # here boots, and the shape the enumerate-everything bug needed.
    ("multidisk",   "multidisk_test.py",       "two disks, two drivers, root=",      True,  None,                   False),
    ("virtio_boot", "virtio_boot_test.py",     "TFS3 on virtio-blk, no IDE",         True,  None,                   False),
    ("ahci",        "ahci_test.py",            "TFS3 on a SATA drive behind an HBA",  True,  None,                   False),
    ("sector4k",    "sector4k_test.py",        "GPT/TFS3/FAT32 on a 4K-sector disk", True,  None,                   False),
    ("nvme",        "nvme_test.py",            "root on NVMe, a 4K namespace, TRIM", True,  None,                   False),
    ("diskmark",    "diskmark_test.py",        "the Disk Mark GUI benchmark",        True,  None,                   True),
    ("ls",          "ls_test.py",              "/bin/ls flags and the listing cap",  True,  None,                   False),
    ("fileop",      "fileop_test.py",          "lib/ufileop through cp/mv/rm",       True,  None,                   False),
    # ATTACHES to a running guest: it only types at the debug console and
    # compares against digests it computes on the host.
    ("sum",         "sum_test.py",             "/bin/sum and /lib/libhash.so",       False, None,                   True),
    # ATTACHES like sum_test; unloads and reloads the e1000 module and
    # waits for netd to lease again, so it takes ~30 s.
    ("module",      "module_test.py",          "loadable modules: hello, refusals, an e1000 reload", False, None,     True),

    # --- shell, console, terminal ------------------------------------
    ("console",     "console_shell_test.py",   "a text boot reaching a ring-3 shell", True, None,                   False),
    ("ctrlc",       "ctrlc_test.py",           "Ctrl-C interrupting a real job",     True,  None,                   False),
    ("jobs",        "jobs_test.py",            "job control: fg, bg, &, Ctrl-Z",     True,  None,                   False),
    # wants_vm: stdin_test ATTACHES to a running guest (its own docstring
    # says to start one first), and without it dies instantly on the
    # serial socket -- the sweep's fault, per the note above.
    ("stdin",       "stdin_test.py",           "blocking fd 0 and /bin/tosh",        True,  None,                   True),
    ("ping_rtt",    "ping_rtt.py",             "the compositor<->client round trip", True,  None,                   True),
    # ATTACHES, and its numbers are only half-resolved on the sweep's own
    # TCG guest -- the compositor's clock is the PIT there. It is listed
    # anyway because the half that DOES resolve (the kernel's TSC-timed
    # stall table) is the half that finds a regression, and because a
    # tool no runner names is never run at all. Quote a real measurement
    # from a `--kvm --cpu host,+invtsc` guest, not from this.
    ("latency_io",  "latency_under_io.py",     "desktop latency under heavy disk I/O", True, None,                  True),
    # ATTACHES, like latency_io, and for the same reason quote a --kvm
    # guest rather than the sweep's TCG one: under TCG the two workloads
    # serialise on the one emulated CPU whatever the locks allow.
    ("fs_isolation", "fs_isolation.py",        "I/O on one path vs. another (fs locks)", True, None,                True),
    ("terminal",    "terminal_probe.py",       "the GUI Terminal's keys and paging", True,  None,                   True),
    # ATTACHES to a running guest and needs a desktop up. Its
    # load-bearing check compares its answer against the SERIAL
    # console's for the same subcommand.
    ("guictl",      "guictl_test.py",          "/bin/guictl against the console's `gui`", False, None,                True),
    ("grep",        "grep_test.py",            "/bin/grep through a real shell",     True,  None,                   True),
    ("doc",         "doc_test.py",             "/bin/doc finds and renders a page",  True,  None,                   False),
    ("ansi",        "ansi_cursor_test.py",     "ANSI cursor movement, as pixels",    True,  None,                   False),

    # --- networking ---------------------------------------------------
    # The longest tool here by far -- ten guests, each booted against a
    # COPY of disk.img -- and the only coverage of the boot-time address
    # path, since nothing in any gate boots a machine and looks at what
    # it configured itself with.
    ("net",         "net_test.py",             "the stack on both NICs, and DHCP",   True,  None,                   False),
    # Network time. Its server is on this machine's loopback, so it
    # reaches no external host and does not need connectivity.
    ("ntp",         "ntp_test.py",             "SNTP, SYS_SETTIME and the wall clock", True, None,                   False),
    # The remote-access pair, which is the only way to work on the
    # bare-metal laptop at all. Enables services that ship DISABLED and
    # leaves them enabled on disk.img -- run `make clean-disk && make
    # iso` after it if that matters.
    ("remote",      "remote_test.py",          "telnetd, tftpd and tools/remote.py end to end", True, None,           False),

    # --- sound --------------------------------------------------------
    # Boots its own guests with an AC97 and a wav audiodev, twice. It is
    # the only run in which the ac97 KTESTs do not skip, which is why
    # its own report treats "0 skipped" as an assertion.
    ("audio",       "audio_test.py",           "AC97, the PCM ring and a WAV file",  True,  None,                   False),
    ("audio_hda",   "audio_test.py --card hda", "Intel HDA through the same oracle", True,  None,                   False),
    ("soundd",      "soundd_test.py",          "two programs audible at once, mixed by the daemon", True, None,    False),
    # Three guests, and the only run in which anything reaches an
    # isochronous endpoint. Its middle phase gives each card its own wav
    # recording, which is the only way "which device played" is an
    # assertion rather than a guess.
    ("usb_audio",   "usb_audio_test.py",       "USB audio: isoch OUT, and which card plays", True, None,             False),
    # THE ONLY TOOL HERE THAT JUDGES THE SOUND RATHER THAN THE DRIVER.
    # It records the G6's analogue output back on the motherboard's line
    # in and counts dropouts and clicks, which is what "it crackles" in
    # docs/bugs.md was missing. `serial` because it owns the G6, and
    # `usb_audio` above passes that same device through to a guest --
    # the two must never overlap. It SKIPS when the cable is not patched
    # in, which is most of the time; see its docstring for why that is a
    # skip and not a failure.
    ("loopback",    "audio_loopback_test.py",  "what the G6 actually plays, recorded back", True,
     ("alsa_loop", "needs alsa-utils and both sound cards -- and the cable patched in"),      False),

    # --- a driver in ring 3 -------------------------------------------
    # Boots its own guest with an HD Audio controller: nothing else
    # here attaches one, so this is the only run in which the kernel
    # actually LETS GO of a device it was driving. The second row is
    # the control.
    ("devclaim",    "devclaim_test.py",        "ring 3 takes the sound card off the kernel", True, None,             False),
    ("devclaim_ctl", "devclaim_test.py --no-card", "...and the same with no card, which must skip the leg", True, None, False),
    # Stage 3, on the same guest for the same reason: the only place a
    # ring-3 process drives a real controller's command ring.
    ("hdacodec",    "hdacodec_test.py",        "ring 3 reads the codec graph and agrees with the kernel", True, None, False),
    ("hdacodec_ctl", "hdacodec_test.py --no-card", "...and the same with no card, which must say so", True, None, False),

    # --- the mixer's per-application volume ---------------------------
    # Its own guest for the same reason as the two above: gui_regress's
    # has no sound card, so soundd never starts and the per-app section
    # is correctly absent from the flyout it drives.
    ("mixer",       "mixer_test.py",           "a per-app volume, from the flyout to what soundd applies", True, None, False),

    # --- interrupts ---------------------------------------------------
    # The only run in which anything reaches the Local APIC. Its own
    # guest, with USB hardware, because the load-bearing check is that
    # moving a mouse delivers interrupts on a vector -- which nothing
    # else here would notice the absence of, since every control
    # transfer polls.
    ("msi",         "msi_test.py",             "the LAPIC, and the xHCI on an MSI-X vector", True, None,              False),

    # --- input --------------------------------------------------------
    ("kbd",         "kbd_test.py",             "`kbd`'s four columns, both drivers", True,  None,                   False),
    ("kbd_paths",   "keyboard_paths_test.py",  "PS/2 and virtio-input agree",        True,  None,                   False),
    ("virtio_input", "virtio_input_test.py",   "virtio keyboard, mouse and tablet",  True,  None,                   False),
    # Boots its own guests with -device qemu-xhci, so it is in the sweep
    # rather than the gate: three phases and two reboots per phase.
    ("usb",         "usb_test.py",             "xHCI, and a HID keyboard and mouse", True,  None,                   False),

    # REBOOTS ITS GUEST TWICE, ON PURPOSE -- that is the property under
    # test. Here rather than in the gate for the same reason usb_test is:
    # it needs `vm.py --reboot`, costs a couple of minutes, and a suite
    # that reboots its own VMs makes every other failure harder to read.
    ("netheal",     "netheal_test.py",         "reboot-once-if-no-network, and its loop guard", True, None,           False),

    # REBOOTS ITS GUEST FIVE TIMES, for the same reason: retention across
    # boots cannot be checked inside one.
    ("logrotate",   "logrotate_test.py",       "one log file per boot, and log -p N",  True,  None,                 False),

    # PANICS AND REBOOTS ITS GUEST, on purpose: a warm reset is the only
    # thing that carries the RAM store across, so it cannot be a KTEST.
    ("panic_store", "panic_store_test.py",     "a kernel panic survives the warm reset, into the dead boot's log", True, None, False),

    # REBOOTS ITS GUEST FOUR TIMES. The property is what survives a
    # reboot, so there is no way to check it inside one.
    ("shutdownsync","shutdown_sync_test.py",   "a write just before reboot survives it", True, None,                False),

    # --- display ------------------------------------------------------
    ("virtio_gpu",  "virtio_gpu_test.py",      "the virtio GPU driver",              True,  None,                   False),
    ("hires",       "hires_test.py",           "a desktop above 1280x720",           True,
     ("kcmdline", "needs an ISO built with KCMDLINE=\"video=1920x1080\""),                   False),
    ("taskbar",     "taskbar_test.py",         "taskbar overflow and grouping",      True,  None,                   True),
    # OUT OF gui_regress.py DELIBERATELY: it compares what the KERNEL
    # reports for a glyph against what a CLIENT draws and requires a
    # match, which stopped being true when the kernel's font path was
    # deleted -- so several of its checks are red BY DESIGN and a gate
    # that always cries wolf gets ignored. docs/bugs.md carries the owed
    # rewrite (ask fontd what the session font is, and wait on the
    # beacon rather than on WIN_EV_FONT). It is here, not dropped,
    # because the ~20 checks around them still test real things.
    ("font",        "font_test.py",            "runtime TTF faces and live switching (PARTLY RED BY DESIGN)", True, None, True),

    # ATTACHES to a running vm.py guest (its usage says to start one
    # first), so wants_vm -- without it, it dies on a missing
    # .vm.serial, which is the sweep's fault and not the tool's.
    ("cursor_ibeam", "cursor_ibeam_test.py",   "named pointer shapes and the clamp", True,  None,                  True),

    # --- power --------------------------------------------------------
    # Ends three guests by stopping the machine, and boots one on q35 --
    # the only chipset here with an ACPI reset register.
    ("poweroff",    "poweroff_test.py",        "ACPI poweroff and reset, from the tables", True, None,             False),

    # --- init ---------------------------------------------------------
    ("init",        "init_test.py",            "init and service supervision",       True,  None,                   False),

    # --- memory -------------------------------------------------------
    # Boots its own 8 GiB guest through ktest_run.py; the check it
    # exists for SKIPS on the 256 MiB boot every other runner uses.
    ("highmem",     "highmem_test.py",         "the whole suite on an 8 GiB guest",  True,  None,                  False),
    ("highmemuse",  "highmem_consume.py",      "ring 3 consuming past 4 GiB",        True,  None,                  False),

    # --- the kernel's own output --------------------------------------
    # A COM1 consumer that stops reading must not stop the machine. Its
    # fixture is a reader that goes deaf, so it needs its own boot and
    # its own serial socket.
    ("backpressure", "serial_backpressure_test.py", "a stalled COM1 must not hang the guest", True, None,           True),
    # LAST, and see DIRTIES_IMAGE below.
    ("console_bleed", "console_bleed_test.py",  "the console must not paint over the desktop", True, None,          True),

    # --- on demand for their own reasons ------------------------------
    ("doom",        "doom_test.py",            "DOOM runs, draws and takes input",   True,
     ("iwad", "no IWAD fetched -- see tools/fetch_wad.py"),                                   True),
    # Boots its OWN guests (twice, with an AC97 attached), so wants_vm is
    # False -- handing it one would leave it attached to a machine with
    # no sound hardware, which is the "green line that means nothing"
    # this file's header warns about.
    ("doom_sound",  "doom_sound_test.py",      "DOOM's effects and OPL music",       False,
     ("iwad", "no IWAD fetched -- see tools/fetch_wad.py"),                                   False),
]

# TOOLS THAT LEAVE disk.img CHANGED, run LAST for that reason.
#
# `console_bleed` deletes /etc/services.d/toywm and cannot put it back
# -- there is nothing in the guest to copy it from -- so the image has
# no autostarted desktop until the next `make iso`. Any tool running
# after it boots into a machine it did not configure, which is this
# file's own "green line that means nothing" hazard pointed at itself.
# The sweep says so at the end rather than running `make iso` on
# somebody's behalf: re-seeding the image is not a thing a test runner
# should do unasked.
DIRTIES_IMAGE = {"console_bleed"}

# Deliberately NOT here, each for a stated reason:
#   boot_rate.py   -- reboots a BARE-METAL machine N times; it needs
#                     hardware this repo cannot assume and leaves the
#                     machine rebooted, which no sweep should do.
#   qemu_matrix.py -- needs Docker, and pulls images.
#   kvm_soak.py    -- needs KVM and takes many minutes by design.
#   flake_hunt.py  -- runs something else N times; it has no verdict of
#                     its own to report.
#   damage_*.py    -- analysis passes over a running desktop, not
#                     pass/fail tools.
#   pixel_probe.py -- a library with a CLI, not a test.
#   mkpart_test.py -- despite the name, a WRITER: it takes a disk image
#                     argument and patches a table onto it. Running it
#                     bare is an argparse error, not a result. Its
#                     encoders are covered by the `partition` KTEST
#                     suite and by partition_test.py.


def precondition_met(kind):
    """Can this tool run here at all? Returns (ok, why_not)."""
    if kind is None:
        return True, ""
    key, why = kind
    if key == "iwad":
        for root, _dirs, files in os.walk(os.path.join(REPO, "seed")):
            if any(f.lower().endswith(".wad") for f in files):
                return True, ""
        return False, why
    if key == "alsa_loop":
        # The CABLE itself is not checked here -- the tool probes it and
        # skips with its own message, because proving the loop means
        # playing a tone through it and that is the tool's job, not a
        # precondition's. What this gates is the cheap half: the
        # userspace and the two cards existing at all.
        if not all(shutil.which(t) for t in ("aplay", "arecord", "amixer")):
            return False, why
        try:
            cards = open("/proc/asound/cards").read()
        except OSError:
            return False, why
        return ("[G6" in cards and "[Generic" in cards), why
    if key == "host_audio":
        # A HOST-side check rather than a guest one: it compiles the
        # decoder with the host gcc and compares against ffmpeg, so what
        # it needs is tools, not a VM. Missing them is a skip -- the gate
        # must not start requiring lame and ffmpeg on every checkout,
        # the same rule that keeps Docker out of preflight.
        return all(shutil.which(t) for t in ("gcc", "lame", "ffmpeg")), why
    if key == "host_pillow":
        # Host gcc plus Pillow, which is the ORACLE rather than a
        # convenience: both image harnesses judge our codecs against it.
        # Missing either is a skip for the same reason lame and ffmpeg
        # are -- a check that cannot pass on a clean checkout is one
        # people learn to skim past.
        if not shutil.which("gcc"):
            return False, why
        try:
            import PIL  # noqa: F401
        except ImportError:
            return False, why
        return True, why
    if key == "host_cc":
        # Host gcc only. A skip rather than a failure on a checkout
        # without it, same rule as host_audio above.
        return shutil.which("gcc") is not None, why
    if key == "openssl":
        # The test certificates are made with `openssl req -x509`.
        # Missing it is a skip for the same reason lame and ffmpeg are:
        # a check that cannot pass on a clean checkout is one people
        # learn to ignore.
        return shutil.which("openssl") is not None, why
    if key == "live_iso":
        # A separate ISO that `make iso` does not build. Missing it is a
        # precondition, not a failure -- reporting it as red would train
        # the reader to skim past a red line.
        return os.path.exists(os.path.join(REPO, "toy-os-live.iso")), why
    if key == "kcmdline":
        # The BUILT config (iso/boot/grub/grub.cfg), and only its
        # `multiboot2` line. The template at the repo root is the wrong
        # file twice over: it still holds the @KCMDLINE@ placeholder,
        # and it documents `video=` in four comment lines -- so a
        # substring search over it returns TRUE on every checkout and
        # runs hires_test against a default-mode ISO, where CLAUDE.md
        # says every check in it passes VACUOUSLY. A precondition that
        # is wrong in the permissive direction manufactures coverage
        # that does not exist, which is worse than reporting a failure.
        # Reading the ISO's copy is still right even though an ordinary
        # boot reads disk.img's: both are generated from the repo-root
        # template with the same KCMDLINE in one `make iso`, so they
        # cannot disagree about what was baked in.
        cfg = os.path.join(REPO, "iso", "boot", "grub", "grub.cfg")
        if not os.path.exists(cfg):
            return False, "no built ISO tree -- run `make iso` first"
        for line in open(cfg).read().splitlines():
            if line.strip().startswith("multiboot2") and "video=" in line:
                return True, ""
        return False, why
    return True, ""


VERDICT = re.compile(r"^\s*\w[\w /-]*:\s*(PASS|FAIL|FAILED|all checks passed|"
                     r"\d+/\d+ checks?|SKIP)", re.M)


def summarise(out, rc):
    """One line from the tool's own output, preferring its verdict."""
    lines = [ln.strip() for ln in out.splitlines() if ln.strip()]
    for ln in reversed(lines):
        if re.search(r"\b(PASS|FAIL|FAILED|SKIP|passed|checks)\b", ln):
            return ln[:96]
    return (lines[-1][:96] if lines else f"exit {rc}")


def vm(*args):
    subprocess.run([sys.executable, os.path.join(HERE, "vm.py"), *args],
                   cwd=REPO, capture_output=True, text=True)


def run_one(entry, timeout, logdir):
    name, script, _what, _serial, _needs, wants_vm = entry
    # A row may carry arguments after the script name (`audio_test.py
    # --card hda`): one tool, run twice against two cards.
    script, *extra = script.split()
    path = os.path.join(HERE, script)

    # ALWAYS stop first, whatever this tool needs. A guest left running
    # by the previous tool is how an attaching tool passes against the
    # wrong machine, and how a launching one hits a busy QMP port.
    vm("stop")
    if wants_vm:
        vm("start")

    t0 = time.time()
    try:
        r = subprocess.run([sys.executable, path, *extra], cwd=REPO,
                           capture_output=True, text=True, timeout=timeout)
        out, rc = r.stdout + r.stderr, r.returncode
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode("utf-8", "replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        out += f"\n[timed out after {timeout}s]"
        rc = 124
    dt = time.time() - t0
    vm("stop")
    if logdir:
        os.makedirs(logdir, exist_ok=True)
        with open(os.path.join(logdir, f"{name}.log"), "w") as f:
            f.write(out)
    # A tool that reports its own SKIP is a skip, not a pass -- doom_test
    # exits 0 when there is no IWAD, and counting that as a pass is how a
    # suite reports coverage it does not have.
    skipped = rc == 0 and re.search(r"\bSKIP(PING)?\b", out) is not None
    return name, ("skip" if skipped else "pass" if rc == 0 else "fail"), dt, summarise(out, rc)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--only", help="run tools whose name contains this")
    ap.add_argument("--list", action="store_true", help="print what would run")
    ap.add_argument("--timeout", type=float, default=600.0, help="per tool (default 600s)")
    ap.add_argument("--logs", metavar="DIR", help="write each tool's output here")
    ap.add_argument("-j", "--jobs", type=int, default=1,
                    help="parallel jobs for the tools that allow it. DEFAULT 1: "
                         "nearly all of these take the shared VM slot or the "
                         "physical console, so this is not gui_regress and "
                         "raising it is usually wrong.")
    args = ap.parse_args()

    chosen = [t for t in TOOLS if not args.only or args.only in t[0]]
    if not chosen:
        sys.exit(f"ondemand_sweep: nothing matches --only {args.only!r}")

    if args.list:
        for name, script, what, serial, needs, _vm in chosen:
            ok, why = precondition_met(needs)
            flag = "" if ok else f"   [would skip: {why}]"
            print(f"  {name:<13} {script:<26} {what}{flag}")
        return 0

    results = []
    runnable = []
    for entry in chosen:
        ok, why = precondition_met(entry[4])
        if not ok:
            results.append((entry[0], "skip", 0.0, why))
        else:
            runnable.append(entry)

    print(f"ondemand_sweep: {len(runnable)} tool(s) to run, "
          f"{len(results)} skipped up front\n")

    # A tool that changes disk.img runs after everything else, or the
    # tools behind it boot a machine it reconfigured -- see
    # DIRTIES_IMAGE.
    runnable.sort(key=lambda e: e[0] in DIRTIES_IMAGE)
    serial = [e for e in runnable if e[3]]
    parallel = [e for e in runnable if not e[3]]

    if parallel and args.jobs > 1:
        with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            for r in ex.map(lambda e: run_one(e, args.timeout, args.logs), parallel):
                results.append(r)
                print(f"  {r[1].upper():<5} {r[0]:<13} {r[2]:5.0f}s  {r[3]}")
    else:
        serial = parallel + serial

    for entry in serial:
        r = run_one(entry, args.timeout, args.logs)
        results.append(r)
        print(f"  {r[1].upper():<5} {r[0]:<13} {r[2]:5.0f}s  {r[3]}", flush=True)

    npass = sum(1 for r in results if r[1] == "pass")
    nfail = sum(1 for r in results if r[1] == "fail")
    nskip = sum(1 for r in results if r[1] == "skip")
    print(f"\nondemand_sweep: {npass} pass, {nfail} fail, {nskip} skip")
    dirtied = sorted(r[0] for r in results if r[0] in DIRTIES_IMAGE and r[1] != "skip")
    if dirtied:
        print(f"\ndisk.img was changed by: {', '.join(dirtied)}"
              "\n  Run `make iso` before anything else boots it.")
    if nfail:
        print("\nFAILED:")
        for r in results:
            if r[1] == "fail":
                print(f"  {r[0]:<13} {r[3]}")
        print("\nBefore assuming a regression: most of these are ON DEMAND and may")
        print("have rotted rather than broken. `python3 tools/predates.py` will say")
        print("which, and a dirty disk.img explains more failures than it should --")
        print("`make clean-disk && make iso` first.")
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
