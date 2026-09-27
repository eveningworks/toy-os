// The GDB Remote Serial Protocol, the subset a kernel stub needs: stop
// reasons, registers, memory, continue/step, breakpoints and
// watchpoints, qOffsets for KASLR, and every process as a THREAD (KGDB
// shows tasks the same way). Everything else gets the empty
// reply, which the protocol defines as "not supported", and GDB falls
// back. Linux's kernel/debug/gdbstub.c is the same shape.
//
// Packets are `$data#xx`, xx the byte sum mod 256, each acknowledged
// with `+` (or `-` to ask again). The acks stay on: a serial line can
// drop bytes, and the ack is how a garbled packet gets resent.
#include "kdebug_internal.h"
#include "reloc.h"     // kernel_reloc_delta() -- qOffsets
#include "barrier.h"   // cpu_relax()
#include "sched_debug.h"  // processes as threads

// PacketSize=1000 (hex) in qSupported: GDB sends nothing longer, and
// sizes its memory reads so a reply fits.
#define KDB_PKT_MAX 4096
static char g_in[KDB_PKT_MAX + 1];
static char g_out[KDB_PKT_MAX + 1];
static int g_outlen;
static uint8_t g_mem[KDB_PKT_MAX / 2];

static const char g_hex[] = "0123456789abcdef";

int kdb_hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

uint8_t kdb_checksum(const char *s, int n) {
    uint8_t sum = 0;
    for (int i = 0; i < n; i++) sum = (uint8_t)(sum + (uint8_t)s[i]);
    return sum;
}

// --- the wire ----------------------------------------------------------

// A datagram transport sends nothing until told: every ack and every
// packet is flushed, or the debugger waits on bytes still in a buffer.
static void flush(void) {
    if (kdb.io->flush) kdb.io->flush();
}

static int getc_wait(void) {
    if (kdb.pushback >= 0) {
        int c = kdb.pushback;
        kdb.pushback = -1;
        return c;
    }
    for (;;) {
        int c = kdb.io->getc();
        if (c >= 0) return c;
        cpu_relax();
    }
}

// The next well-formed packet's data, NUL-terminated. Acks, ^C and noise
// between packets are skipped; a `$` inside one restarts it.
static int get_packet(void) {
    for (;;) {
        while (getc_wait() != '$') {}
    restart:;
        int n = 0, over = 0, c;
        while ((c = getc_wait()) != '#') {
            if (c == '$') goto restart;
            if (n < KDB_PKT_MAX) g_in[n++] = (char)c;
            else over = 1;
        }
        int hi = kdb_hexval(getc_wait()), lo = kdb_hexval(getc_wait());
        if (over || hi < 0 || lo < 0 || ((hi << 4) | lo) != kdb_checksum(g_in, n)) {
            kdb.io->putc('-');
            flush();
            continue;
        }
        kdb.io->putc('+');
        flush();
        g_in[n] = 0;
        return n;
    }
}

// Sends g_out and waits for its ack. A `$` instead means the debugger
// gave up waiting and started a new command: keep it for get_packet().
static void put_packet(void) {
    uint8_t sum = kdb_checksum(g_out, g_outlen);
    for (;;) {
        kdb.io->putc('$');
        for (int i = 0; i < g_outlen; i++) kdb.io->putc(g_out[i]);
        kdb.io->putc('#');
        kdb.io->putc(g_hex[sum >> 4]);
        kdb.io->putc(g_hex[sum & 15]);
        flush();
        for (;;) {
            int c = getc_wait();
            if (c == '+') return;
            if (c == '-') break;
            if (c == '$') { kdb.pushback = c; return; }
        }
    }
}

// --- building replies --------------------------------------------------

static void out_str(const char *s) {
    while (*s && g_outlen < KDB_PKT_MAX) g_out[g_outlen++] = *s++;
}

static void out_byte(uint8_t b) {
    if (g_outlen + 2 > KDB_PKT_MAX) return;
    g_out[g_outlen++] = g_hex[b >> 4];
    g_out[g_outlen++] = g_hex[b & 15];
}

// A number the way GDB writes addresses: big-endian, no leading zeros.
static void out_num(uint64_t v) {
    int shift = 60;
    while (shift > 0 && !((v >> shift) & 15)) shift -= 4;
    for (; shift >= 0; shift -= 4) {
        if (g_outlen < KDB_PKT_MAX) g_out[g_outlen++] = g_hex[(v >> shift) & 15];
    }
}

static int parse_num(const char **p, uint64_t *v) {
    int n = 0;
    *v = 0;
    for (int d; (d = kdb_hexval(**p)) >= 0; (*p)++, n++) *v = (*v << 4) | (uint64_t)d;
    return n;
}

