#!/usr/bin/env python3
"""Boot a guest with COM1 as a SOCKET, and drive it as text in / text out.

WHY THIS EXISTS
---------------
Two harnesses need the same thing and had written half of it each.
`ktest_run.py` drives the in-kernel suite over the serial debug console;
`faulttest_run.py` needs the same channel because the thing it used
instead -- typing at the physical shell over QMP -- stopped working the
day the desktop began starting at boot (a compositor holds the keyboard,
see kernel/proc/win_role.c's keyboard_suspend_blocking() call, so every
ring-0 blocking reader is parked and the keystrokes go to the desktop).

A serial console is the standard answer to exactly this problem, and for
the same reason Linux developers use `console=ttyS0`: it is a SEPARATE
input path that does not care which VT, session or compositor owns the
screen. `-serial file:` is write-only from the guest's side, so a tool
that has to TYPE needs the socket form.

WHAT IT ADDS OVER A RAW SOCKET
------------------------------
Diagnostics, which is the actual reason this is a module and not four
lines inlined twice. A read that fails here used to surface as either a
bare `ConnectionResetError` traceback or a verdict-free "never came up",
and BOTH measure nothing while looking like different problems. So:

  - `ConnectionResetError`/`OSError` on a read is CAUGHT and recorded as
    a dead connection, not raised. A harness that throws where it meant
    to fail cannot report a rate.
  - `diagnostics()` answers the question the failure poses -- how long it
    waited, how many bytes ever arrived, whether QEMU is still alive and
    with what exit code, and the tail of QEMU's OWN log. "0 bytes, QEMU
    exited 1, could not bind" and "40,000 bytes, QEMU healthy, guest
    never printed the banner" are different bugs that used to print the
    same sentence.

THE `wait=on` LAUNCH, which is load-bearing: QEMU blocks until this
process connects before it starts the guest, so the transcript begins at
the very first byte the kernel prints. With `nowait` the guest boots
immediately and everything before the connection lands is discarded --
including the banner a caller then waits for, which hung every run until
the timeout. That is also why this cannot use qmp_test.py's
`launch_qemu_cmd()`: that one passes `-daemonize`, and a QEMU that is
blocking on accept() never daemonizes, so the launch itself would hang.
"""

import socket
import subprocess
import time

import install_grub

# COM1 exposed as a TCP socket. Deliberately NOT the QMP port (4445) and
# not in the range tools/gui_regress.py leases (4445-4448).
DEFAULT_PORT = 4555
PROMPT_MARK = "dbg> "   # the debug console listening again -- see send()


