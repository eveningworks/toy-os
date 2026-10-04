// uchan -- see lib/uchan.h for what this is and lib/uchan_page.h for the
// layout. Nothing here is a syscall of its own: a channel is named
// shared memory plus a futex, both of which exist for other reasons.
#include "lib/uchan.h"
#include "rt/sys.h"
#include "syscall_abi.h"
#include "query_abi.h"
#include <string.h>
#include <stdio.h>

#define RING_BYTES ((uint64_t)sizeof(struct uchan_ring))

// **AN UNMAP IS THE WHOLE REGION OR NOTHING.** sys_munmap() refuses a
// shared mapping that is not exactly the page-rounded region, and it
// refuses SILENTLY to a caller that ignores the return -- so an unmap
// with the object's byte size (20 for a beacon) leaked the mapping
// every time, and a client that probed the beacon every half second
// ran its address space out of mappings and read that as the server
// having died. Every unmap here goes through this.
static void unmap(void *p, uint64_t bytes) {
    if (p) sys_munmap(p, (bytes + 4095) & ~4095ull);
}

// A BEACON is public: every client has to be able to find it. Its rings
// are not -- see uchan_client_open().
static void *map_public(const char *name, uint64_t bytes) {
    int fd = sys_shm_open(name, bytes, SHM_CREATE | SHM_PUBLIC);
    if (fd < 0) return 0;
    void *p = sys_mmap(0, bytes, SYS_PROT_READ | SYS_PROT_WRITE,
                       SYS_MAP_SHARED, fd, 0);
    sys_close(fd);
    return p == (void *)-1 ? 0 : p;
}

static void *map_object(const char *name, uint64_t bytes, int create) {
    int fd = sys_shm_open(name, create ? bytes : 0, create ? SHM_CREATE : 0);
    if (fd < 0) return 0;
    void *p = sys_mmap(0, bytes, SYS_PROT_READ | SYS_PROT_WRITE,
                       SYS_MAP_SHARED, fd, 0);
    sys_close(fd);
    return p == (void *)-1 ? 0 : p;
}

// --- server ----------------------------------------------------------

int uchan_server_open(struct uchan_server *s, const char *name) {
    memset(s, 0, sizeof *s);
    if (!name || !*name || strlen(name) >= UCHAN_NAME_MAX) return -1;
    snprintf(s->name, sizeof s->name, "%s", name);

    // Unlink first: a previous server that died left its beacon behind,
    // and a client opening THAT would wake a pid that no longer exists.
    sys_shm_unlink(name);
    s->beacon = map_public(name, sizeof(struct uchan_beacon));
    if (!s->beacon) return -1;

    s->beacon->wake = 0;
    s->beacon->server_pid = sys_getpid();
    s->beacon->version = 1;
    // LAST, so a client that sees the magic sees a page that is ready.
    s->beacon->magic = UCHAN_MAGIC;

    // The kernel bumps this same word when it queues an event, which is
    // what lets one wait cover clients AND input.
    sys_wakeword(&s->beacon->wake);
    return 0;
}

static int ring_index(struct uchan_server *s, int pid) {
    for (int i = 0; i < s->count; i++) if (s->pid[i] == pid) return i;
    return -1;
}

