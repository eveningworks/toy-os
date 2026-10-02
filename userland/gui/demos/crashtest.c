// Crash Test -- a button per way of breaking things, for exercising the
// fault and panic paths on purpose.
//
// WHY THIS EXISTS: the panic path is the one path a kernel cannot
// exercise by accident and must not get wrong, and until this there was
// no way to reach it at all. Validating a panic report meant editing a
// debug command to dereference a bad pointer, rebuilding, and taking
// the change back out. Linux ships the same idea for the same reason
// (`lkdtm`, and sysrq-c).
//
// **The two halves are genuinely different, and the app says so.**
//
//   RING 3 -- this process dies, the desktop does not. These are
//   performed HERE, in this app's own code, because a program needs no
//   help from anyone to dereference NULL. What they demonstrate is
//   process isolation: the WM notices, reports the crash and carries on.
//
//   DESKTOP -- the desktop restarts. "Crash the desktop" sends toywm a
//   SIGSEGV, whose default action writes a crash report as a fault does
//   (kernel/proc/signal.c): the screen holds the last frame while init
//   restarts it, and the new desktop says what happened.
//
//   RING 0 -- the machine panics. These can only be done by the kernel,
//   so they are asked for through SYS_CRASHTEST, and the KERNEL owns
//   the list (abi/crash_abi.h). This app enumerates rather than
//   hardcoding, so a fault kind added in the kernel appears here with
//   no edit to this file -- the same rule the settings registry and the
//   `.desktop` entries already follow.
//
// The ring-0 half is REFUSED unless the kernel was booted with
// `faultinject`. The buttons are shown either way and the status line
// says which state you are in: a gate nobody can see is one that reads
// as a bug.
//
//     make run KCMDLINE="faultinject"
//
// Log grammar, one line per action, so a test can assert on text:
//   crashtest: ring3 <kind>
//   crashtest: ring0 <kind> refused
//   crashtest: desktop SIGSEGV to pid <n>
//   crashtest: armed <0|1> kinds <n>
#include "rt/sys.h"
#include "ui/ulog.h"
#include "ui/uapp.h"
#include "ui/uui_button.h"
#include "ui/uui_button_group.h"
#include "ui/uui_layout.h"
#include "ui/uui_route.h" // UUI_REASON_RELEASE
#include "ui/uui_statusbar.h"
#include "ui/utheme.h"
#include <stdio.h>
#include <string.h>

#define MAX_KERNEL_KINDS 12

// The ring-3 faults, performed by this program. Codes start at 1
// because uui_button_group_take_activated() reports 0 for "nothing
// completed" -- a button with code 0 can never be told from no click.
enum {
    R3_NULL_WRITE = 1,
    R3_NULL_READ,
    R3_DIVIDE_ZERO,
    R3_BAD_OPCODE,
    R3_STACK_OVERFLOW,
    R3_COUNT_ = R3_STACK_OVERFLOW,
    // Kernel kinds are offset above the ring-3 ones so one button group
    // can hold both and the code alone says which half was pressed.
    K0_BASE = 100,
    DESKTOP_CRASH = 200,
};

static struct uui_button g_r3_btn[5];
static struct uui_button_group g_r3;

static struct uui_button g_k0_btn[MAX_KERNEL_KINDS];
static struct uui_button_group g_k0;
static char g_k0_label[MAX_KERNEL_KINDS][CRASH_NAME_MAX];
static int g_k0_count;
static int g_armed;

static struct uui_button g_dk_btn[1];
static struct uui_button_group g_dk;

static struct uui_statusbar g_status;
static char g_status_msg[96];


// --- the ring-3 faults ------------------------------------------------
//
// `volatile` throughout for the same reason the kernel's triggers use
// it: at -O2 a plain null store is undefined behaviour the compiler may
// simply delete, and a crash button that compiles to nothing is the
// worst outcome available.

static void r3_null_write(void) { *(volatile int *)0 = 1; }
static void r3_null_read(void)  { volatile int v = *(volatile int *)0; (void)v; }
static void r3_divide_zero(void) {
    volatile int z = 0;
    volatile int r = 1 / z;
    (void)r;
}
static void r3_bad_opcode(void) { __asm__ volatile ("ud2"); }

// Recurses through a volatile function pointer and reads the frame
// AFTER the call, so GCC cannot turn this into a loop -- tail-call
// elimination defeated a stack-overflow test in this repo before, and
// `volatile` on the frame alone is not enough.
static void r3_stack_overflow(void) {
    volatile char frame[512];
    frame[0] = 1;
    static void (*volatile again)(void) = r3_stack_overflow;
    again();
    (void)frame[0];
}

