// Adapter settings (SYS_NET_LINK): the core's half -- validate a request
// against what the card declares, merge it with what is applied, and
// hand the driver the whole result. The driver's half is set_link().
//
// THE DRIVER GETS EVERY VALUE, NOT A DIFF. A firmware-run PHY (aq.c)
// takes rates, EEE and pause in one request, so a per-field op would
// have each driver rebuild the whole from pieces; the merge is here once.
// driver-none: the net core's adapter settings
#include "netdev.h"
#include "errno.h"
#include "klog.h"
#include "kfmt.h"
#include "ktest.h"

int net_link_merge(uint32_t caps, uint32_t supported, const struct net_link_values *cur,
                   uint32_t which, const struct net_link_values *want,
                   struct net_link_values *out) {
    if (which & ~NET_LINK_ALL) return -EINVAL;
    if (which & ~caps) return -ENOTSUP;
    *out = *cur;
    if (which & NET_LINK_RATES) {
        // NONE IS NOT A SETTING: a card offering no rate never links,
        // and "off" is `netctl down`, which says what it means.
        if (!want->rates || (want->rates & ~supported)) return -EINVAL;
        out->rates = want->rates;
    }
    if (which & NET_LINK_EEE) {
        if (want->eee > 1) return -EINVAL;
        out->eee = want->eee;
    }
    if (which & NET_LINK_FLOW) {
        if (want->flow & ~(NET_FLOW_RX | NET_FLOW_TX)) return -EINVAL;
        out->flow = want->flow;
    }
    if (which & NET_LINK_MODERATION) {
        if (want->moderation > NET_MOD_HIGH) return -EINVAL;
        out->moderation = want->moderation;
    }
    return 0;
}

int net_link_set(struct net_device *dev, uint32_t which, const struct net_link_values *want) {
    if (!dev->link_caps || !dev->set_link) return -ENOTSUP;
    struct net_link_values v;
    int r;
    if (which & NET_LINK_DEFAULTS) {
        v = dev->link_default;
    } else {
        r = net_link_merge(dev->link_caps, dev->rates_supported, &dev->link, which, want, &v);
        if (r) return r;
    }
    r = dev->set_link(dev, &v);
    if (r) return r;
    dev->link = v;
    klog_printf("net: %s: rates %x, eee %s, flow %u, moderation %u\n", dev->name,
                v.rates, v.eee ? "on" : "off", v.flow, v.moderation);
    return 0;
}

// --- KTESTs ----------------------------------------------------------

static const struct net_link_values k_cur = { NET_RATE_1G | NET_RATE_2G5, 0, 0, NET_MOD_MEDIUM };

KTEST("net_link", "a field the driver cannot change is refused, not ignored") {
    struct net_link_values want = { 0, 1, 0, 0 }, out;
    KTEST_ASSERT_EQ(net_link_merge(NET_LINK_RATES, NET_RATE_ALL, &k_cur, NET_LINK_EEE, &want, &out),
                    -ENOTSUP);
    KTEST_ASSERT_EQ(net_link_merge(NET_LINK_ALL, NET_RATE_ALL, &k_cur, 0x10, &want, &out), -EINVAL);
}

KTEST("net_link", "only the fields named change; the rest are what is applied") {
    struct net_link_values want = { 0, 1, NET_FLOW_RX, NET_MOD_OFF }, out;
    KTEST_ASSERT_EQ(net_link_merge(NET_LINK_ALL, NET_RATE_ALL, &k_cur, NET_LINK_EEE, &want, &out), 0);
    KTEST_ASSERT_EQ(out.eee, 1u);
    KTEST_ASSERT_EQ(out.rates, k_cur.rates);
    KTEST_ASSERT_EQ(out.flow, 0u);
    KTEST_ASSERT_EQ(out.moderation, (uint32_t)NET_MOD_MEDIUM);
}

KTEST("net_link", "rates must be ones the card offers, and never none") {
    struct net_link_values want = { NET_RATE_10G, 0, 0, 0 }, out;
    uint32_t sup = NET_RATE_100M | NET_RATE_1G;
    KTEST_ASSERT_EQ(net_link_merge(NET_LINK_ALL, sup, &k_cur, NET_LINK_RATES, &want, &out), -EINVAL);
    want.rates = 0;
    KTEST_ASSERT_EQ(net_link_merge(NET_LINK_ALL, sup, &k_cur, NET_LINK_RATES, &want, &out), -EINVAL);
    want.rates = NET_RATE_100M;
    KTEST_ASSERT_EQ(net_link_merge(NET_LINK_ALL, sup, &k_cur, NET_LINK_RATES, &want, &out), 0);
    KTEST_ASSERT_EQ(out.rates, (uint32_t)NET_RATE_100M);
}

KTEST("net_link", "values out of range are refused") {
    struct net_link_values out, want = { 0, 2, 0, 0 };
    KTEST_ASSERT_EQ(net_link_merge(NET_LINK_ALL, NET_RATE_ALL, &k_cur, NET_LINK_EEE, &want, &out), -EINVAL);
    want = (struct net_link_values){ 0, 0, 4, 0 };
    KTEST_ASSERT_EQ(net_link_merge(NET_LINK_ALL, NET_RATE_ALL, &k_cur, NET_LINK_FLOW, &want, &out), -EINVAL);
    want = (struct net_link_values){ 0, 0, 0, 4 };
    KTEST_ASSERT_EQ(net_link_merge(NET_LINK_ALL, NET_RATE_ALL, &k_cur, NET_LINK_MODERATION, &want, &out),
                    -EINVAL);
}

static int k_refuse;
static struct net_link_values k_seen;
static int fake_set_link(struct net_device *dev, const struct net_link_values *v) {
    (void)dev;
    if (k_refuse) return -EIO;
    k_seen = *v;
    return 0;
}

KTEST("net_link", "a driver's refusal keeps what was applied; defaults go back") {
    struct net_device d = { 0 };
    d.link_caps = NET_LINK_EEE | NET_LINK_MODERATION;
    d.rates_supported = NET_RATE_1G;
    d.link = (struct net_link_values){ NET_RATE_1G, 0, 0, NET_MOD_MEDIUM };
    d.link_default = d.link;
    d.set_link = fake_set_link;
    struct net_link_values want = { 0, 1, 0, NET_MOD_OFF };

    k_refuse = 1;
    KTEST_ASSERT_EQ(net_link_set(&d, NET_LINK_EEE, &want), -EIO);
    KTEST_ASSERT_EQ(d.link.eee, 0u);

    k_refuse = 0;
    KTEST_ASSERT_EQ(net_link_set(&d, NET_LINK_EEE | NET_LINK_MODERATION, &want), 0);
    KTEST_ASSERT_EQ(d.link.eee, 1u);
    KTEST_ASSERT_EQ(k_seen.moderation, (uint32_t)NET_MOD_OFF);
    KTEST_ASSERT_EQ(k_seen.rates, (uint32_t)NET_RATE_1G);   // the driver got every value

    KTEST_ASSERT_EQ(net_link_set(&d, NET_LINK_DEFAULTS, &want), 0);
    KTEST_ASSERT_EQ(d.link.eee, 0u);
    KTEST_ASSERT_EQ(d.link.moderation, (uint32_t)NET_MOD_MEDIUM);

    d.link_caps = 0;
    KTEST_ASSERT_EQ(net_link_set(&d, NET_LINK_EEE, &want), -ENOTSUP);
}
