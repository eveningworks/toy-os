#include "serial.h"
#include "io.h"
#include "irq.h"
#include "pic.h"
#include "barrier.h" // cpu_relax

#define COM1 0x3F8
#define COM1_IRQ 4 // the fixed legacy IRQ line for COM1 (COM2 shares IRQ3) -- see docs/decisions.md

// Small RX ring buffer, same shape as keyboard.c's -- filled by
// serial_irq_handler() below, drained by serial_try_getc(). Sized for
// a human typing at a terminal, not a bulk transfer: nothing here
// pretends to be a real high-throughput UART driver.
#define SERIAL_RX_CAP 256
static volatile uint8_t rx_buf[SERIAL_RX_CAP];
static volatile uint8_t rx_head = 0, rx_tail = 0;

static int data_ready(void) {
    return inb(COM1 + 5) & 0x01; // LSR bit 0
}

// COM1's IRQ handler -- drains every byte the UART currently has
// buffered (its own small hardware FIFO, enabled by the 0xC7 written
// below) in one pass, same "while there's more, take it" shape
// ata_irq_handler() and i8042_poll() already use for their own status
// registers. A full software ring here just means the newest byte is
// silently dropped in favor of what's already queued -- nothing this
// console does needs guaranteed delivery under a burst.
static void serial_irq_handler(uint64_t *regs) {
    (void)regs;
    while (data_ready()) {
        uint8_t c = (uint8_t)inb(COM1);
        uint8_t next = (uint8_t)(rx_head + 1);
        if (next != rx_tail) {
            rx_buf[rx_head] = c;
            rx_head = next;
        }
    }
}

int serial_try_getc(void) {
    if (rx_head == rx_tail) return -1;
    uint8_t c = rx_buf[rx_tail];
    rx_tail = (uint8_t)(rx_tail + 1);
    return c;
}

void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

// RX wiring -- split out from serial_init() rather than folded into it,
// because serial_init() runs first thing in kernel_main() (before
// idt_init()), so klog_write() has somewhere to send its very first
// byte -- registering/unmasking an IRQ that early would be immediately
// undone by idt_init()'s own "mask everything, then unmask only what's
// wired up" pass (see idt.c's idt_init()). Same "driver registers and
// unmasks its own IRQ line" pattern ata.c's ATA_PRIMARY_IRQ uses, just
// called separately, after idt_init(), instead of from serial_init()
// itself. Safe to call unconditionally even when nothing's plugged into
// the emulated/real COM1 port: an idle line just never raises IRQ4, so
// serial_try_getc() stays permanently empty, same as before this
// existed.
void serial_irq_init(void) {
    irq_register_handler(COM1_IRQ, serial_irq_handler);
    irq_unmask(COM1_IRQ);
    // serial_init()'s outb(COM1 + 1, 0x00) deliberately leaves the
    // UART's own Interrupt Enable Register at 0 -- interrupts disabled
    // at the chip itself, not just masked at the PIC -- since it runs
    // before idt_init() and nothing should be racing to service an IRQ
    // that early. Enable bit 0 (Received Data Available) here, now that
    // the IDT/PIC and this file's own handler are actually ready for
    // it. Without this, pic_clear_mask() above is not enough on its own
    // -- the UART simply never asserts IRQ4 in the first place, and
    // serial_try_getc() stays permanently empty even though the PIC
    // line is unmasked and the handler is registered. Found by testing
    // the debug console live over QMP's TCP-backed serial chardev: not
    // a single byte -- not even a typed character's local echo -- ever
    // came back until this line was added.
    outb(COM1 + 1, 0x01);
}

static int transmit_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

// THE TRAP: a UART whose consumer has stopped reading never raises THRE
// again -- QEMU's socket chardev does exactly that when an attached peer
// stops draining -- so waiting for it unbounded stops the machine with
// interrupts still ON: ticks advance and the scheduler looks healthy
// while nothing runs, and a klog burst inside an FS_OP spins there
// holding the preemption guard. Output is therefore QUEUED rather than
// waited on, the way a real tty driver does it, and pushed whenever the
// line will take it -- here, and from the timer tick, so a consumer that
// comes back finds its backlog intact. Only a full ring loses a byte,
// which the harness cannot afford: five GUI tools read kernel log lines
// as their oracle.
#define SERIAL_TX_CAP  4096
#define SERIAL_TX_SPIN 20000 // bounded flush; a stalled line latches out

static volatile uint8_t tx_buf[SERIAL_TX_CAP];
static volatile uint16_t tx_head, tx_tail;
static volatile uint32_t tx_dropped; // read it in GDB; nothing exports it
static volatile int tx_stalled;

static uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}

static void irq_restore(uint64_t f) {
    __asm__ volatile ("pushq %0; popfq" :: "r"(f) : "memory", "cc");
}

// Hand the UART as much of the backlog as it will take. Callers hold
// interrupts off, because the timer tick pushes from the same ring.
static void tx_push(void) {
    while (tx_head != tx_tail && transmit_empty()) {
        outb(COM1, tx_buf[tx_tail]);
        tx_tail = (uint16_t)((tx_tail + 1u) % SERIAL_TX_CAP);
    }
    if (tx_head == tx_tail) tx_stalled = 0;
}

void serial_putc(char c) {
    uint64_t f = irq_save();
    tx_push(); // backlog first, or this byte would overtake it
    if (tx_head == tx_tail && transmit_empty()) {
        outb(COM1, (uint8_t)c);
    } else {
        uint16_t next = (uint16_t)((tx_head + 1u) % SERIAL_TX_CAP);
        if (next != tx_tail) {
            tx_buf[tx_head] = (uint8_t)c;
            tx_head = next;
        } else {
            tx_dropped++;
        }
    }
    irq_restore(f);
}

// Drains what it can and gives up rather than waiting for a consumer
// that may never return -- a panic still gets its line out while the
// line is alive, and costs nothing once `tx_stalled` has latched.
void serial_flush(void) {
    if (tx_stalled) return;
    for (uint32_t i = 0; i < SERIAL_TX_SPIN; i++) {
        uint64_t f = irq_save();
        tx_push();
        int done = (tx_head == tx_tail);
        irq_restore(f);
        if (done) return;
        cpu_relax();
    }
    tx_stalled = 1;
}

int serial_tx_pending(void) { return tx_head != tx_tail; }

// The timer's call, so a backlog still moves when nothing is printing.
void serial_tx_poll(void) {
    uint64_t f = irq_save();
    tx_push();
    irq_restore(f);
}

void serial_write(const char *s) {
    while (*s) serial_putc(*s++);
    serial_flush();
}
