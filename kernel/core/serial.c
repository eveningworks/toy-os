#include "serial.h"
#include "io.h"
#include "irq.h"
#include "pic.h"

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
    pic_clear_mask(COM1_IRQ);
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

void serial_putc(char c) {
    while (!transmit_empty());
    outb(COM1, c);
}

void serial_write(const char *s) {
    while (*s) serial_putc(*s++);
}
