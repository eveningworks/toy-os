#include "power.h"
#include "io.h"

void system_reboot(void) {
    uint8_t status;
    do {
        status = inb(0x64);
        if (status & 1) inb(0x60);
    } while (status & 2);
    outb(0x64, 0xFE);

    for (;;) __asm__ volatile ("hlt"); // in case the reset didn't take
}
