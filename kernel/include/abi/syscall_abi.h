#ifndef SYSCALL_ABI_H
#define SYSCALL_ABI_H

#include <stdint.h>
#include "errno.h" // error numbers -- what a failed syscall returns
#include "timer.h" // struct rtc_time -- reused by SYS_GETTIME and struct sys_dirent's `modified` below

// Syscall numbers (passed in RAX) and argument conventions for `int
// 0x80`, shared between the kernel's dispatcher (kernel/proc/syscall.c)
// and userland test programs (userland/*.c) so both sides agree on the
// numbers without duplicating them -- kernel/include is on both build's
// include path (see the Makefile), so both just include this file.

// HOW TO READ THE PER-CALL RETURN NOTES BELOW, since error codes landed.
//
// Many of them say "returns -1 on failure". That is what a RING-3 CALLER
// sees, and it is still true -- but it is true one layer up: the raw
// syscall returns the NEGATED error number (abi/errno.h), and libsys's
// wrapper turns that into -1 while stashing the code for sys_errno()
// (userland/rt/sys.h). So `open()` of a missing file puts -2 in RAX and
// hands a program -1 with ENOENT behind it.
//
// Three things follow, and the second is the one that bites:
//
//   - Code using libsys is unaffected. `if (fd < 0)` means what it
//     always did.
//   - Code poking the RAW interface -- the /tests diagnostics, which
//     exist to do exactly that -- sees the code, not -1. A raw test
//     asserting `== -1` is asserting EPERM, since EPERM is 1.
//   - The calls that once returned 0 ON FAILURE (SYS_UNLINK, SYS_KILL,
//     SYS_GETTIME, SYS_PROC_INFO, SYS_WIN_CREATE, SYS_GUI_INIT) were
//     FLIPPED in one commit with every caller: 0 is success and a
//     negative code the reason, like everything else. The stragglers
//     whose SUCCESS was a magic 1 (SYS_SET_COLOR, SYS_PCI_INFO,
//     SYS_CPU_INFO, SYS_WIN_PRESENT, SYS_PIPE) return 0 now too.
//     See docs/errno-design.md.
//
// Stated once here rather than edited into two dozen comments that would
// then have to be kept true individually.

#define SYS_EXIT  1 // RDI = exit code
#define SYS_WRITE 2 // RDI = fd, RSI = buffer pointer, RDX = length;
                    // returns bytes written (RAX), or -1 on a bad fd or
                    // bad pointer. Capped at SYS_WRITE_MAX per call.
                    //
                    // ABI NOTE: this used to be a 2-arg call (RDI =
                    // buffer, RSI = length, always went to the console)
                    // -- changed to take an fd once SYS_OPEN/SYS_READ
                    // below existed and "which stream" stopped being a
                    // foregone conclusion. Every existing caller
                    // (write_test.c, write_bad_test.c, counter_a.c,
                    // counter_b.c, echo.c) was updated to pass 1
                    // (stdout) as RDI. fd 1 and 2 (stdout/stderr) both
                    // go to the console (vga_putc), same behavior as
                    // before; fd >= 3 must come from SYS_OPEN.
                    //
                    // SECOND ABI NOTE, and the one that matters to an
                    // existing caller: a write to a FILE fd used to
                    // append UNCONDITIONALLY, ignoring the position the
                    // read path maintained. Since SYS_LSEEK it writes
                    // AT the fd's position and advances it, which is
                    // what write(2) means and what any seek at all
                    // requires -- a position a write ignores is not a
                    // position. A caller that wants the old behaviour
                    // asks for SYS_O_APPEND. Nothing that opens with
                    // SYS_O_TRUNC (which is almost everything here)
                    // changes behaviour at all: position 0 of an
                    // emptied file IS its end.

// The most one read or write syscall carries. An artefact of the bounce
// buffer the syscall copies through, not a promise to the caller --
// libsys loops to complete a bigger buffer (see sys_write()).
//
// **IT IS A THROUGHPUT CONSTANT, AND IT WAS 1024.** Each fs_write*()
// call is one complete TFS3 transaction, and each transaction commits
// with TWO barriers -- real cache flushes reaching the device. So the
// cap sets how many flushes a megabyte of ring-3 writing costs: at
// 1 KiB it was 2048 per MiB, against the 2 per MiB the ring-0 `stress`
// command pays by handing fs_write_range() a whole megabyte. That is
// the entire reason a ring-3 write measured ~30x slower than the same
// bytes written from the kernel shell.
//
// 64 KiB rather than more because the buffer wants CONTIGUOUS frames
// (sixteen of them), and because the remaining gap is not this constant
// -- it is that toy-os barriers every write while Linux batches its
// journal commits onto a ~5 second timer and lets the page cache absorb
// the rest. See docs/decisions/storage.md.
#define SYS_WRITE_MAX 65536

// The value a BLOCKING syscall returns when the process was woken but
// must call again -- the spurious-wakeup contract every blocking call
// here shares (see SYS_WAIT_EVENT for why the kernel cannot simply
// hand over the result at wake time: the wake runs in an interrupt,
// under an address space where the caller's buffer is not addressable).
//
// It is NOT 0, for a concrete reason: 0 is a legitimate result for
// SYS_READ (end of file). Using it as the retry sentinel made a reader
// treat "woken, ask again" as "there will never be more data" -- a pipe
// read that returned empty the instant its writer produced something. A
// sentinel has to be a value the call can never otherwise return.
//
// It was -2 until error codes existed, and -2 is now -ENOENT. So it
// sits one past the top of the error range instead (abi/errno.h), which
// keeps the same property against a set of values that did not exist
// when it was chosen.
//
// AND IT IS NOT SIMPLY -EAGAIN, which is the obvious-looking move. The
// two mean different things here: EAGAIN is an ERROR a caller reports,
// while SYS_RETRY means the call did not fail at all -- the process was
// woken and must ask again, which libsys does in a loop the caller
// never sees (sys_read/sys_write). Merging them would make every
// blocking call's spurious wakeup look like a failure to the layer
// above, which is the same mistake as using 0.
#define SYS_RETRY (-4095)

// **3, 4 AND 5 ARE RETIRED, NOT FREE.** They were SYS_GUI_INIT (mapped
// the WHOLE framebuffer into any caller, with no role gate -- the
// compositor's grant is WIN_REQ_FB_MAP, held by one process),
// SYS_GUI_POLL_KEY and SYS_READ_KEY (non-blocking keyboard reads from
// before fd 0 could block). Deleted 2026-09-09 with their one caller.
// Declared in kernel/proc/syscall_table.c's SYSCALL_RETIRED.

#define SYS_SBRK     6 // RDI = increment in bytes (a plain heap bump,
                        // not "true" sbrk's signed shrink support -- 0
                        // or positive only). Returns (RAX) the previous
                        // break -- i.e. a pointer to `increment` freshly
                        // mapped, zeroed bytes -- or -1 if the calling
                        // process never had its heap set up (see
                        // syscall_reset_mm() in syscall.h) or ran out
                        // of physical memory while mapping new pages.

// **7 AND 8 ARE RETIRED, NOT FREE.** They were SYS_WIN_CREATE and
// SYS_WIN_PRESENT: a single-window-at-a-time path, predating the window
// server, in which the KERNEL composited the window and drew its title
// bar and close button. Deleted 2026-09-08 with the last GUI drawing in
// ring 0. A ring-3 client's path is SYS_WIN_REQUEST. Do not reuse the
// numbers -- an old binary calling one should find nothing, not
// something else. Declared in kernel/proc/syscall_table.c's
// SYSCALL_RETIRED, so the table's KTEST can still fail on an
// ACCIDENTAL gap.

// Real file I/O against the in-memory filesystem (fs.c) -- the piece a
// future libc's fopen()/fread()/fwrite() would sit on top of (see the
// syscall stubs a newlib-style port needs: _open/_read/_write/_close).
// Previously fs.c was reachable only from kernel-space (the shell's
// ls/cat/write/etc commands) -- this is the first time a ring-3 process
// can touch a real, named file at all, not just the console.
//
// Honest limitation: fs_write() (fs.c) works on null-terminated C
// strings via k_strlen(), not explicit-length byte buffers -- there's
// no offset-based partial write in the underlying filesystem. So
// SYS_WRITE to a file fd always APPENDS (never overwrites at an
// arbitrary offset), and a buffer containing an embedded NUL byte will
// truncate early at that byte, same as it would passed to any C string
// function. Fixing that properly means extending fs.c itself to take
// explicit lengths -- not done here; see README's "Ideas for what's
// next". Reads (SYS_READ) don't have this problem: fs_read() already
// returns a pointer + explicit size, so file content read back is
// exact, including any byte value.
//
// fd numbers start at 3, following the same convention libc expects --
// fd 0/1/2 (stdin/stdout/stderr) stay reserved for SYS_WRITE's console
// path above.
#define SYS_O_WRITE 1 // open for writing (default: read-only)
#define SYS_O_CREAT 2 // create the file if it doesn't exist (write only)
#define SYS_O_TRUNC 4 // truncate to empty on open (write only)
#define SYS_O_APPEND 8 // every write goes to the CURRENT end of the file,
                      // whatever the fd's position is, and the position
                      // follows the write. Write-only, and the flag
                      // exists because SYS_LSEEK gave the position a
                      // meaning for writes that it did not have before:
                      // a write used to append UNCONDITIONALLY (see
                      // SYS_WRITE's ABI NOTE), so a caller that wanted
                      // appending got it by saying nothing. Now it has
                      // to ask -- which is what makes the shell's `>`
                      // and `>>` genuinely different operations rather
                      // than the same one with a truncate in front.

#define SYS_READ  9  // RDI = fd (from SYS_OPEN, read mode), RSI =
                      // buffer pointer, RDX = length. Reads from the
                      // fd's current position (tracked per-fd,
                      // advanced by each call) and returns bytes read
                      // (RAX) -- 0 at end of file, or -1 on a bad fd or
                      // bad pointer. Capped at SYS_WRITE_MAX per call,
                      // same as SYS_WRITE.
                      //
                      // fd 0 is STDIN: the physical console keyboard.
                      // BLOCKS when nothing is typed -- the process is
                      // parked and the keyboard IRQ releases it, so an
                      // idle reader costs no CPU. It NEVER returns 0:
                      // a console has no end of file, and reporting one
                      // would tell a shell its input had closed.
                      //
                      // Raw, one byte per key, exactly the code
                      // keyboard.h's KEY_* namespace uses (specials are
                      // 0x91-0xA6) -- no echo, no line editing, no
                      // escape-sequence translation. All three are a
                      // line discipline, which belongs above a real TTY
                      // (docs/roadmap.md), so a reader echoes what it
                      // reads and does its own editing.
                      //
                      // THE FIRST fd-0 READ CLAIMS THE CONSOLE: the
                      // kernel shell stops taking keys until the
                      // claiming process dies. There is one keyboard,
                      // and two readers splitting it at random is worse
                      // than one reader having it.
#define SYS_OPEN  10 // RDI = pointer to a NUL-terminated path (at most
                      // FS_PATH_MAX - 1 bytes -- fs.c's own limit; a
                      // bare name like "notes.txt" is treated as
                      // "/notes.txt", see fs.h), RSI
                      // = SYS_O_* flags (bitwise OR). Returns a small
                      // fd (RAX, >= 3) on success, or -1 (bad pointer,
                      // file not found without SYS_O_CREAT, or the
                      // open-file table -- 8 slots -- is full).
#define SYS_CLOSE 11 // RDI = fd. Frees the fd table slot. Returns 0, or
                      // -1 if the fd wasn't open (or belongs to a
                      // different process -- fds aren't shared across
                      // processes any more than the heap or window
                      // state above are).

#define SYS_UNLINK 12 // RDI = pointer to a NUL-terminated path (same
                       // length limit as SYS_OPEN). Wraps fs_delete()
                       // (fs.c) -- deletes a file, or an empty
                       // directory. Returns 0 (RAX) on success; -EFAULT
                       // for a bad pointer, -ENOENT when it doesn't
                       // exist, -EIO when it exists and was still
                       // refused (a non-empty directory -- fs_delete()
                       // doesn't do recursive delete).

