/*
 * OpenScope 2C53T - scope front end
 *
 * Range relays, coupling and the converter routing of the two channels.
 * Split out of fpga.c; shared items: fpga_internal.h.
 */

#include "fpga_internal.h"

/* ═══════════════════════════════════════════════════════════════════
 * Scope Reinit Helpers
 * ═══════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════
 * Per-channel coarse analog front end (relay / attenuator bank)
 *
 * Stock has TWO independent range functions, one per channel, each taking
 * that channel's OWN range index (CH1 ms[0x02], CH2 ms[0x03]):
 *
 *   CH1  gpio_mux_portc_porte  flash 0x080088A4  ->  PC12 PE4  PE5  PE6
 *   CH2  gpio_mux_porta_portb  flash 0x08008A58  ->  PA15 PB11 PB10 PA10
 *
 * They are the SAME 10-case table under the pin isomorphism
 *   PC12 <-> PA15,  PE4 <-> PB11,  PE5 <-> PB10,  PE6 <-> PA10
 * (desk sweep 2026-08-15: both functions traced arm-by-arm from objdump and
 * re-derived by an independent verifier — analysis_v120/desk_sweep_2026-08-15.md §4.)
 *
 * This replaces the single 7-bit table of 2026-08-14, which was wrong twice
 * over: it drove BOTH channels' pins from ONE range index (so CH2's bank
 * followed CH1's volts/div), and its CH1 bits disagreed with stock at
 * indices 0, 1, 3, 6 and 7.
 *
 * Bit 0 is the input PATH select (PC12 / PA15): HIGH = direct for the
 * sensitive ranges 0-4, LOW = attenuated for 5-9. It is NOT a coupling or a
 * connect/disconnect control — bench 2026-08-15 measured ~30x attenuation,
 * not silence, with it LOW. AC/DC coupling is PD12/PD13.
 *
 * ⚠ Still known-incomplete: the 10 relay codes are not 1:1 with the 10 vdiv
 * settings (adjacent codes repeat), so this is the COARSE ladder only; fine
 * per-vdiv gain lives in the digital layer. See
 * analysis_v120/trigger_regime_findings_2026-08-14.md Addendum 6/7.
 * ═══════════════════════════════════════════════════════════════════ */

