#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

void serial_init(void);
void serial_write(const char *s);
void serial_putc(char c);

// Output is QUEUED, not waited on -- see serial.c's trap comment. These
// two move the backlog: the timer calls serial_tx_poll() so it drains
// with nothing printing, and serial_write() flushes each line.
void serial_tx_poll(void);
// Bytes queued and not yet sent. A tickless idle keeps the tick while
// this is true: only the tick moves a backlog nothing else is printing.
int serial_tx_pending(void);
void serial_flush(void);

// Wires up COM1's IRQ4 for RX (see serial_try_getc() below) -- MUST be
// called after idt_init(), never from/before serial_init() itself; see
// serial.c's own comment on why the ordering matters.
void serial_irq_init(void);

// Non-blocking read of one received byte, or -1 if none is waiting.
// Bytes arrive via COM1's IRQ4 (see serial.c's serial_irq_handler(),
// registered by serial_init() the same way ata.c registers its own IRQ
// -- irq_register_handler() + pic_clear_mask(), not a special case in
// idt.c), landing in a small ring buffer here rather than being read
// straight off the UART by whoever calls this -- an interrupt-driven
// RX line needs the byte drained promptly (the next one can arrive and
// overwrite it before a slow poller gets around to reading it),
// exactly the same reasoning keyboard.c's ring buffer exists for.
// First real caller: kernel/core/debug_console.c's serial debug
// console -- see docs/decisions.md.
int serial_try_getc(void);

// THE DEBUG CONSOLE'S PORT: COM2 when a UART is really there (probed at
// serial_init()), else COM1 -- so the kernel log and the debug console
// share a wire only on a machine with one port. serial_dbg_separate()
// says which: 1 means COM2 carries the console and COM1 the log alone.
int  serial_dbg_separate(void);
// Keep the debug console on COM1 even though COM2 answered the probe --
// `debugcon=ttyS0`, for a board that decodes a port it has no connector
// for. Call before the console first talks.
void serial_dbg_use_com1(void);
// Hand every byte the debug port receives to `fn`, IN THE IRQ -- the
// serial tty's input (kernel/tty/serial_tty.c). Until it is set, bytes
// wait in the port's ring, and setting it hands them over first.
void serial_dbg_set_rx(void (*fn)(uint8_t c));
void serial_dbg_putc(char c);
void serial_dbg_write(const char *s);
void serial_dbg_flush(void);

// THE KERNEL DEBUGGER'S PORT (kernel/debug/): ttyS1..ttyS3, POLLED and
// never interrupt-driven, because it is read with the machine stopped.
// Claiming COM2 takes it away from the debug console, which falls back
// to COM1. Call before serial_irq_init(). 0 if no UART answers there.
int  serial_kdb_claim(int ttys);
int  serial_kdb_getc(void);   // -1 when nothing is waiting
void serial_kdb_putc(char c); // waits for the UART, bounded

#endif
