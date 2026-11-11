/*
 * usercal.c — capture/apply glue between the live corrections and the
 * persisted record. See usercal.h and usercal_rec.h.
 *
 * RAM: only the report below is static (~12 B). The record (usercal_t, 312 B)
 * and its encode buffer (304 B) live on the caller's stack: main() at boot,
 * the usb_dbg task (3 KB) for the shell.
 */
#include "usercal.h"

#include "cal_backup_core.h"   /* CAL_BACKUP_SRC_ADDR: the one name for 0x08006000 */
#include "scope_vpos.h"
#include "fpga.h"

static usercal_report_t rep = { .load_st = USERCAL_ST_EMPTY, .last_save = FLASH_REGION_OK };

const usercal_report_t *usercal_report(void) { return &rep; }

uint32_t usercal_live_table_crc(void)
{
#ifndef EMULATOR_BUILD
    return usercal_table_crc((const uint8_t *)CAL_BACKUP_SRC_ADDR);
#else
    return 0u;
#endif
}

static int32_t round_i32(float x)
{
    return (int32_t)(x < 0.0f ? x - 0.5f : x + 0.5f);
}

void usercal_capture(usercal_t *u)
{
    usercal_clear(u);
    u->table_crc = usercal_live_table_crc();
    for (uint8_t c = 0; c < CALPAGE_CHANNELS; c++)
        for (uint8_t b = 0; b < CALPAGE_BANKS; b++)
            for (uint8_t r = 0; r < CALPAGE_RANGES; r++) {
                int16_t db, du;
                if (!scope_vpos_corr_get((uint8_t)(c + 1u), b, r, &db, &du)) continue;
                u->valid[c] |= 1u << (b * CALPAGE_RANGES + r);
                u->corr[c][b][r][0] = db;
                u->corr[c][b][r][1] = du;
            }
    float g, o;
    fpga_interleave_corr_get(&g, &o);
    u->il_gain_ppm = round_i32(g * 1e6f);
    u->il_off_e4   = round_i32(o * 1e4f);
    /* Identity (the boot default, fpga.c acq_il_corr) is "no correction". */
    u->il_present  = (u->il_gain_ppm != 1000000 || u->il_off_e4 != 0);
    u->il_n_knots  = fpga_interleave_knots_get(u->il_knot_a, u->il_knot_b);
    u->il_skew_ppm = fpga_interleave_skew_get();
}

void usercal_apply(const usercal_t *u)
{
    rep.applied = rep.refused = 0;
    rep.il_applied = false;
    rep.table_match = (u->table_crc == usercal_live_table_crc());
    const unsigned n = usercal_count(u);
    if (rep.table_match && scope_vpos_active()) {
        for (uint8_t c = 0; c < CALPAGE_CHANNELS; c++)
            for (uint8_t b = 0; b < CALPAGE_BANKS; b++)
                for (uint8_t r = 0; r < CALPAGE_RANGES; r++) {
                    if (!((u->valid[c] >> (b * CALPAGE_RANGES + r)) & 1u)) continue;
                    /* corr_set re-checks the corrected pair against 0..4095 and
                     * refuses a pair the page no longer gives (cal page off). */
                    if (scope_vpos_corr_set((uint8_t)(c + 1u), b, r,
                                            u->corr[c][b][r][0], u->corr[c][b][r][1]))
                        rep.applied++;
                    else
                        rep.refused++;
                }
        scope_vpos_apply();
    } else {
        rep.refused = (uint8_t)n;   /* stale page, or no page in use: none applied */
    }
    if (u->il_present) {
        fpga_interleave_corr_set((float)u->il_gain_ppm / 1e6f, (float)u->il_off_e4 / 1e4f);
        rep.il_applied = true;
    }
    /* After the line: setting the line clears the knots. Both setters
     * re-check what the decoder already checked. */
    if (u->il_n_knots >= 2u && fpga_interleave_knots_set(u->il_n_knots, u->il_knot_a, u->il_knot_b))
        rep.il_applied = true;
    if (u->il_skew_ppm != 0 && fpga_interleave_skew_set(u->il_skew_ppm))
        rep.il_applied = true;
}

void usercal_boot_load(void)
{
    usercal_t u;
    rep.load_st = usercal_store_load(&u);
    rep.loaded  = (rep.load_st == USERCAL_ST_OK);
    if (rep.loaded) usercal_apply(&u);
}

flash_region_status_t usercal_save(void)
{
    usercal_t u;
    usercal_capture(&u);
    rep.last_save = usercal_store_save(&u);
    rep.saved = true;
    return rep.last_save;
}

flash_region_status_t usercal_clear_saved(void)
{
    usercal_t u;
    usercal_clear(&u);
    u.table_crc = usercal_live_table_crc();
    rep.last_save = usercal_store_save(&u);
    rep.saved = true;
    if (rep.last_save == FLASH_REGION_OK) {
        scope_vpos_corr_clear();
        fpga_interleave_corr_set(1.0f, 0.0f);   /* line to identity, knots dropped */
        fpga_interleave_skew_set(0);
    }
    return rep.last_save;
}
