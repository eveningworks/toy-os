// uapp's surfaces: a window's buffers, presenting them, and popups --
// see ui/uapp_internal.h for where the rest of uapp is.
#include "ui/uapp_internal.h"

// The window a popup opened right now should hang off: whichever one is
// dispatching, or the toplevel. A widget cannot say -- it does not know
// it is in a dialog -- so uapp tracks it around the one call that can
// re-enter a widget from a non-toplevel window.
uint32_t g_popup_parent;
struct uapp_surf g_surf[WIN_CLIENT_MAX];
#define TOPLEVEL (&g_surf[0])

// A PRIMARY PRESS IN FLIGHT ON A POPUP, so a popup closed under it can
// end it. The compositor sends no release to a surface that is gone (nor
// does wl_pointer), and the widget that took the press would otherwise
// hold the router's grab until the next release anywhere -- a value
// committed at the press reached the app only then. The close OWES the
// release, delivered after the event that closed it (owed_release()),
// never from inside the widget call that is closing it.
uint32_t g_press_slot;           // 0: none, or the press was not on a popup
struct win_event g_press_up;     // that press, as a release in its parent's coordinates
int g_release_owed;

void popup_press_closed(int id) {
    if (id > 0 && (uint32_t)id == g_press_slot) {
        g_press_slot = 0;
        g_release_owed = 1;
    }
}

int slot_of(const struct uapp_surf *s) { return (int)(s - g_surf); }

static void buf_name(char *out, unsigned cap, int slot, int buf) {
    snprintf(out, cap, WIN_BUF_NAME_FMT, sys_getpid(), slot, buf);
}

// Lets the compositor open this object. **PER OBJECT, NOT PER NAME**: a
// grant lives on the object, and a resize creates a NEW one under the
// same name with an empty grant list -- so this belongs beside every
// create, not once at startup. Without it the first resize is the last
// frame the compositor ever sees of the window.

static void buf_grant(const char *nm) {
    int pid = comp_pid();
    if (pid > 0) sys_shm_grant(nm, pid);
}

// Creates (or REPLACES) one buffer at `bytes`. A replace unlinks the old
// name first and creates a new object under it -- the old one stays
// alive for whoever still maps it, which is exactly what stops a resize
// pulling the pixels out from under the compositor.
int buf_make(struct uapp_surf *s, int buf, int w, int h) {
    int slot = slot_of(s);
    // **PAGE-ALIGNED, because SYS_MUNMAP requires it** (abi/syscall_abi.h)
    // and refuses anything else. A window's pixels are w*h*4, which is
    // almost never a whole number of pages -- so the unrounded length
    // made every unmap fail silently, the old mapping outlive its
    // replacement, and each resize leak a buffer's worth of frames.
    // Twelve objects after three drags, and then the biggest allocation
    // in the run is the one that fails.
    uint64_t bytes = ((uint64_t)w * (uint64_t)h * 4 + 4095) & ~4095ULL;
    char nm[WIN_BUF_NAME_MAX];
    buf_name(nm, sizeof nm, slot, buf);
    if (s->px[buf]) {
        sys_munmap(s->px[buf], s->px_bytes[buf]);
        s->px[buf] = 0;
    }
    sys_shm_unlink(nm);
    int fd = sys_shm_open(nm, bytes, SHM_CREATE | SHM_EXCL);
    if (fd < 0) return 0;
    buf_grant(nm);
    void *p = sys_mmap(0, bytes, SYS_PROT_READ | SYS_PROT_WRITE,
                       SYS_MAP_SHARED, fd, 0);
    sys_close(fd);
    if (p == (void *)-1) return 0;
    s->px[buf] = p;
    s->px_bytes[buf] = bytes;
    s->px_w[buf] = w;
    s->px_h[buf] = h;
    // A NEW OBJECT, WHATEVER ITS SIZE. Bumping only on a size change
    // would leave the compositor mapping an object nobody draws into
    // when a client re-creates a buffer at the size it already had --
    // a window frozen on its last frame, and reachable from a drag that
    // proposes the size the window has.
    s->px_gen[buf]++;
    return 1;
}