int uchan_server_scan(struct uchan_server *s, int *gone, int gone_cap) {
    int ngone = 0;
    if (!s->beacon) return 0;
    char prefix[UCHAN_NAME_MAX + 2];
    snprintf(prefix, sizeof prefix, "%s.", s->name);
    size_t plen = strlen(prefix);

    struct query_shm rec;
    QUERY_FOREACH(QUERY_SHM, rec, qi) {
        if (rec.flags & QUERY_SHM_UNLINKED) continue;
        if (strncmp(rec.name, prefix, plen) != 0) continue;
        int pid = 0;
        for (const char *p = rec.name + plen; *p >= '0' && *p <= '9'; p++)
            pid = pid * 10 + (*p - '0');
        if (pid <= 0 || ring_index(s, pid) >= 0) continue;
        if ((int)(sizeof s->ring / sizeof s->ring[0]) <= s->count) continue;

        struct uchan_ring *r = map_object(rec.name, RING_BYTES, 0);
        if (!r) continue;
        // A ring whose header does not agree is one this build cannot
        // read. Dropped rather than guessed at -- the client is still
        // writing into it and would see its messages vanish either way,
        // but a mis-sized read would scribble past the mapping.
        if (r->magic != UCHAN_MAGIC || r->slot_bytes != UCHAN_SLOT_BYTES ||
            r->slots != UCHAN_SLOTS) {
            unmap(r, RING_BYTES);
            continue;
        }
        s->ring[s->count] = r;
        s->pid[s->count] = pid;
        s->count++;
    }

    // A CLIENT THAT DIED TAKES ITS NAME WITH IT (the kernel unlinks a
    // dead creator's objects), so a name that no longer resolves is how
    // this notices. The frames live until this unmaps, which is why the
    // drop has to happen rather than being left to the allocator.
    for (int k = 0; k < s->count; ) {
        char nm[32];
        snprintf(nm, sizeof nm, "%s.%d", s->name, s->pid[k]);
        int fd = sys_shm_open(nm, 0, 0);
        if (fd >= 0) { sys_close(fd); k++; continue; }
        // **WHICH CLIENT WENT IS REPORTED, NOT JUST RECLAIMED.** A
        // server usually holds state per client -- the compositor holds
        // a window -- and reclaiming the slot silently leaves that
        // state with nothing to retire it. A death that does not fit in
        // `gone` KEEPS its ring, so the next scan reports it: dropping
        // the ring unreported lost the death for good, and its windows
        // with it.
        if (gone && ngone >= gone_cap) { k++; continue; }
        if (gone) gone[ngone] = s->pid[k];
        ngone++;
        unmap(s->ring[k], RING_BYTES);
        s->ring[k] = s->ring[s->count - 1];
        s->pid[k] = s->pid[s->count - 1];
        s->count--;
    }
    return ngone;
}

// The bytes actually moved: never more than the slot holds, never more
// than the caller's buffer takes.
static unsigned long clamp_len(unsigned long n) {
    return n > UCHAN_SLOT_BYTES ? UCHAN_SLOT_BYTES : n;
}

int uchan_server_recv(struct uchan_server *s, void *out, unsigned long cap) {
    if (!s->beacon || s->count <= 0) return 0;
    // ROUND-ROBIN from where the last one left off, so a client sending
    // continuously cannot hold the others off forever.
    for (int n = 0; n < s->count; n++) {
        int i = (int)((s->next + (unsigned)n) % (unsigned)s->count);
        struct uchan_ring *r = s->ring[i];
        if (r->head == r->tail) continue;
        uint32_t slot = r->tail % UCHAN_SLOTS;
        memcpy(out, r->slot[slot], clamp_len(cap));
        // The copy BEFORE the tail bump: the client is free to refill
        // this slot the moment it sees the space, and a bump first is a
        // message read half from the old sender and half from the new.
        r->tail++;
        s->next = (unsigned)(i + 1);
        return s->pid[i];
    }
    return 0;
}

void uchan_server_reply(struct uchan_server *s, int pid, const void *msg,
                        unsigned long len) {
    int i = ring_index(s, pid);
    if (i < 0) return;
    struct uchan_ring *r = s->ring[i];
    len = clamp_len(len);
    memcpy((void *)r->reply, msg, len);
    // The tail of the slot is ZEROED, not left as it was: a reader
    // asking for more than this message carries must see zeros rather
    // than the previous message's bytes.
    memset((void *)r->reply + len, 0, UCHAN_SLOT_BYTES - len);
    r->reply_seq++;   // last, so a client that sees it sees the payload
    sys_futex_wake(&r->reply_seq, 0);
}