def launch_cmd(iso, disk, port, virtio_disk=None, memory=256, ahci_disk=None):
    """The QEMU command line, as a list. No display, no QMP, no input
    head -- this channel needs none of them, which is most of why it is
    cheap enough to be the default for anything not about pixels."""
    cmd = [
        "qemu-system-x86_64",
        # WHICH MEDIUM: the disk carries GRUB and the kernel now, so it
        # boots itself (`order=c`) and the ISO is only for an image that
        # cannot -- see tools/install_grub.py. Never NO order: SeaBIOS
        # boots any disk with 0x55AA at LBA 0, which a partition table
        # provides, and then hangs inside the table with NO serial
        # output at all -- indistinguishable from a kernel that died
        # before its first print.
        *install_grub.qemu_boot_args(install_grub.boot_medium(disk), iso),
        # discard=unmap -- see kernel/drivers/ata.c's ata_trim().
        "-drive", f"file={disk},format=raw,if=ide,discard=unmap",
        "-m", str(memory),
        "-display", "none",
        # `server` WITHOUT `nowait` on purpose -- see the module
        # docstring. QEMU waits for our connection before booting.
        "-serial", f"tcp:127.0.0.1:{port},server",
        # A virtio device on the bus, purely so the PCI capability walk
        # and the 64-bit BAR decode (kernel/include/kernel/pci_internal.h)
        # are COVERED rather than only when a virtio disk is deliberately
        # attached.
        #
        # Measured, which is why it is here: QEMU's default pc-i440fx
        # topology publishes NO PCI capabilities at all -- not on the
        # host bridge, the PIIX3 IDE/ISA, the PIIX4 ACPI bridge, stdvga
        # or the e1000 -- and no 64-bit BAR either. So the walk that
        # every virtio device depends on ran zero times and its KTEST
        # reported a skip. virtio-rng-pci publishes five vendor-specific
        # capabilities and puts its registers in a 64-bit BAR, which is
        # exactly the two uncovered branches.
        #
        # rng rather than blk deliberately: it is not the device under
        # test, so it also exercises virtio_blk_init() DECLINING a virtio
        # device of the wrong type.
        "-device", "virtio-rng-pci",
        # A SECOND rng, and it is load-bearing rather than belt-and-
        # braces. kernel/drivers/virtio/virtio_rng.c now claims index 0
        # at boot and holds it for the life of the kernel, so the
        # transport KTESTs -- which claim a device and reset it when
        # they are done -- would silently kill the live entropy source
        # if they used the same one. They take the LAST rng on the bus;
        # this is it. Drop it and those tests skip themselves (with a
        # message saying why) rather than fighting the driver.
        "-device", "virtio-rng-pci",
        "-no-reboot",
        "-no-shutdown",
    ]
    # An optional SECOND disk on virtio-blk, alongside the IDE one.
    #
    # BOTH, deliberately. The [ata]/[atac] KTESTs need a real IDE drive
    # and skip without one, so replacing the disk rather than adding to
    # it would trade a flake for a coverage hole. With both attached the
    # `virtioblk` boot flag decides which one carries the FILESYSTEM
    # (kernel/fs/vfs.c), so the ata tests keep their drive and every
    # fs/setting test runs over virtio.
    #
    # A separate image file because QEMU takes a write lock -- the same
    # file cannot be attached twice.
    if virtio_disk:
        at = cmd.index("-no-reboot")
        cmd[at:at] = [
            "-drive", f"file={virtio_disk},format=raw,if=none,id=vblk",
            "-device", "virtio-blk-pci,drive=vblk,disable-legacy=on",
        ]
    # ...or on an ICH9 AHCI controller, for the same reason: the IDE drive
    # stays for the [ata] KTESTs. A COPY of disk.img shares its PARTUUIDs,
    # so `bootpart=` names both and AHCI's precedence over IDE makes it
    # the root -- ktest_run.py checks that it did.
    if ahci_disk:
        at = cmd.index("-no-reboot")
        cmd[at:at] = [
            "-device", "ich9-ahci,id=ahci",
            "-drive", f"file={ahci_disk},format=raw,if=none,id=sata0,discard=unmap",
            "-device", "ide-hd,drive=sata0,bus=ahci.0",
        ]
    return cmd


