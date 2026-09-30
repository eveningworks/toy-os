// Sockets, and the calls that configure the network stack: every
// socket syscall, and the fd_ops a socket descriptor dispatches through.
// The stack itself is socket.c, tcp.c and friends; this is its syscall
// face, as net/socket.c is Linux's.
#include "syscalls.h"
#include "errno.h"
#include "klog.h"
#include "heap.h"
#include "vmm.h"
#include "net.h"       // the socket layer
#include "conn_log.h"  // SYS_NET_RESOLVED feeds the connection log
#include "clocksource.h" // a receive deadline is real time
#include "string.h"

// A STREAM READ, shared by SYS_READ and SYS_RECVFROM so a connected
// socket behaves the same whichever the caller reaches for. Returns 1
// when it parked the caller, 0 when it wrote a result.
//
// The park's deadline is net_wait_deadline(): the caller's own timeout
// OR TCP's next retransmit, whichever is sooner. That is what drives
// this stack's timers -- the process waiting for data is the one that
// wakes up in time to retransmit, and it does the work in net_poll()
// on its way back in.
static int stream_read(struct syscall_ctx *c, int sock, uint64_t ubuf,
                       uint32_t cap, uint32_t timeout_ms, int nonblock) {
    if (cap > SYS_NET_STREAM_READ_MAX) cap = SYS_NET_STREAM_READ_MAX;
    net_poll();

    uint8_t *kbuf = kmalloc(cap ? cap : 1);
    if (!kbuf) { c->regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }
    int rc = net_sock_stream_recv(sock, kbuf, cap);

    if (rc == -EAGAIN && !nonblock) {
        kfree(kbuf);
        uint64_t deadline = net_sock_deadline(sock);
        uint64_t now = clocksource_now_ns();
        if (!deadline && timeout_ms) {
            deadline = now + (uint64_t)timeout_ms * 1000000ull;
            net_sock_set_deadline(sock, deadline);
        }
        if (deadline && now >= deadline) {
            net_sock_set_deadline(sock, 0);
            c->regs[14] = 0;              // waited as asked, nothing came
            return 0;
        }
        if (scheduler_block_current_until(c->regs, net_wait_chan(),
                                          SCHED_WAIT_NET, net_wait_deadline(deadline)))
            return 1;
        net_sock_set_deadline(sock, 0);
        c->regs[14] = 0;                  // no slot: cannot wait
        return 0;
    }

    if (rc == -EAGAIN) rc = 0;            // non-blocking: nothing yet
    if (rc > 0 && !vmm_copy_to_user(c->pml4, ubuf, kbuf, (uint64_t)rc)) rc = -EFAULT;
    kfree(kbuf);
    net_sock_set_deadline(sock, 0);
    c->regs[14] = (uint64_t)(int64_t)rc;
    return 0;
}

static int socket_fd_read(struct syscall_ctx *c, struct open_file *f,
                          uint64_t buf_ptr, uint64_t len) {
    // A CONNECTED STREAM IS A STREAM. POSIX guarantees read() and write()
    // work on one, and honouring that is what lets code written against
    // descriptors -- a pager, a copy loop, anything taking an fd -- use a
    // socket without knowing it has one. A DATAGRAM socket still refuses:
    // a read that cannot say who sent it is not a datagram interface.
    if (!net_sock_is_stream(f->socket.idx)) {
        klog_write(KLOG_ERR "syscall: read() rejected -- a datagram socket needs recvfrom()\n");
        c->regs[14] = (uint64_t)(int64_t)-EBADF;
        return 0;
    }
    return stream_read(c, f->socket.idx, buf_ptr, (uint32_t)len, 0, f->nonblock);
}

