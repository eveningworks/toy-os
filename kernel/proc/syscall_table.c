// THE syscall table. One row per number: the handler, the name, and
// what `strace` should make of the arguments and the return value.
//
// This is the only file that knows about every syscall at once. The
// handlers are ordinary functions living with the subsystem that owns
// them; the shape and the reasoning are in kernel/include/kernel/
// syscall_table.h, and the design call (one merged row rather than two
// parallel tables) is written up in docs/decisions.md.
//
// Adding a syscall: define its number in abi/syscall_abi.h, write the
// handler in the subsystem that owns it, declare it in syscalls.h, and
// add a row here. Nothing else is a registry -- there is no init call
// to forget.
#include "syscalls.h"
#include "syscall_abi.h"
#include "scheduler.h"      // SCHED_KSTACK_SYSCALL_MAX -- asserted against below
#include "syscall_stall.h"  // SYSCALL_STALL_MAX -- likewise
#include <stddef.h>

// Designated initializers, so a row's INDEX is its syscall number.
// That is what makes this list unorderable-by-accident, and it carries
// the two compile-time guarantees a generated table was proposed for:
// an undefined SYS_* does not compile, and -Wextra's -Woverride-init
// makes a duplicated number an error rather than a silent last-wins.
// A generator would add a parser over a header that is mostly prose to
// buy the same two things -- see docs/decisions.md.
//
// A number with no row (or a row with a NULL `fn`) is unimplemented:
// dispatch answers -ENOSYS (syscall.c), and `strace` prints it as
// `syscall_<n>` with hex arguments rather than hiding it.
// A number that USED to be a syscall and is not one now. A hole gets
// -ENOSYS from the dispatcher like any other, so what this row adds is
// the NAME: `strace` then says what a stale binary asked for. Retired
// numbers without a row are declared in SYSCALL_RETIRED below;
// `syscall/no row is half-filled in` is the KTEST that insists on it.
static int sys_removed(struct syscall_ctx *c) {
    c->regs[14] = (uint64_t)(int64_t)-ENOSYS;
    return 0;
}

