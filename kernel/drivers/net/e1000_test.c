// KTESTs for the 8254x transmit ring's one rule that fails silently.
//
// Posting into the last free descriptor moves TDT onto TDH, and the card
// reads TDT == TDH as an EMPTY ring: every frame queued stalls, with no
// error anywhere. QEMU transmits on the TDT write, so its ring never fills
// and it cannot show this; the rule has to be checked as arithmetic.
#include "e1000_regs.h"
#include "ktest.h"
#include "string.h"

static void post(struct tx_desc *d) { d->cmd = TX_CMD_EOP | TX_CMD_RS; d->status = 0; }
static void done(struct tx_desc *d) { d->status = TX_STATUS_DD; }

KTEST("e1000", "the transmit ring keeps one slot free, since TDT == TDH reads as empty") {
    static struct tx_desc ring[8];
    k_memset(ring, 0, sizeof ring);
    KTEST_ASSERT(!e1000_tx_full(ring, 8, 0));        // never used: free

    // Seven in flight from slot 0: slot 7 is free, but posting there puts
    // TDT back on TDH.
    for (int i = 0; i < 7; i++) post(&ring[i]);
    KTEST_ASSERT(e1000_tx_full(ring, 8, 7));
    // The card finishes slot 0: slot 7 may be posted now.
    done(&ring[0]);
    KTEST_ASSERT(!e1000_tx_full(ring, 8, 7));
    // The slot being posted into must itself be free, wrapped or not.
    KTEST_ASSERT(e1000_tx_full(ring, 8, 3));
    done(&ring[3]);
    done(&ring[4]);
    KTEST_ASSERT(!e1000_tx_full(ring, 8, 3));
}