void bufs_release(struct uapp_surf *s);

// Claims slot `slot` for a surface of w x h and makes two of its
// buffers; the third is made the first time both others are busy.
int bufs_create(struct uapp_surf *s, int w, int h) {
    if (w <= 0 || h <= 0 || s->used) return 0;
    s->used = 1;
    s->front = 0;
    s->back = -1;
    s->stall_ms = 0;
    s->w = w;
    s->h = h;
    s->dirty = 0;
    for (int b = 0; b < UAPP_BUFS; b++) { s->busy[b] = 0; s->busy_seq[b] = 0; }
    s->busy[0] = 1;   // the compositor shows buffer 0 from the create on
    for (int b = 0; b < 2; b++)
        if (!buf_make(s, b, w, h)) { bufs_release(s); return 0; }
    return 1;
}

// Makes sure buffer `buf` is `w` x `h` before the client draws into it.
//
// **THE STALE HALF IS THE CLIENT'S PROBLEM.** A resize replaces only the
// buffer being drawn into; the other one is still the old size and is
// the one the client draws into NEXT.

static int buf_ensure(struct uapp_surf *s, int buf, int w, int h) {
    // **THE DIMENSIONS DECIDE, NOT THE LENGTH.** Comparing page-rounded
    // byte counts here made a one-pixel resize look like no resize at
    // all -- and NOBODY WAS TOLD, because the size lived in a second
    // record the client had to keep in step. It does not any more: the
    // present carries it, so this only has to make the memory right.
    if (s->px[buf] && s->px_w[buf] == w && s->px_h[buf] == h) return 1;

    // The OBJECT is replaced only when the rounded length actually
    // moved; rebuilding on every present would hand the app a freshly
    // zeroed buffer each frame, which shows up as a scene that will not
    // hold still.
    uint64_t want = ((uint64_t)w * (uint64_t)h * 4 + 4095) & ~4095ULL;
    if (!s->px[buf] || s->px_bytes[buf] != want) return buf_make(s, buf, w, h);
    s->px_w[buf] = w;
    s->px_h[buf] = h;
    return 1;
}

static unsigned long long g_present_seq;

// The buffer to draw this frame into, sized and wrapped as s->surface;
// 0 when every one is busy, and the caller leaves its frame dirty for
// the release to wake. Prefers the lowest free index, so with prompt
// releases two buffers alternate and the third is never made.
struct ugfx_surface *surf_back(struct uapp_surf *s) {
    if (s->back < 0) {
        int pick = -1;
        for (int b = 0; b < UAPP_BUFS && pick < 0; b++)
            if (b != s->front && !s->busy[b]) pick = b;
        if (pick < 0) {
            // A RELEASE THAT NEVER CAME (an overflowed event queue) must
            // not freeze the window: after a quarter second the oldest is
            // long past being read, so it is taken back.
            unsigned long long now = sys_monotonic_ns() / 1000000ULL;
            if (!s->stall_ms) s->stall_ms = now ? now : 1;
            if (now - s->stall_ms < 250) return 0;
            for (int b = 0; b < UAPP_BUFS; b++)
                if (b != s->front && (pick < 0 || s->busy_seq[b] < s->busy_seq[pick]))
                    pick = b;
            s->busy[pick] = 0;
            // SAID, because if the compositor was merely slow rather
            // than the release lost, this frame draws into pixels it may
            // still be reading -- a torn window, and this line its cause.
            ulogf("uapp: buffer %d taken back unreleased after %llu ms\n",
                  pick, now - s->stall_ms);
        }
        s->stall_ms = 0;
        if (!buf_ensure(s, pick, s->w, s->h)) return 0;
        s->back = pick;
    }
    s->surface = ugfx_surface_for_pixels(s->px[s->back], s->w, s->h);
    return &s->surface;
}

void bufs_release(struct uapp_surf *s) {
    for (int b = 0; b < UAPP_BUFS; b++) {
        if (!s->px[b]) continue;
        char nm[WIN_BUF_NAME_MAX];
        buf_name(nm, sizeof nm, slot_of(s), b);
        sys_munmap(s->px[b], s->px_bytes[b]);
        sys_shm_unlink(nm);
        s->px[b] = 0;
    }
    s->used = 0;
    s->done = 0;
    s->owner = 0;
}

