// GMBUS -- the display engine's I2C controller, which reads an HDMI or
// DVI monitor's EDID over its DDC pins. Gen9 only here (gen8's EDID is
// the eDP panel's, over AUX). Linux's intel_gmbus.c is the shape: select
// a pin pair, one INDEX cycle (offset 0, then a repeated start and the
// read), four bytes per HW_RDY, then a STOP.
//
// THE PROBE RUNS BEFORE THE TIMER, so every wait is an iteration-bounded
// spin (docs/conventions/kernel.md). At 100 kHz a 128-byte block is
// ~12 ms of bus time.
#include "intel_internal.h"
#include "barrier.h"
#include "klog.h"
#include "kfmt.h"

// driver-none: part of intel-display (intel_display.c declares it)

#define GMBUS0 0xC5100   // rate | pin pair
#define GMBUS1 0xC5104   // command
#define GMBUS2 0xC5108   // status
#define GMBUS3 0xC510C   // data, bytes 3..0
#define GMBUS4 0xC5110   // interrupt mask
#define GMBUS5 0xC5120   // 2-byte index

#define GMBUS_RATE_100KHZ  (0u << 8)
#define GMBUS_SW_CLR_INT   (1u << 31)
#define GMBUS_SW_RDY       (1u << 30)
#define GMBUS_CYCLE_WAIT   (1u << 25)
#define GMBUS_CYCLE_INDEX  (2u << 25)
#define GMBUS_CYCLE_STOP   (4u << 25)
#define GMBUS_BYTES(n)     ((uint32_t)(n) << 16)
#define GMBUS_INDEX(i)     ((uint32_t)(i) << 8)
#define GMBUS_READ_ADDR(a) (((uint32_t)(a) << 1) | 1u)
#define GMBUS_HW_WAIT      (1u << 14)
#define GMBUS_HW_RDY       (1u << 11)
#define GMBUS_SATOER       (1u << 10)   // the target NAKed
#define GMBUS_ACTIVE       (1u << 9)

#define DDC_ADDR 0x50
#define SPIN_LIMIT 2000000

// SKL/KBL pin pairs, by DDI (i915's gmbus_pins_skl): B on DPB, C on
// DPC, D on DPD. Port A is eDP and E is analogue; neither has one.
int intel_gmbus_pin_for_port(int port) {
    switch (port) {
    case 1: return 5;   // DDI B -> GMBUS_PIN_DPB
    case 2: return 4;   // DDI C -> GMBUS_PIN_DPC
    case 3: return 6;   // DDI D -> GMBUS_PIN_DPD
    default: return 0;
    }
}

// Until any of `bits` is set; the status, or 0 on running out.
static uint32_t wait_status(uint32_t bits) {
    for (int i = 0; i < SPIN_LIMIT; i++) {
        uint32_t st = intel_rd(GMBUS2);
        if (st & bits) return st;
        cpu_relax();
    }
    return 0;
}

// The bus left idle whatever happened: a NAK or a timeout clears the
// interrupt state and drops the pin, as i915's error path does.
static void release(int failed) {
    if (failed) {
        intel_wr(GMBUS1, GMBUS_SW_CLR_INT);
        intel_wr(GMBUS1, 0);
    }
    intel_wr(GMBUS0, 0);
}

int intel_gmbus_read_edid(int pin, uint8_t *out, int len) {
    if (pin <= 0 || !out || len <= 0 || len > 256) return 0;
    intel_wr(GMBUS0, GMBUS_RATE_100KHZ | (uint32_t)pin);
    intel_wr(GMBUS4, 0);
    intel_wr(GMBUS5, 0);
    intel_wr(GMBUS1, GMBUS_SW_RDY | GMBUS_CYCLE_INDEX | GMBUS_CYCLE_WAIT |
                     GMBUS_BYTES(len) | GMBUS_INDEX(0) | GMBUS_READ_ADDR(DDC_ADDR));
    int got = 0;
    while (got < len) {
        uint32_t st = wait_status(GMBUS_HW_RDY | GMBUS_SATOER);
        if (!st || (st & GMBUS_SATOER)) {
            klog_printf("intel-gmbus: pin %d %s after %d bytes (status %#x)\n",
                        pin, st ? "NAK" : "timed out", got, intel_rd(GMBUS2));
            release(1);
            return 0;
        }
        uint32_t v = intel_rd(GMBUS3);
        for (int b = 0; b < 4 && got < len; b++) out[got++] = (uint8_t)(v >> (8 * b));
    }
    // The hardware parks in its wait phase after the last byte; a STOP
    // then ends the transaction and the bus goes idle.
    if (!wait_status(GMBUS_HW_WAIT | GMBUS_SATOER)) {
        release(1);
        return 0;
    }
    intel_wr(GMBUS1, GMBUS_SW_RDY | GMBUS_CYCLE_STOP);
    for (int i = 0; i < SPIN_LIMIT && (intel_rd(GMBUS2) & GMBUS_ACTIVE); i++) cpu_relax();
    release(0);
    return got;
}