// "addr,len", then `sep` or the end.
static int parse_range(const char **p, uint64_t *addr, uint64_t *len) {
    if (!parse_num(p, addr) || **p != ',') return 0;
    (*p)++;
    return parse_num(p, len) > 0;
}

// --- packets -----------------------------------------------------------

// --- threads: every process, and the kernel context ----------------------

static int current_tid(void) {
    struct sched_debug_thread t;
    for (int n = 0; sched_debug_thread(n, &t); n++)
        if (t.running) return t.tid;
    return SCHED_DEBUG_KERNEL_TID;
}

static void stop_reply(void) {
    out_str("T");
    out_byte((uint8_t)kdb.sig);
    out_str("thread:");
    out_num((uint64_t)current_tid());
    out_str(";");
    if (kdb.watch_kind) {
        out_str(kdb.watch_kind == 'w' ? "watch:" : "awatch:");
        out_num(kdb.watch_addr);
        out_str(";");
    }
}

// A thread that is not the stopped one is read from where it is parked;
// what was never saved goes out as "xx", GDB's "unavailable".
static void read_regs(void) {
    struct sched_debug_thread t;
    const struct kernel_context *k = 0;
    if (kdb.sel_tid && kdb.sel_tid != current_tid()) {
        if (!sched_debug_find(kdb.sel_tid, &t)) { out_str("E01"); return; }
        k = t.kctx;
    }
    int other = kdb.sel_tid && kdb.sel_tid != current_tid();
    for (int n = 0; n < KDB_NREGS; n++) {
        int have = 1;
        uint64_t v = !other ? kdb_arch_reg_get(kdb.regs, n)
                   : k ? kdb_arch_ctx_reg(k, n, &have) : (have = 0, 0);
        for (int i = 0; i < kdb_arch_reg_size(n); i++) {
            if (have) out_byte((uint8_t)(v >> (i * 8)));
            else out_str("xx");
        }
    }
}

static void write_regs(const char *p) {
    if (kdb.sel_tid && kdb.sel_tid != current_tid()) {
        out_str("E01");   // a parked thread's registers are the scheduler's
        return;
    }
    for (int n = 0; n < KDB_NREGS; n++) {
        uint64_t v = 0;
        int size = kdb_arch_reg_size(n);
        for (int i = 0; i < size; i++, p += 2) {
            int hi = kdb_hexval(p[0]), lo = hi < 0 ? -1 : kdb_hexval(p[1]);
            if (lo < 0) { out_str("OK"); return; }   // a short G sets what it carries
            v |= (uint64_t)((hi << 4) | lo) << (i * 8);
        }
        kdb_arch_reg_set(kdb.regs, n, v);
    }
    out_str("OK");
}

// Memory is read in the SELECTED thread's address space: `thread N` then
// `x` on a user address means that process's memory, not the stopped one's.
static uint64_t sel_space(void) {
    struct sched_debug_thread t;
    if (!kdb.sel_tid || kdb.sel_tid == current_tid() || !sched_debug_find(kdb.sel_tid, &t))
        return 0;
    return t.pml4;
}

static void read_mem(const char *p) {
    uint64_t addr, len;
    if (!parse_range(&p, &addr, &len)) { out_str("E01"); return; }
    if (len > sizeof g_mem) len = sizeof g_mem;
    uint64_t got = kdb_arch_mem_read_in(sel_space(), addr, g_mem, len);
    if (!got && len) { out_str("E14"); return; }
    for (uint64_t i = 0; i < got; i++) out_byte(g_mem[i]);
}

static void write_mem(const char *p) {
    uint64_t addr, len;
    if (!parse_range(&p, &addr, &len) || *p != ':' || len > sizeof g_mem) {
        out_str("E01");
        return;
    }
    p++;
    for (uint64_t i = 0; i < len; i++, p += 2) {
        int hi = kdb_hexval(p[0]), lo = hi < 0 ? -1 : kdb_hexval(p[1]);
        if (lo < 0) { out_str("E01"); return; }
        g_mem[i] = (uint8_t)((hi << 4) | lo);
    }
    out_str(kdb_arch_mem_write_in(sel_space(), addr, g_mem, len) == len ? "OK" : "E14");
}

// Z/z type,addr,kind
static void breakpoint(const char *p) {
    int insert = p[0] == 'Z', type = kdb_hexval(p[1]);
    uint64_t addr, len;
    p += 2;
    if (type < 0 || *p++ != ',' || !parse_range(&p, &addr, &len)) { out_str("E01"); return; }
    int r = insert ? kdb_bp_insert(type, addr, (int)len) : kdb_bp_remove(type, addr, (int)len);
    if (r > 0) out_str("OK");
    else if (r == 0) out_str("E22");
    // r < 0: the empty reply, "not supported", and GDB says so itself
}