// WHAT THIS FRAME CHANGED, against the frame the compositor shows now --
// the buffer last presented, which nothing draws into. The toolkit
// repaints the whole buffer every frame and keeps no invalidation record,
// so the damage is MEASURED rather than declared: a row compare of the
// two buffers (uregion_diff()), which cannot miss a change the way a
// forgotten uapp_redraw_rect() would. A caret blink is then a caret-sized
// present, not a window-sized one. A buffer of another size, or the
// front itself (a single-buffered window), damages everything.
static void present_damage(const struct uapp_surf *s, int shown, struct win_damage *d) {
    memset(d, 0, sizeof *d);
    int f = s->front;
    if (f == shown || !s->px[f] || s->px_w[f] != s->w || s->px_h[f] != s->h ||
        s->px_w[shown] != s->w || s->px_h[shown] != s->h) return;
    struct uregion g;
    uregion_diff(&g, (const uint32_t *)s->px[shown], (const uint32_t *)s->px[f],
                 s->w, s->h, s->w, WIN_DAMAGE_MAX, 16);
    d->flags = WIN_DAMAGE_LIST;
    d->n = (uint16_t)g.n;
    for (int i = 0; i < g.n; i++) {
        d->r[i].x = (uint16_t)g.r[i].x; d->r[i].y = (uint16_t)g.r[i].y;
        d->r[i].w = (uint16_t)g.r[i].w; d->r[i].h = (uint16_t)g.r[i].h;
    }
}

// **THE FRAME NAMES ITSELF.** A present says which buffer holds the
// finished pixels, which OBJECT is behind it, and how big it is -- so
// the compositor adopts the geometry of what it is about to show and
// never a size it was promised earlier. The kernel used to hold that
// per buffer and answer from its copy; nothing does now.
//
// FIRE AND FORGET, which is what lets it run once a frame per client.
// The flip is this side's: the buffer just handed over is the one the
// compositor reads, so the next frame goes into the other.
int surf_present(struct uapp_surf *s) {
    int shown = s->back;        // the one that has been drawn into
    if (shown < 0 || !s->px[shown]) return 0;

    // A FULL RING MEANS NO FLIP. Dropping the frame is fine -- the next
    // present supersedes it -- but flipping anyway would leave the
    // compositor reading the buffer this process is about to draw into,
    // which is the tearing double buffering exists to remove.
    // THE SEQUENCE TRAVELS, so a late release of an EARLIER present of
    // this buffer cannot free it while the compositor shows this one.
    unsigned long long seq = g_present_seq + 1;
    if ((uint32_t)seq == 0) seq++;   // 0 means "no sequence" on the wire
    struct win_damage dmg;
    present_damage(s, shown, &dmg);
    if (!wmchan_send_damage(WIN_REQ_PRESENT, (uint32_t)slot_of(s),
                            WIN_PRESENT_B(shown, s->px_gen[shown]),
                            (int)WIN_PRESENT_SIZE(s->w, s->h), (int)(uint32_t)seq,
                            0, &dmg)) return 0;

    g_present_seq = seq;
    s->front = shown;
    s->busy[shown] = 1;
    s->busy_seq[shown] = seq;
    s->back = -1;     // the next frame picks one that has come back
    s->dirty = 0;
    return 1;
}

// THE POPUPS AFTER THEIR PARENT, in slot order -- the order they were
// opened, which is the order the compositor stacks them. Only those
// drawn into this frame: a menu nobody hovered is unchanged and
// re-presenting it would be a wake-up for nothing. EVERY toplevel's
// frame calls this for its own: a dialog's dropdown sent only on the
// MAIN window's frame never appeared over an idle app.
void present_popups(uint32_t parent) {
    for (int i = 1; i < WIN_CLIENT_MAX; i++)
        if (g_surf[i].used && g_surf[i].dirty && g_surf[i].parent == parent)
            surf_present(&g_surf[i]);
}

