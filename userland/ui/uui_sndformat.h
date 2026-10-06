#ifndef UUI_SNDFORMAT_H
#define UUI_SNDFORMAT_H

// uui_sndformat -- one sound card's output FORMAT as a panel: a Sample
// rate list (Match what plays, then each rate the card takes), the rates
// Match may switch to as checkboxes, a Bit depth list (Automatic, then
// each width), and what the card plays now. System Settings shows it for
// the card in use (Sound > Output), Device Manager for the selected
// sound device -- the Windows Advanced tab and the macOS Audio MIDI
// Setup format, in one place each.
//
// **IT OWNS THE SETTING, NOT THE CARD.** A change is saved at once to
// the card's section of SND_CARDS_FILE (lib/usndfmt.h); whoever plays
// next decides a rate from it. Nothing here reaches the hardware.
//
// A PANEL OF STANDARD CONTROLS, not a drawn widget: the app embeds
// uui_sndformat_item() in its own layout, adds the focusables to its
// ring, and offers each on_widget id to uui_sndformat_on_widget() first.

#include <stdint.h>
#include "ui/uui_widget.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_label.h"
#include "ui/uui_layout.h"
#include "ui/uui_focus.h"
#include "lib/usndfmt.h"
#include "query_abi.h"
#include "sound_abi.h"

// The ids it reports under: id_base .. id_base + UUI_SNDFORMAT_IDS - 1.
#define UUI_SNDFORMAT_IDS 16

struct uui_sndformat {
    int id_base;
    char card[16];               // the card's stable name; "" = none loaded
    uint32_t rates, depths;      // what the card takes
    struct usndfmt f;            // its setting, as last saved

    // Row indices into the dropdowns' lists.
    uint32_t rate_hz[SND_RATE_COUNT + 1];   // [0] = 0, Match
    uint32_t bits_of[6];                    // [0] = 0, Automatic
    char rate_text[SND_RATE_COUNT + 1][24];
    char bits_text[6][24];
    const char *rate_ptr[SND_RATE_COUNT + 1], *bits_ptr[6];

    struct uui_label rate_l, allow_l, bits_l, now_l;
    struct uui_dropdown rate_dd, bits_dd;
    struct uui_checkbox allow[SND_RATE_COUNT];
    uint32_t allow_hz[SND_RATE_COUNT];
    int nallow;
    char allow_text[SND_RATE_COUNT][16];
    char allow_name[SND_RATE_COUNT][24];    // "sndfmt_allow_44100", for the layout log
    char now_text[160];

    struct uui_item rate_row_it[2], allow_row_it[2], bits_row_it[2];
    struct uui_item allow_grid_it[SND_RATE_COUNT];
    struct uui_layout rate_row, allow_row, bits_row, allow_grid;
    struct uui_item col_it[4];
    struct uui_layout col;
};

void uui_sndformat_init(struct uui_sndformat *s, int id_base);

// Fills the controls from a card -- QUERY_SOUND's record of it -- and
// its saved setting. The panel's shape may change (a card with more
// rates has more checkboxes): lay out again after.
void uui_sndformat_load(struct uui_sndformat *s, const struct query_sound *q);

// The panel, for the app's layout.
struct uui_item uui_sndformat_item(struct uui_sndformat *s);

// Its controls, in tab order, appended to `out`; returns how many.
int uui_sndformat_focusables(struct uui_sndformat *s, struct uui_focusable *out, int max);

// The app's on_widget, offered first. 1 when `id` was the panel's: the
// setting has been saved, and the panel may have changed shape (the
// Allowed rates row shows only under Match) -- lay out and redraw.
int uui_sndformat_on_widget(struct uui_sndformat *s, int id);

#endif
