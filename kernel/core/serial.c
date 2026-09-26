#include "serial.h"
#include "io.h"
#include "irq.h"
#include "pic.h"
#include "barrier.h" // cpu_relax

// TWO PORTS, TWO JOBS. COM1 carries the kernel log; COM2, when a UART is
// really there, carries the debug console alone -- so a command's reply
// can never be torn by a log line landing between its chunks, which is
// what happened when both shared one wire (docs/decisions.md, "The
// kernel log and the debug console are two serial ports"). With no COM2
// everything stays on COM1 exactly as before: most real machines have
// no second port, and every tool that builds a one-port QEMU line keeps
// working unchanged. Linux's `console=ttyS0` plus a getty on ttyS1 is
// the same split.
#define COM1 0x3F8
#define COM1_IRQ 4 // the fixed legacy IRQ lines -- see docs/decisions.md
#define COM2 0x2F8
#define COM2_IRQ 3

// Small RX ring buffer, same shape as keyboard.c's -- filled by the IRQ
// handler below, drained by *_try_getc(). Sized for a human typing at a
// terminal, not a bulk transfer.
#define SERIAL_RX_CAP 256

// THE TRAP: a UART whose consumer has stopped reading never raises THRE
// again -- QEMU's socket chardev does exactly that when an attached peer
// stops draining -- so waiting for it unbounded stops the machine with
// interrupts still ON: ticks advance and the scheduler looks healthy
// while nothing runs, and a klog burst inside an FS_OP spins there
// holding the preemption guard. Output is therefore QUEUED rather than
// waited on, the way a real tty driver does it, and pushed whenever the
// line will take it -- here, and from the timer tick, so a consumer that
// comes back finds its backlog intact. Only a full ring loses a byte,
// which the harness cannot afford: GUI tools read kernel log lines as
// their oracle.
#define SERIAL_TX_CAP  4096
#define SERIAL_TX_SPIN 20000 // bounded flush; a stalled line latches out

struct uart {
    uint16_t base;
    uint8_t irq;
    int present;
    volatile uint8_t rx_buf[SERIAL_RX_CAP];
    volatile uint8_t rx_head, rx_tail;
    volatile uint8_t tx_buf[SERIAL_TX_CAP];
    volatile uint16_t tx_head, tx_tail;
    volatile uint32_t tx_dropped; // read it in GDB; nothing exports it
    volatile int tx_stalled;
};

static struct uart g_com1 = { .base = COM1, .irq = COM1_IRQ, .present = 1 };
static struct uart g_com2 = { .base = COM2, .irq = COM2_IRQ };

// Where the debug console talks: COM2 if it exists, else COM1 -- or
// COM1 regardless, when `debugcon=ttyS0` asked (serial_dbg_use_com1()).
static int g_dbg_com1;
static struct uart *dbg_port(void) {
    return g_com2.present && !g_dbg_com1 ? &g_com2 : &g_com1;
}

static uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}

static void irq_restore(uint64_t f) {
    __asm__ volatile ("pushq %0; popfq" :: "r"(f) : "memory", "cc");
}

// --- receive -----------------------------------------------------------

// Drains every byte the UART has buffered (its own small FIFO, enabled
// by the 0xC7 written below) in one pass. A full software ring drops the
// newest byte -- nothing typed at this console needs guaranteed
// delivery under a burst.
static void rx_drain(struct uart *u) {
    while (inb(u->base + 5) & 0x01) { // LSR bit 0: data ready
        uint8_t c = (uint8_t)inb(u->base);
        uint8_t next = (uint8_t)(u->rx_head + 1);
        if (next != u->rx_tail) {
            u->rx_buf[u->rx_head] = c;
            u->rx_head = next;
        }
    }
}

static void com1_irq(uint64_t *regs) { (void)regs; rx_drain(&g_com1); }
static void com2_irq(uint64_t *regs) { (void)regs; rx_drain(&g_com2); }

static int rx_getc(struct uart *u) {
    if (u->rx_head == u->rx_tail) return -1;
    uint8_t c = u->rx_buf[u->rx_tail];
    u->rx_tail = (uint8_t)(u->rx_tail + 1);
    return c;
}

// --- bring-up ----------------------------------------------------------

static void uart_program(uint16_t base) {
    outb(base + 1, 0x00);
    outb(base + 3, 0x80);
    outb(base + 0, 0x03);
    outb(base + 1, 0x00);
    outb(base + 3, 0x03);
    outb(base + 2, 0xC7);
    outb(base + 4, 0x0B);
}

// IS A UART REALLY THERE? The scratch register (base+7) holds whatever is
// written to it; an unassigned I/O port reads back 0xFF whatever you
// wrote, so two different patterns both surviving is a UART. Asked
// rather than assumed: the fallback to COM1 is what keeps a one-port
// machine -- and every one-port QEMU line in tools/ -- exactly as it was.
static int uart_probe(uint16_t base) {
    outb(base + 7, 0xA5);
    if (inb(base + 7) != 0xA5) return 0;
    outb(base + 7, 0x5A);
    return inb(base + 7) == 0x5A;
}