static void do_ring3(int code) {
    switch (code) {
    case R3_NULL_WRITE:     ulogf("crashtest: ring3 null-write\n");  r3_null_write();  break;
    case R3_NULL_READ:      ulogf("crashtest: ring3 null-read\n");   r3_null_read();   break;
    case R3_DIVIDE_ZERO:    ulogf("crashtest: ring3 divide-zero\n"); r3_divide_zero(); break;
    case R3_BAD_OPCODE:     ulogf("crashtest: ring3 bad-opcode\n");  r3_bad_opcode();  break;
    case R3_STACK_OVERFLOW: ulogf("crashtest: ring3 stack-overflow\n"); r3_stack_overflow(); break;
    default: return;
    }
    // Only reached if a fault did not happen, which is worth saying --
    // silence would read as "the button did nothing".
    ulogf("crashtest: ring3 trigger RETURNED without faulting\n");
}

// The desktop is a process like any other: found by name in the process
// table and sent the signal. A real crash of toywm's own code would need
// a way to ask it to fault; the signal's report is the same evidence.
static void do_desktop(void) {
    struct proc_info p;
    for (int i = 0; i < SYS_PROC_MAX; i++) {
        if (sys_proc_info(i, &p) != 0 || p.pid == 0 || strcmp(p.name, "toywm") != 0) continue;
        char buf[64];
        snprintf(buf, sizeof buf, "crashtest: desktop SIGSEGV to pid %d\n", (int)p.pid);
        ulog(buf);
        sys_kill(p.pid, SIGSEGV);
        return;
    }
    ulog("crashtest: desktop -- no toywm process found\n");
}

static void do_kernel(struct uapp *a, int index) {
    struct crash_msg m;
    memset(&m, 0, sizeof m);
    m.op = CRASH_OP_TRIGGER;
    m.index = index;
    sys_crashtest(&m);
    // Reached only when the kernel refused: a successful trigger
    // panics and never comes back.
    g_armed = (m.flags & CRASH_F_ARMED) ? 1 : 0;
    char buf[96];
    snprintf(buf, sizeof buf, "crashtest: ring0 %s refused\n",
             index >= 0 && index < g_k0_count ? g_k0_label[index] : "?");
    ulog(buf);
    snprintf(g_status_msg, sizeof g_status_msg,
             "Refused: boot with `faultinject` to arm kernel faults");
    uapp_redraw(a);
}

static void on_widget(struct uapp *a, int id, int reason) {
    (void)id;
    // Commit on release, per docs/gui-guidelines.md -- and the group
    // enforces it too: take_activated() reports a code only when a
    // press and release landed on the same button, so a press dragged
    // off triggers nothing. On a crash button that matters more than
    // usual.
    if (reason != UUI_REASON_RELEASE) return;

    int code = uui_button_group_take_activated(&g_r3);
    if (code) { do_ring3(code); return; }

    code = uui_button_group_take_activated(&g_dk);
    if (code == DESKTOP_CRASH) { do_desktop(); return; }

    code = uui_button_group_take_activated(&g_k0);
    if (code >= K0_BASE) do_kernel(a, code - K0_BASE);
}

static struct uui_item ITEMS[] = {
    { .ops = &uui_button_group_ops, .widget = &g_r3, .id = 1 },
    { .ops = &uui_button_group_ops, .widget = &g_dk, .id = 4 },
    { .ops = &uui_button_group_ops, .widget = &g_k0, .id = 2 },
    { .ops = &uui_statusbar_ops,    .widget = &g_status, .id = 3 },
};

