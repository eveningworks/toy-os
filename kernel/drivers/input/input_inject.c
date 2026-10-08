// Injected input: a remote desktop's keyboard and pointer, entering the
// input core where a device's would (SYS_INPUT_INJECT).
//
// driver-none: the input core's injection half, not a device
//
// Linux's uinput shape rather than the compositor's: a key goes through
// keyboard_key_event() like any keyboard's, so the layout, Caps Lock,
// dead keys, the Ctrl encoding, the by-position stream and the key tap
// all treat it as typed. Injecting above the kernel would have needed a
// second copy of that translation, and two encoders drift.
//
// **ONLY /bin/remoted MAY INJECT.** There is no privilege model to ask,
// so the gate is the caller's spawn path -- the identity rule the
// compositor already uses for screensavers and app grouping.
//
// **WHAT THE INJECTOR HOLDS IS RELEASED WHEN IT EXITS**
// (input_inject_process_exit()): a server that crashed mid-chord would
// otherwise leave Shift down for the whole machine. Keys are tracked per
// keycode; buttons sit in their own mask beside the devices'
// (mouse_inject_buttons()), so a local mouse moving does not let go of a
// remote drag.
#include "input.h"
#include "keyboard.h"
#include "keyboard_layout.h"
#include "mouse.h"
#include "scheduler.h"
#include "syscalls.h"
#include "syscall_table.h"
#include "syscall_abi.h"   // struct input_inject, INPUT_INJECT_*
#include "vmm.h"
#include "errno.h"
#include "irqflags.h"
#include "klog.h"
#include "string.h"
#include "ktest.h"

#define INJECT_PATH "/bin/remoted"
#define INJECT_KEYS 256   // every evdev keycode a layout can name (keyboard_layout.c)
#define KEY_WORDS (INJECT_KEYS / 32)

static uint32_t g_held[KEY_WORDS];   // keycodes this path pressed
static int g_owner;                   // the pid that last injected; 0 = none

static void key(uint16_t kc, int down) {
    if (!kc || kc >= INJECT_KEYS) return;
    uint32_t bit = 1u << (kc % 32);
    if (down) g_held[kc / 32] |= bit;
    else      g_held[kc / 32] &= ~bit;
    input_report_key(kc, down);
}

static int held(uint16_t kc) {
    return kc < INJECT_KEYS && (g_held[kc / 32] & (1u << (kc % 32)));
}

// Types `ch` on the active layout: its key, with Shift and AltGr pressed
// or let go around it as the level needs -- x11vnc's approach, since a
// VNC viewer sends the character, already shifted, and not the key.
static int inject_char(int ch, int down) {
    uint16_t kc;
    int sh, ag;
    if (!keyboard_layout_find(ch, &kc, &sh, &ag)) return -ENOENT;
    if (!down) { key(kc, 0); return 0; }
    uint8_t mods = keyboard_mods_now();
    int has_sh = (mods & KEY_MOD_SHIFT) != 0, has_ag = (mods & KEY_MOD_ALTGR) != 0;
    if (sh != has_sh) key(INPUT_KEY_LEFTSHIFT, sh);
    if (ag != has_ag) key(INPUT_KEY_RIGHTALT, ag);
    key(kc, 1);
    if (ag != has_ag) key(INPUT_KEY_RIGHTALT, has_ag);
    if (sh != has_sh) key(INPUT_KEY_LEFTSHIFT, has_sh);
    return 0;
}

void input_inject_release_all(void) {
    uint64_t f = irq_save();
    for (uint16_t kc = 1; kc < INJECT_KEYS; kc++)
        if (held(kc)) key(kc, 0);
    mouse_inject_buttons(0);
    irq_restore(f);
}

void input_inject_process_exit(int pid) {
    if (!g_owner || pid != g_owner) return;
    input_inject_release_all();
    g_owner = 0;
}

int input_inject_one(const struct input_inject *e) {
    switch (e->op) {
    case INPUT_INJECT_KEY:
        key(e->code, e->x != 0);
        return 0;
    case INPUT_INJECT_CHAR:
        return inject_char(e->code, e->x != 0);
    case INPUT_INJECT_SCANCODE: {
        uint16_t kc;
        if (!keyboard_wire_keycode((uint8_t)e->code, (e->code & INPUT_INJECT_E0) != 0, &kc))
            return -ENOENT;
        key(kc, e->x != 0);
        return 0;
    }
    case INPUT_INJECT_POINTER:
        mouse_set_position(e->x, e->y);
        mouse_inject_buttons((uint8_t)e->code);
        return 0;
    case INPUT_INJECT_WHEEL:
        input_report_wheel(e->x);
        return 0;
    case INPUT_INJECT_RELEASE:
        input_inject_release_all();
        return 0;
    default:
        return -EINVAL;
    }
}