// A directory entry as filled in by SYS_LISTDIR below -- deliberately
// reuses FS_PATH_MAX for `name` even though a single path component is
// always shorter than a full path, just to avoid a second size constant
// (fs.c already guarantees every component fits in FS_PATH_MAX, since
// it's a substring of a path that does).
// NAMED sys_dirent, NOT dirent, and the rename was forced by the C
// library: POSIX's <dirent.h> declares its own `struct dirent` with
// `d_name`, and two structs cannot share a tag in one translation unit.
// The syscall ABI's own structs are `sys_*` anyway (struct sys_stat is
// the precedent), so this is the odd one out being brought into line
// rather than a name being surrendered.
struct sys_dirent {
    char name[64];   // FS_PATH_MAX (fs.h) -- last path component only,
                      // e.g. "notes.txt", not "/docs/notes.txt"
    uint32_t size;    // meaningless (0) for directories, same as fs_list()
    uint32_t is_dir;
    // Added for /bin/ls's `-l` (see userland/ls.c) -- SYS_LISTDIR's
    // kernel-side handler fills this via an extra fs_stat() call per
    // entry (fs.c/fs.h), same `struct rtc_time` SYS_GETTIME already
    // hands to ring-3, reused rather than declaring a syscall-private
    // copy (same precedent as SYS_PCI_INFO reusing struct pci_device).
    // Zeroed (all-0 rtc_time) for the implicit root's own children if
    // fs_stat() ever legitimately fails for an entry -- shouldn't
    // happen for anything fs_list() itself just reported, but the
    // syscall handler doesn't treat that as fatal for the whole call.
    struct rtc_time modified;
};

#define SYS_LISTDIR_MAX 256 // caps how many entries a single SYS_LISTDIR
                            // call can fill. A `max` argument above this
                            // is silently clamped down to it, not
                            // rejected.
                            //
                            // IT WAS 32, and its comment claimed that
                            // "matches FS_MAX_FILES (fs.h), since that's
                            // the most any directory could ever hold" --
                            // which was wrong twice: FS_MAX_FILES is 256,
                            // and TFS3 has no per-directory cap at all.
                            // So `ls` silently listed the first 32
                            // entries of a bigger directory and stopped,
                            // with nothing said. Measured 2026-08-19 by
                            // putting 40 files in one directory.
                            //
                            // 256 matches FS_MAX_FILES for real, which
                            // bounds a TFS2 volume. IT STILL DOES NOT
                            // BOUND TFS3, so this remains a truncation
                            // point rather than a guarantee -- the real
                            // fix is an offset argument so a caller can
                            // page through, which changes this call's
                            // ABI. SYS_LISTDIR_AT is that offset, and
                            // this cap is now a BATCH SIZE rather than a
                            // ceiling: a caller that pages through reads
                            // every entry however many there are.
                            // A caller that does not page can still
                            // DETECT the cut -- a full array means
                            // "there may be more", which is what
                            // /bin/ls said before it learned to page.

// SYS_LISTDIR_AT's request. A struct because this call needs FOUR
// arguments and `int 0x80` carries three -- the same answer SYS_MKPART
// and SYS_SPAWN already give.
struct listdir_request {
    uint64_t path;      // user pointer to a NUL-terminated path
    uint64_t entries;   // user pointer to an array of struct sys_dirent
    uint32_t max;       // capacity, clamped to SYS_LISTDIR_MAX
    uint32_t start;     // how many entries to SKIP first
};

#define SYS_LISTDIR 13 // RDI = pointer to a NUL-terminated directory
                        // path (same length limit as SYS_OPEN), RSI =
                        // pointer to an array of `struct sys_dirent` (out),
                        // RDX = capacity of that array (clamped to
                        // SYS_LISTDIR_MAX). Wraps fs_list() (fs.c).
                        // Returns the number of entries written (RAX,
                        // 0..max), or a NEGATIVE ERRNO. Table order,
                        // not sorted, same as fs_list().
                        //
                        // **0 MEANS AN EMPTY DIRECTORY AND NOTHING
                        // ELSE.** It used to also mean "no such
                        // directory", because fs_list() returns void
                        // and simply does nothing for a path that is
                        // not a listable directory -- so `ls /nope`
                        // printed an empty listing and exited 0. The
                        // handler now probes the path when the count
                        // comes back zero: -ENOENT if nothing is
                        // there, -ENOTDIR if something is but is not a
                        // directory. -EFAULT for a bad path or
                        // output-array pointer, as before (the older
                        // comment here said -1, which this syscall has
                        // not returned since it grew errnos).

#define SYS_GETTIME 14 // RDI = pointer to a `struct rtc_time` (out, see
                        // timer.h). Wraps rtc_read_local() (tz.c) --
                        // the same timezone-adjusted wall-clock time the
                        // shell's `time` command and the taskbar clock
                        // show, not raw UTC hardware time. Returns 0
                        // (RAX) on success, -EFAULT on a bad pointer.

#define SYS_YIELD 15 // No arguments. Cooperatively gives up the rest of
                      // this process's timeslice to the next
                      // scheduler-managed process (scheduler.c), if any
                      // is ready -- otherwise a plain no-op. Only does
                      // anything for a process spawned under the
                      // preemptive scheduler (scheduler_demo_run());
                      // for the older single-process-at-a-time path
                      // (process_run_ring3(), used by every *test
                      // command except the counter_a/counter_b demo)
                      // there's nothing else to yield to, so it's
                      // always a no-op there. Always returns 0 (RAX).

// Sockets. There IS a transport now (kernel/net/) -- these were
// scaffolding over an empty fd kind for a long time, and the ABI they
// settled on then is the one still here, which is what that
// scaffolding was for.
//
// ONE PROTOCOL TODAY: AF_INET + SOCK_DGRAM + IPPROTO_ICMP, i.e. what
// `ping` needs. The kernel owns the ICMP header (type, code, checksum
// and the identifier that demultiplexes replies); a caller sends and
// receives PAYLOAD. That is Linux's ping-socket shape rather than a
// raw socket, and it is not politeness: a raw socket lets a process
// emit any ICMP type it likes, which Linux gates behind CAP_NET_RAW
// and this kernel has no privilege model to gate with.
#define SYS_SOCKET 16 // RDI = domain (AF_INET = 2), RSI = type
                       // (SOCK_DGRAM = 2), RDX = protocol (IPPROTO_ICMP
                       // = 1). Anything else is -EINVAL rather than
                       // silently ignored. On success, allocates a
                       // socket-kind fd (same table, same fd namespace
                       // as SYS_OPEN's file fds) and returns it (RAX);
                       // -EMFILE if the fd table is full, -ENOSPC if
                       // the kernel's socket table is. SYS_READ/
                       // SYS_WRITE reject a socket fd (-EBADF).
#define SYS_SEND   17 // RDI = fd, RSI = buffer, RDX = length. A
                       // datagram socket has no peer until one is
                       // named, and nothing here names one, so this is
                       // -EDESTADDRREQ's situation with no such code
                       // defined: -EINVAL. Use SYS_SENDTO.
#define SYS_RECV   18 // RDI = fd, RSI = buffer, RDX = length. Same:
                       // use SYS_RECVFROM, which also reports WHO
                       // sent it -- which for ICMP is the whole point.
#define SYS_SENDTO 82 // RDI = fd, RSI = a `struct net_msg *`
                       // (abi/net_abi.h): payload, length, and the
                       // destination IPv4 address in HOST byte order.
                       // A struct because the table carries three
                       // arguments and this needs four -- SYS_MKPART's
                       // call. Returns the byte count sent, or -errno.
                       // -EAGAIN means the next hop's MAC is not
                       // cached yet and the ARP request is already on
                       // the wire: a RETRY, not a failure. -ENODEV
                       // means no device has an address.
#define SYS_RECVFROM 83 // RDI = fd, RSI = a `struct net_msg *`: `buf`
                       // and `len` say where to put it, `addr` and
                       // `port` are written with the SENDER's.
                       // BLOCKS until a datagram arrives.
                       // `timeout_ms` is a ceiling (0 waits forever),
                       // and 0 comes back when it expires -- not
                       // -EAGAIN, since a datagram socket has no
                       // end-of-stream a zero could be confused with.
                       // Returns SYS_RETRY when woken, so the CALLER
                       // re-runs it (libsys loops); the deadline lives
                       // on the socket, so a re-run does not restart
                       // the clock. Interruptible: a signal rewinds
                       // the call. SYS_SET_NONBLOCK restores the old
                       // poll-and-return-0 behaviour.
#define SYS_LISTEN 87  // RDI = fd. Makes a BOUND stream socket a
                       // listener. Returns 0, or -EINVAL if it was
                       // never bound -- a port the kernel picked is one
                       // no client could know to connect to. There is
                       // no backlog argument: the depth is the stack's
                       // (kernel/net/tcp.c), because each queued
                       // connection costs a whole connection block.
#define SYS_ACCEPT 88  // RDI = fd (a listener), RSI = a
                       // `struct net_msg *` or 0. BLOCKS until a
                       // client completes its handshake; `timeout_ms`
                       // bounds the wait and 0 waits forever. Returns
                       // a NEW fd for the connection, with the peer's
                       // address and port written into the struct.
                       // -EAGAIN on a non-blocking socket with nobody
                       // waiting.
#define SYS_CONNECT 86 // RDI = fd, RSI = a `struct net_msg *`: `addr` and
                       // `port` are the peer's. BLOCKS until the
                       // handshake completes, `timeout_ms` bounds it
                       // (0 uses a default), and 0 comes back on
                       // success. -ECONNREFUSED when a RST answered the
                       // SYN, -ECONNRESET when nobody answered at all,
                       // -ETIMEDOUT is deliberately NOT distinguished
                       // from the latter. Stream sockets only.
                       //
                       // Once connected, SYS_READ and SYS_WRITE work on
                       // the fd, which is what POSIX guarantees and what
                       // lets code written against descriptors use one.
#define SYS_BIND   85 // RDI = fd, RSI = a `struct net_msg *`: `addr` is
                       // the local address (0 for any), `port` the
                       // local port (0 asks the kernel to pick an
                       // ephemeral one), `dev` an optional device name
                       // to bind to -- Linux's SO_BINDTODEVICE, here
                       // because a DHCP client must broadcast out of a
                       // NAMED card before any card has an address.
                       // Returns the port actually bound, or -errno:
                       // -EBUSY if it is taken, -EINVAL on an ICMP
                       // socket (whose demux key is an identifier the
                       // kernel owns, so there is nothing to bind).
#define SYS_NET_CONFIG 84 // RDI = a `struct net_ifconfig *`: which
                       // device, and the addresses to give it. A zero
                       // field is left alone. Returns 0, or -ENODEV
                       // for a name no device answers to. There is no
                       // privilege check because this kernel has no
                       // privilege model -- see docs/roadmap.md's
                       // multi-user track, which is where one goes.

#define SYS_NET_RENAME 98 // RDI = a `struct net_rename *`: an
                       // interface, and what to call it instead.
                       // Returns 0, -ENODEV for a name no device
                       // answers to, or -EINVAL for a new name that is
                       // empty, too long, already taken, or carries a
                       // character a lease filename or a socket
                       // binding could not survive (space, '=', '/').
                       //
                       // NAMING IS POLICY AND POLICY IS RING 3's. The
                       // kernel gives a card a bootstrap name from its
                       // MAC and nothing else; /bin/netd reads the
                       // rules in /etc/net.conf and calls this. That is
                       // udev renaming what the kernel called eth0, and
                       // the same split this project already made for
                       // NTP, DHCP and DNS.

#define SYS_NET_ARP_PROBE 90 // RDI = a `struct net_arp_probe *`: does
                       // anybody on this device's segment answer for
                       // this address? Returns 1 if a reply is already
                       // cached, 0 if not (a request went out), or
                       // -ENODEV / -EFAULT / -EINVAL.
                       //
                       // NON-BLOCKING, so the answer is only ever "not
                       // yet" and the caller asks again -- which is
                       // what keeps RFC 3927's probe count and spacing
                       // in ring 3 (`/bin/dhcp`). The frame is an ARP
                       // Probe from a device with no address (sender
                       // 0.0.0.0) and an Announcement from one that has
                       // it.

#define SYS_NET_RESOLVED 97 // RDI = a `struct net_resolved *` (abi/
                       // net_abi.h): the name a resolver just looked up
                       // and the address it got. Returns 0, or -EFAULT
                       // / -EINVAL for an empty name or a zero address.
                       //
                       // IT REPORTS, IT DOES NOT ASK. The kernel's only
                       // use for it is naming an address in the
                       // connection log (QUERY_CONNLOG); nothing here
                       // resolves anything, and a program that never
                       // calls it is not disadvantaged.