static void build(void) {
    static const char *const r3_names[] = {
        "null write", "null read", "divide by 0", "bad opcode", "stack overflow",
    };
    for (int i = 0; i < 5; i++) {
        uui_button_init(&g_r3_btn[i], 0, i * 30, 150, 26, r3_names[i],
                         UTHEME_BUTTON_BG, UTHEME_TEXT, i + 1);
    }
    uui_button_group_init(&g_r3, g_r3_btn, 5);
    uui_button_init(&g_dk_btn[0], 0, 0, 150, 26, "crash the desktop",
                    UTHEME_BUTTON_BG, UTHEME_TEXT, DESKTOP_CRASH);
    uui_button_group_init(&g_dk, g_dk_btn, 1);

    // The kernel's list, enumerated rather than hardcoded.
    struct crash_msg m;
    memset(&m, 0, sizeof m);
    m.op = CRASH_OP_LIST;
    m.index = 0;
    g_k0_count = 0;
    g_armed = 0;
    if (sys_crashtest(&m) == 0) {
        g_armed = (m.flags & CRASH_F_ARMED) ? 1 : 0;
        int n = m.count;
        if (n > MAX_KERNEL_KINDS) n = MAX_KERNEL_KINDS;
        for (int i = 0; i < n; i++) {
            memset(&m, 0, sizeof m);
            m.op = CRASH_OP_LIST;
            m.index = i;
            if (sys_crashtest(&m) != 0) break;
            strlcpy(g_k0_label[i], m.name, sizeof g_k0_label[i]);
            uui_button_init(&g_k0_btn[i], 0, i * 30, 150, 26, g_k0_label[i],
                             UTHEME_BUTTON_BG, UTHEME_TEXT, K0_BASE + i);
            g_k0_count = i + 1;
        }
    }
    uui_button_group_init(&g_k0, g_k0_btn, g_k0_count);

    snprintf(g_status_msg, sizeof g_status_msg,
             g_armed ? "Kernel faults ARMED -- a button panics the machine"
                      : "Kernel faults disarmed (boot with `faultinject`)");
    uui_statusbar_init(&g_status);
    g_status.panes[0].text = g_status_msg; // one stretching pane
    g_status.count = 1;

    char buf[64];
    snprintf(buf, sizeof buf, "crashtest: armed %d kinds %d\n", g_armed, g_k0_count);
    ulog(buf);
}

// Reported once the layout has placed everything, so a test clicks
// where the buttons ARE rather than re-deriving offsets in Python --
// four tools in this repo have drifted that way (CLAUDE.md).
static void log_layout(void) {
    static int done;
    if (done) return;
    done = 1;
    char buf[96];
    if (g_r3.count > 0) {
        snprintf(buf, sizeof buf, "crashtest: layout ring3 %d %d %d %d pitch %d count %d\n",
                 g_r3.buttons[0].x, g_r3.buttons[0].y, g_r3.buttons[0].w,
                 g_r3.buttons[0].h,
                 g_r3.count > 1 ? g_r3.buttons[1].y - g_r3.buttons[0].y : 0,
                 g_r3.count);
        uapp_log_layout_line(buf);
    }
    if (g_k0.count > 0) {
        snprintf(buf, sizeof buf, "crashtest: layout ring0 %d %d %d %d pitch %d count %d\n",
                 g_k0.buttons[0].x, g_k0.buttons[0].y, g_k0.buttons[0].w,
                 g_k0.buttons[0].h,
                 g_k0.count > 1 ? g_k0.buttons[1].y - g_k0.buttons[0].y : 0,
                 g_k0.count);
        uapp_log_layout_line(buf);
    }
    snprintf(buf, sizeof buf, "crashtest: layout desktop %d %d %d %d\n",
             g_dk.buttons[0].x, g_dk.buttons[0].y, g_dk.buttons[0].w, g_dk.buttons[0].h);
    uapp_log_layout_line(buf);
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    log_layout();
    struct ugfx_surface *s = d->surface;
    // Headings the layout does not draw for us. Deliberately plain: the
    // app is a test fixture, and the only thing worth emphasising is
    // which half of the window ends the process and which ends the
    // machine.
    ugfx_draw_string(s, 8, 4, "Ring 3 -- this app dies", d->fg, d->bg);
    if (g_dk.count > 0)
        ugfx_draw_string(s, g_dk.buttons[0].x, 4, "Desktop -- it restarts", d->fg, d->bg);
    if (g_k0.count > 0)
        ugfx_draw_string(s, g_k0.buttons[0].x, 4, "Ring 0 -- the machine panics", d->fg, d->bg);
}

// A ROW of the two button groups; the status bar rides along at the end.
// No coordinates in this file beyond each button's own row offset, which
// is the group's business.
static struct uui_layout LAYOUT = {
    .dir = UUI_ROW,
    .items = ITEMS,
    .count = 4,
};

int main(void) {
    build();
    struct uapp_desc desc = {
        .title = "Crash Test",
        .app_id = "crashtest",
        .flags = UAPP_SINGLE_INSTANCE,
        .layout = &LAYOUT,
        .widgets = ITEMS,
        .widget_count = 4,
        .on_widget = on_widget,
        .on_draw_over = on_draw,
    };
    return uapp_run(&desc);
}
