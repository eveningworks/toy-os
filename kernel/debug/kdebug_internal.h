#ifndef KDEBUG_INTERNAL_H
#define KDEBUG_INTERNAL_H

// State shared by the debugger's core (kdebug.c), its GDB protocol
// (gdbstub.c) and its KTESTs. Nothing outside kernel/debug/ includes it.

#include <stdint.h>
#include "kdebug_arch.h"

// WHERE THE BYTES GO: serial, the network (kdebug_net.c), and the
// KTEST's script. getc() never blocks: -1 means nothing yet. flush()
// pushes out what putc() buffered -- a datagram -- and may be NULL.
struct kdb_transport {
    int  (*getc)(void);
    void (*putc)(char c);
    void (*flush)(void);
};

// GDB's own signal numbers, which are not Linux's past 9.
#define KDB_SIGINT  2
#define KDB_SIGILL  4
#define KDB_SIGTRAP 5
#define KDB_SIGABRT 6
#define KDB_SIGFPE  8
#define KDB_SIGBUS  10
#define KDB_SIGSEGV 11

enum kdb_resume { KDB_CONTINUE, KDB_STEP, KDB_DETACH };

// Software breakpoints are PATCHED ONLY WHILE THE KERNEL RUNS: lifted on
// every stop, written back on every resume. So memory reads while stopped
// show the real bytes, and the debugger's own code -- the serial poll a
// breakpoint could land in -- never meets one while it is talking.
#define KDB_SWBP_MAX 32
struct kdb_swbp {
    uint64_t addr;
    uint8_t  saved, used, patched;
};

struct kdb_state {
    int armed;                  // `kdebug=` claimed a port at boot
    int active;                 // stopped, talking to the debugger
    int connected;              // a debugger has spoken since the last detach
    int stepping;               // a single step is outstanding
    int fatal_seen;             // already stopped for this panic
    int pending_sig;            // what a compiled-in int3 reports, or 0
    int pushback;               // a byte read ahead by the poll, or -1
    const struct kdb_transport *io;
    uint64_t *regs;             // the stopped frame
    int sig;                    // why it stopped
    int sel_tid;                // the thread `g` reads (Hg); 0 = the one that stopped
    char watch_kind;            // 'w'/'a' when a watchpoint fired, else 0
    uint64_t watch_addr;
    uint32_t stops;
    struct kdb_swbp sw[KDB_SWBP_MAX];
    struct kdb_hw hw[KDB_HW_SLOTS];
};
extern struct kdb_state kdb;

// kdebug.c. insert: 1 done, 0 refused, -1 a type this CPU cannot do.
int  kdb_bp_insert(int type, uint64_t addr, int len);
int  kdb_bp_remove(int type, uint64_t addr, int len);
void kdb_bp_clear_all(void);

// kdebug_net.c: `kdebug=net,ip=A.B.C.D,key=HEX[,port=N][,nic=BB:DD.F][,wait]`.
#define KDB_NET_KEY_MIN 16
#define KDB_NET_KEY_MAX 64
struct kdb_net_cfg {
    uint32_t ip;                   // host byte order
    uint16_t port;
    uint8_t  key[KDB_NET_KEY_MAX];
    int      klen;
    int      wait;
    int      nic_bus, nic_dev, nic_fn;   // -1: the last card a backend matches
};
int kdb_net_parse(const char *v, struct kdb_net_cfg *c);   // 1 when usable
const struct kdb_transport *kdb_net_init(const char *v, int *wait);

// The wire: magic, a little-endian sequence number, the first 16 bytes of
// HMAC-SHA256(key, magic | seq | payload), then the payload -- RSP bytes.
// A host datagram must carry a sequence number above every one accepted
// before it, so a captured datagram cannot be replayed.
#define KDB_NET_HDR 28
void kdb_net_configure(const struct kdb_net_cfg *c);   // key + fresh sequence state
int  kdb_net_seal(const char magic[4], uint64_t seq, const uint8_t *payload, int len,
                  uint8_t *out, int cap);
int  kdb_net_open(const uint8_t *dgram, int len, const uint8_t **payload);
void kdb_hmac_sha256(const uint8_t *key, int klen, const uint8_t *a, int alen,
                     const uint8_t *b, int blen, uint8_t out[32]);

// kdebug_files.c: `remote put`. The stage_* calls are the STOPPED half --
// bytes into RAM, nothing else; kick() wakes /bin/kdfiled after resume.
void    kdb_files_init(void);
int     kdb_stage_open(const char *path);                  // fd, or -errno
int64_t kdb_stage_write(int fd, uint64_t off, const uint8_t *data, uint32_t len);
int     kdb_stage_close(int fd);   // -EIO, and the file dropped, if any write failed
void    kdb_stage_fail(int fd);    // a write gdb sent could not be applied
void    kdb_stage_kick(void);

// gdbstub.c: talk until the debugger resumes the machine.
enum kdb_resume kdb_gdb_session(void);
uint8_t kdb_checksum(const char *s, int n);
int     kdb_hexval(int c);

#endif