#define SYS_SETTIME 95 // RDI = seconds since 1970-01-01 00:00:00 **UTC**,
                       // RSI = nanoseconds within that second (0 is
                       // fine). Steps the wall clock there and writes
                       // the RTC, so the correction survives a reboot.
                       // Returns 0, or -EINVAL for a time outside
                       // 1970..9999 or an out-of-range nanosecond.
                       //
                       // **THE NANOSECONDS MATTER.** This took whole
                       // seconds at first and threw away the sub-second
                       // part of every correction, leaving the clock up
                       // to a second late right after a sync that had
                       // just measured a 7 ms round trip. The RTC still
                       // stores whole seconds -- it has no other field
                       // -- so it is written ROUNDED.
                       //
                       // **THE ARGUMENT IS UTC; SYS_GETTIME'S ANSWER IS
                       // LOCAL.** They are not inverses, and that is
                       // deliberate rather than an oversight: the kernel
                       // holds UTC (api/ktime.h) and applies the
                       // configured city's offset only when handing out
                       // broken-down civil time (api/tz.h). A client
                       // that reads SYS_GETTIME, adds a second and
                       // passes it back here moves the clock by the
                       // timezone offset. NTP hands out UTC, which is
                       // the caller this exists for.
                       //
                       // Note also that libc's time() is neither -- it
                       // is a LOCAL-derived epoch, matching the
                       // filesystem's stored timestamps. See
                       // userland/include/time.h.
                       //
                       // A STEP, not a slew: SYS_MONOTONIC_NS is what an
                       // interval is measured with, and this cannot move
                       // it.


// RDI = an open file descriptor. Commits whatever the filesystem is
// holding back for that file's volume and flushes the device under it,
// so the bytes already written through this fd are on the platter when
// it returns. 0, or -EBADF / -EIO.
//
// SCOPED TO THE VOLUME, not to the file, and the name is POSIX's rather
// than a promise this kernel can keep more narrowly. Nothing is held
// per file -- `storage.sync = batched` defers a journal transaction
// that may carry several files' inodes, and a device flush is a
// whole-drive operation regardless. So this is everything needed for
// THIS file, plus whatever shares its transaction: narrower than
// SYS_SYNC (other mounts are untouched), wider than POSIX describes.
//
// `fdatasync()` in tolibc calls this same number. There is no cheaper
// subset here: what a batched write defers IS the inode, so the
// metadata fdatasync is allowed to skip is exactly the thing that has
// to land for the data to be findable.
#define SYS_FSYNC 96

#define SYS_LISTDIR_AT 94 // RDI = pointer to a `struct listdir_request`.
                          // SYS_LISTDIR with an OFFSET: fills the array
                          // from the `start`'th entry of the directory
                          // rather than the first. Returns how many were
                          // filled, or the same negative errnos
                          // SYS_LISTDIR does.
                          //
                          // WHY: SYS_LISTDIR_MAX caps ONE call, and a
                          // directory bigger than it could not be read
                          // at all -- GRUB's module directory is 305
                          // files against a cap of 256, which stopped
                          // the installer copying /boot. Paging is what
                          // Linux does through the directory stream's
                          // own position; toy-os has no directory
                          // handle, so the position is an argument.
                          //
                          // THE ORDER MUST BE STABLE ACROSS CALLS, and
                          // it is only as stable as the backend's own
                          // walk -- a directory being written while it
                          // is paged can repeat or skip an entry. Same
                          // hazard readdir() has, and the same answer:
                          // do not do that.

#define SYS_INSTALL_BOOT 93 // RDI = pointer to a `struct install_boot_request`
                           // (abi/partition_abi.h). Writes a BIOS
                           // bootloader onto a disk: the 512-byte boot
                           // sector at LBA 0, and the core image into
                           // that disk's BIOS boot partition. Returns 0,
                           // or a negative errno: -EINVAL for a boot
                           // sector that is not 512 bytes or a core
                           // image that does not fit, -ENODEV for no
                           // such disk or no BIOS boot partition on it,
                           // -EPERM without INSTALL_BOOT_CONFIRM on a
                           // disk in use, -EFAULT for a bad pointer,
                           // -EIO if a write failed.
                           //
                           // TWO IMAGES IN, NOT A PATH. The kernel does
                           // not know what a bootloader is and should
                           // not learn: it is handed the bytes, applies
                           // the two patches that depend on WHERE they
                           // land (which is the half only the kernel
                           // knows), and writes them. `/bin/install`
                           // reads them out of /boot.
                           //
                           // WHERE core.img GOES IS THE KERNEL'S ANSWER,
                           // not the caller's -- it reads the target's
                           // partition table and finds the BIOS boot
                           // partition. The same reasoning as SYS_MKPART
                           // taking a table rather than a sector: a
                           // caller naming an LBA can name any LBA, and
                           // "which sector is safe" is exactly what the
                           // table answers.

#define SYS_MKFS 92 // RDI = pointer to a `struct mkfs_request`
                    // (abi/mount_abi.h). Writes an empty filesystem of
                    // the named type onto the named PARTITION. Returns
                    // 0, or a negative errno: -EINVAL for an unknown
                    // fstype or a source naming no partition, -EPERM
                    // without MKFS_CONFIRM, -EBUSY if anything is
                    // mounted from that volume, -EIO if the format
                    // failed, -ENODEV with no such device.
                    //
                    // REFUSES A MOUNTED VOLUME rather than unmounting
                    // it. `fsformat` reformats the RUNNING root and
                    // does the unmount-and-reprobe dance for it; this
                    // is the installer's operation, and a target it has
                    // to unmount first is a target it should not have
                    // been given.
                    //
                    // It wipes other backends' signatures first
                    // (mount_wipe_others), because formatting a TFS3
                    // volume as something else once left TFS3's backup
                    // superblocks intact and the next probe mounted the
                    // corpse.

// 91 is RETIRED. It was SYS_WIN_CLIP, the kernel's clipboard; the
// clipboard is a ring-3 service over shared memory now
// (userland/lib/uclip_page.h).
//
// It keeps a ROW in the syscall table that refuses with -ENOSYS, rather
// than becoming a hole. A hole is a NULL handler, and dispatch no-ops
// on those -- which a stale binary cannot tell from a call that
// succeeded and returned 0. `syscall/no row is half-filled in` is the
// KTEST that insists on the difference, and it caught this exact gap.

// The first syscalls added specifically so a real disk-hosted ELF64
// binary (not just a kernel-space shell built-in) can do something
// other than file I/O -- see docs/roadmap.md's real-disk-hosted-ELF-
// binaries entry and userland/lspci.c, the first program to use them.
// Wrap pci_init()'s already-recorded device table (kernel/drivers/
// pci.c) -- there's no live rescan here, same as the `lspci` shell
// command (apps/shell_sys.c) reading the same table kernel-space-side.
// `struct pci_device` itself lives in pci.h, not duplicated here --
// same precedent as SYS_GETTIME reusing timer.h's `struct rtc_time`
// directly rather than declaring a syscall-private copy; userland code
// just `#include`s "pci.h" for the type (Makefile's USERLAND_CFLAGS
// already has `-Ikernel/include`), even though it can't call any of
// that header's functions (those live in kernel/drivers/pci.c, never
// linked into a userland ELF).
#define SYS_PCI_COUNT 19 // No arguments. Returns the number of PCI
                          // devices pci_init() found at boot (RAX) --
                          // wraps pci_device_count() directly.

#define SYS_PCI_INFO  20 // RDI = device index (0 .. SYS_PCI_COUNT's
                          // result - 1), RSI = pointer to a
                          // `struct pci_device` (out, see pci.h).
                          // Returns 0 (RAX) on success, -EINVAL for an
                          // out-of-range index, -EFAULT for an invalid
                          // output pointer. Wraps pci_device_at().

// Added for /bin/ls's `--color=auto`-by-default output (userland/ls.c)
// -- the first syscall letting a ring-3 process affect its own console
// color, mirroring the shell's own `color` command (apps/shell_sys.c's
// cmd_color(), which just calls vga_set_color() directly from kernel
// space). RDI/RSI are raw `enum vga_color` values (vga.h -- see the
// same reuse-the-kernel-struct precedent as SYS_PCI_INFO/SYS_GETTIME;
// userland code #includes "vga.h" for the enum only, same as ls.c does
// for `pci.h`'s struct in lspci.c). Out-of-range values (not 0-15) are
// rejected (-1) rather than clamped or ignored, so a caller passing a
// bad value finds out immediately instead of drawing in some arbitrary
// fallback color.
#define SYS_SET_COLOR 21 // RDI = foreground vga_color, RSI = background
                          // vga_color. Wraps vga_set_color() directly.
                          // Returns 0 (RAX) on success, -EINVAL if either
                          // value is outside 0-15 (VGA_BLACK..VGA_WHITE).

#define SYS_CPU_INFO 22 // RDI = pointer to a `struct cpu_info` (out, see
                         // api/cpuinfo.h). Returns 0 (RAX), or -EFAULT
                         // if the pointer isn't a writable user range.
                         //
                         // Note what this syscall is FOR, because half
                         // of what it returns needs no kernel at all:
                         // CPUID is unprivileged, so a ring-3 program
                         // can identify the CPU by itself. What it
                         // cannot do is read CR0/CR4/EFER to find out
                         // which of those capabilities the OS actually
                         // switched ON -- that's privileged, and it's
                         // the `enabled` field here. Returning the whole
                         // struct rather than only the privileged half
                         // keeps one decoder (family/model combining,
                         // leaf-4 cache maths) in one place instead of
                         // duplicating it into every ring-3 caller --
                         // the mistake userland/lspci.c's own copy of
                         // the PCI class table already demonstrates.

// The COMPOSITOR's queue -- raw input and the kernel's notices to it
// (abi/win_proto.h has the message format). **A CLIENT NEVER CALLS
// THESE**: its events are written by the compositor into the client's
// own channel ring (lib/uwmchan.h), evdev-then-a-socket as on Linux.
// Every one of the three answers -EPERM to a process that does not hold
// the compositor role (WIN_REQ_SET_COMPOSITOR).
#include "win_proto.h" // struct win_event

#define SYS_POLL_EVENT 23 // RDI = pointer to a `struct win_event` (out).
                           // Never blocks. Returns 1 if an event was
                           // written, 0 if the queue is empty, -EFAULT
                           // on a bad pointer, -EPERM from anyone but
                           // the compositor.

#define SYS_WAIT_EVENT 24 // RDI = pointer to a `struct win_event` (out).
                           // Returns 1 with an event written, or 0
                           // meaning "you were woken, ask again" -- see
                           // below. -EFAULT on a bad pointer, -EPERM
                           // from anyone but the compositor -- which is
                           // also what a caller that cannot be parked
                           // gets, so a non-schedulable caller fails
                           // loudly rather than spinning forever on a
                           // syscall that silently never blocks.
                           //
                           // CALLERS MUST LOOP. A 0 return does not
                           // mean "no event" -- it means the process
                           // was woken and should call again:
                           //
                           //   while (sys_wait_event(&ev) != 1) { }
                           //
                           // This is the same spurious-wakeup contract
                           // a condition variable has, and it is here
                           // for a concrete reason rather than
                           // sloppiness: the wake happens inside an
                           // interrupt handler, under whatever address
                           // space happened to be current, so the
                           // kernel CANNOT copy the event into the
                           // waiting process's buffer at that moment.
                           // The copy has to happen back inside the
                           // client's own syscall, which means the
                           // client has to re-enter it. (Linux's
                           // alternative is to rewind RIP over the
                           // trapping instruction and let the syscall
                           // restart itself -- ERESTARTSYS. Not used
                           // here: it hides a hard assumption about the
                           // syscall instruction's length inside the
                           // scheduler, and the explicit loop costs a
                           // client three lines.)
                           //
                           // The loop does NOT spin the CPU: each pass
                           // that finds nothing parks the process again
                           // via scheduler_block_current(), so a
                           // waiting client uses no timeslices at all.

#define SYS_WAIT_READY 89 // RDI = a timeout in MILLISECONDS. Parks the
                           // caller until its event queue is non-empty
                           // or that long has passed, whichever comes
                           // first, and CONSUMES NOTHING. Returns 1 if
                           // an event was already queued (it does not
                           // block then), 0 otherwise; -EPERM from
                           // anyone but the compositor, the same
                           // refusal SYS_WAIT_EVENT gives.
                           //
                           // READINESS, NOT DELIVERY -- this is `poll()`
                           // and SYS_WAIT_EVENT is `read()`. A caller
                           // that drains its own queue cannot use the
                           // delivering call to wait: the event it
                           // returned has left the queue, so the drain
                           // that follows never sees it and one event
                           // per wait is silently lost. A compositor is
                           // exactly that caller (userland/wm/wm_rawin.c
                           // owns the queue and dispatches every type),
                           // which is why the wait it needed is this one.
                           //
                           // A 0 DOES NOT DISTINGUISH "TIMED OUT" from
                           // "woken by an event", and must not: the
                           // caller drains and re-checks its deadlines
                           // either way, so the distinction has no
                           // correct use and a caller branching on it
                           // would be wrong the first time both
                           // happened at once.
                           //
                           // A timeout of 0 does not block at all. The
                           // maximum is SYS_SLEEP_MAX_MS, CLAMPED rather
                           // than refused -- a caller computing a
                           // deadline from "nothing is due" wants a long
                           // wait, not an error.

