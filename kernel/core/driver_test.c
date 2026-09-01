// The driver registry's own logic. It records rather than dispatches,
// so what can go wrong is bookkeeping: a driver missing from the
// listing, a binding lost, a class that never gets filled in.
//
// IT USES THE LIVE REGISTRY, which is already populated by whatever
// this machine has -- so every check works against a real driver or a
// name that cannot collide with one, and asserts about DELTAS rather
// than absolute counts. A test that assumed an empty registry would
// pass alone and fail in a boot.
//
// NO DRIVER_DECLARE HERE, deliberately: a declaration is data in the
// image, so a fake one would sit in every `lsdrv` a user ever runs. The
// declaration half is checked against a REAL driver instead, and the
// binding half through the undeclared-driver path, which only records
// anything once these tests have run.
//
// driver-none: the driver registry's tests
#include "ktest.h"
#include "driver.h"
#include "string.h"

static int index_of(const char *name) {
    for (int i = 0; i < driver_count(); i++)
        if (k_strcmp(driver_name_at(i), name) == 0) return i;
    return -1;
}

static const char *devs_of(const char *name) {
    int i = index_of(name);
    return i < 0 ? 0 : driver_devices_at(i);
}

KTEST("driver", "a declaration reaches the registry with all four fields") {
    // i8042 rather than a fixture: it is the PS/2 controller, compiled
    // into every build of this kernel, and reaching it at all is what
    // proves the .drivers section arrives.
    int i = index_of("i8042");
    KTEST_ASSERT(i >= 0);
    KTEST_ASSERT(k_strcmp(driver_class_at(i), "input") == 0);
    KTEST_ASSERT(k_strstr(driver_file_at(i), "i8042.c") != 0);
    KTEST_ASSERT(driver_desc_at(i)[0] != '\0');
}

KTEST("driver", "a driver that found no hardware is still listed") {
    // THE REGRESSION THIS EXISTS FOR. DRIVER_REGISTER used to be a call
    // inside init(), and e1000's sat after the "no card on this bus"
    // return -- so a build containing the driver listed no driver, which
    // is the one question lsdrv answers that no class registry does.
    // True whether or not this machine has the card -- and asserted on
    // the DECLARATION rather than on mere presence, because a machine
    // that HAS the card gets an entry from driver_bound() either way.
    // Only a declaration carries a class and a file, so a control that
    // deletes one reddens this on both kinds of machine.
    int i = index_of("e1000");
    KTEST_ASSERT(i >= 0);
    KTEST_ASSERT(k_strcmp(driver_class_at(i), "net") == 0);
    KTEST_ASSERT(k_strstr(driver_file_at(i), "e1000.c") != 0);
    KTEST_ASSERT(driver_devices_at(i) != 0);   // never NULL, so it prints
}

KTEST("driver", "a bind naming an undeclared driver still records it") {
    // The order is not guaranteed: a USB driver binds from the
    // enumerator, which may run before anything called its init. Losing
    // the fact would be worse than an unknown class, since the binding
    // is the half that had no other home at all.
    driver_bound("ktest-c", "usb:3");
    int i = index_of("ktest-c");
    KTEST_ASSERT(i >= 0);
    KTEST_ASSERT(k_strcmp(driver_class_at(i), "?") == 0);
    KTEST_ASSERT(driver_devices_at(i)[0] != '\0');
}

KTEST("driver", "bindings accumulate, space-separated") {
    driver_bound("ktest-b", "net9");
    const char *d = devs_of("ktest-b");
    KTEST_ASSERT(d != 0);
    // A DELTA, not an absolute: `ktest` is runnable more than once in a
    // boot and the list is append-only.
    uint32_t before = (uint32_t)k_strlen(d);
    if (before + 7 >= DRIVER_DEVS_MAX) return;   // filled by an earlier run

    driver_bound("ktest-b", "usb:7");
    d = devs_of("ktest-b");
    uint32_t now = (uint32_t)k_strlen(d);
    KTEST_ASSERT_EQ(now, before + 6);            // " usb:7"
    KTEST_ASSERT(k_strcmp(d + now - 5, "usb:7") == 0);
}

KTEST("driver", "a device name that does not fit is dropped whole") {
    // Half a device name reads as a device that does not exist, which
    // is worse than a device missing from the list.
    char big[DRIVER_DEVS_MAX + 8];
    for (unsigned i = 0; i < sizeof big - 1; i++) big[i] = 'x';
    big[sizeof big - 1] = '\0';
    driver_bound("ktest-d", big);
    KTEST_ASSERT(devs_of("ktest-d")[0] == '\0');   // refused, not truncated
}
