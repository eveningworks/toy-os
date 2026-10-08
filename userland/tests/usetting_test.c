// A SETTING DECLARED BY A FILE behaves like one registered in the
// kernel -- which is the whole claim of moving the desktop's settings
// out of ring 0 (lib/usetting_schema.h).
//
// WHAT A BROKEN VERSION WOULD STILL PASS, which is what shaped this:
//
//   - Reading a value back through the same library proves nothing: a
//     library that never wrote the file answers from what it was told.
//     So the round trip is checked through uconf_get(), an INDEPENDENT
//     path to the same bytes.
//   - Every check below passes with SETTING_OP_TOUCH removed, except
//     the generation one -- and a machine where the generation does not
//     move is one where System Settings writes /etc and the desktop
//     never notices. That check is the reason this file exists.
//   - The bounds are asserted against the CODE's constants
//     (wm_taskbar.h), not against themselves, because the declaration
//     file and the clamp are twins now and nothing else compares them.
//
// It puts every value back before returning, on every path: a test that
// leaves taskbar_height at 56 changes the desktop for every later tool
// (CLAUDE.md records this happening with settings_test's mouse values).
#include <stdio.h>
#include <string.h>
#include "lib/usetting.h"
#include "lib/usetting_schema.h"
#include "lib/uconf.h"
#include "lib/utest.h"
#include "wm/wm_taskbar.h" // TASKBAR_H_MIN/MAX/STEP -- the twins

#define DESKTOP_CONF "/etc/desktop.conf"

static uint32_t generation(void) {
    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_COUNT;
    if (usetting_dispatch(&m) != 0) return 0;
    return m.generation;
}

int main(void) {
    utest_begin("usetting_test", "a schema-declared setting behaves like a registered one",
                UTEST_VERDICT_FILE);

    // --- it is in the merged list at all -----------------------------
    struct setting_msg info;
    int idx = usetting_find("desktop.taskbar_height", &info);
    utest_check(idx >= 0, "desktop.taskbar_height is in the merged registry");
    if (idx < 0) return utest_end();

    utest_check(info.type == SETTING_ABI_TYPE_INT, "it is an INT");
    utest_check(strcmp(info.ns, "desktop") == 0, "its namespace is its file's name");
    utest_check(strcmp(info.file, DESKTOP_CONF) == 0, "and it names that file");
    utest_check(strcmp(info.category, "Desktop") == 0 &&
                strcmp(info.group, "Taskbar") == 0, "with a category and a page");

    // THE DRIFT RATCHET. The declaration file bounds a value arriving
    // from System Settings; wm_taskbar.h bounds what the strip draws.
    // Nothing but this compares them.
    utest_checkf(info.imin == TASKBAR_H_MIN && info.imax == TASKBAR_H_MAX &&
                 info.istep == TASKBAR_H_STEP,
                 "its declared bounds match the code's (%d..%d/%d vs %d..%d/%d)",
                 info.imin, info.imax, info.istep,
                 TASKBAR_H_MIN, TASKBAR_H_MAX, TASKBAR_H_STEP);
    utest_check(strcmp(info.unit, "px") == 0, "and carries its unit");

    // **THE PRECONDITION IS ESTABLISHED, NOT INHERITED.** A previous run
    // of this test leaves 56 on disk, and the generation check below
    // asks whether a write MOVED it -- which a write of the value
    // already there correctly does not. Starting from "the key is
    // absent" makes the run independent of what ran before, and is what
    // the dirty-fixture rule in CLAUDE.md is about.
    char was[SETTING_ABI_VALUE_MAX];
    int had = uconf_get(DESKTOP_CONF, "taskbar_height", was, sizeof was) && was[0];
    if (had) uconf_unset(DESKTOP_CONF, "taskbar_height");

    // --- validation --------------------------------------------------
    utest_check(usetting_set("desktop.taskbar_height", "3") == SETTING_INVALID,
                "a value below the range is refused");
    utest_check(usetting_set("desktop.taskbar_height", "900") == SETTING_INVALID,
                "and one above it");
    utest_check(usetting_set("desktop.icon_size", "enormous") == SETTING_INVALID,
                "an enum value that is not a choice is refused");

    // --- the round trip, through an independent path -----------------
    uint32_t before = generation();
    int rc = usetting_set("desktop.taskbar_height", "56");
    utest_check(rc == SETTING_SAVED, "a legal value is accepted and saved");

    char on_disk[SETTING_ABI_VALUE_MAX];
    utest_check(uconf_get(DESKTOP_CONF, "taskbar_height", on_disk, sizeof on_disk) &&
                strcmp(on_disk, "56") == 0,
                "and the FILE says so, read without the settings library");

    // --- the generation, which is what the desktop watches -----------
    uint32_t after = generation();
    utest_checkf(after != before,
                 "the generation moved (%u -> %u), so a consumer notices",
                 before, after);

    // AND DOES NOT MOVE FOR A WRITE THAT CHANGES NOTHING. Without this
    // a slider applying on every pointer motion wakes every consumer,
    // which is a measured freeze rather than a theory.
    uint32_t again_before = generation();
    utest_check(usetting_set("desktop.taskbar_height", "56") == SETTING_SAVED,
                "setting it to what it already is still succeeds");
    utest_checkf(generation() == again_before,
                 "and does not move the generation (%u)", again_before);

    // --- unset -------------------------------------------------------
    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_UNSET;
    strlcpy(m.name, "desktop.taskbar_height", sizeof m.name);
    utest_check(usetting_dispatch(&m) == 0 && m.result == SETTING_SAVED,
                "unset removes the key");
    utest_check(!uconf_get(DESKTOP_CONF, "taskbar_height", on_disk, sizeof on_disk) ||
                !on_disk[0], "and the file no longer carries it");
    char now[SETTING_ABI_VALUE_MAX];
    utest_check(usetting_get("desktop.taskbar_height", now, sizeof now) &&
                strcmp(now, "48") == 0, "so the declared default answers");

    // --- the schema half is reachable directly -----------------------
    struct uschema s;
    utest_check(uschema_find("desktop.wallpaper", &s), "the wallpaper is declared");
    int n = uschema_choice_count(&s), aurora = 0;
    utest_check(n > 1, "and its choices come from a directory");
    for (int i = 0; i < n; i++) {
        char c[SETTING_ABI_VALUE_MAX];
        if (uschema_choice(&s, i, c, sizeof c) && strcmp(c, "aurora") == 0) aurora = 1;
    }
    utest_check(aurora, "as stems: aurora.jpg is the choice `aurora`");
    utest_check(!uschema_find("system.font_size", &s),
                "a KERNEL setting is not declared by a file");

    // ALWAYS, and before the verdict -- including the case where the key
    // was NOT there to begin with, which an earlier version of this test
    // got wrong and left 56 on disk for every later tool.
    if (had) usetting_set("desktop.taskbar_height", was);
    else uconf_unset(DESKTOP_CONF, "taskbar_height");
    return utest_end();
}