#define SYS_WIN_REQUEST 25 // RDI = pointer to a `struct win_request_msg`
                            // (in/out -- WIN_REQ_CREATE writes the new
                            // window id back into `window`). Returns 1
                            // on success, 0 if the server refused the
                            // request, -1 on a bad pointer, an unknown
                            // request type, a caller with no scheduler
                            // slot, or no window server registered
                            // (i.e. the desktop isn't running).
                            //
                            // ONE syscall for every windowing
                            // operation, dispatched on the message's
                            // own `type`, rather than a syscall per
                            // operation. That is the whole point: the
                            // client/server boundary stays a protocol,
                            // so adding an operation is a new message
                            // type and moving the server to ring 3 is a
                            // transport swap. See abi/win_proto.h.
                            //
                            // This supersedes SYS_WIN_CREATE/
                            // SYS_WIN_PRESENT (7/8) above, which stay
                            // for userland/win_test.c: those are modal
                            // and single-window, and their window never
                            // enters the WM's window list at all.

#define SYS_PIPE    26 // RDI = pointer to int[2] (out): [0] = read fd,
                        // [1] = write fd. Returns 0; -EFAULT for a bad
                        // pointer, -ENFILE/-EMFILE for a full pipe or
                        // descriptor table.

#define SYS_SPAWN 27 // RDI = pointer to a `struct spawn_msg` (below).
                      // Returns the child's pid, or a negative errno.
                      //
                      // ABI NOTE: this took three registers (path,
                      // args, stdout_fd) until the environment needed a
                      // fourth. A struct rather than a second syscall
                      // number, so there stays ONE spawn -- the shape
                      // SYS_SETTING and SYS_WIN_REQUEST already use.
                      // libsys's sys_spawn() keeps its old C signature,
                      // so nothing above it changed.

// What SYS_SPAWN takes. `path` and `args` are what they always were;
// `stdin_fd`/`stdout_fd` are a pipe end or a connected SOCKET this
// process owns, or -1.
//
// **`env` IS PASSED EXPLICITLY AND THE KERNEL STORES NONE OF IT.** It
// is a NUL-separated run of "KEY=VALUE" strings ending in an empty one
// ("A=1\0B=2\0\0"), or NULL for no environment -- one blob rather than
// a char** the kernel would walk pointer by pointer, validating each
// entry out of user memory, which is the same reasoning that makes
// `args` a single string.
//
// There is no inheritance HERE, deliberately. On Unix `execve()` is the
// primitive and takes envp explicitly; `execv()` is the C LIBRARY
// function that passes `environ` for you. toy-os copies that split:
// this is execve, and libsys's sys_spawn() is execv. A kernel that
// inherited would have to store an environment per process, and the
// one thing every caller then wants -- "like my parent's, but with one
// change" -- would need a second syscall to express.
// stdout_fd: everything this program writes to fd 1 goes to the
// application log, tagged with its own name (api/applog.h). What init
// gives a service, so `log -u toywm` means what it says.
#define SPAWN_FD_LOG (-2)

struct spawn_msg {
    const char *path;
    // The child's arguments, or NULL for none. Two forms, chosen by
    // SPAWN_ARGV below: without it a whitespace-separated STRING the
    // kernel splits (argv[0] is `path`); with it a VECTOR of
    // NUL-terminated strings, `args_len` bytes long, carried as-is --
    // so an argument may hold a space, or be empty.
    const char *args;
    const char *env;       // "K=V\0K=V\0\0", or NULL
    // A pipe write end or socket this process owns, or -1 for the
    // console, or SPAWN_FD_LOG for the application log.
    //
    // SPAWN_FD_LOG IS A SENTINEL RATHER THAN AN fd, and that is what
    // makes per-service logging cost nothing: the alternative is a pipe
    // per service, and PIPE_MAX is 8 KERNEL-WIDE against six services --
    // which would leave the shell unable to run `ls | grep`. A sentinel
    // needs no resource, cannot fill, and cannot block the writer, which
    // a pipe to a stalled reader does.
    int32_t stdout_fd;
    // The child's fd 0, same rule as `stdout_fd` above. A SOCKET is
    // accepted on both because that is what a connection per child
    // needs: inetd's handler is an ordinary filter reading fd 0 and
    // writing fd 1, and only 0/1/2 cross a spawn.
    //
    // NAMED HERE RATHER THAN dup2'd BEFORE THE SPAWN, because a spawn
    // ABI has no child-side window to redirect in -- the parent would
    // have to point its OWN fd 0/1 at the connection and put them back
    // afterwards, so anything it printed in between would go to the
    // client. posix_spawn has file_actions for the same reason.
    int32_t stdin_fd;
    // The process GROUP to start the child in, or 0 to inherit the
    // caller's -- which is what every existing caller passed, since this
    // field was `reserved` and had to be 0.
    //
    // HERE RATHER THAN A setpgid() AFTER THE SPAWN, and that is the
    // point: POSIX has both the parent and the child call setpgid()
    // after fork() because neither can be sure which runs first, and a
    // Ctrl-C landing in that window signals the wrong group. This kernel
    // has no fork -- SYS_SPAWN creates a process that is already
    // running -- so the race would be unfixable rather than merely
    // awkward. Passing the group at creation removes it. `posix_spawn`
    // reached the same answer with POSIX_SPAWN_SETPGROUP.
    int32_t pgid;

    // SPAWN_* below, or 0. **A FIELD RATHER THAN A SYSCALL, and the
    // reason is the one this struct exists for**: spawn outgrew three
    // registers once already, and the answer then was one message
    // instead of a second spawn. A capability that belongs to the act of
    // starting a process belongs in the message that starts it.
    //
    // Unknown bits are REFUSED, not ignored. A flag word that silently
    // drops what it does not recognise cannot ever be extended safely --
    // an old kernel would accept a new flag and do nothing, which is the
    // worst of both answers.
    uint32_t flags;

    // With SPAWN_ARGV: the byte length of the vector `args` points at,
    // every entry's NUL included, 1..SPAWN_ARGS_MAX. Without the flag it
    // is NOT READ: a binary built before this field existed passes a
    // shorter struct, and what lies past it is its stack.
    //
    // A LENGTH, NOT `env`'s DOUBLE-NUL TERMINATOR, because an argument
    // may legitimately be empty and the empty string IS that terminator
    // -- `prog "" x` would arrive as `prog` (the ambiguity Linux's
    // /proc/<pid>/cmdline is known for). An environment entry is never
    // empty, so `env` keeps its shape.
    uint32_t args_len;
};

// The child is TRACED: every syscall it makes is decoded and printed
// (kernel/proc/strace.c). `/bin/strace` is the only caller.
//
// **A PROPERTY OF THE SPAWN, NOT A MODE THE TRACER TURNS ON.** The
// alternative was an arm-then-spawn pair -- "the next process created is
// traced" -- which is what the kernel-side `strace` builtin did and
// which has a window in it: the arming process can be preempted between
// the two calls, and somebody else's spawn claims the arm. Naming the
// child at the moment it is created has no window to have a race in.
// posix_spawn's flags word is the same shape, for the same reason.
#define SPAWN_TRACE 1

// The child's process group goes IN FRONT of the caller's terminal (fd
// 0), atomically with its creation -- for a shell starting a
// foreground job. A tcsetpgrp() AFTER the spawn has fork's race
// without fork's fix: the child's first read can beat it (a whole
// timeslice, when the spawn itself ends the caller's), and the child
// is then stopped by its own SIGTTIN with everything looking correct.
// POSIX's shells close this from the child's side between fork and
// exec; a spawn ABI cannot, so the flag is the kernel doing the
// child-side half -- musl's POSIX_SPAWN_TCSETPGROUP, same reasoning.
// A no-op when fd 0 is not a terminal the caller owns, exactly as the
// after-the-fact tcsetpgrp was.
#define SPAWN_FOREGROUND 2

// `args` is an argv VECTOR -- NUL-terminated strings back to back,
// `args_len` bytes in all, at most SPAWN_ARGS_MAX -- and the kernel
// splits nothing.
// The string form cannot carry a space inside one argument, which is
// what a shell with quoting needs to do; CreateProcess passes a line
// and every program re-parses it, execve passes a vector. A FLAG rather
// than a second field, so a caller that predates it is unchanged.
// The vector INCLUDES argv[0], as execve's does; an empty vector is
// refused, since a program entered with argc == 0 dereferences NULL.
#define SPAWN_ARGV 4

// Every flag this kernel knows. Anything outside it is -EINVAL.
#define SPAWN_FLAGS_ALL (SPAWN_TRACE | SPAWN_FOREGROUND | SPAWN_ARGV)

// The most an environment blob may be, including its terminator. It has
// to fit the child's single argv/env stack page alongside the strings
// argv already needs, so this is a ceiling rather than a promise: a
// long argv leaves less. An oversized environment is REFUSED, never
// truncated -- a silently missing variable is a bug somewhere else.
#define SYS_ENV_MAX 2048

#define SYS_WNOHANG 1

// SYS_WAITPID's other flag: report a child that STOPPED as well as one
// that exited. POSIX's WUNTRACED, and opt-in for the same reason it is
// opt-in there -- a caller that does not ask must never be handed a
// result for a process that is still alive.
//
// The result is SIGNAL_STOP_BASE + the signal (abi/signal_abi.h), and
// the child is NOT reaped: it still holds its slot, its memory and its
// place in the process tree, because it has not finished. A stop is
// reported ONCE per suspension.
#define SYS_WUNTRACED 2

#define SYS_WAITPID 28 // RDI = pid from SYS_SPAWN, RSI = pointer to an
                        // int (out) for the exit code, or NULL.
                        // RDX = flags: SYS_WNOHANG to poll instead of
                        // wait; 0 for the blocking behaviour that
                        // predates it.
                        // BLOCKS until that child exits, then reaps it,
                        // UNLESS SYS_WNOHANG is set -- in which case a
                        // child that is still running answers
                        // SYS_RETRY (0) immediately and nothing is
                        // reaped.
                        // Returns the pid on success, or -1 for a pid
                        // that isn't this caller's live child.
                        //
                        // Same "0 means woken, ask again" retry
                        // contract as SYS_WAIT_EVENT, and for the same
                        // reason -- see that entry. libsys wraps the
                        // loop (sys_waitpid()).
                        //
                        // RDI = -1 means ANY child of the caller,
                        // POSIX's wait() convention -- it cannot
                        // collide with a real pid, which is 1-based.
                        // It returns the pid it reaped, or -1 when the
                        // caller has NO CHILDREN AT ALL. That -1 is
                        // permanent rather than "not yet", so a caller
                        // must not loop on it: with no children,
                        // nothing can ever change the answer. "Has
                        // children, none dead yet" is the ordinary
                        // block (or SYS_RETRY under SYS_WNOHANG).
                        //
                        // SYS_WUNTRACED additionally reports a child
                        // that was STOPPED (SIGSTOP/SIGTSTP): the
                        // return is that child's pid and the out code
                        // is SIGNAL_STOP_BASE + the signal. Nothing is
                        // reaped -- the child is alive and suspended --
                        // so a caller that loops must be able to tell
                        // that result from an exit, which is what the
                        // separate base exists for.

#define SYS_GETRANDOM 29 // RDI = buffer (out), RSI = byte count. Fills
                          // the buffer with random bytes and returns
                          // how many it wrote, or -1 for an invalid
                          // pointer or a count over
                          // SYS_GETRANDOM_MAX.
                          //
                          // ALWAYS fills the whole buffer or fails --
                          // it never returns a short count the way
                          // Linux's getrandom() can, because nothing
                          // here blocks waiting for entropy. What it
                          // cannot tell the caller is how GOOD the
                          // bytes are: the kernel may be running on a
                          // CPU with no RDSEED/RDRAND, in which case
                          // they come from timing jitter that is weak
                          // under emulation (see api/krandom.h's
                          // krandom_quality, which has deliberately not
                          // been exposed here -- a ring-3 program that
                          // could read it would mostly use it to decide
                          // to carry on anyway).

