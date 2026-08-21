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
// dispatch no-ops on it, which is what the if/else chain's
// fall-through did, and `strace` prints it as `syscall_<n>` with hex
// arguments rather than hiding it.
static const struct syscall_desc SYSCALL_TABLE[] = {
    [SYS_EXIT]          = { "exit",          sys_exit,          { A_INT } },
    [SYS_WRITE]         = { "write",         sys_write,         { A_FD, A_BUF, A_INT } },
    [SYS_GUI_INIT]      = { "gui_init",      sys_gui_init,      { A_HEX } },
    [SYS_GUI_POLL_KEY]  = { "gui_poll_key",  sys_gui_poll_key,  { A_END } },
    [SYS_READ_KEY]      = { "read_key",      sys_read_key,      { A_END } },
    // The one syscall returning a pointer rather than a count/status --
    // and the reason a handler reports "I parked" separately from its
    // return value, since no 64-bit value is free to mean anything else.
    [SYS_SBRK]          = { "sbrk",          sys_sbrk,          { A_INT }, R_HEX },
    [SYS_WIN_CREATE]    = { "win_create",    sys_win_create,    { A_HEX } },
    [SYS_WIN_PRESENT]   = { "win_present",   sys_win_present,   { A_END } },
    // read()'s buffer isn't filled until the handler runs, and the
    // trace line is formatted before that (see strace.c's top comment),
    // so it prints as a pointer rather than as a string -- same for
    // recv().
    [SYS_READ]          = { "read",          sys_read,          { A_FD, A_HEX, A_INT } },
    [SYS_OPEN]          = { "open",          sys_open,          { A_PATH, A_OFLAGS } },
    [SYS_CLOSE]         = { "close",         sys_close,         { A_FD } },
    [SYS_UNLINK]        = { "unlink",        sys_unlink,        { A_PATH } },
    [SYS_LISTDIR]       = { "listdir",       sys_listdir,       { A_PATH, A_HEX, A_INT } },
    [SYS_GETTIME]       = { "gettime",       sys_gettime,       { A_HEX } },
    [SYS_YIELD]         = { "yield",         sys_yield,         { A_END } },
    [SYS_SOCKET]        = { "socket",        sys_socket,        { A_INT, A_INT } },
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
    [SYS_CRASHTEST]     = { "crashtest",     sys_crashtest,     { A_HEX } },
    [SYS_POWEROFF]      = { "poweroff",      sys_poweroff,      { A_INT } },
    [SYS_WIN_DEBUG]     = { "win_debug",     sys_win_debug,     { A_HEX } },
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
    [SYS_QUERY]         = { "query",         sys_query,         { A_HEX } },
    // The offset traces as a signed decimal and the whence as a plain
    // one: SEEK_SET/CUR/END would want a third argument formatter for
    // three values, and `lseek(3, -16, 2)` is already readable.
    [SYS_LSEEK]         = { "lseek",         sys_lseek,         { A_FD, A_INT, A_INT } },
    [SYS_FSTAT]         = { "fstat",         sys_fstat,         { A_FD, A_HEX } },
    [SYS_GETPID]        = { "getpid",        sys_getpid,        { A_END } },
};

#define SYSCALL_TABLE_COUNT (sizeof SYSCALL_TABLE / sizeof SYSCALL_TABLE[0])

const struct syscall_desc *syscall_desc_at(uint64_t nr) {
    if (nr >= SYSCALL_TABLE_COUNT) return NULL;
    return &SYSCALL_TABLE[nr];
}
