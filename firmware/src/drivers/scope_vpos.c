/*
 * scope_vpos.c — device glue for the stock calibration page: vertical
 * position and offset DACs (parity item C4). See scope_vpos.h; the decode,
 * validation and formula are in src/util/calpage.c and are host-tested.
 *
 * RAM: the image sits within ~1.3 KB of the 224 KB ceiling, so this keeps
 * only the 60-byte substitution map. The endpoints are read from the page on
 * demand — it is memory-mapped flash at 0x08006000 and never written by this
 * module (the app links at 0x08007000; the only writer in this firmware is
 * the explicit `cal restore CONFIRM`, after which `cal page reload` re-reads
 * it). The full decode (calpage_t, ~440 B) lives on a stack only at boot and
 * in the shell.
 *
 * Every write goes through scope_trigger_dac_raw()/scope_trigger_ch2_raw(),
 * which self-initialise DAC1 (stock CTRL 0x3D) and TMR13 (PR 4094, PSC 0)
 * as stock's master_init does; stock's codes therefore transfer 1:1
 * (ADDENDUM_A S5, bench B2). Neither touches SPI3, so no acquisition park is
 * needed — the same reason fpga_apply_vdiv() needs none.
 */
#include "scope_vpos.h"

#include <string.h>

#include "cal_backup_core.h"   /* CAL_BACKUP_SRC_ADDR: the one name for 0x08006000 */
#include "scope_cal.h"
#include "scope_state.h"
#include "scope_trigger.h"
#include "dac_output.h"
#include "fpga.h"

/* Same default as fpga.c: CH2's reference is armed in every build except the
 * `guest-coldtrace-noch2` A/B control, which must not have PA6 driven. */
#ifndef FPGA_CH2_TRIGGER
#define FPGA_CH2_TRIGGER  1
#endif

/* The fixed arm fpga_init() uses when there is no page (fpga.c: DAC1 2048 =
 * mid voltage; TMR13 2544 = the code that centred CH2 on bench unit #1). */
#define VPOS_LEGACY_DAC1   2048u
#define VPOS_LEGACY_TMR13  2544u

static struct {
    calpage_status_t status;
    bool     enabled;                 /* `cal page on|off`, default on */
    float    g[CALPAGE_CHANNELS];     /* calpage_gain(): 0 = invalid m */
    uint8_t  n_substituted, n_unusable;
    int8_t   src_bank[CALPAGE_CHANNELS][CALPAGE_BANKS][CALPAGE_RANGES];
    /* last write, per channel, for the shell */
    bool     wrote[CALPAGE_CHANNELS];
    uint8_t  last_bank[CALPAGE_CHANNELS];
    uint8_t  last_range[CALPAGE_CHANNELS];
    uint16_t last_code[CALPAGE_CHANNELS];
} vp = { .status = CALPAGE_ST_NONE, .enabled = true };

static const uint8_t *page_bytes(void)
{
    return (const uint8_t *)CAL_BACKUP_SRC_ADDR;
}

static void publish_gains(void)
{
    /* g per channel, or 0 (= compiled table) when inactive or m is invalid. */
    for (uint8_t c = 0; c < CALPAGE_CHANNELS; c++) {
        scope_cal_set_stock_gain((uint8_t)(c + 1u), scope_vpos_active() ? vp.g[c] : 0.0f);
    }
}

void scope_vpos_init(void)
{
#ifndef EMULATOR_BUILD
    calpage_t p;   /* ~440 B, boot/shell stack only */
    calpage_parse(page_bytes(), CALPAGE_LEN, &p);
    vp.status        = p.status;
    vp.g[0]          = calpage_gain(&p, 0u);
    vp.g[1]          = calpage_gain(&p, 1u);
    vp.n_substituted = p.n_substituted;
    vp.n_unusable    = p.n_unusable;
    memcpy(vp.src_bank, p.src_bank, sizeof(vp.src_bank));
#else
    vp.status = CALPAGE_ST_NONE;
#endif
    publish_gains();
    scope_vpos_apply();
}