int sys_input_inject(struct syscall_ctx *c) {
    int pid = scheduler_current_pid();
    char path[64];
    if (!scheduler_exec_path(pid, path, sizeof path) || k_strcmp(path, INJECT_PATH) != 0) {
        c->regs[14] = (uint64_t)(int64_t)-EPERM;
        return 0;
    }
    uint64_t n = c->a1;
    if (n == 0 || n > INPUT_INJECT_BATCH) {
        c->regs[14] = (uint64_t)(int64_t)-EINVAL;
        return 0;
    }
    struct input_inject ev[INPUT_INJECT_BATCH];
    if (!vmm_copy_from_user(c->pml4, ev, c->a0, n * sizeof ev[0])) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    // A second server's input lands too, but the release-on-exit follows
    // the newest -- one remote desktop at a time is what this is for.
    g_owner = pid;
    int done = 0, rc = 0;
    for (uint64_t i = 0; i < n; i++) {
        // Interrupts off around each event: the same state the keyboard
        // and mouse interrupts write, and each event is a few stores.
        uint64_t f = irq_save();
        rc = input_inject_one(&ev[i]);
        irq_restore(f);
        if (rc == -EINVAL) break;
        done++;
    }
    c->regs[14] = (uint64_t)(int64_t)(rc == -EINVAL ? -EINVAL : done);
    return 0;
}

// --- KTESTs --------------------------------------------------------------

KTEST("input", "an injected character presses Shift around a shifted key, then lets it go") {
    char saved[KB_LAYOUT_NAME_MAX];
    k_strlcpy(saved, keyboard_layout_current(), sizeof saved);
    KTEST_ASSERT(keyboard_layout_load("us") == 1);
    uint8_t before = keyboard_mods_now();
    struct input_inject a = { INPUT_INJECT_CHAR, 'A', 1, 0 };
    struct input_inject b = { INPUT_INJECT_CHAR, 'A', 0, 0 };
    uint64_t f = irq_save();
    int r1 = input_inject_one(&a);
    int r2 = input_inject_one(&b);
    irq_restore(f);
    uint8_t after = keyboard_mods_now();
    int still = 0;
    for (int i = 0; i < KEY_WORDS; i++) still |= g_held[i] != 0;
    keyboard_layout_load(saved);
    KTEST_ASSERT_EQ(r1, 0);
    KTEST_ASSERT_EQ(r2, 0);
    KTEST_ASSERT_EQ(after & KEY_MOD_SHIFT, before & KEY_MOD_SHIFT);
    KTEST_ASSERT_EQ(still, 0);
}

KTEST("input", "a character the layout cannot type is refused, and nothing is pressed") {
    struct input_inject e = { INPUT_INJECT_CHAR, 0x100, 1, 0 };
    KTEST_ASSERT_EQ(input_inject_one(&e), -ENOENT);
    struct input_inject bad = { 99, 0, 0, 0 };
    KTEST_ASSERT_EQ(input_inject_one(&bad), -EINVAL);
}

KTEST("input", "an injected button survives the real mouse reporting none, and is released at exit") {
    int x, y;
    uint8_t btn;
    mouse_get_state(&x, &y, &btn);
    uint64_t f = irq_save();
    struct input_inject p = { INPUT_INJECT_POINTER, 0x1, x, y };
    input_inject_one(&p);
    mouse_feed_buttons(0);          // a device's report: nothing held
    int px, py;
    uint8_t during, after;
    mouse_get_state(&px, &py, &during);
    g_owner = 4242;
    input_inject_process_exit(4242);
    mouse_get_state(&px, &py, &after);
    while (mouse_try_get_button_edge(0, 0, 0)) {}   // the test's own edges
    irq_restore(f);
    KTEST_ASSERT_EQ(during & 1, 1);
    KTEST_ASSERT_EQ(after & 1, 0);
    KTEST_ASSERT_EQ(g_owner, 0);
}