#define SYS_PROC_INFO 30 // RDI = process-table slot index, RSI = pointer
                          // to a `struct proc_info` (out, see
                          // abi/proc_info.h). Returns 0 on success;
                          // -EINVAL for a bad index (every enumerator's
                          // terminator), -EFAULT for a bad pointer. An
                          // EMPTY slot (pid 0) is a SUCCESS -- callers
                          // skip it themselves.
                          //
                          // Indexed by SLOT, not by pid, so a caller can
                          // walk the whole table without knowing which
                          // pids exist -- which is what a task manager
                          // does. An EMPTY slot is a successful call
                          // reporting pid 0, so enumeration skips rather
                          // than stops. The bound is SYS_PROC_MAX below.
                          //
                          // Read-only and unprivileged: every process
                          // can see every other. This kernel has no user
                          // model to hang a permission on, and inventing
                          // one here would be a check with nothing
                          // behind it -- see SYS_KILL, which makes the
                          // same call about a far more dangerous
                          // operation and says so.

#define SYS_KILL      31 // RDI = pid, RSI = SIGNAL number (abi/signal_abi.h).
                          // Returns 0 if the signal was delivered,
                          // -ESRCH if there was no such process (or the
                          // group was empty, or the number is not a
                          // signal).
                          //
                          // **RDI < 0 NAMES A PROCESS GROUP**, POSIX's
                          // rule: `kill(-pgid, sig)` signals every live
                          // member of that group and succeeds if it
                          // reached at least one. Pids are 1-based, so a
                          // negative number cannot collide with one.
                          //
                          // **THE SECOND ARGUMENT USED TO BE AN EXIT
                          // CODE**, and this call used to be `kill -9`
                          // and nothing else. It is a signal now, and a
                          // signalled process reports SIGNAL_EXIT_BASE +
                          // the signal as its exit code (130 for a
                          // Ctrl-C, 143 for a SIGTERM) -- the convention
                          // every Unix shell prints, and what makes a
                          // signalled death distinguishable from an
                          // ordinary non-zero exit. Callers that passed
                          // a raw code were changed to pass SIGKILL,
                          // which is exactly what they meant.
                          //
                          // WHAT EACH SIGNAL DOES depends on the target's
                          // disposition (SYS_SIGACTION) and on WHEN the
                          // kernel can safely act. SIGKILL is immediate
                          // and unconditional -- that is what keeps a
                          // force-quit trustworthy against a wedged
                          // process. Everything else is marked PENDING
                          // and acted on when the target next returns to
                          // ring 3, which is the only point the kernel
                          // holds its register state and holds nothing
                          // on its behalf. A target parked in a blocking
                          // syscall is woken with -EINTR so that it gets
                          // there. See docs/signals-design.md.
                          //
                          // **UNPRIVILEGED, DELIBERATELY.** Any process
                          // may kill any other, including the window
                          // manager once it is a process (Milestone 41
                          // stage 4). There is no user model, no
                          // capability and no process-group notion in
                          // this kernel, so a permission check here
                          // would be decoration -- it would have to
                          // invent the very thing it claims to enforce.
                          // Written down rather than quietly assumed;
                          // when a privilege model lands, THIS is the
                          // syscall it has to gate first.
                          //
                          // Killing the WM is not a hole to be closed,
                          // it is the property stage 4 has to prove: the
                          // kernel must survive it. See
                          // docs/wm-ring3-design.md.
                          //
                          // This is the FORCE path. The polite one is
                          // the window close handshake (WIN_EV_CLOSE),
                          // which an app may refuse; both exist for the
                          // same reason Windows separates End Task from
                          // End Process.

#define SYS_TICKS     32 // No arguments. Returns the monotonic timer tick
                          // count since boot.
                          //
                          // MONOTONIC, unlike SYS_GETTIME, which reports
                          // RTC wall-clock time -- that one goes
                          // backwards when the clock is set and has
                          // one-second resolution, so it can measure
                          // neither an interval nor an animation. This
                          // TICKS, at whatever rate the timer runs
                          // (PIT_HZ, 100 today). Fine for pacing
                          // something coarse; useless for measuring
                          // anything shorter than 10ms, which is why
                          // CPU accounting no longer uses it -- see
                          // SYS_MONOTONIC_NS below, which is what a
                          // proc_info cpu_ns delta divides by.

#define SYS_MONOTONIC_NS 33 // No arguments. Returns NANOSECONDS since
                          // boot, from the kernel's best available
                          // clocksource (kernel/clocksource.h) -- the
                          // TSC where it is invariant and calibrated,
                          // the 100Hz timer otherwise.
                          //
                          // The DENOMINATOR for a CPU percentage: the
                          // numerator is a delta of proc_info's cpu_ns
                          // and this is a delta of the same clock, which
                          // is what makes the ratio a real percentage
                          // rather than one counter over an unrelated
                          // other one.
                          //
                          // Its RESOLUTION is not promised and a caller
                          // must not infer one -- on a machine with no
                          // invariant TSC this advances in 10ms steps
                          // and two reads inside one tick return the
                          // same value. Code that needs to know should
                          // measure across a long enough interval, not
                          // ask how precise the clock is.

#define SYS_SETTING   34 // RDI = pointer to a `struct setting_msg`
                          // (abi/setting_abi.h), IN and OUT -- the
                          // kernel copies it in, fills the out fields
                          // and copies it back. Returns 0, or -1 for a
                          // bad op, a bad index, or an unreadable
                          // pointer.
                          //
                          // One syscall with an op field rather than
                          // five syscalls, for the same reason
                          // SYS_WIN_REQUEST is one: the boundary is
                          // then a MESSAGE, and adding an operation
                          // costs an enum value instead of an ABI
                          // number that can never be reused.
                          //
                          // Note SETTING_OP_SET reports its outcome in
                          // `result`, not in the return value -- the
                          // syscall succeeded in asking; whether the
                          // setting applied AND persisted is a separate
                          // three-way answer (etc_config.h's `enum
                          // setting_result`), and collapsing it to
                          // ok/failed is exactly the lie that made
                          // `timezone Helsinki` claim success while
                          // writing nothing.

#define SYS_SYSINFO   35 // RDI = pointer to a `struct sys_info` (out,
                          // abi/setting_abi.h). Memory and disk usage
                          // -- the whole-machine facts no other syscall
                          // reports. CPU identity is SYS_CPU_INFO, the
                          // PCI count SYS_PCI_COUNT and uptime
                          // SYS_MONOTONIC_NS; none are duplicated here.
                          // Returns 0, or -1 if the pointer is bad.

#define SYS_CRASHTEST 37 // RDI = pointer to a `struct crash_msg`
                          // (abi/crash_abi.h), in and out.
                          //
                          // Enumerates the kernel's DELIBERATE fault
                          // kinds, and triggers one. It exists because
                          // the panic path is the one path a kernel
                          // cannot exercise by accident and must not
                          // get wrong -- validating a panic report
                          // otherwise means editing a debug command to
                          // dereference a bad pointer and taking it out
                          // again. Linux ships the same thing (lkdtm).
                          //
                          // CRASH_OP_TRIGGER MAY NOT RETURN: on success
                          // the machine has panicked. It returns 0 with
                          // CRASH_F_ARMED clear when the kernel is not
                          // armed -- which is the default, since a
                          // deliberate crash hole has no business being
                          // open unless `faultinject` was asked for on
                          // the GRUB command line.
                          //
                          // Ring-0 kinds only: a ring-3 program needs
                          // no help to dereference NULL, only to make
                          // the KERNEL fault.
                          // Returns 0, or -1 for a bad pointer or op.

// 39 -- SYS_WIN_DEBUG, RETIRED 2026-09-09. The window server's `gui`
// relay became the diagnostic REGISTRY (SYS_DIAG, abi/diag_abi.h): the
// endpoint is a registered name rather than the compositor, so a service
// with no window is reachable too. The number is retired rather than
// reused, as the other deleted syscalls' are.
#define SYS_POWEROFF  38 // RDI = 0 to power off, 1 to reboot. Does not
                          // return on success.
                          //
                          // For the DESKTOP, which offers both from the
                          // Start menu and stops being able to reach
                          // system_poweroff() the moment it is a ring-3
                          // process (Milestone 41). The physical shell's
                          // own `poweroff`/`reboot` are unaffected --
                          // they are already ring 0.
                          //
                          // **Unprivileged, on purpose, and the same
                          // reasoning as SYS_KILL**: there is no user
                          // model here to gate it on, so a gate would be
                          // decoration. Anything that can spawn a
                          // process can already end the session.
                          //
                          // Both paths flush the disk cache first, so a
                          // write that returned success is on the
                          // platter before the machine stops -- the one
                          // thing this must not get wrong.
                          //
                          // Returns -1 for an op it does not recognise.
                          // On success it does not return at all, so a
                          // caller that continues past it should treat
                          // that as failure.

#define SYS_FS_GENERATION 36 // No arguments. Returns fs_generation()
                          // (api/fs.h) -- a counter the VFS bumps on
                          // every mutation of the filesystem, so a
                          // caller can answer "has anything changed?"
                          // with one integer compare instead of a
                          // directory scan.
                          //
                          // Deliberately its own syscall rather than a
                          // field in SYS_SYSINFO: the desktop polls
                          // this ONCE PER FRAME to decide whether to
                          // re-read /usr/wm/applications, and a free poll is
                          // the entire reason the counter exists. A
                          // sysinfo field would cost a validated struct
                          // copy per frame for figures nobody asked
                          // for. Never zero once a filesystem is
                          // mounted, so 0 is usable as "not sampled
                          // yet"; it only ever increases.

// --- descriptor plumbing --------------------------------------------
//
// fds 0/1/2 are ORDINARY DESCRIPTORS, not special numbers: they simply
// start out naming the console (0, 1) and the kernel log (2). So they
// can be redirected like any other, which is what these two are for.
//
// Together with SYS_SPAWN's inheritance they give a shell the classic
// redirection dance WITHOUT fork() -- the parent redirects itself
// around the spawn, rather than the child redirecting itself before an
// exec that does not exist here:
//
//     int saved = dup(1);
//     dup2(file_fd, 1);
//     spawn("/bin/ls", 0, -1);   // inherits fd 1 -> the file
//     dup2(saved, 1);
//     close(saved);              // refcounted: does NOT close the file
#define SYS_DUP  41 // RDI = fd. Returns the LOWEST FREE descriptor
                     // naming the same open file, or -1. The two share
                     // everything about the stream, including a file's
                     // read offset -- they are two names, not two opens.

#define SYS_DUP2 42 // RDI = oldfd, RSI = newfd. Makes newfd name what
                     // oldfd names, CLOSING whatever newfd named first,
                     // and returns newfd (or -1 if oldfd is not open).
                     //
                     // dup2(fd, fd) is a NO-OP and specifically does not
                     // close -- POSIX says so, and getting it wrong
                     // destroys the stream the call was asked to
                     // preserve.

// The console's size in TEXT CELLS: rows in the low 32 bits of RAX,
// columns in the high 32. No arguments, cannot fail.
//
// Ring 3 had no way to ask. The console's size is not a constant here
// -- it is derived from the active font (gfx_char_h()/gfx_char_w()),
// and `font_size` is a runtime setting -- so a program that pages or
// draws columns had nothing to work from but a guess, and a guess is
// wrong on any machine whose font was changed. That is the same
// font-derived rule the GUI's whole layout already follows.
//
// Rows and columns in ONE return value rather than a struct through a
// pointer: two small numbers fit, and it keeps the call free of a
// user-memory copy (and of the validation that goes with one).
//
// First caller is /bin/less. The TTY milestone wants this too --
// klineedit's console front end already has a documented bug from not
// knowing the width (a line longer than the console repaints wrongly).
#define SYS_CONSOLE_SIZE 43

// ---- the current directory, and the rest of the path-keyed calls ----
//
// THE CWD IS THE KERNEL'S, NOT EACH SHELL'S. Every path argument in
// this ABI -- open, unlink, listdir, and the six below -- is resolved
// against the calling process's cwd inside the kernel, so a relative
// path means one thing everywhere. Before this, `apps/shell.c` and
// `struct tosh` each held a private `cwd` and resolved before calling,
// which made a bare "docs" mean "/docs" to any program started from
// either -- silently creating the wrong thing rather than failing.
// A cwd is inherited across SYS_SPAWN and starts at "/".
//
// Absolute paths are unaffected, and a process that never calls
// SYS_CHDIR sees exactly the old behaviour.

#define SYS_CHDIR 44 // RDI = path (relative paths resolve against the
                      // current cwd, as everywhere else). Returns 0, or
                      // -ENOENT / -ENOTDIR / -ENAMETOOLONG / -EFAULT.
                      // -EPERM for a caller with no scheduler slot and
                      // no legacy slot armed.

#define SYS_GETCWD 45 // RDI = buffer (out), RSI = its capacity. Copies
                       // the cwd as a NUL-terminated normalized
                       // absolute path and returns its LENGTH (not
                       // counting the NUL), or -ERANGE if the buffer is
                       // too small -- never a truncated path, which is
                       // a different directory, not a shorter answer.