static int socket_fd_write(struct syscall_ctx *c, struct open_file *f,
                           uint64_t buf_ptr, uint64_t len) {
    // The write half of the same rule. It does NOT park: the send buffer
    // takes what it can and reports the count, and a short write is a
    // stream's own convention -- libsys loops.
    if (!net_sock_is_stream(f->socket.idx)) {
        klog_write(KLOG_ERR "syscall: write() rejected -- a datagram socket needs sendto()\n");
        c->regs[14] = (uint64_t)(int64_t)-EBADF;
        return 0;
    }
    uint64_t n = len;
    char *kbuf = fd_bounce_alloc(&n);
    if (!kbuf) { c->regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }
    if (!vmm_copy_from_user(c->pml4, kbuf, buf_ptr, n)) {
        kfree(kbuf);
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    int rc = net_sock_stream_send(f->socket.idx, kbuf, (uint32_t)n);
    kfree(kbuf);
    net_poll();   // put it on the wire before returning
    c->regs[14] = (uint64_t)(int64_t)rc;
    return 0;
}

static void socket_fd_open(struct open_file *f, int sock) { f->socket.idx = sock; }
// A socket holds a table entry, a bound port and a queue. Without this
// it leaks all three: the table is eight entries wide, so two runs of a
// program that opens four sockets leave the third unable to open ANY --
// which presents as the socket call failing rather than as anything to
// do with closing.
static void socket_fd_release(struct open_file *f) { net_sock_close(f->socket.idx); }

const struct fd_ops socket_fd_ops = {
    .name = "socket",
    .read = socket_fd_read, .write = socket_fd_write,
    .open = socket_fd_open, .release = socket_fd_release,
};

int sys_socket(struct syscall_ctx *c) {
    // AF_INET + SOCK_DGRAM + IPPROTO_ICMP is the whole supported set --
    // see abi/syscall_abi.h for why it is a ping socket rather than a
    // raw one. net_sock_open() decides; this only plumbs an fd onto it,
    // so a second protocol is a change there and not here.
    uint64_t pml4 = c->pml4;
    int sock = net_sock_open((int)c->a0, (int)c->a1, (int)c->a2);
    if (sock < 0) {
        c->regs[14] = (uint64_t)(int64_t)sock;
        return 0;
    }

    int di = fd_desc_alloc(&socket_fd_ops, sock);
    int fd = di >= 0 ? fd_install(pml4, di) : -1;
    if (fd < 0) {
        // The description owns the socket once it exists: its release
        // closes it, so closing it here as well would close it twice.
        if (di >= 0) fd_desc_unref(di); else net_sock_close(sock);
        // Which table ran out -- see sys_open()'s note.
        klog_write(di < 0
            ? "syscall: socket() rejected -- no free open-file description\n"
            : "syscall: socket() rejected -- fd table full\n");
        c->regs[14] = (uint64_t)(int64_t)(di < 0 ? -ENFILE : -EMFILE);
        return 0;
    }
    c->regs[14] = (uint64_t)fd;
    return 0;
}

// The socket table index behind an fd, or -1. Every call below starts
// here, so "is this fd a socket" is asked in one place.
static int sock_of_fd(struct syscall_ctx *c, int fd) {
    struct open_file *f = fd_get(c->pml4, fd);
    if (!f || f->ops != &socket_fd_ops) return -1;
    return f->socket.idx;
}

// SYS_SEND/SYS_RECV. A datagram socket has no peer until something
// names one, and nothing here does -- so these cannot say where to
// send or who sent it. EINVAL rather than ENOSYS: the call exists and
// the arguments are the problem (POSIX would say EDESTADDRREQ, which
// this kernel does not define -- see abi/errno.h's rule on adding one).
static int send_recv(struct syscall_ctx *c, int is_send) {
    if (sock_of_fd(c, (int)c->a0) < 0) {
        klog_write(is_send ? "syscall: send() rejected -- bad fd\n"
                           : "syscall: recv() rejected -- bad fd\n");
        c->regs[14] = (uint64_t)(int64_t)-EBADF;
    } else {
        klog_write(is_send ? "syscall: send() rejected -- no peer; use sendto()\n"
                           : "syscall: recv() rejected -- no peer; use recvfrom()\n");
        c->regs[14] = (uint64_t)(int64_t)-EINVAL;
    }
    return 0;
}

int sys_send(struct syscall_ctx *c) { return send_recv(c, 1); }
int sys_recv(struct syscall_ctx *c) { return send_recv(c, 0); }

int sys_sendto(struct syscall_ctx *c) {
    int sock = sock_of_fd(c, (int)c->a0);
    if (sock < 0) { c->regs[14] = (uint64_t)(int64_t)-EBADF; return 0; }

    struct net_msg m;
    if (!vmm_copy_from_user(c->pml4, &m, c->a1, sizeof m)) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    if (m.len > SYS_NET_MSG_MAX) { c->regs[14] = (uint64_t)(int64_t)-EINVAL; return 0; }

    // Bounce through the kernel's own buffer: the stack builds a frame
    // around this and hands the result to a driver, so a user page that
    // could be unmapped mid-transmit must not be the thing being sent.
    // From the heap and at the EXACT size -- fd_bounce_alloc() shrinks on
    // a fragmented heap, which is right for a byte stream and wrong for
    // a datagram, where a short buffer is a different message.
    uint8_t *payload = m.len ? kmalloc(m.len) : (uint8_t *)"";
    if (!payload) { c->regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }
    if (m.len && !vmm_copy_from_user(c->pml4, payload, m.buf, m.len)) {
        kfree(payload);
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }

    net_poll();   // answer anything outstanding first, so ARP resolves
    int rc = net_sock_sendto(sock, m.addr, m.port, payload, m.len);
    if (m.len) kfree(payload);
    c->regs[14] = (uint64_t)(int64_t)rc;
    return 0;
}

int sys_recvfrom(struct syscall_ctx *c) {
    int sock = sock_of_fd(c, (int)c->a0);
    if (sock < 0) { c->regs[14] = (uint64_t)(int64_t)-EBADF; return 0; }

    struct net_msg m;
    if (!vmm_copy_from_user(c->pml4, &m, c->a1, sizeof m)) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    if (m.len > SYS_NET_MSG_MAX) m.len = SYS_NET_MSG_MAX;

    if (net_sock_is_stream(sock)) {
        struct open_file *sf = fd_get(c->pml4, (int)c->a0);
        return stream_read(c, sock, m.buf, m.len, m.timeout_ms, sf && sf->nonblock);
    }

    // Run the stack before looking: a reply that arrived while this
    // process was not scheduled is sitting in the receive queue, and a
    // process woken by net_rx() is here precisely to do this -- the
    // interrupt only queued a frame and woke everybody, so the parsing
    // happens HERE, in a process context, and not in the driver's ISR.
    net_poll();

    uint8_t *payload = kmalloc(m.len ? m.len : 1);
    if (!payload) { c->regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }
    uint32_t src = 0;
    uint16_t port = 0;
    int rc = net_sock_recvfrom(sock, payload, m.len, &src, &port);

    if (rc == 0) {
        struct open_file *f = fd_get(c->pml4, (int)c->a0);
        if (f && f->nonblock) {
            kfree(payload);
            net_sock_set_deadline(sock, 0);
            c->regs[14] = 0;
            return 0;
        }

        // THE DEADLINE IS THE SOCKET'S, NOT THIS FRAME'S. A blocking
        // syscall here is RE-RUN rather than resumed (SYS_RETRY, and a
        // signal rewinds it), so a deadline computed from `timeout_ms`
        // on every entry would restart the clock on every wake and a
        // repeatedly-woken receive would never time out.
        uint64_t deadline = net_sock_deadline(sock);
        uint64_t now = clocksource_now_ns();
        if (!deadline && m.timeout_ms) {
            deadline = now + (uint64_t)m.timeout_ms * 1000000ull;
            net_sock_set_deadline(sock, deadline);
        }

        if (deadline && now >= deadline) {
            kfree(payload);
            net_sock_set_deadline(sock, 0);
            c->regs[14] = 0;   // waited as asked, and nothing came
            return 0;
        }

        kfree(payload);
        if (scheduler_block_current_until(c->regs, net_wait_chan(),
                                          SCHED_WAIT_NET, deadline))
            return 1;   // parked: do NOT write a return value

        // No scheduler slot -- the legacy loader. It cannot block, so
        // it gets the old non-blocking answer rather than a hang.
        net_sock_set_deadline(sock, 0);
        c->regs[14] = 0;
        return 0;
    }

    if (rc > 0) {
        m.addr = src;
        m.port = port;
        m.len = (uint32_t)rc;
        if (!vmm_copy_to_user(c->pml4, m.buf, payload, (uint64_t)rc) ||
            !vmm_copy_to_user(c->pml4, c->a1, &m, sizeof m)) {
            kfree(payload);
            net_sock_set_deadline(sock, 0);
            c->regs[14] = (uint64_t)(int64_t)-EFAULT;
            return 0;
        }
    }
    kfree(payload);
    net_sock_set_deadline(sock, 0);   // this wait is over, however it ended
    c->regs[14] = (uint64_t)(int64_t)rc;
    return 0;
}

int sys_bind(struct syscall_ctx *c) {
    int sock = sock_of_fd(c, (int)c->a0);
    if (sock < 0) { c->regs[14] = (uint64_t)(int64_t)-EBADF; return 0; }

    struct net_msg m;
    if (!vmm_copy_from_user(c->pml4, &m, c->a1, sizeof m)) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    m.dev[sizeof m.dev - 1] = 0;
    c->regs[14] = (uint64_t)(int64_t)net_sock_bind(sock, m.addr, m.port, m.dev);
    return 0;
}

int sys_connect(struct syscall_ctx *c) {
    int sock = sock_of_fd(c, (int)c->a0);
    if (sock < 0) { c->regs[14] = (uint64_t)(int64_t)-EBADF; return 0; }

    struct net_msg m;
    if (!vmm_copy_from_user(c->pml4, &m, c->a1, sizeof m)) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }

    net_poll();
    int state = net_sock_connect_state(sock);
    if (state == -ENOTCONN) {
        // Not started yet: this is the first entry. Every later entry
        // is a RE-RUN after a wake, which must not start a second
        // handshake -- so the decision is made from the connection's
        // own state rather than from a flag this handler would have to
        // keep across a park.
        int rc = net_sock_connect(sock, m.addr, m.port);
        if (rc < 0) { c->regs[14] = (uint64_t)(int64_t)rc; return 0; }
        state = -EAGAIN;
    }
    if (state != -EAGAIN) {
        net_sock_set_deadline(sock, 0);
        c->regs[14] = (uint64_t)(int64_t)state;   // 0, or why it failed
        return 0;
    }

    uint64_t deadline = net_sock_deadline(sock);
    uint64_t now = clocksource_now_ns();
    if (!deadline) {
        uint32_t ms = m.timeout_ms ? m.timeout_ms : SYS_NET_CONNECT_MS;
        deadline = now + (uint64_t)ms * 1000000ull;
        net_sock_set_deadline(sock, deadline);
    }
    if (now >= deadline) {
        net_sock_set_deadline(sock, 0);
        c->regs[14] = (uint64_t)(int64_t)-ECONNRESET;   // nobody answered
        return 0;
    }
    if (scheduler_block_current_until(c->regs, net_wait_chan(),
                                      SCHED_WAIT_NET, net_wait_deadline(deadline)))
        return 1;

    net_sock_set_deadline(sock, 0);
    c->regs[14] = (uint64_t)(int64_t)-EAGAIN;   // no slot: cannot wait
    return 0;
}

