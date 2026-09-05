// The network device registry: naming, renaming, and what removal has
// to undo.
//
// IT USES THE LIVE REGISTRY, which already holds whatever card this
// machine has, so every check works on a fixture device and asserts
// DELTAS. A fixture must be unregistered again even when a check fails
// early, or the next boot's real card finds one fewer slot.
//
// driver-none: the net registry's tests
#include "netdev.h"
#include "net.h"
#include "ktest.h"
#include "string.h"

static int fake_tx(struct net_device *dev, const void *frame, uint32_t len) {
    (void)dev; (void)frame; (void)len;
    return 0;
}

static struct net_device g_fixture;

static void fixture_init(uint8_t tail0, uint8_t tail1, uint8_t tail2) {
    k_memset(&g_fixture, 0, sizeof g_fixture);
    g_fixture.driver = "ktest-net";
    g_fixture.transmit = fake_tx;
    g_fixture.mac[0] = 0x02;                       // locally administered
    g_fixture.mac[3] = tail0;
    g_fixture.mac[4] = tail1;
    g_fixture.mac[5] = tail2;
}

// The last three bytes of a MAC are the vendor's own serial for that
// card; the first three name the vendor. So the name is an identity the
// card carries, not a truncation chosen for length.
KTEST("net-registry", "a name is made from the card's own serial") {
    fixture_init(0x71, 0x8E, 0xBF);
    KTEST_ASSERT(net_register(&g_fixture));
    KTEST_ASSERT(k_strcmp(g_fixture.name, "net-718ebf") == 0);
    net_unregister(&g_fixture);
}

// THE POINT OF THE WHOLE SCHEME. A card moved to another socket is the
// same card, so it must come back as the same interface -- which is
// where an address-derived name (systemd's enp3s0) was rejected.
KTEST("net-registry", "moving a card to another socket does not rename it") {
    fixture_init(0x11, 0x22, 0x33);
    k_strlcpy(g_fixture.location, "usb13", NET_LOC_MAX);
    KTEST_ASSERT(net_register(&g_fixture));
    char first[NET_NAME_MAX];
    k_strlcpy(first, g_fixture.name, sizeof first);
    net_unregister(&g_fixture);

    k_strlcpy(g_fixture.location, "usb5", NET_LOC_MAX);   // a different port
    KTEST_ASSERT(net_register(&g_fixture));
    KTEST_ASSERT(k_strcmp(g_fixture.name, first) == 0);
    net_unregister(&g_fixture);
}

// Nothing about probe order may reach a name. It used to: the name was
// the table index, so it was handed to the next card once the first
// went away -- and sockets bind by NAME.
KTEST("net-registry", "two cards get different names whatever the order") {
    static struct net_device other;
    fixture_init(0xAA, 0xBB, 0xCC);
    k_memset(&other, 0, sizeof other);
    other.driver = "ktest-net"; other.transmit = fake_tx;
    other.mac[0] = 0x02; other.mac[3] = 0xDD; other.mac[4] = 0xEE; other.mac[5] = 0xFF;

    KTEST_ASSERT(net_register(&g_fixture));
    KTEST_ASSERT(net_register(&other));
    KTEST_ASSERT(k_strcmp(g_fixture.name, "net-aabbcc") == 0);
    KTEST_ASSERT(k_strcmp(other.name, "net-ddeeff") == 0);
    net_unregister(&g_fixture);
    KTEST_ASSERT(k_strcmp(other.name, "net-ddeeff") == 0);  // not renumbered
    net_unregister(&other);
}

// This is the bug that started it: an adapter unplugged and plugged
// back in registered a second time, and one card was listed twice.
KTEST("net-registry", "registering the same device twice is refused") {
    fixture_init(0x09, 0x09, 0x09);
    int before = net_device_count();
    KTEST_ASSERT(net_register(&g_fixture));
    KTEST_ASSERT_EQ(net_device_count(), before + 1);
    KTEST_ASSERT(!net_register(&g_fixture));
    KTEST_ASSERT_EQ(net_device_count(), before + 1);
    net_unregister(&g_fixture);
    KTEST_ASSERT_EQ(net_device_count(), before);
}