bool scope_vpos_active(void)
{
    return vp.enabled && vp.status == CALPAGE_ST_OK;
}

bool scope_vpos_enable(bool on)
{
    if (on && vp.status != CALPAGE_ST_OK) return false;
    const bool was = scope_vpos_active();
    vp.enabled = on;
    publish_gains();
    if (on) {
        scope_vpos_apply();
    } else if (was) {
        /* Back to the pre-C4 operating point, as fpga_init() leaves it. */
        scope_trigger_dac_raw(VPOS_LEGACY_DAC1);
#if FPGA_CH2_TRIGGER
        scope_trigger_ch2_raw(VPOS_LEGACY_TMR13);
#endif
        vp.wrote[0] = vp.wrote[1] = false;
    }
    return true;
}

calpage_status_t scope_vpos_status(void)
{
    return vp.status;
}

static channel_state_t *chan(uint8_t ch)
{
    scope_state_t *ss = scope_state_get();
    return (ch == 2u) ? &ss->ch2 : &ss->ch1;
}

void scope_vpos_apply_ch(uint8_t ch, uint8_t range)
{
    if ((ch != 1u && ch != 2u) || !scope_vpos_active()) return;
#if !FPGA_CH2_TRIGGER
    if (ch == 2u) return;
#endif
    /* Stock's bank rule on the code in force (reg 0x01) and the converter
     * routing in force (PC1/PC2 mask) — the hardware state, not the UI's
     * intent, because the bank describes how the converters are clocked. */
    const uint8_t bank = calpage_bank_select(fpga_acq_rate_idx_get(), fpga.channel_mask);
    /* DAC1/PA4 belongs to the signal generator while it runs: the dac_raw
     * writer would re-init it as the CH1 reference and kill the output (e.g.
     * `fpga scope vdiv` from the shell during siggen). Re-applied on scope
     * entry instead (fpga_enter_scope_mode). */
    if (ch == 1u && dac_output_is_running()) return;
    const int pos = calpage_clamp_pos(chan(ch)->position);
    uint16_t code;
    if (!calpage_dac_code_page(page_bytes(), (const int8_t (*)[CALPAGE_BANKS][CALPAGE_RANGES])vp.src_bank,
                               (uint8_t)(ch - 1u), bank, range, pos, &code))
        return;   /* no clean bank for this range: leave the reference alone */
    if (ch == 1u) scope_trigger_dac_raw(code);    /* DAC1 D1DTH12R, PA4 */
    else          scope_trigger_ch2_raw(code);    /* TMR13 C1DT, PA6    */
    const unsigned i = ch - 1u;
    vp.wrote[i] = true;
    vp.last_bank[i] = bank;
    vp.last_range[i] = range;
    vp.last_code[i] = code;
}

void scope_vpos_apply(void)
{
    const scope_state_t *ss = scope_state_get();
    scope_vpos_apply_ch(1u, ss->ch1.vdiv_idx);
    scope_vpos_apply_ch(2u, ss->ch2.vdiv_idx);
}

bool scope_vpos_set(uint8_t ch, int pos)
{
    if (ch != 1u && ch != 2u) return false;
    channel_state_t *c = chan(ch);
    c->position = (int16_t)calpage_clamp_pos(pos);
    scope_vpos_apply_ch(ch, c->vdiv_idx);
    return true;
}

int scope_vpos_get(uint8_t ch)
{
    if (ch != 1u && ch != 2u) return 0;
    return calpage_clamp_pos(chan(ch)->position);
}

bool scope_vpos_last(uint8_t ch, uint8_t *bank, uint8_t *range, uint16_t *code)
{
    if ((ch != 1u && ch != 2u) || !vp.wrote[ch - 1u]) return false;
    if (bank)  *bank  = vp.last_bank[ch - 1u];
    if (range) *range = vp.last_range[ch - 1u];
    if (code)  *code  = vp.last_code[ch - 1u];
    return true;
}

void scope_vpos_recover(void)
{
    if (scope_vpos_active()) scope_vpos_apply();
    else                     scope_trigger_dac_raw(VPOS_LEGACY_DAC1);
}
