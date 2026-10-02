"""Boot a guest whose serial console is a TCP server, and drive it.

The plumbing four boot tests had written four times: virtio_boot_test,
ahci_test, partition_test and poweroff_test each launch QEMU with
`-serial tcp:127.0.0.1:<port>,server`, connect, wait for the debug
console, send commands and read a transcript. What differs between
them -- the disk controller, the machine type, whether QEMU may exit --
stays in each tool's own `launch()`; this is only the part that did not.

`server` WITHOUT `nowait` on the QEMU side is what makes it work: QEMU
waits for this connection, so nothing printed before it is lost (see
ktest_run.py). tools/serial_console.py is the unix-socket equivalent
behind ktest_run and faulttest_run.
"""
import socket
import time

READY = "debug console ready"
PROMPT = "dbg>"


def connect(port, timeout):
    """A connection to the guest's serial port, or None."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=1.0)
        except OSError:
            time.sleep(0.2)
    return None


def read_until(sock, needle, timeout, transcript):
    """Append what arrives to `transcript` until `needle` has (True), or
    the time is up or the guest hangs up (False). A None needle just
    reads for the slice."""
    deadline = time.time() + timeout
    sock.settimeout(0.5)
    while time.time() < deadline:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        except OSError:
            break
        if not chunk:
            break
        transcript.append(chunk.decode("utf-8", "replace"))
        if needle and needle in "".join(transcript):
            return True
    return False


def stop(qemu):
    try:
        if qemu.poll() is None:
            qemu.kill()
        qemu.wait(timeout=5)
    except Exception:          # noqa: BLE001 -- teardown must not raise
        pass


def run_session(qemu, port, commands, timeout, settle=3.0):
    """Wait for the debug console, run `commands`, return (transcript, error).

    A command is a string, or (string, needle): with a needle, wait on
    that ARTIFACT -- a fixed settle silently measures a partial write
    (40 MiB took ~17 s where 1.5 s had been allowed) -- otherwise read
    for `settle` seconds. QEMU is killed on the way out."""
    transcript = []
    try:
        sock = connect(port, timeout)
        if sock is None:
            return None, "could not connect to the guest's serial console"
        if not read_until(sock, READY, timeout, transcript):
            return None, "the debug console never came up"
        # READY arrives mid-line; a command typed before the PROMPT is lost
        # and the rest of the banner reads as its answer.
        read_until(sock, PROMPT, timeout, transcript)
        for cmd in commands:
            cmd, needle = cmd if isinstance(cmd, tuple) else (cmd, None)
            sock.sendall((cmd + "\n").encode())
            read_until(sock, needle, timeout if needle else settle, transcript)
        return "".join(transcript), None
    finally:
        stop(qemu)
