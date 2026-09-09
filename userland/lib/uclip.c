// The system clipboard, over the shared page /bin/clipboardd owns --
// see lib/uclip.h for the API and lib/uclip_page.h for why the kernel
// is not in this any more.
#include "lib/uclip.h"
#include "rt/sys.h"
#include "syscall_abi.h"
#include <string.h>

static volatile struct clip_page *g_page;
static int g_fd = -1;

// Maps the clipboard, once. Returns NULL when the service is not
// running, which is an ordinary answer and not an error: a machine can
// boot with clipboardd disabled, and every caller here degrades to
// "the clipboard is empty" or "the copy did not happen".
static volatile struct clip_page *page(void) {
    if (g_page) return g_page;
    g_fd = sys_shm_open(CLIP_SHM_NAME, 0, 0);
    if (g_fd < 0) return 0;
    void *p = sys_mmap(0, sizeof(struct clip_page),
                        SYS_PROT_READ | SYS_PROT_WRITE, SYS_MAP_SHARED, g_fd, 0);
    if (p == (void *)-1) { sys_close(g_fd); g_fd = -1; return 0; }
    volatile struct clip_page *pg = (volatile struct clip_page *)p;
    if (pg->magic != CLIP_MAGIC) {
        // The daemon is mid-initialisation. Not cached, so the next
        // call looks again rather than remembering a page that was not
        // ready the first time anybody asked.
        sys_munmap(p, sizeof(struct clip_page));
        sys_close(g_fd);
        g_fd = -1;
        return 0;
    }
    g_page = pg;
    return g_page;
}

// --- reading ----------------------------------------------------------

int uclip_load(struct uclip *c) {
    if (!c) return 0;
    memset(c, 0, sizeof *c);
    volatile struct clip_page *pg = page();
    if (!pg) return 1;   // no service: the clipboard is empty, not broken

    // THE SEQLOCK. Odd means a writer is inside; a change across the
    // copy means it landed while we were reading. Bounded, because a
    // writer that died mid-update would otherwise spin us forever --
    // and the daemon repairs that within a second or two.
    for (int tries = 0; tries < 64; tries++) {
        uint32_t s1 = __atomic_load_n(&pg->seq, __ATOMIC_ACQUIRE);
        if (s1 & 1u) continue;

        c->op = pg->op;
        c->kind = pg->kind;
        c->count = pg->count;
        c->len = pg->len;
        c->serial = pg->serial;
        if (c->len > CLIP_BYTES) c->len = CLIP_BYTES;
        for (uint32_t i = 0; i < c->len; i++) c->data[i] = pg->data[i];

        uint32_t s2 = __atomic_load_n(&pg->seq, __ATOMIC_ACQUIRE);
        if (s1 == s2) return 1;
    }
    memset(c, 0, sizeof *c);   // never a HALF-read clipboard
    return 1;
}

int uclip_op(const struct uclip *c) { return c ? (int)c->op : UCLIP_NONE; }
int uclip_count(const struct uclip *c) { return c ? (int)c->count : 0; }
unsigned uclip_serial(const struct uclip *c) { return c ? c->serial : 0; }
int uclip_kind(const struct uclip *c) { return c ? (int)c->kind : UCLIP_KIND_FILES; }

unsigned uclip_peek_serial(void) {
    volatile struct clip_page *pg = page();
    return pg ? __atomic_load_n(&pg->serial, __ATOMIC_ACQUIRE) : 0;
}

// The op WITHOUT the payload. uclip_load() copies the whole 64 KiB
// object, which is far too much to spend telling an app that a cut it
// is drawing has been replaced by somebody else's copy.
int uclip_peek_op(void) {
    volatile struct clip_page *pg = page();
    return pg ? (int)__atomic_load_n(&pg->op, __ATOMIC_ACQUIRE) : UCLIP_NONE;
}

const char *uclip_path(const struct uclip *c, int i) {
    if (!c || c->kind != UCLIP_KIND_FILES) return 0;
    if (i < 0 || i >= (int)c->count) return 0;
    // WALKED, not indexed: the entries are variable-length and packed,
    // which is what keeps a clipboard of short names cheap.
    const char *p = c->data;
    const char *end = c->data + c->len;
    for (int n = 0; n < i; n++) {
        while (p < end && *p) p++;
        if (p >= end) return 0;
        p++;                       // step over the NUL
    }
    return p < end ? p : 0;
}