void uchan_server_wait(struct uchan_server *s, int timeout_ms) {
    if (!s->beacon) { sys_sleep_ms(timeout_ms); return; }
    uint32_t seen = s->beacon->wake;
    // ANYTHING ALREADY QUEUED MEANS DO NOT PARK. Checked after sampling
    // the word, so a message arriving between the two moves it and the
    // wait below returns at once instead of sleeping through it.
    for (int i = 0; i < s->count; i++)
        if (s->ring[i]->head != s->ring[i]->tail) return;
    sys_futex_wait(&s->beacon->wake, seen, timeout_ms);
}

int uchan_server_room(struct uchan_server *s, int pid) {
    int i = ring_index(s, pid);
    if (i < 0) return 0;
    struct uchan_ring *r = s->ring[i];
    return (int)(UCHAN_IN_SLOTS - (r->in_head - r->in_tail));
}

int uchan_server_send(struct uchan_server *s, int pid, const void *msg,
                      unsigned long len) {
    int i = ring_index(s, pid);
    if (i < 0) return -1;
    struct uchan_ring *r = s->ring[i];
    if (r->in_head - r->in_tail >= UCHAN_IN_SLOTS) {
        r->in_dropped++;
        return -1;
    }
    if (len > UCHAN_IN_BYTES) len = UCHAN_IN_BYTES;
    uint8_t *slot = r->in_slot[r->in_head % UCHAN_IN_SLOTS];
    memcpy(slot, msg, len);
    memset(slot + len, 0, UCHAN_IN_BYTES - len);
    r->in_head++;   // after the payload, for the reader's sake
    // Bump, then wake -- the same order uchan_send() keeps, for the
    // same lost-wakeup reason. Atomic because the client's own threads
    // bump this word too (uchan_client_kick).
    __atomic_fetch_add(&r->in_wake, 1, __ATOMIC_SEQ_CST);
    sys_futex_wake((void *)&r->in_wake, 0);
    return 0;
}

void uchan_server_close(struct uchan_server *s) {
    if (!s->beacon) return;
    sys_wakeword(0);
    for (int i = 0; i < s->count; i++) unmap(s->ring[i], RING_BYTES);
    sys_shm_unlink(s->name);
    unmap(s->beacon, sizeof(struct uchan_beacon));
    memset(s, 0, sizeof *s);
}

// --- client ----------------------------------------------------------

int uchan_client_open(struct uchan_client *c, const char *name) {
    memset(c, 0, sizeof *c);
    if (!name || !*name || strlen(name) >= UCHAN_NAME_MAX) return -1;

    c->beacon = map_object(name, sizeof(struct uchan_beacon), 0);
    if (!c->beacon) return -1;              // no server
    if (c->beacon->magic != UCHAN_MAGIC) {
        unmap(c->beacon, sizeof(struct uchan_beacon));
        c->beacon = 0;
        return -1;
    }

    snprintf(c->ring_name, sizeof c->ring_name, "%s.%d", name, sys_getpid());
    sys_shm_unlink(c->ring_name);   // a previous holder of this pid
    c->ring = map_object(c->ring_name, RING_BYTES, 1);
    if (!c->ring) {
        unmap(c->beacon, sizeof(struct uchan_beacon));
        c->beacon = 0;
        return -1;
    }
    c->ring->slot_bytes = UCHAN_SLOT_BYTES;
    c->ring->slots = UCHAN_SLOTS;
    c->ring->client_pid = sys_getpid();
    // THE SERVER IS LET IN EXPLICITLY. A ring is a channel between two
    // processes and is private like any other named object, so the one
    // that has to read it is granted by name -- and nobody else can,
    // which before this any process could do by reading `lsshm`.
    sys_shm_grant(c->ring_name, c->beacon->server_pid);
    c->ring->magic = UCHAN_MAGIC;   // last: the server's admission check
    return 0;
}