#define SYS_MKDIR 46 // RDI = path. Returns 0, or -EEXIST / -ENOENT (no
                      // parent) / -ENAMETOOLONG / -EFAULT / -EIO.

#define SYS_RENAME 47 // RDI = old path, RSI = new path. Returns 0, or a
                       // negative errno. Whether a cross-directory
                       // rename of a DIRECTORY is possible depends on
                       // the volume's journal (fs.h / TFS3 credits): a
                       // v1 image refuses exactly that one case.

#define SYS_TRUNCATE 48 // RDI = path, RSI = new size in bytes (grow or
                         // shrink). Returns 0, or a negative errno.

#define SYS_STAT 49 // RDI = path, RSI = pointer to a `struct sys_stat`
                     // (out, below). Returns 0, or a negative errno.

#define SYS_LINK 50 // RDI = existing path, RSI = the new name. Returns
                     // 0, or -EPERM when the mounted filesystem's format
                     // has no link counts (tfs2 -- ask
                     // SYS_STAT/FS_CAP_HARDLINKS through sysinfo first
                     // for a better message), -EISDIR for a directory.

#define SYS_SYNC 51 // No arguments. Flushes the disk write-back cache
                     // and returns the number of SECTORS written, or a
                     // negative errno if some could not be. Zero is a
                     // real answer (nothing was pending), which is why
                     // this reports a count rather than a bare status:
                     // "the flush failed and your data is still only in
                     // RAM" is the one disk answer a caller must not
                     // read as success.

// What SYS_STAT reports. Deliberately NOT POSIX's `struct stat` -- there
// are no modes, owners, devices or link counts to put in one, and a
// struct full of zeroed fields invites a caller to believe them. The
// timestamps are `struct rtc_time` for the same reason `struct sys_dirent`'s
// is: the epoch shape is kernel-internal (fs.h's fs_stat_info) and is
// converted back to civil time at this boundary.
struct sys_stat {
    uint64_t size;          // bytes; 0 for a directory, as fs_list() reports
    uint64_t ino;           // real on a backend with inodes, otherwise a
                            // stable synthetic (the table slot) -- see
                            // SYS_STAT_INODES below for which
    uint32_t is_dir;
    uint32_t flags;         // SYS_STAT_*
    struct rtc_time created;
    struct rtc_time modified;
};

// The inode number above is the filesystem's own, not a synthetic one.
// Mirrors fs.h's FS_CAP_INODES for the one caller that needs to say so.
#define SYS_STAT_INODES (1u << 0)

// The next two are what SYS_FSTAT adds, and they are FLAGS rather than
// new struct fields on purpose: a bare `is_tty` word would be a field
// that means nothing for the path-keyed SYS_STAT, which is exactly the
// "struct full of zeroed fields invites a caller to believe them"
// hazard the struct's own comment above warns about. A flag that is
// simply not set reads correctly either way.
#define SYS_STAT_TTY      (1u << 1) // this fd is a terminal. What a
                      // buffered stdio asks before choosing line
                      // buffering over full buffering -- the ONE
                      // question `isatty()` exists to answer, which is
                      // why there is no SYS_ISATTY: it would be a
                      // syscall returning a single bit that this call
                      // already carries.
#define SYS_STAT_SEEKABLE (1u << 2) // SYS_LSEEK works on this fd. Set
                      // for a file, clear for a console, a pipe or a
                      // socket. A path always names something seekable,
                      // so SYS_STAT sets it unconditionally.

#define SYS_GETPID 55 // No arguments. Returns the caller's own pid.
                      //
                      // It exists because a process could not find
                      // ITSELF: SYS_PROC_INFO is indexed by process-
                      // table SLOT, so reading your own cpu_ns meant
                      // guessing which row was yours. clock() (ring 3's
                      // <time.h>) is the caller that made that a real
                      // gap rather than a curiosity.
                      //
                      // Never fails, and never returns 0 -- a caller
                      // with no scheduler slot (the legacy loader) gets
                      // -1, which is the one value a real pid cannot be.

#define SYS_LSEEK 53 // RDI = fd, RSI = a SIGNED byte offset, RDX =
                      // one of SYS_SEEK_* below. Moves the fd's
                      // position and returns the NEW position, or a
                      // negative errno: -EBADF for a bad fd, -ESPIPE
                      // for a console, pipe or socket (which have no
                      // position to move), -EINVAL for an unknown
                      // whence or a result that would land before byte
                      // zero.
                      //
                      // Seeking PAST the end is legal and is not an
                      // error -- a following write zero-fills the gap
                      // (fs.h's fs_write_range() already does), which
                      // is what makes a sparse-ish file possible and
                      // what POSIX requires. Reading there returns 0.
                      //
                      // There is no SYS_TELL: `lseek(fd, 0, CUR)` is
                      // it, as in every Unix.

#define SYS_SEEK_SET 0 // from the start of the file
#define SYS_SEEK_CUR 1 // from the current position
#define SYS_SEEK_END 2 // from the end -- a NEGATIVE offset moves back
                       // into the file, and 0 means "the end"

#define SYS_FSTAT 54 // RDI = fd, RSI = pointer to a `struct sys_stat`
                      // (out, the SAME struct SYS_STAT fills). Returns
                      // 0, or -EBADF.
                      //
                      // One struct for both, as POSIX has it, rather
                      // than a second one that would mean almost the
                      // same thing -- the drift the shared-source rule
                      // exists to prevent elsewhere in this tree. What
                      // an fd adds is the two flags above: a path can
                      // never name a terminal or a pipe, and an fd can.
                      //
                      // For a console, pipe or socket the size and the
                      // timestamps are ZERO and `flags` says why. That
                      // is honest rather than lossy: there is no length
                      // for a pipe to have.

#define SYS_QUERY 52 // RDI = pointer to a `struct query_msg`
                      // (abi/query_abi.h), in and out.
                      //
                      // Reads a FACT: live kernel state, computed on
                      // every read and never persisted (see
                      // docs/settings-and-queries.md's "The vocabulary").
                      // One syscall with an information class rather
                      // than one syscall per fact, which is the growth
                      // SYS_SYSINFO / SYS_PROC_INFO / SYS_PCI_INFO /
                      // SYS_CPU_INFO were on -- and none of those four
                      // could answer "what facts exist?".
                      //
                      // Class 0 is the registry describing itself, so a
                      // caller needs to know exactly one number to
                      // discover every other class.
                      //
                      // Returns 0, or a negative errno.

#define SYS_SLEEP 40 // RDI = milliseconds. Parks the caller until that
                          // long has passed, then returns 0. Returns -1
                          // for a caller with no scheduler slot (the
                          // legacy loader, kernel code), which is a
                          // refusal rather than an instant return: a
                          // caller that cannot sleep must decide what to
                          // do instead, not silently spin.
                          //
                          // The RESOLUTION is one timer tick, so a 1 ms
                          // sleep lasts until the next tick and a sleep
                          // never returns EARLY. 0 ms is a valid
                          // request and means "until the next tick",
                          // which is a yield that gives up the rest of
                          // the slice rather than re-entering the
                          // rotation immediately -- SYS_YIELD is still
                          // the call for the latter.
                          //
                          // Why this exists: an idle loop with nothing
                          // to wait ON had no way to stop consuming CPU.
                          // init spends most of its life in exactly that
                          // state -- it blocks in waitpid(-1) while it
                          // has children and has nothing to block on
                          // when it does not. See docs/init-design.md.

#define SYS_SETPGID   56 // RDI = pid (0 = the caller), RSI = pgid
                          // (0 = the same value as `pid`, i.e. lead a
                          // new group). Returns 0, or -errno: -ESRCH for
                          // a pid that is not a live process, -EPERM for
                          // a pgid no live process is in and that is not
                          // `pid` itself.
                          //
                          // **UNPRIVILEGED, like SYS_KILL and for the
                          // same stated reason** -- there is no user
                          // model here to gate it on, and POSIX's "your
                          // own children, before they exec" rule needs a
                          // session concept this kernel does not have.
                          //
                          // MOSTLY YOU WILL NOT NEED IT: a child
                          // inherits its spawner's group, and SYS_SPAWN
                          // carries a `pgid` for the case a shell
                          // actually has -- putting a pipeline's stages
                          // in one group. POSIX makes both the parent
                          // and the child call setpgid() after fork()
                          // precisely because neither can be sure which
                          // runs first; spawn taking the group removes
                          // the race instead of documenting it. This
                          // call exists for the leader naming ITSELF
                          // (`setpgid(0, 0)`), which nothing else can do.

#define SYS_GETPGID   57 // RDI = pid (0 = the caller). Returns that
                          // process's group, or -ESRCH.

#define SYS_SIGACTION 58 // RDI = signal, RSI = a `const struct k_sigaction *`
                          // or 0, RDX = a `struct k_sigaction *` to write
                          // the previous action into, or 0. Returns 0,
                          // or -errno: -EINVAL for a bad signal or a
                          // handler with no restorer, -EPERM for SIGKILL,
                          // SIGQUIT or SIGSTOP (SIGNAL_UNIGNORABLE),
                          // -EFAULT for a pointer outside the caller's
                          // address space.
                          //
                          // **THIS IS `sigaction` NOW.** It took
                          // (signal, SIG_DFL|SIG_IGN) and returned the
                          // previous disposition while a handler was
                          // stage 3 of docs/signals-design.md and did
                          // not exist. A handler needs a restorer and
                          // flags beside it, which is four arguments in
                          // a three-register ABI -- so it takes POSIX's
                          // struct instead, in POSIX's argument order.
                          // `sys_signal()` in userland/rt/sys.h is the
                          // one-liner every existing caller wanted.
                          //
                          // AN IGNORED SIGNAL IS DROPPED AT ARRIVAL, not
                          // held pending -- POSIX's rule. So changing a
                          // disposition back to SIG_DFL does not
                          // resurrect signals sent while it was ignored.
                          // A signal with a HANDLER is not dropped: it
                          // is what the pending set is now mostly for.

#define SYS_TCSETPGRP 59 // RDI = an fd naming a TERMINAL, RSI = the pgid
                          // to put in front of it. Returns 0, or -errno:
                          // -ENOTTY if the fd is not a terminal, -EPERM
                          // if the caller does not own that terminal,
                          // -ENODEV if nothing owns it, -ESRCH for a
                          // group with no live member.
                          //
                          // This is `tcsetpgrp()`. A shell calls it
                          // around each job: put the job's group in
                          // front, wait, then put its own back. Until it
                          // does, the shell's own group is in front,
                          // which is why a shell must also ignore SIGINT
                          // (SYS_SIGACTION).
                          //
                          // **IT TOOK NO fd UNTIL THERE WAS MORE THAN
                          // ONE TERMINAL**, because "the console" was a
                          // complete answer. It is not: a Terminal
                          // window has a foreground group of its own,
                          // and a shell that could only ever move the
                          // console's would be pointing the physical
                          // keyboard's Ctrl-C at its own child.

#define SYS_TCGETPGRP 60 // RDI = an fd naming a terminal. Returns its
                          // foreground group, -ENOTTY if the fd is not a
                          // terminal, or -ENODEV when nobody owns it.

#define SYS_OPENPTY 61 // RDI = pointer to a `struct openpty_msg`, filled
                       // with a MASTER and a SLAVE descriptor. Returns
                       // 0, or -errno: -EMFILE with no descriptors free,
                       // -ENOSPC with no terminals free, -EFAULT for a
                       // bad pointer.
                       //
                       // **BOTH ENDS AT ONCE, AND NO PATH.** Linux opens
                       // /dev/ptmx for a master and names the slave
                       // /dev/pts/N; there are no device nodes here and
                       // vfs.c has no mount table to hang them on (the
                       // same reason there is no /proc -- see
                       // docs/query-design.md). This is BSD's
                       // openpty(3) shape, and it deletes the
                       // setsid()+TIOCSCTTY dance with it: a process is
                       // HANDED a terminal rather than acquiring one by
                       // opening a path.
                       //
                       // The caller owns the terminal (tcsetpgrp), and
                       // the slave is an ordinary fd -- dup2 it onto a
                       // child's 0/1/2 and that child is on a terminal.

#define SYS_TCGETATTR 62 // RDI = an fd naming a terminal, RSI = pointer
                         // to a `struct tty_termios` (abi/tty_abi.h) to
                         // fill. Returns 0 or -errno.