void serial_init(void) {
    uart_program(COM1);
    g_com2.present = uart_probe(COM2);
    if (g_com2.present) uart_program(COM2);
}

// RX wiring -- split out from serial_init() rather than folded into it,
// because serial_init() runs first thing in kernel_main() (before
// idt_init()), so klog_write() has somewhere to send its very first
// byte -- registering/unmasking an IRQ that early would be immediately
// undone by idt_init()'s own "mask everything, then unmask only what's
// wired up" pass. Safe unconditionally: an idle line just never raises
// its IRQ, so *_try_getc() stays empty.
//
// serial_init() deliberately leaves each UART's own Interrupt Enable
// Register at 0 -- interrupts disabled at the chip, not just masked at
// the PIC. Enabling bit 0 (Received Data Available) here is what makes
// the line ever assert its IRQ at all: without it not a single byte came
// back over QMP's TCP-backed chardev, not even a typed character's echo.
static void uart_irq_init(struct uart *u, irq_handler_fn fn) {
    irq_register_handler(u->irq, fn);
    irq_unmask(u->irq);
    outb(u->base + 1, 0x01);
}

void serial_irq_init(void) {
    uart_irq_init(&g_com1, com1_irq);
    if (g_com2.present) uart_irq_init(&g_com2, com2_irq);
}

// --- transmit ----------------------------------------------------------

static int transmit_empty(struct uart *u) {
    return inb(u->base + 5) & 0x20;
}

// Hand the UART as much of the backlog as it will take. Callers hold
// interrupts off, because the timer tick pushes from the same ring.
static void tx_push(struct uart *u) {
    while (u->tx_head != u->tx_tail && transmit_empty(u)) {
        outb(u->base, u->tx_buf[u->tx_tail]);
        u->tx_tail = (uint16_t)((u->tx_tail + 1u) % SERIAL_TX_CAP);
    }
    if (u->tx_head == u->tx_tail) u->tx_stalled = 0;
}

static void tx_putc(struct uart *u, char c) {
    uint64_t f = irq_save();
    tx_push(u); // backlog first, or this byte would overtake it
    if (u->tx_head == u->tx_tail && transmit_empty(u)) {
        outb(u->base, (uint8_t)c);
    } else {
        uint16_t next = (uint16_t)((u->tx_head + 1u) % SERIAL_TX_CAP);
        if (next != u->tx_tail) {
            u->tx_buf[u->tx_head] = (uint8_t)c;
            u->tx_head = next;
        } else {
            u->tx_dropped++;
        }
    }
    irq_restore(f);
}

// Drains what it can and gives up rather than waiting for a consumer
// that may never return -- a panic still gets its line out while the
// line is alive, and costs nothing once `tx_stalled` has latched.
static void tx_flush(struct uart *u) {
    if (u->tx_stalled) return;
    for (uint32_t i = 0; i < SERIAL_TX_SPIN; i++) {
        uint64_t f = irq_save();
        tx_push(u);
        int done = (u->tx_head == u->tx_tail);
        irq_restore(f);
        if (done) return;
        cpu_relax();
    }
    u->tx_stalled = 1;
}

// --- COM1: the kernel log (and everything, with no COM2) ---------------

int serial_try_getc(void) { return rx_getc(&g_com1); }
void serial_putc(char c) { tx_putc(&g_com1, c); }
void serial_flush(void) { tx_flush(&g_com1); }

void serial_write(const char *s) {
    while (*s) serial_putc(*s++);
    serial_flush();
}

int serial_tx_pending(void) {
    return g_com1.tx_head != g_com1.tx_tail ||
           (g_com2.present && g_com2.tx_head != g_com2.tx_tail);
}

// The timer's call, so a backlog still moves when nothing is printing.
void serial_tx_poll(void) {
    uint64_t f = irq_save();
    tx_push(&g_com1);
    if (g_com2.present) tx_push(&g_com2);
    irq_restore(f);
}

// --- the debug console's port ------------------------------------------

int serial_dbg_separate(void) { return g_com2.present && !g_dbg_com1; }

// A Super I/O chip can decode 0x2F8 with no connector fitted, so the
// probe says "COM2" on a machine whose only wired port is COM1.
void serial_dbg_use_com1(void) { g_dbg_com1 = 1; }
int serial_dbg_try_getc(void) { return rx_getc(dbg_port()); }
void serial_dbg_putc(char c) { tx_putc(dbg_port(), c); }
void serial_dbg_flush(void) { tx_flush(dbg_port()); }

void serial_dbg_write(const char *s) {
    struct uart *u = dbg_port();
    while (*s) tx_putc(u, *s++);
    tx_flush(u);
}
