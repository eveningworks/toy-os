#ifndef KERNEL_REMOTE_LOG_H
#define KERNEL_REMOTE_LOG_H

#include <stdint.h>
#include "query_abi.h"

// WHAT A REMOTE SESSION DID -- the ring behind QUERY_REMOTELOG.
//
// The machine's owner can see, live, what somebody reaching it over the
// network is doing: KDE's krfb raises a tray indicator while a remote
// party is connected and every Wayland screen-share portal does the
// same. The compositor's tray item reads this.
//
// **REMOTENESS IS DERIVED, NOT DECLARED.** A session is remote when the
// process that CREATED it was itself reading a socket -- telnetd is
// handed its connection on fd 0 and then makes a session for the shell
// it spawns, so the fact is available exactly where a session is born
// (scheduler_make_session_leader). Nothing in ring 3 has to be trusted
// to say so, and a local shell cannot claim it.
//
// The peer travels with the session, so every later record -- a command,
// a spawn, a transfer -- carries the address it came from without
// anybody passing it down.

void remote_log_record(int kind, uint32_t remote_ip, int pid,
                       const char *comm, const char *text);

// For the query provider.
uint64_t remote_log_total(void);   // records ever written; 0 = none yet
uint64_t remote_log_oldest(void);  // the oldest seq still held
int remote_log_get(uint64_t seq, struct query_remotelog *out);

// Is a session live right now, and whose? The tray item shows itself
// only while one is, which is `auto`'s "ask the hardware" for this item.
uint32_t remote_log_session_of(int leader_pid); // 0 = not a remote session
int remote_log_sessions(void);        // how many remote sessions are open
uint32_t remote_log_session_ip(void); // the most recent one's peer, or 0

// `creator` is the program that made the session (its parent's name):
// what the record and QUERY_REMOTESESS call it.
void remote_log_session_opened(int leader_pid, uint32_t ip, const char *creator);
// QUERY_REMOTESESS: the `index`th open session; 0 past the end.
int remote_log_session_count(void);
int remote_log_session_get(int index, struct query_remotesess *out);
void remote_log_session_closed(int leader_pid);

#endif // KERNEL_REMOTE_LOG_H