int sys_listen(struct syscall_ctx *c) {
    int sock = sock_of_fd(c, (int)c->a0);
    if (sock < 0) { c->regs[14] = (uint64_t)(int64_t)-EBADF; return 0; }
    c->regs[14] = (uint64_t)(int64_t)net_sock_listen(sock);
    return 0;
}

int sys_accept(struct syscall_ctx *c) {
    int sock = sock_of_fd(c, (int)c->a0);
    if (sock < 0) { c->regs[14] = (uint64_t)(int64_t)-EBADF; return 0; }

    struct net_msg m;
    k_memset(&m, 0, sizeof m);
    if (c->a1 && !vmm_copy_from_user(c->pml4, &m, c->a1, sizeof m)) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }

    // The stack runs first for the same reason a receive runs it: the
    // handshake that finished while this process was parked completes
    // HERE, in a process context, not in the driver's interrupt.
    net_poll();
    int conn = net_sock_accept(sock);

    if (conn == -EAGAIN) {
        struct open_file *f = fd_get(c->pml4, (int)c->a0);
        if (f && f->nonblock) { c->regs[14] = (uint64_t)(int64_t)-EAGAIN; return 0; }

        uint64_t deadline = net_sock_deadline(sock);
        uint64_t now = clocksource_now_ns();
        if (!deadline && m.timeout_ms) {
            deadline = now + (uint64_t)m.timeout_ms * 1000000ull;
            net_sock_set_deadline(sock, deadline);
        }
        if (deadline && now >= deadline) {
            net_sock_set_deadline(sock, 0);
            c->regs[14] = (uint64_t)(int64_t)-EAGAIN;
            return 0;
        }
        // A LISTENER BLOCKED HERE IS WHAT DRIVES EVERY CONNECTION IT
        // MADE. net_wait_deadline() folds in TCP's next retransmit, so
        // a server waiting for its next client still wakes in time to
        // retransmit for the one it is already serving.
        if (scheduler_block_current_until(c->regs, net_wait_chan(),
                                          SCHED_WAIT_NET, net_wait_deadline(deadline)))
            return 1;
        net_sock_set_deadline(sock, 0);
        c->regs[14] = (uint64_t)(int64_t)-EAGAIN;
        return 0;
    }
    net_sock_set_deadline(sock, 0);
    if (conn < 0) { c->regs[14] = (uint64_t)(int64_t)conn; return 0; }

    int di = fd_desc_alloc(&socket_fd_ops, conn);
    int fd = di >= 0 ? fd_install(c->pml4, di) : -1;
    if (fd < 0) {
        if (di >= 0) fd_desc_unref(di); else net_sock_close(conn);
        c->regs[14] = (uint64_t)(int64_t)(di < 0 ? -ENFILE : -EMFILE);
        return 0;
    }

    if (c->a1) {
        net_sock_peer(conn, &m.addr, &m.port);
        (void)vmm_copy_to_user(c->pml4, c->a1, &m, sizeof m);
    }
    c->regs[14] = (uint64_t)fd;
    return 0;
}