int uchan_send(struct uchan_client *c, const void *msg, unsigned long len) {
    if (!c->ring || !c->beacon) return -1;
    if (c->ring->head - c->ring->tail >= UCHAN_SLOTS) return -1;  // full
    len = clamp_len(len);
    uint8_t *slot = c->ring->slot[c->ring->head % UCHAN_SLOTS];
    memcpy(slot, msg, len);
    memset(slot + len, 0, UCHAN_SLOT_BYTES - len);
    c->ring->head++;   // after the payload, for the reader's sake

    // THE BUMP, THEN THE WAKE, and the order is what closes the race: a
    // server that sampled `wake` before this and parks after it finds
    // the value moved and does not park at all. A wake alone would land
    // in that window and be lost.
    c->beacon->wake++;
    sys_futex_wake(&c->beacon->wake, 0);
    return 0;
}

int uchan_call(struct uchan_client *c, const void *msg, unsigned long len,
               void *reply, unsigned long reply_cap, int timeout_ms) {
    if (!c->ring) return -1;
    uint32_t seq = c->ring->reply_seq;
    if (uchan_send(c, msg, len) < 0) return -1;
    // Sampled BEFORE the send, so an answer that arrives while this is
    // still in uchan_send() is not waited for a second time.
    while (c->ring->reply_seq == seq) {
        if (sys_futex_wait(&c->ring->reply_seq, seq, timeout_ms) < 0 &&
            c->ring->reply_seq == seq)
            return -1;   // -EAGAIN means it moved; anything else is the timeout
    }
    memcpy(reply, (const void *)c->ring->reply, clamp_len(reply_cap));
    return 0;
}

void uchan_client_close(struct uchan_client *c) {
    if (c->ring) {
        sys_shm_unlink(c->ring_name);
        unmap(c->ring, RING_BYTES);
    }
    if (c->beacon) unmap(c->beacon, sizeof(struct uchan_beacon));
    memset(c, 0, sizeof *c);
}

// --- client: the inbox ----------------------------------------------

int uchan_client_pending(const struct uchan_client *c) {
    if (!c->ring) return 0;
    return (int)(c->ring->in_head - c->ring->in_tail);
}

int uchan_client_recv(struct uchan_client *c, void *out, unsigned long cap) {
    if (!c->ring) return 0;
    struct uchan_ring *r = c->ring;
    if (r->in_head == r->in_tail) return 0;
    if (cap > UCHAN_IN_BYTES) cap = UCHAN_IN_BYTES;
    memcpy(out, r->in_slot[r->in_tail % UCHAN_IN_SLOTS], cap);
    r->in_tail++;   // the copy first -- the server refills on seeing space
    return 1;
}

void uchan_client_wait(struct uchan_client *c, int timeout_ms) {
    if (!c->ring || timeout_ms <= 0) return;
    uint32_t seen = c->ring->in_wake;
    // Sampled BEFORE the emptiness test, so a message landing between
    // the two moves the word and the wait returns at once.
    if (c->ring->in_head != c->ring->in_tail) return;
    sys_futex_wait((void *)&c->ring->in_wake, seen, timeout_ms);
}

void uchan_client_kick(struct uchan_client *c) {
    if (!c->ring) return;
    __atomic_fetch_add(&c->ring->in_wake, 1, __ATOMIC_SEQ_CST);
    sys_futex_wake((void *)&c->ring->in_wake, 0);
}

uint32_t uchan_client_dropped(const struct uchan_client *c) {
    return c->ring ? c->ring->in_dropped : 0;
}

int uchan_client_server_alive(const struct uchan_client *c) {
    if (!c->beacon) return 0;
    // THE NAME, NOT THE MAPPING: this process's view of the beacon is
    // the old page whatever happened, so the live object is opened
    // afresh and its pid compared. A successor compositor has a new
    // pid, and a ring granted to the old one is nothing to it.
    char name[UCHAN_NAME_MAX];
    const char *dot = strchr(c->ring_name, '.');
    unsigned long n = dot ? (unsigned long)(dot - c->ring_name) : 0;
    if (!n || n >= sizeof name) return 0;
    memcpy(name, c->ring_name, n);
    name[n] = '\0';
    struct uchan_beacon *b = map_object(name, sizeof *b, 0);
    if (!b) return 0;
    int alive = b->magic == UCHAN_MAGIC && b->server_pid == c->beacon->server_pid;
    unmap(b, sizeof *b);
    return alive;
}
