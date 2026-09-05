#ifndef KERNEL_CONN_LOG_H
#define KERNEL_CONN_LOG_H

#include <stdint.h>
#include "query_abi.h" // struct query_connlog -- what a record IS

// WHO THIS MACHINE HAS TALKED TO: a ring of connection records, read
// through QUERY_CONNLOG by `/bin/netlog`.
//
// A CONNECTION, NOT A PACKET, and that is the whole design. Linux's
// three answers to "log outgoing connections" are netfilter's
// `-m conntrack --ctstate NEW -j LOG`, auditd on connect(2), and eBPF's
// tcpconnect; Windows has WFP audit 5156 and Sysmon Event 3. Every one
// of them records the START of a flow rather than its traffic, because
// a packet log fills faster than anybody reads it. This does the same:
// one record per TCP open, and one per (destination, port) a datagram
// socket first sends to.
//
// THE HOOKS ARE IN kernel/net/socket.c, at the socket layer rather than
// at ipv4_output(), for two reasons. A retransmit, an ARP retry and a
// datagram number two are all the same connection and must not each
// make a record. And only the socket layer runs in the CALLING
// PROCESS's context -- ipv4_output() is also reached from net_poll()
// on behalf of whoever happened to be in a syscall, which would
// attribute a connection to the wrong program. An inbound connection is
// logged at accept() for exactly that reason: the SYN arrives in
// somebody else's poll, and the server is only named when it takes it.

// The ring's depth. 128 records is ~20 KB of bss and about a minute of
// a busy boot -- enough that a reader arriving after the fact still
// sees the DHCP, DNS and HTTP that started it.
#define CONN_LOG_MAX 128

// What gets recorded -- `system.conn_log`'s three values, ordered by
// how much they log.
#define CONN_LOG_OFF 0
#define CONN_LOG_TCP 1 // TCP opens only, in both directions
#define CONN_LOG_ALL 2 // ...plus a UDP or ICMP socket's first send to a
                       // destination

// Record one connection. `remote_port` is 0 for ICMP. Silently does
// nothing when the mode excludes it, so a caller never tests the mode.
void conn_log_record(uint32_t direction, uint8_t proto,
                     uint32_t remote_ip, uint16_t remote_port,
                     uint16_t local_port);

// SYS_NET_RESOLVED's landing point: remember that `name` resolved to
// `ip`, so a later record for that address can carry it.
void conn_log_name_hint(uint32_t ip, const char *name);

// The ring, oldest first. `index` past the last one returns 0.
int conn_log_count(void);
int conn_log_at(int index, struct query_connlog *out);

// Sets what is recorded, WITHOUT touching /etc -- the setting's apply
// persists separately, and a KTEST restores what it found.
void conn_log_set_mode(int mode);

// What is being recorded right now. Also the setting's live state --
// the /etc file is read ONCE at boot, because this is asked on every
// connection and etc_config_get() re-reads a whole file per key.
int conn_log_mode(void);

// Registers `system.conn_log`. Called from settings_init().
void conn_log_setting_register(void);

#endif // KERNEL_CONN_LOG_H