int sys_net_config(struct syscall_ctx *c) {
    struct net_ifconfig req;
    if (!vmm_copy_from_user(c->pml4, &req, c->a0, sizeof req)) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    req.name[sizeof req.name - 1] = 0;

    struct net_device *d = net_device_by_name(req.name);
    if (!d) { c->regs[14] = (uint64_t)(int64_t)-ENODEV; return 0; }

    if (req.ip) d->ip = req.ip;
    if (req.netmask) d->netmask = req.netmask;
    if (req.gateway) d->gateway = req.gateway;

    // The cache is keyed by (device, IP) and every entry on this device
    // was learned under the OLD address; keeping them would answer for
    // a subnet this card has just left.
    arp_cache_flush();
    c->regs[14] = 0;
    return 0;
}

// Renaming is the whole of what the kernel lets ring 3 do to a name:
// it validates and applies, and has no opinion about what the name
// should be. /bin/netd holds the rules.
int sys_net_rename(struct syscall_ctx *c) {
    struct net_rename req;
    if (!vmm_copy_from_user(c->pml4, &req, c->a0, sizeof req)) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    req.name[sizeof req.name - 1] = 0;
    req.to[sizeof req.to - 1] = 0;

    struct net_device *d = net_device_by_name(req.name);
    if (!d) { c->regs[14] = (uint64_t)(int64_t)-ENODEV; return 0; }

    c->regs[14] = net_rename(d, req.to) ? 0 : (uint64_t)(int64_t)-EINVAL;
    return 0;
}

