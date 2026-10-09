// THE SYSCALL ROWS: one line per syscall -- number, name, handler, the
// three argument kinds and the return kind (abi/syscall_meta.h) -- for
// BOTH rings. Deliberately no include guard: an X-macro list, expanded
// by whoever includes it with SYSCALL_ROW() defined --
// kernel/proc/syscall_table.c builds the dispatch table with the
// handler, userland/lib/utrace.c the decoder's names without it (a
// macro argument that is never used is never looked up). So a
// syscall's name, handler and decoding cannot drift apart, in either
// ring: they are one row.
//
// Adding a syscall: its number in abi/syscall_abi.h, its handler with
// the subsystem that owns it (declared in syscalls.h), and a row here.
// Designated initializers in the expansion make a duplicate number an
// error (-Woverride-init) and an undefined SYS_* fail to compile.
#ifndef SYSCALL_ROW
#error "define SYSCALL_ROW(nr, name, fn, a0, a1, a2, ret) before including syscall_rows.h"
#endif

SYSCALL_ROW(SYS_EXIT, "exit", sys_exit, A_INT, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_WRITE, "write", sys_write, A_FD, A_BUF, A_INT, R_DEC)
// The one syscall returning a pointer rather than a count/status --
// and the reason a handler reports "I parked" separately from its
// return value, since no 64-bit value is free to mean anything else.
SYSCALL_ROW(SYS_SBRK, "sbrk", sys_sbrk, A_INT, A_END, A_END, R_HEX)
// mmap returns a pointer too, so R_HEX; its errors are the small
// negatives every libc wrapper already tests for.
SYSCALL_ROW(SYS_MMAP, "mmap", sys_mmap, A_HEX, A_END, A_END, R_HEX)
SYSCALL_ROW(SYS_MUNMAP, "munmap", sys_munmap, A_HEX, A_INT, A_END, R_DEC)
SYSCALL_ROW(SYS_DEV_MAP_BAR, "dev_map_bar", sys_dev_map_bar, A_INT, A_INT, A_END, R_HEX)
SYSCALL_ROW(SYS_DEV_CLAIM, "dev_claim", sys_dev_claim, A_INT, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_DEV_RELEASE, "dev_release", sys_dev_release, A_INT, A_HEX, A_END, R_DEC)
SYSCALL_ROW(SYS_DEV_DMA_ALLOC, "dev_dma_alloc", sys_dev_dma_alloc, A_INT, A_INT, A_HEX, R_HEX)
SYSCALL_ROW(SYS_DEV_IRQ_ENABLE, "dev_irq_enable", sys_dev_irq_enable, A_INT, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_DEV_IRQ_ACK, "dev_irq_ack", sys_dev_irq_ack, A_INT, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_DEV_IO, "dev_io", sys_dev_io, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_USB_CLAIM, "usb_claim", sys_usb_claim, A_INT, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_USB_RELEASE, "usb_release", sys_usb_release, A_INT, A_INT, A_END, R_DEC)
SYSCALL_ROW(SYS_USB_CONTROL, "usb_control", sys_usb_control, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_USB_ISOCH_OPEN, "usb_isoch_open", sys_usb_isoch_open, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_USB_ISOCH_POST, "usb_isoch_post", sys_usb_isoch_post, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_USB_ISOCH_STATUS, "usb_isoch_status", sys_usb_isoch_status, A_INT, A_INT, A_END, R_DEC)
SYSCALL_ROW(SYS_SND_RING_MAP, "snd_ring_map", sys_snd_ring_map, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_SETPRIORITY, "setpriority", sys_setpriority, A_INT, A_INT, A_INT, R_DEC)
SYSCALL_ROW(SYS_GETPRIORITY, "getpriority", sys_getpriority, A_INT, A_INT, A_END, R_DEC)
SYSCALL_ROW(SYS_SND_REGISTER, "snd_register", sys_snd_register, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_SND_PERIOD, "snd_period", sys_snd_period, A_INT, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_SND_OPEN, "snd_open", sys_snd_open, A_END, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_SND_CTL, "snd_ctl", sys_snd_ctl, A_END, A_END, A_END, R_DEC)
// read()'s buffer isn't filled until the handler runs, and the
// trace line is formatted before that (see strace.c's top comment),
// so it prints as a pointer rather than as a string -- same for
// recv().
SYSCALL_ROW(SYS_READ, "read", sys_read, A_FD, A_HEX, A_INT, R_DEC)
SYSCALL_ROW(SYS_OPEN, "open", sys_open, A_PATH, A_OFLAGS, A_END, R_DEC)
SYSCALL_ROW(SYS_CLOSE, "close", sys_close, A_FD, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_UNLINK, "unlink", sys_unlink, A_PATH, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_LISTDIR, "listdir", sys_listdir, A_PATH, A_HEX, A_INT, R_DEC)
SYSCALL_ROW(SYS_LISTDIR_AT, "listdir_at", sys_listdir_at, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_GETTIME, "gettime", sys_gettime, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_SETTIME, "settime", sys_settime, A_INT, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_YIELD, "yield", sys_yield, A_END, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_SOCKET, "socket", sys_socket, A_INT, A_INT, A_INT, R_DEC)
SYSCALL_ROW(SYS_SEND, "send", sys_send, A_FD, A_BUF, A_INT, R_DEC)
SYSCALL_ROW(SYS_RECV, "recv", sys_recv, A_FD, A_HEX, A_INT, R_DEC)
SYSCALL_ROW(SYS_PCI_COUNT, "pci_count", sys_pci_count, A_END, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_PCI_INFO, "pci_info", sys_pci_info, A_INT, A_HEX, A_END, R_DEC)
SYSCALL_ROW(SYS_SET_COLOR, "set_color", sys_set_color, A_INT, A_INT, A_END, R_DEC)
SYSCALL_ROW(SYS_CPU_INFO, "cpu_info", sys_cpu_info, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_POLL_EVENT, "poll_event", sys_poll_event, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_WAIT_EVENT, "wait_event", sys_wait_event, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_GETRANDOM, "getrandom", sys_getrandom, A_HEX, A_INT, A_END, R_DEC)
SYSCALL_ROW(SYS_WIN_REQUEST, "win_request", sys_win_request, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_PIPE, "pipe", sys_pipe, A_HEX, A_END, A_END, R_DEC)
// One pointer now: spawn outgrew three registers when the
// environment arrived, so it takes a struct (abi/syscall_abi.h).
SYSCALL_ROW(SYS_SPAWN, "spawn", sys_spawn, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_WAITPID, "waitpid", sys_waitpid, A_INT, A_HEX, A_INT, R_DEC)
SYSCALL_ROW(SYS_PROC_INFO, "proc_info", sys_proc_info, A_INT, A_HEX, A_END, R_DEC)
SYSCALL_ROW(SYS_KILL, "kill", sys_kill, A_INT, A_INT, A_END, R_DEC)
SYSCALL_ROW(SYS_TICKS, "ticks", sys_ticks, A_END, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_MONOTONIC_NS, "monotonic_ns", sys_monotonic_ns, A_END, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_SLEEP, "sleep", sys_sleep, A_INT, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_SETTING, "setting", sys_setting, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_SYSINFO, "sysinfo", sys_sysinfo, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_FS_GENERATION, "fs_generation", sys_fs_generation, A_END, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_FS_GENERATION_OF, "fs_generation_of", sys_fs_generation_of, A_PATH, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_MKPART, "mkpart", sys_mkpart, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_INSTALL_BOOT, "install_boot", sys_install_boot, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_CRASHTEST, "crashtest", sys_crashtest, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_POWEROFF, "poweroff", sys_poweroff, A_INT, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_DUP, "dup", sys_dup, A_FD, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_DUP2, "dup2", sys_dup2, A_FD, A_FD, A_END, R_DEC)
SYSCALL_ROW(SYS_CONSOLE_SIZE, "console_size", sys_console_size, A_END, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_CHDIR, "chdir", sys_chdir, A_PATH, A_END, A_END, R_DEC)
// The buffer is filled by the handler, so it traces as a pointer --
// same reason read() and recv() do (strace.c formats the line first).
SYSCALL_ROW(SYS_GETCWD, "getcwd", sys_getcwd, A_HEX, A_INT, A_END, R_DEC)
SYSCALL_ROW(SYS_MKDIR, "mkdir", sys_mkdir, A_PATH, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_RENAME, "rename", sys_rename, A_PATH, A_PATH, A_END, R_DEC)
SYSCALL_ROW(SYS_TRUNCATE, "truncate", sys_truncate, A_PATH, A_INT, A_END, R_DEC)
SYSCALL_ROW(SYS_STAT, "stat", sys_stat, A_PATH, A_HEX, A_END, R_DEC)
SYSCALL_ROW(SYS_LINK, "link", sys_link, A_PATH, A_PATH, A_END, R_DEC)
SYSCALL_ROW(SYS_SYNC, "sync", sys_sync, A_END, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_FSYNC, "fsync", sys_fsync, A_FD, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_QUERY, "query", sys_query, A_HEX, A_END, A_END, R_DEC)
// The offset traces as a signed decimal and the whence as a plain
// one: SEEK_SET/CUR/END would want a third argument formatter for
// three values, and `lseek(3, -16, 2)` is already readable.
SYSCALL_ROW(SYS_LSEEK, "lseek", sys_lseek, A_FD, A_INT, A_INT, R_DEC)
SYSCALL_ROW(SYS_FSTAT, "fstat", sys_fstat, A_FD, A_HEX, A_END, R_DEC)
SYSCALL_ROW(SYS_GETPID, "getpid", sys_getpid, A_END, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_FORK, "fork", sys_fork, A_END, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_EXEC, "exec", sys_exec, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_NOTIFY_READY, "notify_ready", sys_notify_ready, A_END, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_SETPGID, "setpgid", sys_setpgid, A_INT, A_INT, A_END, R_DEC)
SYSCALL_ROW(SYS_SETSID, "setsid", sys_setsid, A_END, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_GETSID, "getsid", sys_getsid, A_INT, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_REMOTE_LOG, "remote_log", sys_remote_log, A_INT, A_HEX, A_PATH, R_DEC)
SYSCALL_ROW(SYS_GETPGID, "getpgid", sys_getpgid, A_INT, A_END, A_END, R_DEC)
// The signal traces as a plain number: a name would want a third
// argument formatter for a dozen values, and `sigaction(2, 0x...)`
// beside abi/signal_abi.h is already readable -- the same call
// `lseek`'s whence made just above.
SYSCALL_ROW(SYS_SIGACTION, "sigaction", sys_sigaction, A_INT, A_HEX, A_HEX, R_DEC)
SYSCALL_ROW(SYS_TCSETPGRP, "tcsetpgrp", sys_tcsetpgrp, A_INT, A_INT, A_END, R_DEC)
SYSCALL_ROW(SYS_TCGETPGRP, "tcgetpgrp", sys_tcgetpgrp, A_INT, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_OPENPTY, "openpty", sys_openpty, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_TCGETATTR, "tcgetattr", sys_tcgetattr, A_INT, A_HEX, A_END, R_DEC)
SYSCALL_ROW(SYS_TCSETATTR, "tcsetattr", sys_tcsetattr, A_INT, A_HEX, A_END, R_DEC)
SYSCALL_ROW(SYS_SET_NONBLOCK, "set_nonblock", sys_set_nonblock, A_INT, A_INT, A_END, R_DEC)
SYSCALL_ROW(SYS_TCGETWINSZ, "tcgetwinsz", sys_tcgetwinsz, A_INT, A_HEX, A_END, R_DEC)
SYSCALL_ROW(SYS_TCSETWINSZ, "tcsetwinsz", sys_tcsetwinsz, A_INT, A_HEX, A_END, R_DEC)
// NO RETURN VALUE TO TRACE, and strace prints one anyway -- whatever
// RAX holds in the restored frame. That is not a bug to paper over:
// this call does not return to its caller, so the honest reading of
// that number is "what the interrupted code is about to see", which
// is exactly what a person debugging a handler wants.
SYSCALL_ROW(SYS_SIGRETURN, "sigreturn", sys_sigreturn, A_END, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_SIGPROCMASK, "sigprocmask", sys_sigprocmask, A_INT, A_HEX, A_HEX, R_DEC)
SYSCALL_ROW(SYS_SIGSUSPEND, "sigsuspend", sys_sigsuspend, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_DUPFD, "dupfd", sys_dupfd, A_FD, A_INT, A_END, R_DEC)
SYSCALL_ROW(SYS_FD_CLOEXEC, "fd_cloexec", sys_fd_cloexec, A_FD, A_INT, A_END, R_DEC)
SYSCALL_ROW(SYS_CHMOD, "chmod", sys_chmod, A_PATH, A_INT, A_END, R_DEC)
SYSCALL_ROW(SYS_MKFS, "mkfs", sys_mkfs, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_MOUNT, "mount", sys_mount, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_UMOUNT, "umount", sys_umount, A_PATH, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_THREAD_CREATE, "thread_create", sys_thread_create, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_THREAD_EXIT, "thread_exit", sys_thread_exit, A_INT, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_THREAD_JOIN, "thread_join", sys_thread_join, A_INT, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_THREAD_DETACH, "thread_detach", sys_thread_detach, A_INT, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_GETTID, "gettid", sys_gettid, A_END, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_SET_TLS, "set_tls", sys_set_tls, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_SENDTO, "sendto", sys_sendto, A_FD, A_HEX, A_END, R_DEC)
SYSCALL_ROW(SYS_RECVFROM, "recvfrom", sys_recvfrom, A_FD, A_HEX, A_END, R_DEC)
SYSCALL_ROW(SYS_NET_CONFIG, "net_config", sys_net_config, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_NET_RENAME, "net_rename", sys_net_rename, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_NET_ARP_PROBE, "net_arp_probe", sys_net_arp_probe, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_NET_RESOLVED, "net_resolved", sys_net_resolved, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_BIND, "bind", sys_bind, A_FD, A_HEX, A_END, R_DEC)
SYSCALL_ROW(SYS_CONNECT, "connect", sys_connect, A_FD, A_HEX, A_END, R_DEC)
SYSCALL_ROW(SYS_LISTEN, "listen", sys_listen, A_FD, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_ACCEPT, "accept", sys_accept, A_FD, A_HEX, A_END, R_DEC)
SYSCALL_ROW(SYS_WAIT_READY, "wait_ready", sys_wait_ready, A_INT, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_FS_WATCH, "fs_watch", sys_fs_watch, A_PATH, A_END, A_END, R_DEC)
// 91 was SYS_WIN_CLIP, the kernel's clipboard. The clipboard is a
// ring-3 service over shared memory now (userland/lib/uclip_page.h);
// the number is refused rather than reused.
SYSCALL_ROW(91, "win_clip[gone]", sys_removed, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_SHM_OPEN, "shm_open", sys_shm_open, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_SHM_UNLINK, "shm_unlink", sys_shm_unlink, A_PATH, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_FUTEX_WAIT, "futex_wait", sys_futex_wait, A_HEX, A_INT, A_INT, R_DEC)
SYSCALL_ROW(SYS_FUTEX_WAKE, "futex_wake", sys_futex_wake, A_HEX, A_INT, A_END, R_DEC)
SYSCALL_ROW(SYS_WAKEWORD, "wakeword", sys_wakeword, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_SHM_GRANT, "shm_grant", sys_shm_grant, A_PATH, A_INT, A_END, R_DEC)
SYSCALL_ROW(SYS_DIAG, "diag", sys_diag, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_MODLOAD, "modload", sys_modload, A_PATH, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_MODUNLOAD, "modunload", sys_modunload, A_PATH, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_KDFILE, "kdfile", sys_kdfile, A_INT, A_HEX, A_INT, R_DEC)
SYSCALL_ROW(SYS_RENAME2, "rename2", sys_rename2, A_PATH, A_PATH, A_HEX, R_DEC)
SYSCALL_ROW(SYS_FS_CHECK, "fs_check", sys_fs_check, A_PATH, A_HEX, A_HEX, R_DEC)
SYSCALL_ROW(SYS_NET_LINK, "net_link", sys_net_link, A_HEX, A_END, A_END, R_DEC)
SYSCALL_ROW(SYS_INPUT_INJECT, "input_inject", sys_input_inject, A_HEX, A_INT, A_END, R_DEC)