/* bit0 = path select, bit1 = PE4/PB11, bit2 = PE5/PB10, bit3 = PE6/PA10 */
static const uint8_t fpga_relay_tbl[10] = {
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

/* PB11 is CH2's b1 relay, NOT an FPGA run/arm line (desk sweep 2026-08-15:
 * stock writes it only from the CH2 range/cal functions; the netlist pad we
 * had read as "IOR1B = the run line" is actually an ADC data bit; and on the
 * bench that evening driving PB11 LOW did not stop an armed capture). It is
 * held HIGH across the upload and the arm writes (main() and fpga_init()
 * drive it); the CH2 range table moves it afterwards. */
static void fpga_set_ch1_frontend_range(uint8_t range_idx)
{
    uint8_t b;

    if (range_idx >= 10) range_idx = 9;
    b = fpga_relay_tbl[range_idx];

    if (b & 0x01) GPIOC->scr = (1U << 12); else GPIOC->clr = (1U << 12); /* PC12 */
    if (b & 0x02) GPIOE->scr = (1U << 4);  else GPIOE->clr = (1U << 4);  /* PE4  */
    if (b & 0x04) GPIOE->scr = (1U << 5);  else GPIOE->clr = (1U << 5);  /* PE5  */
    if (b & 0x08) GPIOE->scr = (1U << 6);  else GPIOE->clr = (1U << 6);  /* PE6  */
}

static void fpga_set_ch2_frontend_range(uint8_t range_idx)
{
    uint8_t b;

    if (range_idx >= 10) range_idx = 9;
    b = fpga_relay_tbl[range_idx];

    if (b & 0x01) GPIOA->scr = (1U << 15); else GPIOA->clr = (1U << 15); /* PA15 */
    if (b & 0x02) GPIOB->scr = PB11_MASK;  else GPIOB->clr = PB11_MASK;  /* PB11 */
    if (b & 0x04) GPIOB->scr = (1U << 10); else GPIOB->clr = (1U << 10); /* PB10 */
    if (b & 0x08) GPIOA->scr = (1U << 10); else GPIOA->clr = (1U << 10); /* PA10 */
}

/* Apply BOTH channels from their own volts/div indices. */
void fpga_set_scope_frontend_ranges(const scope_state_t *ss)
{
    /* Channel mask FIRST — it decides which converter each buffer is fed from,
     * so applying ranges before it just attenuates the wrong path. Left
     * floating this sits in mask 1 (CH1 into both buffers), which is what the
     * "CH2 is dead" symptom actually was. */
    {
        bool c1 = (ss == NULL) || ss->ch1.enabled;
        bool c2 = (ss != NULL) && ss->ch2.enabled;
        fpga_set_channel_mask((c1 && c2) ? 3u : (c2 ? 2u : 1u));
    }

    uint8_t r1 = (ss->ch1.vdiv_idx < VDIV_COUNT) ? ss->ch1.vdiv_idx : (VDIV_COUNT - 1);
    uint8_t r2 = (ss->ch2.vdiv_idx < VDIV_COUNT) ? ss->ch2.vdiv_idx : (VDIV_COUNT - 1);

    fpga_set_ch1_frontend_range(r1);
    fpga_set_ch2_frontend_range(r2);
}

/* Bench-cal hook: apply one range index to a channel (ch 0 = CH1, 1 = CH2,
 * any other value = both) without cycling volts/div through the UI. See
 * `fpga scope range <n> [ch]` in usb_debug.c.
 *
 * NOTE the previous version of this hook also forced PC12 HIGH, believing
 * PC12 to be the DC-coupling select. It is not — it is the input path
 * select, and forcing it HIGH pinned every range to the sensitive path
 * (which rails a DC-coupled input). Coupling is PD12/PD13, set in the
 * frontend init. */
void fpga_scope_set_range_diag_ch(uint8_t ch, uint8_t range_idx)
{
    if (range_idx >= VDIV_COUNT) range_idx = VDIV_COUNT - 1;

    if (ch == 0)      fpga_set_ch1_frontend_range(range_idx);
    else if (ch == 1) fpga_set_ch2_frontend_range(range_idx);
    else            { fpga_set_ch1_frontend_range(range_idx);
                      fpga_set_ch2_frontend_range(range_idx); }
}

void fpga_scope_set_range_diag(uint8_t range_idx)
{
    fpga_scope_set_range_diag_ch(0xFF, range_idx);   /* both channels */
}

/* ── Per-mode GPIO posture (2026-08-17) ──────────────────────────────────
 *
 * SOLVED TWO MONTH-OLD SYMPTOMS. Stock applies a per-mode GPIO posture that our
 * runtime never replicated. An MCU reset wipes it while the FPGA's SRAM config
 * survives — which is exactly why the warm handoff (docs/experiments/
 * 2026-08-17-03-warm-handoff-2x2.md) pointed at our runtime rather than at our
 * bit-bang configuration.
 *
 * CHANNEL MASK — ms[0x14] (1 = CH1, 2 = CH2, 3 = BOTH) drives GPIO as well as
 * SPI3 op 0x02. Decoded at stock 0x0802E202, then bench-confirmed A/B/A with a
 * 100 Hz tone on the CH1 jack and 250 Hz on the CH2 jack:
 *
 *   mask 3  PC2 H, PC1 L   op04 = CH1 22.3   op05 = CH2 65.2   <- two channels
 *   mask 1  PC2 H, PC1 H   op04 = CH1 22.5   op05 = CH1 21.8
 *   mask 2  PC2 L, PC1 L   op04 = CH2 64.4   op05 = CH2 65.4
 *
 * We left PC1/PC2 FLOATING, which lands in mask 1 — CH1 routed into BOTH
 * converters. That one fact explains every CH2 symptom we collected: both
 * buffers carrying CH1 at full amplitude, CH1's attenuator moving both, CH2's
 * relay bank moving neither, and two genuinely distinct converters (proved by a
 * 2.6-code offset, t=-357) both fed from one source.
 *
 * We were ALREADY sending op 0x02 = 3 in the arm writes. The GPIO half is the
 * load-bearing one, which is why every reg-0x02 sweep read as negative.
 *
 * ⚠ PC1/PC2 appear in older notes as "swept negative". That sweep PULSED them on
 * an UNCONFIGURED part while hunting config entry — a different question
 * entirely. Do not treat it as covering this.
 */
void fpga_set_channel_mask(uint8_t mask)
{
    /* Configure before driving: a scr/clr write to a floating input does
     * nothing at all, which is the single most repeated measurement bug in this
     * project's history. PC1 = CRL bits 7:4, PC2 = CRL bits 11:8. */
    GPIOC->cfglr = (GPIOC->cfglr & ~0x00000FF0u) | 0x00000330u;   /* PC1,PC2 PP 50MHz */

    switch (mask) {
    case 1:  GPIOC->scr = (1u << 2); GPIOC->scr = (1u << 1); break;  /* CH1  */
    case 2:  GPIOC->clr = (1u << 2); GPIOC->clr = (1u << 1); break;  /* CH2  */
    default: GPIOC->scr = (1u << 2); GPIOC->clr = (1u << 1); break;  /* BOTH */
    }
    fpga.channel_mask = mask;
}

volatile uint8_t fpga_scope_posture_gen;      /* fpga.h: moves when the scope's posture is back */

/* Apply the channel enables to the converter routing (mask 1/2/3 on PC1/PC2).
 * The record in flight is dropped: it was captured with the old routing.
 * Refuses both-off. The trigger source is not this function's: the
 * chN.enabled setter moves it off a channel that goes off and has
 * fpga_apply_trigger_source() write reg 0x06 before it calls this
 * (model/props_scope.c ch_enabled_set; stock flips ms[0x16] + op06 for the
 * same reason, 0x080235E8). */
bool fpga_apply_channel_enables(void)
{
    const scope_state_t *ss = scope_state_get();
    if (!ss->ch1.enabled && !ss->ch2.enabled)
        return false;
    fpga_set_channel_mask((ss->ch1.enabled && ss->ch2.enabled) ? 3u : (ss->ch2.enabled ? 2u : 1u));
    /* The mask is an input of stock's bank rule (both on -> bank 1 at
     * tb <= 3, one channel -> bank 2), so the offset references move with it
     * (stock re-runs 0x080089C2 / 0x08008BD4 after a channel change). */
    scope_vpos_apply();
    acq_reprime_req = true;
    return true;
}

/* Vertical sibling of fpga_apply_timebase(), for the same vintage of bug
 * found the same way (EXP-19, 2026-08-20: every Vpp in the badge-validation
 * grid refused). The UI's vdiv button mutated scope_state.vdiv_idx and
 * NOTHING else — the exact shape of the decorative timebase button (EXP-17):
 * since the labels became measured (2026-08-18), pressing it changed the
 * printed volts/div AND the counts->volts k while the relay bank stayed put,
 * i.e. a confidently wrong voltage. Relay writes are plain GPIO — no SPI3
 * frame — so no acq park is needed here.
 *
 * NOTE this drives the RELAYS only. The new range's vertical-offset
 * operating point is wherever DAC1 (CH1) / TMR13 (CH2) last left it, so the
 * trace can sit off-centre or clip until a re-center (`fpga scope center`).
 * Gain quantities (Vpp/Vrms) are offset-independent short of clipping. */
bool fpga_apply_vdiv(uint8_t ch, uint8_t idx)
{
    if ((ch != 1u && ch != 2u) || idx >= VDIV_COUNT)
        return false;
    fpga_scope_set_range_diag_ch((uint8_t)(ch - 1u), idx);
    /* With a usable stock cal page the new range's offset operating point
     * comes from it (scope_vpos.c), so the NOTE above no longer applies on
     * such units. `idx`, not vdiv_idx: callers store it after this returns. */
    scope_vpos_apply_ch(ch, idx);
    return true;
}

/* Vertical sibling of the reconcile above: fpga_init() applies the frontend
 * relays from scope_state BEFORE settings_store_init() restores it, so a
 * persisted volts/div would relabel the axis and change the counts->volts k
 * at boot while the relays stayed at the defaults. Re-applying from the
 * restored state closes the gap (EXP-19, 2026-08-20). */
void fpga_reconcile_frontend_after_arm(void)
{
    scope_state_t *ss = scope_state_get();
    fpga_set_scope_frontend_ranges(ss);
    /* Coupling too (2026-10-03): init drives PD12/PD13 HIGH (DC) regardless,
     * so a restored AC would otherwise label a DC relay. A restored GND has
     * no hardware path (see fpga_apply_coupling) and falls back to DC, so the
     * label never claims a state the relay is not in. */
    for (uint8_t ch = 0; ch < 2; ch++) {
        channel_state_t *c = ch ? &ss->ch2 : &ss->ch1;
        if (c->coupling == COUPLING_GND) c->coupling = COUPLING_DC;
        (void)fpga_apply_coupling(ch, c->coupling);
    }
}

/* AC/DC coupling relays: PD12 = CH1, PD13 = CH2, HIGH = DC (desk sweep
 * 2026-08-15 from stock; CH1 bench-confirmed the same week -- PD12 LOW is the
 * ~9 Hz high-pass we spent a morning characterising, PD12 HIGH passed a DC
 * step). The one writer of those pins after init.
 *
 * Until 2026-10-03 nothing drove them after init: CH1/CH2 cycled a DC/AC/GND
 * LABEL, the popup and info bar said "AC", and the relay stayed DC -- an
 * inert control that looked correct (the timebase button's shape, EXP-17).
 *
 * Returns true only when the pin is a GPIO push-pull output AND reads back at
 * the level asked for. In builds that leave PD12/PD13 to EXMC (alternate
 * function, ODR ignored) that is false, and so is GND: no ground path for the
 * input is known on this board, so it is refused rather than labelled. */
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