class SerialGuest:
    """One booted guest and the socket onto its debug console.

    Usage is start() -> connect() -> wait_for()/send() -> stop(), and
    every one of those returns a bool rather than raising, so a caller
    reports a failure instead of dying with a traceback in the middle of
    a batch.
    """

    def __init__(self, iso, disk, port=DEFAULT_PORT, qemu_log="serial_qemu.log",
                 virtio_disk=None, memory=256, ahci_disk=None):
        self.ahci_disk = ahci_disk
        self.iso = iso
        self.disk = disk
        self.port = port
        self.qemu_log = qemu_log
        self.virtio_disk = virtio_disk
        self.memory = memory

        self.transcript = ""
        self._sent_at = None   # transcript length when the last command went out
        self.proc = None
        self.sock = None
        self.started_at = None
        # Why the connection ended, when it ended abnormally. None while
        # healthy -- distinct from "" so a caller can tell "never set"
        # from "set to nothing".
        self.connection_error = None

    # -- lifecycle ---------------------------------------------------

    def start(self):
        cmd = launch_cmd(self.iso, self.disk, self.port,
                         virtio_disk=self.virtio_disk, memory=self.memory,
                         ahci_disk=self.ahci_disk)
        self.started_at = time.time()
        self._log = open(self.qemu_log, "wb")
        self.proc = subprocess.Popen(cmd, stdout=self._log, stderr=subprocess.STDOUT)

    def connect(self, deadline):
        """Connect to the waiting QEMU. False if it never accepted."""
        while time.time() < deadline and self.sock is None:
            # If QEMU has already exited there is nothing to wait for --
            # the common cause is a port it could not bind, and spinning
            # here until the timeout reports that as a guest problem.
            if self.proc.poll() is not None:
                return False
            try:
                self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=1.0)
            except OSError:
                time.sleep(0.2)
        if self.sock is None:
            return False
        self.sock.settimeout(0.5)
        return True

    def stop(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None
        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        if getattr(self, "_log", None):
            self._log.close()
            self._log = None

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop()
        return False

    # -- the wire ----------------------------------------------------

    def pump(self):
        """Read whatever is waiting, into the transcript.

        A dead connection is RECORDED, not raised: this is called from
        inside poll loops, and a ConnectionResetError escaping one of
        them is how this failure used to present as a traceback with no
        measurement attached.
        """
        if self.sock is None:
            return
        try:
            chunk = self.sock.recv(65536)
        except socket.timeout:
            return
        except OSError as e:
            self.connection_error = f"{type(e).__name__}: {e}"
            self.sock = None
            return
        if not chunk:
            # A clean EOF is still the console going away mid-run.
            self.connection_error = "the guest closed the serial connection (EOF)"
            self.sock = None
            return
        self.transcript += chunk.decode("utf-8", errors="replace")

    def wait_for(self, needle, deadline):
        """Pump until `needle` appears in the transcript, or time runs
        out, or the connection dies. Returns True only for the first."""
        while needle not in self.transcript:
            if time.time() >= deadline or self.sock is None:
                return needle in self.transcript
            self.pump()
        return True

    def wait_quiet(self, quiet_for, deadline):
        """Read until the guest has said NOTHING for `quiet_for` seconds.

        The boundary a caller actually wants before sending a command,
        and the reason a banner is not one: the debug console announces
        itself early and the kernel keeps printing for seconds afterwards
        (init starting, the desktop claiming the framebuffer, the cursor
        theme loading). A window anchored on the banner therefore still
        contains boot output, so a test asserting on a string the boot
        happens to print PASSES VACUOUSLY -- which is exactly what a
        control on this caught. Quiescence is the boundary that holds
        without needing to know which line comes last.
        """
        last_len, last_change = len(self.transcript), time.time()
        while time.time() < deadline:
            self.pump()
            if self.sock is None:
                return False
            if len(self.transcript) != last_len:
                last_len, last_change = len(self.transcript), time.time()
            elif time.time() - last_change >= quiet_for:
                return True
        return False

    def send(self, line, prompt_wait=60.0):
        """Send one command line. False if the wire is already gone.

        WAITS FOR THE PREVIOUS COMMAND'S PROMPT FIRST (bounded). The debug
        console is a terminal, and on a terminal a line typed while a
        command runs is that COMMAND's input -- `ktest` sent while
        `sh fsck repair` was still running arrived as `nt`, the rest read
        by fsck. What a caller waits for before sending (`fsck: clean.`)
        prints before the command has finished, so the prompt is the only
        boundary that means "the console is listening again". A caller
        that deliberately types AT a running command passes 0.
        """
        if self.sock is None:
            return False
        if self._sent_at is not None and prompt_wait > 0:
            deadline = time.time() + prompt_wait
            while (PROMPT_MARK not in self.transcript[self._sent_at:]
                   and self.sock is not None and time.time() < deadline):
                self.pump()
            if self.sock is None:
                return False
        try:
            self._sent_at = len(self.transcript)
            self.sock.sendall((line + "\n").encode())
            return True
        except OSError as e:
            self.connection_error = f"{type(e).__name__}: {e}"
            self.sock = None
            return False

    def drain(self, seconds):
        """Keep reading for a fixed stretch. For output still in flight
        after the substring a caller was waiting for -- that substring
        arrives mid-line, and the rest of the line is still on the wire."""
        until = time.time() + seconds
        while time.time() < until:
            self.pump()

    # -- diagnostics -------------------------------------------------

    def qemu_log_tail(self, lines=4):
        try:
            with open(self.qemu_log, "r", errors="replace") as f:
                tail = [ln.rstrip() for ln in f.read().splitlines() if ln.strip()]
        except OSError:
            return []
        return tail[-lines:]

    def diagnostics(self):
        """Why a wait failed, as facts rather than a verdict.

        Every line here answers a question that the old bare failure
        message left open, and they distinguish causes that used to look
        identical: 0 bytes with a dead QEMU is a launch problem, plenty
        of bytes with a live QEMU is the guest not reaching the marker,
        and a connection_error is the socket dying mid-reply.
        """
        out = []
        waited = time.time() - self.started_at if self.started_at else 0.0
        out.append(f"waited {waited:.1f}s, {len(self.transcript)} bytes received on the wire")

        if self.proc is None:
            out.append("QEMU was never launched")
        else:
            code = self.proc.poll()
            out.append("QEMU is still running"
                       if code is None else f"QEMU exited with code {code}")

        if self.connection_error:
            out.append(f"serial connection ended: {self.connection_error}")

        for line in self.qemu_log_tail():
            out.append(f"qemu: {line}")

        # The last thing the guest actually said is the other half of
        # "how far did it get" -- a timeout at the GRUB menu and one
        # inside the test suite are the same sentence without it.
        last = [ln for ln in self.transcript.splitlines() if ln.strip()]
        if last:
            out.append(f"last guest output: {last[-1].strip()[:100]!r}")
        return out
