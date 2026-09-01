// The driver registry's own logic. It records rather than dispatches,
// so what can go wrong is bookkeeping: a driver counted twice, a
// binding lost, a class that never gets filled in.
//
// IT USES THE LIVE REGISTRY, which is already populated by whatever
// this machine has -- so every check works on names that cannot collide
// with a real driver, and asserts about DELTAS rather than absolute
// counts. A test that assumed an empty registry would pass alone and
// fail in a boot.
#include "ktest.h"
#include "driver.h"
#include "string.h"

static const char *devs_of(const char *name) {
    for (int i = 0; i < driver_count(); i++)
        if (k_strcmp(driver_name_at(i), name) == 0) return driver_devices_at(i);
    return 0;
}

KTEST("driver", "a driver registers once, however often it says so") {
    int before = driver_count();
    driver_register_at("ktest-a", "block", "kernel/core/driver_test.c");
    KTEST_ASSERT_EQ(driver_count(), before + 1);
    driver_register_at("ktest-a", "block", "kernel/core/driver_test.c");
    KTEST_ASSERT_EQ(driver_count(), before + 1);
}

KTEST("driver", "bindings accumulate, and a driver with none says so") {
    driver_register_at("ktest-b", "net", "kernel/core/driver_test.c");
    const char *d = devs_of("ktest-b");
    KTEST_ASSERT(d && d[0] == '\0');      // present, driving nothing

    driver_bound("ktest-b", "net9");
    KTEST_ASSERT(k_strcmp(devs_of("ktest-b"), "net9") == 0);
    driver_bound("ktest-b", "usb:7");
    KTEST_ASSERT(k_strcmp(devs_of("ktest-b"), "net9 usb:7") == 0);
}

KTEST("driver", "a bind before a declaration still records the device") {
    // The order is not guaranteed: a USB driver binds from the
    // enumerator, which may run before anything called its init. Losing
    // the fact would be worse than an unknown class, since the binding
    // is the half that had no other home at all.
    int before = driver_count();
    driver_bound("ktest-c", "usb:3");
    KTEST_ASSERT_EQ(driver_count(), before + 1);
    KTEST_ASSERT(k_strcmp(devs_of("ktest-c"), "usb:3") == 0);

    // ...and the later declaration fills in what it did not know.
    driver_register_at("ktest-c", "input", "kernel/core/driver_test.c");
    for (int i = 0; i < driver_count(); i++)
        if (k_strcmp(driver_name_at(i), "ktest-c") == 0)
            KTEST_ASSERT(k_strcmp(driver_class_at(i), "input") == 0);
}

KTEST("driver", "a device name that does not fit is dropped whole") {
    // Half a device name reads as a device that does not exist, which
    // is worse than a device missing from the list.
    driver_register_at("ktest-d", "block", "kernel/core/driver_test.c");
    char big[DRIVER_DEVS_MAX + 8];
    for (unsigned i = 0; i < sizeof big - 1; i++) big[i] = 'x';
    big[sizeof big - 1] = '\0';
    driver_bound("ktest-d", big);
    KTEST_ASSERT(devs_of("ktest-d")[0] == '\0');   // refused, not truncated
}