KTEST("net-registry", "a removed device is not found by name and is not listed") {
    fixture_init(0x09, 0x09, 0x08);
    KTEST_ASSERT(net_register(&g_fixture));
    KTEST_ASSERT(net_device_by_name("net-090908") == &g_fixture);
    net_unregister(&g_fixture);
    KTEST_ASSERT(net_device_by_name("net-090908") == 0);
}

// A card that comes back must not come back configured: the address was
// a lease on a network it may no longer be attached to.
KTEST("net-registry", "removal clears the address and the link state") {
    fixture_init(0x09, 0x09, 0x07);
    KTEST_ASSERT(net_register(&g_fixture));
    g_fixture.ip = 0xC0A80001; g_fixture.netmask = 0xFFFFFF00;
    g_fixture.gateway = 0xC0A800FE;
    g_fixture.link_known = 1; g_fixture.link_up = 1; g_fixture.link_bps = 1000000000u;

    net_unregister(&g_fixture);
    KTEST_ASSERT_EQ(g_fixture.ip, 0u);
    KTEST_ASSERT_EQ(g_fixture.netmask, 0u);
    KTEST_ASSERT_EQ(g_fixture.gateway, 0u);
    KTEST_ASSERT_EQ((uint32_t)g_fixture.link_known, 0u);
    KTEST_ASSERT_EQ((uint32_t)g_fixture.link_up, 0u);
}

KTEST("net-registry", "removing an unknown device does nothing") {
    fixture_init(0x09, 0x09, 0x06);
    int before = net_device_count();
    net_unregister(&g_fixture);
    KTEST_ASSERT_EQ(net_device_count(), before);
}

// --- renaming, which is what /bin/netd does over SYS_NET_RENAME ------

KTEST("net-registry", "a device can be renamed and answers to the new name") {
    fixture_init(0x09, 0x09, 0x05);
    KTEST_ASSERT(net_register(&g_fixture));
    KTEST_ASSERT(net_rename(&g_fixture, "lan"));
    KTEST_ASSERT(k_strcmp(g_fixture.name, "lan") == 0);
    KTEST_ASSERT(net_device_by_name("lan") == &g_fixture);
    KTEST_ASSERT(net_device_by_name("net-090905") == 0);
    net_unregister(&g_fixture);
}

// A name reaches /var/dhcp-<name>.lease and a socket's device binding,
// so what would break either is refused here rather than wherever it
// was first noticed.
KTEST("net-registry", "a name a lease file could not survive is refused") {
    fixture_init(0x09, 0x09, 0x04);
    KTEST_ASSERT(net_register(&g_fixture));
    KTEST_ASSERT(!net_rename(&g_fixture, ""));
    KTEST_ASSERT(!net_rename(&g_fixture, "two words"));
    KTEST_ASSERT(!net_rename(&g_fixture, "a=b"));
    KTEST_ASSERT(!net_rename(&g_fixture, "a/b"));
    KTEST_ASSERT(!net_rename(&g_fixture, "0123456789abcdef"));  // NET_NAME_MAX
    KTEST_ASSERT(k_strcmp(g_fixture.name, "net-090904") == 0);  // none took
    net_unregister(&g_fixture);
}

KTEST("net-registry", "a name another device already holds is refused") {
    static struct net_device other;
    fixture_init(0x09, 0x09, 0x03);
    k_memset(&other, 0, sizeof other);
    other.driver = "ktest-net"; other.transmit = fake_tx;
    other.mac[0] = 0x02; other.mac[5] = 0x02;

    KTEST_ASSERT(net_register(&g_fixture));
    KTEST_ASSERT(net_register(&other));
    KTEST_ASSERT(!net_rename(&other, g_fixture.name));
    KTEST_ASSERT(k_strcmp(other.name, "net-000002") == 0);
    // Renaming a device to what it is already called is a no-op, not a
    // clash with itself.
    KTEST_ASSERT(net_rename(&other, "net-000002"));
    net_unregister(&other);
    net_unregister(&g_fixture);
}