// A resolver reporting what it just looked up, so the connection log
// can print a name beside an address. See abi/syscall_abi.h for why
// this is a report rather than a lookup, and what trusting it costs.
int sys_net_resolved(struct syscall_ctx *c) {
    struct net_resolved req;
    if (!vmm_copy_from_user(c->pml4, &req, c->a0, sizeof req)) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    req.name[sizeof req.name - 1] = 0;
    if (!req.ip || !req.name[0]) { c->regs[14] = (uint64_t)(int64_t)-EINVAL; return 0; }

    conn_log_name_hint(req.ip, req.name);
    c->regs[14] = 0;
    return 0;
}

// Has anybody claimed this address? The mechanism half of RFC 3927's
// duplicate-address detection: one ARP request, and whether a reply has
// come back for it yet.
//
// arp_resolve() IS the probe -- it broadcasts a request whose sender
// field is the device's own address, which is 0.0.0.0 on a device that
// has none, and rate-limits itself to one frame a second. A caller
// therefore gets RFC 3927's probe spacing by asking three times a
// second apart, and its announcement by asking once more after the
// address has been applied.
int sys_net_arp_probe(struct syscall_ctx *c) {
    struct net_arp_probe req;
    if (!vmm_copy_from_user(c->pml4, &req, c->a0, sizeof req)) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    req.name[sizeof req.name - 1] = 0;
    if (!req.ip) { c->regs[14] = (uint64_t)(int64_t)-EINVAL; return 0; }

    struct net_device *d = net_device_by_name(req.name);
    if (!d) { c->regs[14] = (uint64_t)(int64_t)-ENODEV; return 0; }

    // Drain what has arrived before answering, the same thing a woken
    // socket reader does: the stack otherwise runs only from
    // scheduler_idle(), so a reply sitting in the receive queue would
    // be reported as silence.
    net_poll();

    uint8_t mac[NET_MAC_LEN];
    c->regs[14] = (uint64_t)(int64_t)(arp_resolve(d, req.ip, mac) ? 1 : 0);
    return 0;
}