static int starts(const char *s, const char *prefix) {
    while (*prefix) if (*s++ != *prefix++) return 0;
    return 1;
}

static const char *state_name(const struct sched_debug_thread *t) {
    if (t->running) return "running";
    if (t->stopped) return "stopped";
    switch (t->state) {   // enum sched_state
    case 1:  return "ready";
    case 2:  return "running";
    case 4:  return "blocked";
    case -1: return "parked";
    default: return "?";
    }
}

static void thread_query(const char *q) {
    struct sched_debug_thread t;
    if (starts(q, "fThreadInfo")) {
        out_str("m");
        for (int n = 0; sched_debug_thread(n, &t); n++) {
            if (n) out_str(",");
            out_num((uint64_t)t.tid);
        }
    } else if (starts(q, "sThreadInfo")) {
        out_str("l");   // the whole list went in the first reply
    } else if (starts(q, "ThreadExtraInfo,")) {
        const char *p = q + 16;
        uint64_t tid;
        if (!parse_num(&p, &tid) || !sched_debug_find((int)tid, &t)) { out_str("E01"); return; }
        // Hex-encoded text: "toywm, blocked".
        for (const char *s = t.name; *s; s++) out_byte((uint8_t)*s);
        out_byte(','); out_byte(' ');
        for (const char *s = state_name(&t); *s; s++) out_byte((uint8_t)*s);
    }
}

// Hg<tid> picks the thread `g` reads; 0 and -1 mean the stopped one.
// Hc is accepted and changes nothing: a stop is the whole machine.
static void set_thread(const char *p) {
    if (p[0] != 'g') { out_str("OK"); return; }
    p++;
    if (p[0] == '-' || (p[0] == '0' && p[1] == 0)) { kdb.sel_tid = 0; out_str("OK"); return; }
    uint64_t tid;
    struct sched_debug_thread t;
    if (!parse_num(&p, &tid) || !sched_debug_find((int)tid, &t)) { out_str("E01"); return; }
    kdb.sel_tid = (int)tid;
    out_str("OK");
}

static void thread_alive(const char *p) {
    uint64_t tid;
    struct sched_debug_thread t;
    out_str(parse_num(&p, &tid) && sched_debug_find((int)tid, &t) ? "OK" : "E01");
}

static void query(const char *q) {
    if (q[0] == 'C' && q[1] == 0) {
        out_str("QC");
        out_num((uint64_t)current_tid());
    } else if (starts(q, "fThreadInfo") || starts(q, "sThreadInfo") ||
               starts(q, "ThreadExtraInfo,")) {
        thread_query(q);
    } else if (starts(q, "Supported")) {
        out_str("PacketSize=1000");
    } else if (starts(q, "Offsets")) {
        // KASLR: the whole image moved by one delta, so GDB relocates the
        // symbols of an unmodified build/kernel.bin to match.
        uint64_t d = kernel_reloc_delta();
        out_str("Text="); out_num(d);
        out_str(";Data="); out_num(d);
        out_str(";Bss="); out_num(d);
    } else if (starts(q, "Attached")) {
        out_str("1");   // detaching must not kill anything
    } else if (starts(q, "Symbol")) {
        out_str("OK");
    }
}

// -1: reply and keep talking. Otherwise how to resume (with a reply
// only if one was built -- `c`, `s` and `k` have none).
static int dispatch(const char *p) {
    uint64_t addr;
    const char *a;
    switch (p[0]) {
    case '?': stop_reply(); return -1;
    case 'g': read_regs(); return -1;
    case 'G': write_regs(p + 1); return -1;
    case 'm': read_mem(p + 1); return -1;
    case 'M': write_mem(p + 1); return -1;
    case 'Z': case 'z': breakpoint(p); return -1;
    case 'H': set_thread(p + 1); return -1;
    case 'T': thread_alive(p + 1); return -1;
    case 'q': query(p + 1); return -1;
    case 'c': case 's':
        a = p + 1;
        if (parse_num(&a, &addr)) kdb_arch_set_pc(kdb.regs, addr);
        return p[0] == 's' ? KDB_STEP : KDB_CONTINUE;
    case 'D': out_str("OK"); return KDB_DETACH;
    case 'k': return KDB_DETACH;   // a kernel is not killed by its debugger
    default: return -1;
    }
}

enum kdb_resume kdb_gdb_session(void) {
    kdb.sel_tid = 0;
    // A debugger that is waiting on `c` needs to be told; one that has
    // not attached yet asks with `?` when it does.
    if (kdb.connected) {
        g_outlen = 0;
        stop_reply();
        put_packet();
    }
    for (;;) {
        get_packet();
        kdb.connected = 1;
        g_outlen = 0;
        int r = dispatch(g_in);
        if (r < 0 || g_outlen) put_packet();
        if (r >= 0) return (enum kdb_resume)r;
    }
}