// --- popup surfaces: the provider ui/uui_popup.h asks through --------
//
// A popup is a slot above 0 with its own two buffers, opened by the
// third round trip (WIN_REQ_POPUP) and placed by the compositor; the
// reply's offset is what turns its input back into toplevel coordinates
// in dispatch(). Widgets never see a slot: they get an id, which IS the
// slot, and a surface to draw into.

static int popup_open(void *ctx, int ax, int ay, int aw, int ah, int w, int h,
                      int gravity, unsigned flags, void (*done)(void *owner),
                      void *owner, int *out_x, int *out_y) {
    (void)ctx;
    if (w <= 0 || h <= 0 || comp_pid() <= 0) return 0;
    clamp_to_screen(&w, &h);
    struct uapp_surf *s = 0;
    for (int i = 1; i < WIN_CLIENT_MAX; i++)
        if (!g_surf[i].used) { s = &g_surf[i]; break; }
    if (!s) return 0;
    if (!bufs_create(s, w, h)) return 0;

    struct wmchan_msg m, r;
    memset(&m, 0, sizeof m);
    m.type = WIN_REQ_POPUP;
    m.window = (uint32_t)slot_of(s);
    m.a = w;
    m.b = h;
    m.c = (int32_t)(g_popup_parent ? g_popup_parent : g_app.window);
    m.pos.ax = ax; m.pos.ay = ay; m.pos.aw = aw; m.pos.ah = ah;
    m.pos.gravity = gravity == UUI_POPUP_RIGHT ? WIN_POPUP_RIGHT : WIN_POPUP_BELOW;
    _Static_assert(UUI_POPUP_GLASS_PX == WIN_GLASS_PX, "one glass mark, two names");
    m.pos.flags = ((flags & UUI_POPUP_GRAB) ? WIN_POPUP_GRAB : 0) |
                  ((flags & UUI_POPUP_GLASS) ? WIN_POPUP_GLASS : 0);
    if (!wmchan() ||
        uchan_call(&g_wmchan, &m, sizeof m, &r, sizeof r, UAPP_CALL_TIMEOUT_MS) < 0 ||
        r.a < 0) {
        bufs_release(s);
        return 0;
    }
    s->ox = r.b;
    s->oy = r.c;
    s->parent = g_popup_parent ? g_popup_parent : g_app.window;
    s->done = done;
    s->owner = owner;
    surf_back(s);
    if (out_x) *out_x = r.b;
    if (out_y) *out_y = r.c;
    return slot_of(s);
}

void popup_close(void *ctx, int id) {
    (void)ctx;
    if (id <= 0 || id >= WIN_CLIENT_MAX || !g_surf[id].used) return;
    popup_press_closed(id);
    wmchan_send(WIN_REQ_DESTROY, (uint32_t)id, 0, 0, 0, 0);
    bufs_release(&g_surf[id]);
    g_app.top.dirty = 1;   // the widget that owned it repaints its own state
}

static struct ugfx_surface *popup_surface(void *ctx, int id) {
    (void)ctx;
    if (id <= 0 || id >= WIN_CLIENT_MAX || !g_surf[id].used) return 0;
    struct uapp_surf *s = &g_surf[id];
    struct ugfx_surface *sf = surf_back(s);
    if (!sf) { g_app.top.dirty = 1; return 0; }   // drawn once a buffer is back
    s->dirty = 1;   // present() sends it after the toplevel
    return sf;
}

const struct uui_popup_ops g_popup_ops = {
    .open = popup_open,
    .close = popup_close,
    .surface = popup_surface,
};

// The compositor dismissed popup `id` (WIN_EV_POPUP_DONE) -- or asked it
// to close, which for a popup means the same. The window is already
// gone, so no DESTROY goes back; the widget is told and the buffers
// released.
void popup_dismissed(int id) {
    if (id <= 0 || id >= WIN_CLIENT_MAX || !g_surf[id].used) return;
    struct uapp_surf *s = &g_surf[id];
    void (*done)(void *) = s->done;
    void *owner = s->owner;
    popup_press_closed(id);
    bufs_release(s);
    if (done) done(owner);
    g_app.top.dirty = 1;
}