static const struct syscall_desc SYSCALL_TABLE[] = {
    [SYS_EXIT]          = { "exit",          sys_exit,          { A_INT } },
    [SYS_WRITE]         = { "write",         sys_write,         { A_FD, A_BUF, A_INT } },
    // The one syscall returning a pointer rather than a count/status --
    // and the reason a handler reports "I parked" separately from its
    // return value, since no 64-bit value is free to mean anything else.
    [SYS_SBRK]          = { "sbrk",          sys_sbrk,          { A_INT }, R_HEX },
    // mmap returns a pointer too, so R_HEX; its errors are the small
    // negatives every libc wrapper already tests for.
    [SYS_MMAP]          = { "mmap",          sys_mmap,          { A_HEX }, R_HEX },
    [SYS_MUNMAP]        = { "munmap",        sys_munmap,        { A_HEX, A_INT } },
    [SYS_DEV_MAP_BAR]   = { "dev_map_bar",   sys_dev_map_bar,   { A_INT, A_INT }, R_HEX },
    [SYS_DEV_CLAIM]     = { "dev_claim",     sys_dev_claim,     { A_INT } },
    [SYS_DEV_RELEASE]   = { "dev_release",   sys_dev_release,   { A_INT, A_HEX } },
    [SYS_DEV_DMA_ALLOC] = { "dev_dma_alloc", sys_dev_dma_alloc, { A_INT, A_INT, A_HEX }, R_HEX },
    [SYS_DEV_IRQ_ENABLE] = { "dev_irq_enable", sys_dev_irq_enable, { A_INT } },
    [SYS_DEV_IRQ_ACK]   = { "dev_irq_ack",   sys_dev_irq_ack,   { A_INT } },
    [SYS_DEV_IO]        = { "dev_io",        sys_dev_io,        { A_HEX } },
    [SYS_USB_CLAIM]     = { "usb_claim",     sys_usb_claim,     { A_INT } },
    [SYS_USB_RELEASE]   = { "usb_release",   sys_usb_release,   { A_INT, A_INT } },
    [SYS_USB_CONTROL]   = { "usb_control",   sys_usb_control,   { A_HEX } },
    [SYS_USB_ISOCH_OPEN]   = { "usb_isoch_open",   sys_usb_isoch_open,   { A_HEX } },
    [SYS_USB_ISOCH_POST]   = { "usb_isoch_post",   sys_usb_isoch_post,   { A_HEX } },
    [SYS_USB_ISOCH_STATUS] = { "usb_isoch_status", sys_usb_isoch_status, { A_INT, A_INT } },
    [SYS_SND_RING_MAP]     = { "snd_ring_map",     sys_snd_ring_map,     { A_HEX } },
    [SYS_SETPRIORITY]      = { "setpriority",      sys_setpriority,      { A_INT, A_INT, A_INT } },
    [SYS_GETPRIORITY]      = { "getpriority",      sys_getpriority,      { A_INT, A_INT } },
    [SYS_SND_REGISTER]  = { "snd_register",  sys_snd_register,  { A_HEX } },
    [SYS_SND_PERIOD]    = { "snd_period",    sys_snd_period,    { A_INT } },
    [SYS_SND_OPEN]      = { "snd_open",      sys_snd_open,      { 0 }, 0 },
    [SYS_SND_CTL]       = { "snd_ctl",       sys_snd_ctl,       { 0 }, 0 },
    // read()'s buffer isn't filled until the handler runs, and the
    // trace line is formatted before that (see strace.c's top comment),
    // so it prints as a pointer rather than as a string -- same for
    // recv().
    [SYS_READ]          = { "read",          sys_read,          { A_FD, A_HEX, A_INT } },
    [SYS_OPEN]          = { "open",          sys_open,          { A_PATH, A_OFLAGS } },
    [SYS_CLOSE]         = { "close",         sys_close,         { A_FD } },
    [SYS_UNLINK]        = { "unlink",        sys_unlink,        { A_PATH } },
    [SYS_LISTDIR]       = { "listdir",       sys_listdir,       { A_PATH, A_HEX, A_INT } },
    [SYS_LISTDIR_AT]    = { "listdir_at",    sys_listdir_at,    { A_HEX } },
    [SYS_GETTIME]       = { "gettime",       sys_gettime,       { A_HEX } },
    [SYS_SETTIME]       = { "settime",       sys_settime,       { A_INT } },
    [SYS_YIELD]         = { "yield",         sys_yield,         { A_END } },
    [SYS_SOCKET]        = { "socket",        sys_socket,        { A_INT, A_INT, A_INT } },
    [SYS_SEND]          = { "send",          sys_send,          { A_FD, A_BUF, A_INT } },
    [SYS_RECV]          = { "recv",          sys_recv,          { A_FD, A_HEX, A_INT } },
    [SYS_PCI_COUNT]     = { "pci_count",     sys_pci_count,     { A_END } },
    [SYS_PCI_INFO]      = { "pci_info",      sys_pci_info,      { A_INT, A_HEX } },
    [SYS_SET_COLOR]     = { "set_color",     sys_set_color,     { A_INT, A_INT } },
    [SYS_CPU_INFO]      = { "cpu_info",      sys_cpu_info,      { A_HEX } },
    [SYS_POLL_EVENT]    = { "poll_event",    sys_poll_event,    { A_HEX } },
    [SYS_WAIT_EVENT]    = { "wait_event",    sys_wait_event,    { A_HEX } },
    [SYS_GETRANDOM]     = { "getrandom",     sys_getrandom,     { A_HEX, A_INT } },
    [SYS_WIN_REQUEST]   = { "win_request",   sys_win_request,   { A_HEX } },
    [SYS_PIPE]          = { "pipe",          sys_pipe,          { A_HEX } },
    // One pointer now: spawn outgrew three registers when the
    // environment arrived, so it takes a struct (abi/syscall_abi.h).
    [SYS_SPAWN]         = { "spawn",         sys_spawn,         { A_HEX } },
    [SYS_WAITPID]       = { "waitpid",       sys_waitpid,       { A_INT, A_HEX, A_INT } },
    [SYS_PROC_INFO]     = { "proc_info",     sys_proc_info,     { A_INT, A_HEX } },
    [SYS_KILL]          = { "kill",          sys_kill,          { A_INT, A_INT } },
    [SYS_TICKS]         = { "ticks",         sys_ticks,         { A_END } },
    [SYS_MONOTONIC_NS]  = { "monotonic_ns",  sys_monotonic_ns,  { A_END } },
    [SYS_SLEEP]         = { "sleep",         sys_sleep,         { A_INT } },
    [SYS_SETTING]       = { "setting",       sys_setting,       { A_HEX } },
    [SYS_SYSINFO]       = { "sysinfo",       sys_sysinfo,       { A_HEX } },
    [SYS_FS_GENERATION] = { "fs_generation", sys_fs_generation, { A_END } },
    [SYS_FS_GENERATION_OF] = { "fs_generation_of", sys_fs_generation_of, { A_PATH, A_END } },
    [SYS_MKPART]        = { "mkpart",        sys_mkpart,        { A_HEX } },
    [SYS_INSTALL_BOOT]  = { "install_boot",  sys_install_boot,  { A_HEX } },
    [SYS_CRASHTEST]     = { "crashtest",     sys_crashtest,     { A_HEX } },
    [SYS_POWEROFF]      = { "poweroff",      sys_poweroff,      { A_INT } },
    [SYS_DUP]           = { "dup",           sys_dup,           { A_FD } },
    [SYS_DUP2]          = { "dup2",          sys_dup2,          { A_FD, A_FD } },
    [SYS_CONSOLE_SIZE]  = { "console_size",  sys_console_size,  { A_END } },
    [SYS_CHDIR]         = { "chdir",         sys_chdir,         { A_PATH } },
    // The buffer is filled by the handler, so it traces as a pointer --
    // same reason read() and recv() do (strace.c formats the line first).
    [SYS_GETCWD]        = { "getcwd",        sys_getcwd,        { A_HEX, A_INT } },
    [SYS_MKDIR]         = { "mkdir",         sys_mkdir,         { A_PATH } },
    [SYS_RENAME]        = { "rename",        sys_rename,        { A_PATH, A_PATH } },
    [SYS_TRUNCATE]      = { "truncate",      sys_truncate,      { A_PATH, A_INT } },
    [SYS_STAT]          = { "stat",          sys_stat,          { A_PATH, A_HEX } },
    [SYS_LINK]          = { "link",          sys_link,          { A_PATH, A_PATH } },
    [SYS_SYNC]          = { "sync",          sys_sync,          { A_END } },
    [SYS_FSYNC]         = { "fsync",         sys_fsync,         { A_FD } },
    [SYS_QUERY]         = { "query",         sys_query,         { A_HEX } },
    // The offset traces as a signed decimal and the whence as a plain
    // one: SEEK_SET/CUR/END would want a third argument formatter for
    // three values, and `lseek(3, -16, 2)` is already readable.
    [SYS_LSEEK]         = { "lseek",         sys_lseek,         { A_FD, A_INT, A_INT } },
    [SYS_FSTAT]         = { "fstat",         sys_fstat,         { A_FD, A_HEX } },
    [SYS_GETPID]        = { "getpid",        sys_getpid,        { A_END } },
    [SYS_FORK]          = { "fork",          sys_fork,          { A_END } },
    [SYS_EXEC]          = { "exec",          sys_exec,          { A_HEX } },
    [SYS_NOTIFY_READY]  = { "notify_ready",  sys_notify_ready,  { A_END } },
    [SYS_SETPGID]       = { "setpgid",       sys_setpgid,       { A_INT, A_INT } },
    [SYS_SETSID]        = { "setsid",        sys_setsid,        { A_END } },
    [SYS_GETSID]        = { "getsid",        sys_getsid,        { A_INT } },
    [SYS_REMOTE_LOG]    = { "remote_log",    sys_remote_log,    { A_INT, A_HEX, A_PATH } },
    [SYS_GETPGID]       = { "getpgid",       sys_getpgid,       { A_INT } },
    // The signal traces as a plain number: a name would want a third
    // argument formatter for a dozen values, and `sigaction(2, 0x...)`
    // beside abi/signal_abi.h is already readable -- the same call
    // `lseek`'s whence made just above.
    [SYS_SIGACTION]     = { "sigaction",     sys_sigaction,     { A_INT, A_HEX, A_HEX } },
    [SYS_TCSETPGRP]     = { "tcsetpgrp",     sys_tcsetpgrp,     { A_INT, A_INT } },
    [SYS_TCGETPGRP]     = { "tcgetpgrp",     sys_tcgetpgrp,     { A_INT } },
    [SYS_OPENPTY]       = { "openpty",       sys_openpty,       { A_HEX } },
    [SYS_TCGETATTR]     = { "tcgetattr",     sys_tcgetattr,     { A_INT, A_HEX } },
    [SYS_TCSETATTR]     = { "tcsetattr",     sys_tcsetattr,     { A_INT, A_HEX } },
    [SYS_SET_NONBLOCK]  = { "set_nonblock",  sys_set_nonblock,  { A_INT, A_INT } },
    [SYS_TCGETWINSZ]    = { "tcgetwinsz",    sys_tcgetwinsz,    { A_INT, A_HEX } },
    [SYS_TCSETWINSZ]    = { "tcsetwinsz",    sys_tcsetwinsz,    { A_INT, A_HEX } },
    // NO RETURN VALUE TO TRACE, and strace prints one anyway -- whatever
    // RAX holds in the restored frame. That is not a bug to paper over:
    // this call does not return to its caller, so the honest reading of
    // that number is "what the interrupted code is about to see", which
    // is exactly what a person debugging a handler wants.
    [SYS_SIGRETURN]     = { "sigreturn",     sys_sigreturn,     { A_END } },
    [SYS_SIGPROCMASK]   = { "sigprocmask",   sys_sigprocmask,   { A_INT, A_HEX, A_HEX } },
    [SYS_SIGSUSPEND]    = { "sigsuspend",    sys_sigsuspend,    { A_HEX } },
    [SYS_DUPFD]         = { "dupfd",         sys_dupfd,         { A_FD, A_INT } },
    [SYS_FD_CLOEXEC]    = { "fd_cloexec",    sys_fd_cloexec,    { A_FD, A_INT } },
    [SYS_CHMOD]         = { "chmod",         sys_chmod,         { A_PATH, A_INT } },
    [SYS_MKFS]          = { "mkfs",          sys_mkfs,          { A_HEX } },
    [SYS_MOUNT]         = { "mount",         sys_mount,         { A_HEX } },
    [SYS_UMOUNT]        = { "umount",        sys_umount,        { A_PATH } },
    [SYS_THREAD_CREATE] = { "thread_create", sys_thread_create, { A_HEX } },
    [SYS_THREAD_EXIT]   = { "thread_exit",   sys_thread_exit,   { A_INT } },
    [SYS_THREAD_JOIN]   = { "thread_join",   sys_thread_join,   { A_INT } },
    [SYS_THREAD_DETACH] = { "thread_detach", sys_thread_detach, { A_INT } },
    [SYS_GETTID]        = { "gettid",        sys_gettid,        { A_END } },
    [SYS_SET_TLS]       = { "set_tls",       sys_set_tls,       { A_HEX } },
    [SYS_SENDTO]        = { "sendto",        sys_sendto,        { A_FD, A_HEX } },
    [SYS_RECVFROM]      = { "recvfrom",      sys_recvfrom,      { A_FD, A_HEX } },
    [SYS_NET_CONFIG]    = { "net_config",    sys_net_config,    { A_HEX } },
    [SYS_NET_RENAME]    = { "net_rename",    sys_net_rename,    { A_HEX } },
    [SYS_NET_ARP_PROBE] = { "net_arp_probe", sys_net_arp_probe, { A_HEX } },
    [SYS_NET_RESOLVED]  = { "net_resolved",  sys_net_resolved,  { A_HEX } },
    [SYS_BIND]          = { "bind",          sys_bind,          { A_FD, A_HEX } },
    [SYS_CONNECT]       = { "connect",       sys_connect,       { A_FD, A_HEX } },
    [SYS_LISTEN]        = { "listen",        sys_listen,        { A_FD } },
    [SYS_ACCEPT]        = { "accept",        sys_accept,        { A_FD, A_HEX } },
    [SYS_WAIT_READY]    = { "wait_ready",    sys_wait_ready,    { A_INT } },
    [SYS_FS_WATCH]      = { "fs_watch",      sys_fs_watch,      { A_PATH } },
    // 91 was SYS_WIN_CLIP, the kernel's clipboard. The clipboard is a
    // ring-3 service over shared memory now (userland/lib/uclip_page.h);
    // the number is refused rather than reused.
    [91]                = { "win_clip[gone]", sys_removed,      { A_HEX } },
    [SYS_SHM_OPEN]      = { "shm_open",      sys_shm_open,      { A_HEX } },
    [SYS_SHM_UNLINK]    = { "shm_unlink",    sys_shm_unlink,    { A_PATH } },
    [SYS_FUTEX_WAIT]    = { "futex_wait",    sys_futex_wait,    { A_HEX, A_INT, A_INT } },
    [SYS_FUTEX_WAKE]    = { "futex_wake",    sys_futex_wake,    { A_HEX, A_INT } },
    [SYS_WAKEWORD]      = { "wakeword",      sys_wakeword,      { A_HEX } },
    [SYS_SHM_GRANT]     = { "shm_grant",     sys_shm_grant,     { A_PATH, A_INT } },
    [SYS_DIAG]          = { "diag",          sys_diag,          { A_HEX } },
    [SYS_MODLOAD]       = { "modload",       sys_modload,       { A_PATH } },
    [SYS_MODUNLOAD]     = { "modunload",     sys_modunload,     { A_PATH } },
    [SYS_KDFILE]        = { "kdfile",        sys_kdfile,        { A_INT, A_HEX, A_INT } },
};

