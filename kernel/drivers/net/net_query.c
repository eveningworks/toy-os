// The network device table, as queryable facts -- what `/bin/ifconfig`
// reads. A LIST and nothing else, the same shape (and for the same
// reason) as block_query.c: zero records means one thing only, that no
// card was found.
#include "query.h"
#include "netdev.h"
#include "string.h"
#include <stddef.h>

// driver-none: a QUERY provider over the net devices

static int netdev_count(void) { return net_device_count(); }

static int netdev_fill(int index, void *out) {
    struct net_device *d = net_device_at(index);
    if (!d) return 0;

    struct query_netdev *q = out;
    k_memset(q, 0, sizeof *q);
    k_strlcpy(q->name, d->name, sizeof q->name);
    k_strlcpy(q->driver, d->driver ? d->driver : "?", sizeof q->driver);

    // Six bytes in one field, low-order first, so the record stays a
    // flat struct of scalars -- a char[6] would be the only array in
    // it and would need its own formatting rule on the far side.
    uint64_t mac = 0;
    for (int i = 0; i < NET_MAC_LEN; i++) mac |= (uint64_t)d->mac[i] << (i * 8);
    q->mac = mac;

    q->ip = d->ip;
    q->netmask = d->netmask;
    q->gateway = d->gateway;
    q->mtu = d->mtu;
    q->rx_packets = d->rx_packets;
    q->rx_bytes = d->rx_bytes;
    q->rx_dropped = d->rx_dropped;
    q->tx_packets = d->tx_packets;
    q->tx_bytes = d->tx_bytes;
    q->tx_dropped = d->tx_dropped;
    q->link_known = d->link_known;
    q->link_up    = d->link_up;
    q->link_bps   = d->link_bps;
    return 1;
}

static const struct query_provider netdev_provider = {
    .cls = QUERY_NETDEV,
    .name = "netdev",
    .record_size = sizeof(struct query_netdev),
    .flags = QUERY_F_LIST,
    .count = netdev_count,
    .fill = netdev_fill,
    .fields = 0,
    .field_count = 0,
};

void net_query_init(void) {
    query_register(&netdev_provider);
}