const char *uclip_text(const struct uclip *c, int *out_len) {
    if (out_len) *out_len = 0;
    if (!c || c->kind != UCLIP_KIND_TEXT || c->count != 1) return 0;
    if (c->len == 0 || c->len > CLIP_BYTES) return 0;
    // A payload arriving without its terminator is a malformed one, and
    // reading past it would run off the buffer.
    if (c->data[c->len - 1] != '\0') return 0;
    if (out_len) *out_len = (int)c->len - 1;
    return c->data;
}

// --- writing ----------------------------------------------------------
//
// Staged into a process-local buffer and copied in under the lock, so
// the page is never partly a new clipboard: a reader that arrives
// mid-copy is caught by the seqlock and retries, and one that arrives
// before or after sees a whole clipboard either way.

static struct uclip g_out;

static int lock_page(volatile struct clip_page *pg) {
    uint32_t want = (uint32_t)sys_getpid();
    if (!want) want = 1;   // 0 means UNLOCKED, so a pid of 0 cannot be an owner
    for (long i = 0; i < CLIP_LOCK_SPINS; i++) {
        uint32_t free_ = 0;
        if (__atomic_compare_exchange_n(&pg->lock, &free_, want, 0,
                                         __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            pg->lock_pid = want;
            pg->lock_ms = sys_monotonic_ns() / 1000000ull;
            return 1;
        }
        __builtin_ia32_pause();
    }
    // REFUSED rather than forced. A lock this stuck has a dead owner,
    // and breaking it here would race a live one; clipboardd breaks it
    // within CLIP_LOCK_STALE_MS and the next copy works.
    return 0;
}

static void unlock_page(volatile struct clip_page *pg) {
    pg->lock_ms = 0;
    pg->lock_pid = 0;
    __atomic_store_n(&pg->lock, 0u, __ATOMIC_RELEASE);
}

// Publishes what is staged in g_out. Returns 1 on success.
static int commit(void) {
    volatile struct clip_page *pg = page();
    if (!pg) return 0;
    if (g_out.len > CLIP_BYTES || g_out.count > CLIP_MAX) return 0;
    if (!lock_page(pg)) return 0;

    // ODD while the update is in flight -- the reader's whole signal.
    __atomic_store_n(&pg->seq, pg->seq + 1u, __ATOMIC_RELEASE);
    pg->op = g_out.op;
    pg->kind = g_out.kind;
    pg->count = g_out.count;
    pg->len = g_out.len;
    for (uint32_t i = 0; i < g_out.len; i++) pg->data[i] = g_out.data[i];
    // Bumped INSIDE the update, so a reader either sees the whole new
    // clipboard with its new serial or the whole old one with the old.
    pg->serial++;
    __atomic_store_n(&pg->seq, pg->seq + 1u, __ATOMIC_RELEASE);

    unlock_page(pg);
    return 1;
}

void uclip_begin(struct uclip *c, int op) {
    (void)c;   // the staging buffer is this library's; see uclip.h
    memset(&g_out, 0, sizeof g_out);
    g_out.op = (uint32_t)op;
    g_out.kind = UCLIP_KIND_FILES;
}

int uclip_add(struct uclip *c, const char *path) {
    (void)c;
    if (!path || !*path) return 0;
    uint32_t n = (uint32_t)strlen(path) + 1;      // the NUL is payload
    if (g_out.count >= CLIP_MAX) return 0;
    if (g_out.len + n > CLIP_BYTES) return 0;
    memcpy(g_out.data + g_out.len, path, n);
    g_out.len += n;
    g_out.count++;
    return 1;
}

int uclip_commit(struct uclip *c) {
    (void)c;
    if (g_out.count == 0) return 0;
    return commit();
}

int uclip_clear(void) {
    memset(&g_out, 0, sizeof g_out);
    g_out.op = UCLIP_NONE;
    g_out.kind = UCLIP_KIND_FILES;
    return commit();
}

int uclip_set_text(const char *s, int n) {
    if (!s || n < 0 || n > UCLIP_TEXT_MAX) return 0;
    memset(&g_out, 0, sizeof g_out);
    // COPY even for a cut: see uclip.h -- there is nothing to move once
    // the characters are in the page, so an editor's Cut deletes its
    // own selection and copies.
    g_out.op = UCLIP_COPY;
    g_out.kind = UCLIP_KIND_TEXT;
    g_out.count = 1;
    g_out.len = (uint32_t)n + 1;          // the NUL is payload
    memcpy(g_out.data, s, (size_t)n);
    g_out.data[n] = '\0';
    return commit();
}
