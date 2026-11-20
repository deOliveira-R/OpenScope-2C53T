/*
 * OpenScope 2C53T - scope front end
 *
 * Range relays, AC/DC coupling and the converter routing of the two channels.
 * Shared items: fpga_internal.h.
 */

#include "fpga_internal.h"

/* ═══════════════════════════════════════════════════════════════════
 * Range relays, one bank per channel
 *
 * Each channel has four relay lines, set from that channel's own range
 * index (its volts/div index, 0-9). Both channels use the same table; the
 * pins correspond as
 *   CH1:  PC12  PE4   PE5   PE6
 *   CH2:  PA15  PB11  PB10  PA10
 * (decoded from stock's two range functions).
 *
 * Bit 0 is the input path select (PC12 / PA15): HIGH = direct, for the
 * sensitive ranges 0-4; LOW = attenuated (about 30x, measured), for 5-9.
 * It is not the coupling: AC/DC coupling is PD12/PD13.
 *
 * The relays give only a coarse ladder; the gain of each volts/div step
 * is finished in the digital layer.
 * ═══════════════════════════════════════════════════════════════════ */

/* bit0 = path select, bit1 = PE4/PB11, bit2 = PE5/PB10, bit3 = PE6/PA10 */
static const uint8_t fpga_relay_tbl[VDIV_COUNT] = {
    0x0B, /* 0: path H  b1 H  b2 L  b3 H */
    0x0F, /* 1: path H  b1 H  b2 H  b3 H */
    0x05, /* 2: path H  b1 L  b2 H  b3 L */
    0x03, /* 3: path H  b1 H  b2 L  b3 L */
    0x07, /* 4: path H  b1 H  b2 H  b3 L */
    0x0A, /* 5: path L  b1 H  b2 L  b3 H */
    0x0E, /* 6: path L  b1 H  b2 H  b3 H */
    0x0C, /* 7: path L  b1 L  b2 H  b3 H */
    0x02, /* 8: path L  b1 H  b2 L  b3 L */
    0x06, /* 9: path L  b1 H  b2 H  b3 L */
};

/* Note on PB11: it is CH2's bit-1 relay, not an FPGA control line. Stock
 * writes it only from its CH2 range and calibration functions, and driving it
 * LOW on the bench did not stop a capture. main() and fpga_init() drive it
 * HIGH during boot; fpga_set_ch2_frontend_range() sets it from then on. */
static void fpga_set_ch1_frontend_range(uint8_t range_idx)
{
    uint8_t b;

    if (range_idx >= VDIV_COUNT) range_idx = VDIV_COUNT - 1;
    b = fpga_relay_tbl[range_idx];

    if (b & 0x01) GPIOC->scr = (1U << 12); else GPIOC->clr = (1U << 12); /* PC12 */
    if (b & 0x02) GPIOE->scr = (1U << 4);  else GPIOE->clr = (1U << 4);  /* PE4  */
    if (b & 0x04) GPIOE->scr = (1U << 5);  else GPIOE->clr = (1U << 5);  /* PE5  */
    if (b & 0x08) GPIOE->scr = (1U << 6);  else GPIOE->clr = (1U << 6);  /* PE6  */
}

static void fpga_set_ch2_frontend_range(uint8_t range_idx)
{
    uint8_t b;

    if (range_idx >= VDIV_COUNT) range_idx = VDIV_COUNT - 1;
    b = fpga_relay_tbl[range_idx];

    if (b & 0x01) GPIOA->scr = (1U << 15); else GPIOA->clr = (1U << 15); /* PA15 */
    if (b & 0x02) GPIOB->scr = PB11_MASK;  else GPIOB->clr = PB11_MASK;  /* PB11 */
    if (b & 0x04) GPIOB->scr = (1U << 10); else GPIOB->clr = (1U << 10); /* PB10 */
    if (b & 0x08) GPIOA->scr = (1U << 10); else GPIOA->clr = (1U << 10); /* PA10 */
}

/* The converter routing for the stored channel enables: 3 = both, 2 = CH2
 * only, 1 = CH1 only. With neither on (the enable setter does not allow it)
 * CH1 is routed. */
static uint8_t channel_mask_from_enables(const scope_state_t *ss)
{
    if (ss->ch1.enabled && ss->ch2.enabled) return 3u;
    return ss->ch2.enabled ? 2u : 1u;
}

/* Set the converter routing, then both channels' relays, from the stored
 * settings. The routing comes first: it decides which channel feeds each
 * converter. */
void fpga_set_scope_frontend_ranges(void)
{
    const scope_state_t *ss = scope_state_get();

    fpga_set_channel_mask(channel_mask_from_enables(ss));
    fpga_set_ch1_frontend_range(ss->ch1.vdiv_idx);
    fpga_set_ch2_frontend_range(ss->ch2.vdiv_idx);
}

/* Set the relays of one channel to a range index (ch 0 = CH1, 1 = CH2, any
 * other value = both). Relays only: the scope state is not changed. Used by
 * fpga_apply_vdiv() and by the shell's `fpga scope range <0-9> <1|2|both>`. */
void fpga_scope_set_range_diag_ch(uint8_t ch, uint8_t range_idx)
{
    if (ch == 0)      fpga_set_ch1_frontend_range(range_idx);
    else if (ch == 1) fpga_set_ch2_frontend_range(range_idx);
    else            { fpga_set_ch1_frontend_range(range_idx);
                      fpga_set_ch2_frontend_range(range_idx); }
}

void fpga_scope_set_range_diag(uint8_t range_idx)
{
    fpga_scope_set_range_diag_ch(0xFF, range_idx);   /* both channels */
}