#define SYSCALL_TABLE_COUNT (sizeof SYSCALL_TABLE / sizeof SYSCALL_TABLE[0])

// THE TWO PER-SYSCALL DIAGNOSTIC TABLES ARE SIZED AGAINST THIS ONE, and
// this is the only place that can check it. Both index by syscall number
// and both silently DROP anything past their end, so a table that falls
// behind reports a clean, plausible, incomplete answer -- which is what
// SCHED_KSTACK_SYSCALL_MAX did for every number from 64 up.
_Static_assert(SYSCALL_TABLE_COUNT <= SCHED_KSTACK_SYSCALL_MAX,
               "SCHED_KSTACK_SYSCALL_MAX is below the syscall table -- "
               "`kstack syscalls` would silently drop the numbers above it");
_Static_assert(SYSCALL_TABLE_COUNT <= SYSCALL_STALL_MAX,
               "SYSCALL_STALL_MAX is below the syscall table -- "
               "`stalls` would silently drop the numbers above it");

// **RETIRED NUMBERS, WHICH ARE NOT FREE NUMBERS.** A syscall that is
// deleted leaves a hole: reusing the number would make an old binary's
// call land on something else, and renumbering everything above it
// would break every other caller to tidy one gap. So the hole stays and
// is DECLARED here, where the table's own KTEST can tell a deliberate
// one from the accident it exists to catch -- a number defined with no
// row, which would look to a caller exactly like a syscall that does not
// exist yet -- which is what it is, and the KTEST keeps the two apart.
static const uint64_t SYSCALL_RETIRED[] = {
    3,   // SYS_GUI_INIT     -- mapped the whole framebuffer to ANY caller
    4,   // SYS_GUI_POLL_KEY -- deleted 2026-09-09 with their one caller
    5,   // SYS_READ_KEY     -- fd 0 blocks now; see abi/syscall_abi.h
    7,   // SYS_WIN_CREATE  -- the kernel composited a window itself
    8,   // SYS_WIN_PRESENT -- deleted 2026-09-08, see abi/syscall_abi.h
    39,  // SYS_WIN_DEBUG   -- deleted 2026-09-09; the `gui` relay became
         //                    the diagnostic registry, SYS_DIAG
};

int syscall_is_retired(uint64_t nr) {
    for (uint64_t i = 0; i < sizeof SYSCALL_RETIRED / sizeof SYSCALL_RETIRED[0]; i++)
        if (SYSCALL_RETIRED[i] == nr) return 1;
    return 0;
}

const struct syscall_desc *syscall_desc_at(uint64_t nr) {
    if (nr >= SYSCALL_TABLE_COUNT) return NULL;
    return &SYSCALL_TABLE[nr];
}