#define SYS_TCSETATTR 63 // RDI = an fd naming a terminal, RSI = pointer
                         // to a `struct tty_termios`. Returns 0 or
                         // -errno.
                         //
                         // A HALF-TYPED CANONICAL LINE IS DISCARDED,
                         // always -- POSIX's TCSAFLUSH and the only one
                         // of its three flush modes worth having here.
                         // Carrying the line into raw mode would make
                         // its bytes readable at the instant of the
                         // change, which is not what a program asking
                         // for raw mode asked for: it asked for what
                         // arrives NEXT.
                         //
                         // Every shell here calls it at startup to turn
                         // ICANON and ECHO off, because they all edit
                         // for themselves (kernel/lib/klineedit.c) --
                         // exactly as readline does on Linux.

#define SYS_SET_NONBLOCK 64 // RDI = fd, RSI = 1 to make a read that would
                            // block return -EAGAIN instead, 0 to restore
                            // blocking. Returns 0 or -EBADF.
                            //
                            // **fcntl(F_SETFL, O_NONBLOCK) CUT TO THE
                            // ONE THING ANYTHING HERE NEEDS.** A real
                            // fcntl is a dozen commands over descriptor
                            // flags, file-status flags and locks; this
                            // is the flag, by name, with no command
                            // multiplexer in front of it -- copy the
                            // shape, not the size.
                            //
                            // **AND IT EXISTS BECAUSE THERE IS NO
                            // poll().** A terminal emulator has to
                            // service its window's events and drain its
                            // child's output, and cannot sit blocked in
                            // either; with poll() it would wait on both
                            // at once, and that is the right answer and
                            // a bigger project (docs/roadmap.md). Until
                            // then it drains on a tick, which costs a
                            // wakeup per frame and is honest about it.
                            //
                            // It affects the DESCRIPTION, not the
                            // descriptor -- so a dup2'd copy shares it,
                            // as on Linux, and a child that inherits fd
                            // 0 inherits the flag with it. Set it on a
                            // master you own, never on a slave you are
                            // about to hand to somebody else.

#define SYS_TCGETWINSZ 65 // RDI = an fd naming a terminal, RSI = pointer
                          // to a `struct tty_winsize` to fill. Returns 0
                          // or -errno.
                          //
                          // `ioctl(TIOCGWINSZ)`, which is the FIRST
                          // thing every full-screen program does -- an
                          // editor that guessed 80x25 would paint past
                          // the bottom of anything else.
#define SYS_TCSETWINSZ 66 // RDI = fd, RSI = pointer to a `struct
                          // tty_winsize`. Returns 0 or -errno.
                          //
                          // For a TERMINAL EMULATOR, which is the only
                          // thing that knows how big its window is in
                          // CELLS -- it has the font. The physical
                          // console derives its own size from vga.c and
                          // ignores this: a stored copy of a fact the
                          // kernel already has is the copy that goes
                          // stale when the font size changes.
                          //
                          // IT RAISES SIGWINCH on the terminal's
                          // foreground group, and only when the size
                          // actually MOVED -- an emulator calls this on
                          // every window event, so an unconditional
                          // signal would interrupt the foreground
                          // program's read once per frame of a drag.
                          // A program with no handler is unaffected:
                          // SIGWINCH's default action is to be ignored.

// HOW MANY BYTES `int $0x80` IS, so that a restart can rewind over it.
//
// It is 0xCD 0x80 -- the two-byte form, because 0x80 does not fit the
// one-byte `int3`. Named rather than written as a literal 2 in the two
// places that rewind (api/scheduler.h's signal wake and signal.c's
// SA_RESTART), since a bare `-= 2` next to an instruction pointer is
// unreadable and this is the ONE fact both of them depend on.
//
// It would change if this kernel ever moved to `syscall`/`sysret`, which
// is also two bytes (0x0F 0x05) -- so the number survives that, and the
// name is what makes the coincidence obvious rather than lucky.
#define SYSCALL_INSN_LEN 2

#define SYS_SIGRETURN 67 // No arguments. Unwinds the signal frame the
                          // kernel pushed before entering a handler and
                          // resumes the interrupted code. **NEVER
                          // RETURNS in the ordinary sense** -- the
                          // trapframe it returns through is the one
                          // saved at delivery, not the one it was called
                          // with.
                          //
                          // NOT A CALL A PROGRAM MAKES. The kernel
                          // pushes the restorer from `struct k_sigaction`
                          // as the handler's return address, so this is
                          // reached by the handler doing an ordinary
                          // `ret` -- see userland/rt/sigtramp.asm, which
                          // is the only thing in this tree that issues
                          // it. Calling it by hand finds no valid frame
                          // (SIGFRAME_MAGIC) and kills the caller, which
                          // is the honest answer: there is nothing to
                          // return to.
                          //
                          // WHAT IT REFUSES TO RESTORE VERBATIM: CS, SS,
                          // and the privileged bits of RFLAGS. Those are
                          // reimposed by the kernel, so a program that
                          // corrupts its own frame gets a ring-3 fault
                          // rather than a ring transition it did not
                          // earn. Everything else -- every general
                          // register, RIP, RSP and the blocked mask --
                          // is restored exactly, because putting the
                          // process back the way it was is the entire
                          // job.

#define SYS_NOTIFY_READY 68 // No arguments. The caller declares that it
                          // has finished starting up and is now doing
                          // whatever it exists to do. Returns 0, or
                          // -ESRCH for a caller with no scheduler slot
                          // (the kernel context and the legacy `run`
                          // loader, which nothing supervises).
                          //
                          // **IT SETS A BIT AND NOTHING ELSE HAPPENS.**
                          // The kernel does not act on it, wake anybody
                          // or attach a meaning to it: the bit is
                          // reported through SYS_PROC_INFO (`ready` in
                          // abi/proc_info.h) and init is the only thing
                          // that reads it. That is what keeps a
                          // service-manager concept out of the
                          // scheduler -- the kernel stores the
                          // announcement, userland decides what it is
                          // worth.
                          //
                          // WHY A SYSCALL RATHER THAN A SOCKET. systemd's
                          // Type=notify has the service send READY=1 to
                          // an AF_UNIX datagram socket named in
                          // $NOTIFY_SOCKET, and attributes it to a
                          // sender with SO_PASSCRED; s6 has it write a
                          // byte to an inherited fd. Neither ports:
                          // there are no unix sockets here, PIPE_MAX is
                          // 8 kernel-wide (shared with every shell
                          // pipeline, so one held for a whole boot is an
                          // eighth of the supply), and a pipe carries no
                          // credentials -- the child would have to
                          // declare its own pid and be believed. Calling
                          // the manager instead is Windows' shape
                          // (SetServiceStatus(SERVICE_RUNNING), which
                          // the SCM makes dependent services wait for),
                          // and going through the kernel makes the
                          // caller's identity the kernel's rather than a
                          // claim in a message.
                          //
                          // ONE BIT, DELIBERATELY. sd_notify also
                          // carries STATUS=, RELOADING= and a watchdog
                          // ping; none has a consumer here, and the
                          // second real caller is this project's bar for
                          // adding one. What it costs to have chosen
                          // this shape is that a richer protocol later
                          // means a different channel, not a longer
                          // message -- said plainly because it is the
                          // real trade.
                          //
                          // IDEMPOTENT. A second call is not an error; a
                          // process that announces twice is announcing
                          // the same thing.

#define SYS_MKPART 69 // RDI = pointer to a `struct mkpart_request`
                      // (abi/partition_abi.h). Writes an MBR or GPT
                      // partition table to the disk, replacing whatever
                      // was there. Returns 0, or a negative errno:
                      // -EINVAL for a table that fails validation
                      // (overlap, out of bounds, more than MBR's four),
                      // -EPERM without MKPART_CONFIRM while a
                      // persistent filesystem is mounted, -EIO if the
                      // write itself failed, -ENODEV with no disk.
                      //
                      // DOES NOT REMOUNT ANYTHING. The volume in use is
                      // untouched and the new table takes effect at the
                      // next boot -- Linux behaves the same way, and
                      // refuses to re-read a table on a busy disk.
                      //
                      // A table DESCRIPTION rather than a raw sector
                      // write, deliberately -- abi/partition_abi.h has
                      // the reasoning, and it is mostly about this
                      // kernel having no privilege model to gate a
                      // general write-any-sector primitive with.

#define SYS_MOUNT 70 // RDI = pointer to a `struct mount_request`
                     // (abi/mount_abi.h). Attaches a filesystem at a
                     // path. Returns 0, or a negative errno:
                     // -EINVAL for a bad request (a mount point that is
                     // not an absolute path, an unknown filesystem
                     // type, a source that names no partition),
                     // -ENOTDIR when the mount point does not exist or
                     // is not a directory, -EBUSY when something is
                     // already mounted there or that volume is already
                     // mounted somewhere, -ENOSPC when the mount table
                     // is full, -ENODEV when nothing recognises the
                     // filesystem on that volume, -EFAULT for a bad
                     // pointer.
                     //
                     // NOT PRIVILEGED, because this kernel has no
                     // privilege model -- see abi/mount_abi.h, which
                     // names sys_mount() as where the check goes when
                     // one exists.

#define SYS_UMOUNT 71 // RDI = pointer to a NUL-terminated mount point
                      // (at most MOUNT_POINT_MAX bytes). Detaches what
                      // is mounted there. Returns 0, or a negative
                      // errno: -EINVAL for the root (which cannot be
                      // unmounted) or a path nothing is mounted at,
                      // -EBUSY when a file on it is still open or
                      // another filesystem is mounted underneath it,
                      // -EFAULT for a bad pointer.
                      //
                      // FLUSHES BEFORE IT FORGETS. A write-back cache's
                      // failure surfaces at the flush, and after the
                      // slot is cleared there is no owner left to
                      // report it to.

// --- threads ---------------------------------------------------------
//
// A THREAD IS A PROCESS THAT SHARES ITS CREATOR'S ADDRESS SPACE, which
// makes it a slot in the same table with the same kind of id. Three
// facts a caller has to know:
//
//   - **A tid comes out of the pid space**, so `SYS_KILL` on one kills
//     the whole process (there is no tkill), and a thread costs a
//     process slot.
//   - **SYS_GETPID answers for the PROCESS**, the same value in every
//     thread. SYS_GETTID is what tells them apart.
//   - **SYS_EXIT ends the PROCESS**, whichever thread calls it -- POSIX's
//     exit_group. SYS_THREAD_EXIT ends one thread.
//
// The stack and everything on top of it (return values, destructors,
// the pthread_t) are RING 3's: userland/libc/pthread.c. The kernel does
// not allocate a thread stack, exactly as clone(2) does not.

#define SYS_THREAD_CREATE 72 // RDI = pointer to a `struct thread_create_msg`.
                             // Starts another thread of the calling
                             // process. Returns the new tid (> 0), or a
                             // negative errno: -EFAULT for a stack or
                             // entry pointer that is not the caller's
                             // memory, -EAGAIN when the process table
                             // is full, -EPERM when the caller is not a
                             // scheduled process.

#define SYS_THREAD_EXIT 73 // RDI = exit code. Ends the CALLING thread
                           // and does not return. From the initial
                           // thread it exits the process instead --
                           // see kernel/proc/scheduler.c's
                           // scheduler_on_thread_exit() for why.

#define SYS_THREAD_JOIN 74 // RDI = tid. Blocks until that thread of
                           // this process exits, reaps it, and returns
                           // its exit code. -ESRCH when the tid is not
                           // a joinable thread of the caller's process
                           // (already joined, detached, another
                           // program's, or the caller itself).

#define SYS_THREAD_DETACH 75 // RDI = tid. Says nobody will join it, so
                             // its exit frees the slot. Reaps it if it
                             // has already exited. 0, or -ESRCH/-EINVAL.

#define SYS_GETTID 76 // No arguments. The calling THREAD's id, where
                      // SYS_GETPID is its process's. Equal in a program
                      // that never creates a thread.

#define SYS_SET_TLS 77 // RDI = the calling thread's thread pointer,
                       // what %fs-relative addressing resolves against.
                       // Ring 3 owns the layout behind it entirely
                       // (userland/rt/tls.c); the kernel remembers the
                       // number and reloads it on every switch. Returns
                       // 0, or -EPERM off a scheduled process.
                       //
                       // arch_prctl(ARCH_SET_FS)'s job, named for what
                       // it does rather than for the register, because
                       // this is the only architecture-specific thing
                       // in the ABI and hiding it behind a generic name
                       // would make it look portable.

