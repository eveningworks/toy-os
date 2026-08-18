// The TWP transport registry, and the one implementation there is.
//
// See kernel/include/kernel/win_transport.h for what this is for and why
// it exists a whole stage before anything needs two of them.

#include "win_transport.h"
#include "win_server.h"
#include "klog.h"
#include "win_debug.h"   // the apps/-facing wrapper at the bottom
#include "string.h"

// --- the direct transport --------------------------------------------
//
// Client and server are in the same address space today, so "carriage"
// is a function call. That is not a placeholder: with the WM in ring 0 it
// is the correct implementation, and it stays the correct one for any
// future in-kernel server. What stage 4 adds is a SECOND one, for a
// server that is a process.

static int direct_request(int pid, struct win_request_msg *req) {
    return win_server_request(pid, req);
}

static int direct_debug(int pid, struct win_debug_msg *msg) {
    return win_server_debug(pid, msg);
}

static const struct win_transport direct_transport = {
    .name    = "direct",
    .request = direct_request,
    .debug   = direct_debug,
};

static const struct win_transport *g_transport = &direct_transport;

void win_transport_register(const struct win_transport *t) {
    // NULL restores the built-in rather than leaving none installed --
    // see the header. A transport with a missing slot is refused the same
    // way display_probe() refuses a driver whose capabilities and
    // function pointers disagree: a half-installed carriage fails at the
    // first message, far from the registration that caused it.
    if (!t || !t->request || !t->debug) {
        if (t) klog_write("win: transport rejected -- missing a required slot\n");
        g_transport = &direct_transport;
        return;
    }
    g_transport = t;
}

const char *win_transport_name(void) {
    return g_transport->name;
}

int win_transport_request(int pid, struct win_request_msg *req) {
    return g_transport->request(pid, req);
}

int win_transport_debug(int pid, struct win_debug_msg *msg) {
    return g_transport->debug(pid, msg);
}

// See api/win_debug.h -- the apps/-facing one-liner over the transport.
int win_debug_command(const char *cmd) {
    struct win_debug_msg msg;
    k_memset(&msg, 0, sizeof msg);
    msg.type = WIN_REQ_DEBUG_CMD;
    k_strlcpy(msg.text, cmd ? cmd : "", WIN_DEBUG_CMD_LEN);
    return win_transport_debug(WIN_PID_KERNEL, &msg) ? 1 : 0;
}