/* Converter routing: PC1 and PC2 choose which channel feeds each of the two
 * converters (record 0x04 and record 0x05). Decoded from stock and confirmed
 * on the bench with a different tone on each input:
 *
 *   mask 3 (both)  PC2 H, PC1 L   0x04 = CH1   0x05 = CH2
 *   mask 1 (CH1)   PC2 H, PC1 H   0x04 = CH1   0x05 = CH1
 *   mask 2 (CH2)   PC2 L, PC1 L   0x04 = CH2   0x05 = CH2
 *
 * Floating pins act as mask 1, so CH2 is lost unless these are driven.
 */
/* The last PC1/PC2 code applied: 1 = CH1, 2 = CH2, 3 = both. */
static volatile uint8_t applied_channel_mask;

uint8_t scope_frontend_channel_mask(void) { return applied_channel_mask; }

void fpga_set_channel_mask(uint8_t mask)
{
    /* Make both pins outputs before driving them: a set/clear write to an
     * input does nothing. PC1 = CRL bits 7:4, PC2 = CRL bits 11:8. */
    GPIOC->cfglr = (GPIOC->cfglr & ~0x00000FF0u) | 0x00000330u;   /* PC1, PC2 push-pull outputs */

    switch (mask) {
    case 1:  GPIOC->scr = (1u << 2); GPIOC->scr = (1u << 1); break;  /* CH1  */
    case 2:  GPIOC->clr = (1u << 2); GPIOC->clr = (1u << 1); break;  /* CH2  */
    default: GPIOC->scr = (1u << 2); GPIOC->clr = (1u << 1); break;  /* BOTH */
    }
    applied_channel_mask = mask;
}

volatile uint8_t fpga_scope_posture_gen;      /* fpga.h: moves when the scope's posture is back */

/* Apply the channel enables to the converter routing (mask 1/2/3 on PC1/PC2).
 * The record in flight is dropped: it was captured with the old routing.
 * Refuses both-off. The trigger source is not handled here: the channel
 * enable setter (ch_enabled_set in model/props_scope.c) moves it off a
 * channel that goes off, and writes reg 0x06, before it calls this. */
bool fpga_apply_channel_enables(void)
{
    const scope_state_t *ss = scope_state_get();
    if (!ss->ch1.enabled && !ss->ch2.enabled)
        return false;
    fpga_set_channel_mask(channel_mask_from_enables(ss));
    /* The offset references depend on the calibration bank, and the bank
     * depends on the mask (stock: both channels on use bank 1 at timebase
     * index 3 or lower, one channel uses bank 2), so re-apply them. */
    scope_vpos_apply();
    acq_reprime_req = true;
    return true;
}

/* Apply a new volts/div index (idx) to channel ch (1 or 2): the relays, then
 * the channel's offset reference for that range. Without a usable stock
 * calibration page the offset reference is not changed, so the trace can sit
 * off-centre or clip until `fpga scope center`. The relays are plain GPIO, so
 * the acquisition task need not be paused. */
bool fpga_apply_vdiv(uint8_t ch, uint8_t idx)
{
    if ((ch != 1u && ch != 2u) || idx >= VDIV_COUNT)
        return false;
    fpga_scope_set_range_diag_ch((uint8_t)(ch - 1u), idx);
    /* `idx`, not vdiv_idx: callers store it after this returns. */
    scope_vpos_apply_ch(ch, idx);
    return true;
}

/* Called by main() after settings_store_init(). fpga_init() sets the relays
 * from the default scope state, before the saved settings are restored; this
 * applies the restored volts/div and coupling, so the relays match the
 * labels. The timebase counterpart is fpga_reconcile_timebase_after_arm()
 * in fpga_acq.c. */
void fpga_reconcile_frontend_after_arm(void)
{
    scope_state_t *ss = scope_state_get();
    fpga_set_scope_frontend_ranges();
    /* fpga_init() drives both coupling relays to DC. A restored GND has no
     * hardware path (see fpga_apply_coupling) and becomes DC, so the label
     * never claims a state the relay is not in. */
    for (uint8_t ch = 0; ch < 2; ch++) {
        channel_state_t *c = ch ? &ss->ch2 : &ss->ch1;
        if (c->coupling == COUPLING_GND) c->coupling = COUPLING_DC;
        (void)fpga_apply_coupling(ch, c->coupling);
    }
}

/* AC/DC coupling relays: PD12 = CH1, PD13 = CH2, HIGH = DC (decoded from
 * stock; on CH1 the bench showed LOW is a high-pass of about 9 Hz and HIGH
 * passes a DC step). The only writer of these pins after fpga_init().
 *
 * Returns true only when the pin is a GPIO push-pull output and its output
 * latch holds the level asked for. A pin left in an alternate function
 * (EXMC) ignores the write, so that returns false. GND is refused too: no
 * ground path for the input is known on this board. */
bool fpga_apply_coupling(uint8_t ch, uint8_t coupling)
{
    const coupling_t c = (coupling_t)coupling;
    if (ch > 1u || (c != COUPLING_DC && c != COUPLING_AC)) return false;
    const uint32_t pin = ch ? 13u : 12u;
    const uint32_t mode = (GPIOD->cfghr >> ((pin - 8u) * 4u)) & 0xFu;
    if (mode != 0x1u && mode != 0x2u && mode != 0x3u)
        return false;                       /* not a push-pull GPIO output */
    if (c == COUPLING_DC) GPIOD->scr = (1u << pin);
    else                  GPIOD->clr = (1u << pin);
    const bool high = (GPIOD->odt >> pin) & 1u;
    return high == (c == COUPLING_DC);
}