#define SYS_MMAP 78   // RDI = pointer to a `struct mmap_msg` (below).
                      // Returns the mapping's address -- page-aligned,
                      // inside the mmap arena -- or -ERRNO. A struct
                      // because the call needs six arguments and this
                      // ABI carries three, same as SYS_THREAD_CREATE.
                      //
                      // The mapping is a RESERVATION, like SYS_SBRK's:
                      // no frame moves until a page is touched. A
                      // file-backed page is read from the file at
                      // first touch; what the file held AT THAT MOMENT
                      // is what the page gets, and a later write to
                      // the file does not update pages already faulted
                      // in. MAP_SHARED is accepted ONLY over a
                      // descriptor from SYS_SHM_OPEN: a shared mapping
                      // of a FILE is still refused (-EINVAL), because
                      // writes never reach the file.

#define SYS_SND_OPEN 80 // No arguments. Claims the machine's ONE PCM
                         // playback stream (exclusive -- -EBUSY while
                         // another process holds it; -ENODEV with no
                         // sound hardware) and maps the control page +
                         // sample ring at UADDR_SND_BASE. See
                         // abi/sound_abi.h for the ring contract; the
                         // format is fixed 48kHz s16le stereo. Returns
                         // 0, or -errno.
#define SYS_SND_CTL 81  // RDI = a SND_CTL_* op (abi/sound_abi.h):
                         // START begins playback at the ring's start,
                         // STOP halts the engine (ring stays mapped),
                         // CLOSE stops, unmaps and releases the
                         // stream. Owner only (-EPERM). Returns 0, or
                         // -errno. The stream is also released when
                         // its owner dies, like fds and windows.

#define SYS_MUNMAP 79 // RDI = addr, RSI = length (both page-aligned).
                      // Unmaps [addr, addr+len) and frees the frames
                      // behind any pages that were touched. The range
                      // must lie within ONE mapping (POSIX allows
                      // spanning several; this does not, yet) -- it
                      // may trim an edge or split the middle. Returns
                      // 0 or -ERRNO.

#define SYS_SHM_OPEN 99   // RDI = pointer to a `struct shm_open_msg`.
                          // Returns a descriptor naming a SHARED
                          // MEMORY OBJECT -- a run of frames the
                          // kernel owns, which any process that knows
                          // the name can map with SYS_MMAP's
                          // MAP_SHARED. Returns an fd, or -errno.
                          //
                          // THE NAME IS THE RENDEZVOUS. There are no
                          // unix sockets here and no fd passing, so a
                          // name in a kernel-held namespace is how two
                          // processes that never shared a parent find
                          // one channel. POSIX shm_open(3)'s shape,
                          // minus a mode: this system has no users, so
                          // any process may open any name.

#define SYS_FUTEX_WAIT 101 // RDI = a 4-byte-aligned user address, RSI =
                           // the value it is expected to still hold,
                           // RDX = a timeout in milliseconds (0 = no
                           // deadline). Parks the caller until
                           // SYS_FUTEX_WAKE names that word, or the
                           // timeout passes. Returns 0 when woken;
                           // -EAGAIN if the word ALREADY HOLDS
                           // SOMETHING ELSE, which is not an error but
                           // the answer -- it closes the lost-wakeup
                           // race, since a caller that parked on a
                           // stale read would wait for a wake that had
                           // already happened. -EINVAL for a misaligned
                           // address, -EFAULT for one nothing maps.
                           //
                           // THE WORD IS NAMED BY ITS FRAME, not by the
                           // caller's pointer, so two processes sharing
                           // an shm page reach the same futex at
                           // whatever address each of them mapped it.

#define SYS_DIAG      105  // RDI = pointer to a `struct diag_msg`

// Loadable kernel modules (kernel/core/module.c). RDI = a path (MODLOAD)
// or a module name (MODUNLOAD). Returns 0 or -errno; the reason is in
// the kernel log (`dmesg`), since a load can fail a dozen ways and
// naming the unexported symbol is worth more than a code.
#define SYS_MODLOAD   106
#define SYS_MODUNLOAD 107
                           // (abi/diag_abi.h). Asks a NAMED ring-3
                           // service a question, or -- from the service
                           // side -- claims that name and answers.
                           //
                           // The window server's `gui` relay
                           // generalised: the endpoint is a registered
                           // name rather than the compositor, so a
                           // service with no window is reachable too.
                           // Copy in, act, copy back, like every other
                           // message-carrying syscall here.

#define SYS_SHM_GRANT 104  // RDI = a name this process created, RSI = a
                           // pid that may now open it. Returns 0, -ENOENT
                           // if there is no such object, -EPERM from
                           // anyone but the creator, -ENOSPC past the
                           // grant limit. SYS_SHM_OPEN answers -EPERM
                           // to a process with no claim on the name.
                           //
                           // A CAPABILITY, not a mode: it names ONE
                           // process rather than a class of them. When
                           // this system gains users a mode arrives
                           // beside it, the way Linux has both file
                           // permissions and fd passing.

#define SYS_WAKEWORD  103  // RDI = a 4-byte-aligned user address, or 0
                           // to deregister. Names ONE word this process
                           // waits on for everything: the kernel bumps
                           // it and wakes it whenever it queues an
                           // event, and anything else sharing the page
                           // may do the same.
                           //
                           // It exists because a futex waits on one word
                           // and a compositor has two sources -- its
                           // event queue and its clients' messages --
                           // with no poll() here to wait on both. Every
                           // source bumping ONE word is what an event
                           // loop without a unified poll turns into;
                           // eventfd and the self-pipe trick are the
                           // same answer.
                           //
                           // The word must stay mapped for as long as it
                           // is registered. A shm page is the point:
                           // that is how a SENDER in another process
                           // reaches it.

#define SYS_FUTEX_WAKE 102 // RDI = the same address, RSI = how many
                           // waiters to release (0 = all). Returns the
                           // number actually woken. A lock's unlock
                           // passes 1: releasing every waiter so that
                           // all but one park again is a thundering
                           // herd.

#define SYS_SHM_UNLINK 100 // RDI = a name. Removes it from the
                           // namespace; the frames go when the last
                           // descriptor and mapping do, so unlinking
                           // an object somebody is still using is safe
                           // and is how a creator says "no new
                           // openers". Returns 0, or -ENOENT.

// What SYS_SHM_OPEN takes.
#define SHM_NAME_MAX 32 // including the terminator

#define SHM_CREATE 0x1 // make it if it does not exist
#define SHM_EXCL   0x2 // with CREATE: refuse (-EEXIST) if it does
#define SHM_PUBLIC 0x4 // with CREATE: ANY process may open it. For a
                        // BEACON -- a rendezvous every client has to be
                        // able to find. Without it an object belongs to
                        // its creator, who grants others with
                        // SYS_SHM_GRANT.
                        //
                        // PRIVATE IS THE DEFAULT, and deliberately: a
                        // permission model added later cannot make
                        // existing callers private retroactively, so the
                        // sharing has to be the thing that is asked for.
                        // It also is not a hypothetical -- `lsshm` lists
                        // every name, so before this any process could
                        // open another's audio ring or window buffer.

struct shm_open_msg {
    const char *name;   // no '/' and no '..'; SHM_NAME_MAX bytes
    uint64_t    length; // bytes, rounded up to whole pages. Ignored
                        // when opening an object that already exists --
                        // its size was fixed at creation, which is why
                        // there is no ftruncate() here.
    int32_t     flags;  // SHM_*
    int32_t     reserved;
};

// What SYS_THREAD_CREATE takes. A struct because the call needs five
// arguments and this ABI carries three -- the same thing SYS_SPAWN did
// when an environment arrived.
struct thread_create_msg {
    uint64_t entry;     // where the new thread starts
    uint64_t stack_top; // its stack pointer; grows DOWN from here
    uint64_t arg;       // the SysV first argument to `entry`
    uint64_t tls;       // its thread pointer, or 0 for none
    int32_t  detached;  // non-zero: leave no zombie to join
    int32_t  reserved;  // keeps the struct 8-byte aligned in both rings
};

// What SYS_OPENPTY fills in. A struct rather than two out-registers
// because a syscall here returns one value, and two `int *` arguments
// would be two user pointers to validate instead of one.
struct openpty_msg {
    int32_t master_fd;
    int32_t slave_fd;
};

// What SYS_MMAP takes. The values are Linux's on purpose -- a libc
// wrapper passes POSIX's constants straight through, and a ported
// program's `mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE |
// MAP_ANONYMOUS, -1, 0)` means here what it means there.
#define SYS_PROT_READ  0x1
#define SYS_PROT_WRITE 0x2
#define SYS_PROT_EXEC  0x4

#define SYS_MAP_SHARED    0x01 // shm objects only (SYS_SHM_OPEN). Every
                               // mapper sees one set of frames, so a
                               // write in one process is visible in
                               // another -- the only way two ring-3
                               // processes share memory here.
#define SYS_MAP_PRIVATE   0x02
#define SYS_MAP_FIXED     0x10 // `addr` is a demand, not a hint; it must
                               // be page-aligned and inside the arena,
                               // and the range must be FREE -- overlap
                               // is -EEXIST where POSIX silently
                               // replaces. Deliberate: replace is
                               // munmap-then-map, and a caller that
                               // wants it (the dynamic loader carving
                               // segments out of a reservation) can
                               // say so in two calls, where a typo'd
                               // addr silently unmapping live pages
                               // cannot be taken back.
#define SYS_MAP_ANONYMOUS 0x20 // zero-filled; `fd` and `offset` ignored

struct mmap_msg {
    uint64_t addr;   // 0 = kernel picks; else a hint (FIXED: a demand)
    uint64_t length; // bytes; rounded up to whole pages
    int32_t  prot;   // SYS_PROT_* -- PROT_READ is required
    int32_t  flags;  // SYS_MAP_* -- exactly one of PRIVATE/SHARED
    int32_t  fd;     // an open file or shm object; -1 with ANONYMOUS
    int32_t  reserved;
    uint64_t offset; // into the file; page-aligned
};

// The longest single SYS_SLEEP, one hour. Not a security limit: it
// keeps a garbage argument from parking a process for the rest of the
// boot, which looks exactly like a hang. A caller wanting longer calls
// it again.
// A SINGLE SLEEP IS CAPPED, AND THE CAP IS SILENT: a longer request
// returns early rather than failing, so a caller that needs a real
// deadline must LOOP against the clock rather than trust one call.
// `/bin/dhcp` asked for T1 of a 24-hour lease, got an hour, and renewed
// hourly -- which on a server with a pool moved the machine's address
// every time.
#define SYS_SLEEP_MAX_MS 3600000

// The longest ARGUMENT STRING a spawn may carry. It used to be
// FS_PATH_MAX -- a PATH limit, 64 bytes, applied to something that is
// not a path -- so every program's arguments were silently cut at 63
// characters: a long URL to `wget`, a long string to `echo`, a `-o`
// with a deep destination. Found by asserting on the CONTENT a spawned
// program wrote rather than on the file appearing, which is the
// difference between "it ran" and "it ran with what I typed".
//
// A separate constant, because a path inside the argument string is
// still bounded by FS_PATH_MAX where it is used AS a path.
#define SPAWN_ARGS_MAX 1024

// The number of process-table slots SYS_PROC_INFO can be asked about.
// Mirrors the kernel's SCHED_MAX_PROCS; a caller loops 0..this-1.
#define SYS_PROC_MAX 64

// The largest single SYS_GETRANDOM request. Not a security limit -- it
// stops a bad count from turning into a long uninterruptible fill in
// ring 0, the same reasoning as every other bounded copy across this
// boundary.
#define SYS_GETRANDOM_MAX 4096

// --- fork and exec ---------------------------------------------------
//
// The compatibility pair beside SYS_SPAWN, which stays the primitive
// every program here uses (docs/fork-design.md). A ported POSIX shell
// is what needs these; nothing else should reach for them.
#define SYS_FORK 108 // No arguments. Returns the child's pid to the
                     // caller and 0 to the child, whose address space
                     // is the caller's shared copy-on-write, whose fd
                     // table is the caller's WHOLE table (there is no
                     // CLOEXEC), and whose pending signals are none.
                     // A caller with no scheduler slot gets -EPERM;
                     // a full process table -EAGAIN; no memory -ENOMEM.
#define SYS_EXEC 109 // RDI = pointer to a `struct spawn_msg`: `path`,
                     // `args` (with or without SPAWN_ARGV) and `env`
                     // as for SYS_SPAWN. Replaces the CALLER's image
                     // with that program and does not return on
                     // success; on failure returns -errno with the
                     // caller untouched. stdin_fd/stdout_fd must be
                     // -1, pgid 0 and no flag but SPAWN_ARGV set --
                     // an exec has no child to redirect. Descriptors,
                     // the cwd, the group and the parent survive;
                     // caught signals go back to their default, other
                     // threads of the caller end, and a non-leader
                     // thread is refused (-EPERM).

#endif
