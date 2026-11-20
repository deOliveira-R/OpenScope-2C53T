/*
 * OpenScope 2C53T - shell commands: oscilloscope
 *
 * The capture core, interleave, trigger, ranges, centring, measurements,
 * roll, self-calibration runs and the mask test.
 */

#include "shell_parts.h"

/* `status` ends with the timebase in force (defined with the scope commands). */
void timebase_print_status(bool after_set);

/* `fpga cfgsettle [ms|off|default]`: the capture-configuration rule
 * (dsp/acq_cfg.h). A record is published only after routing, range,
 * position, probe, timebase and trigger have been stable for fill + poll gap
 * + this margin. `off` (stored as 0xFFFF) admits every record, for an A/B in
 * one boot; `default` (stored as 0) uses ACQ_CFG_SETTLE_MS. Prints the
 * setting and the count of refused records. */
void cmd_fpga_cfgsettle(const char *args)
{
    while (*args == ' ') args++;
    if (strcmp(args, "off") == 0) {
        fpga_acq_cfg_settle_set(0xFFFFu);
    } else if (strcmp(args, "default") == 0) {
        fpga_acq_cfg_settle_set(0u);
    } else if (*args) {
        uint32_t ms = 0;
        if (parse_int(args, &ms) != 0 || ms < 1u || ms > 2000u) {
            usb_send_str("usage: fpga cfgsettle [ms 1..2000|off|default]\r\n");
            return;
        }
        fpga_acq_cfg_settle_set((uint16_t)ms);
    }
    const uint16_t c = fpga_acq_cfg_settle_get();
    if (c == 0xFFFFu)
        usb_debug_printf("cfgsettle: off (every record admitted); refused so far %lu\r\n",
                         (unsigned long)fpga_acq_cfg_drops());
    else
        usb_debug_printf("cfgsettle: %u ms%s after fill + poll gap; records refused %lu\r\n",
                         (unsigned)(c ? c : ACQ_CFG_SETTLE_MS), c ? "" : " (default)",
                         (unsigned long)fpga_acq_cfg_drops());
}

/* `fpga unrotate [on|off]`: time-order strobed records at the seam the core
 * reports. Prints the setting, the last seam position and how many held
 * records carried a seam. */
void cmd_fpga_unrotate(const char *args)
{
    while (*args == ' ') args++;
    if (*args) {
        if      (strncmp(args, "on",  2) == 0) fpga_acq_unrotate_set(true);
        else if (strncmp(args, "off", 3) == 0) fpga_acq_unrotate_set(false);
        else { usb_send_str("usage: fpga unrotate [on|off]\r\n"); return; }
    }
    usb_debug_printf("acq unrotate %s (seam-based); last strobed record's seam at %ld (-1 = none found)\r\n",
                     fpga_acq_unrotate_get() ? "ON" : "OFF", (long)acq.acq_last_rot);
    usb_debug_printf("held records ordered by the header's seam: %lu; held without a seam: %lu\r\n",
                     (unsigned long)fpga_acq_seam_count(0), (unsigned long)fpga_acq_seam_count(1));
}

/* `fpga core [fix on|off]`: which capture core runs, by its own account
 * (op E2, asked once when it was loaded), whether this firmware can use it,
 * the acquisition counters and the core's status block. `fix` turns on or off
 * the rewrite of registers that differ from the firmware's copies. */
void cmd_fpga_core(const char *args)
{
    while (*args == ' ') args++;
    if (strncmp(args, "fix on", 6) == 0)         fpga_core_watch_fix_set(true);
    else if (strncmp(args, "fix off", 7) == 0)   fpga_core_watch_fix_set(false);
    else if (*args) { usb_send_str("usage: fpga core [fix on|off]\r\n"); return; }
    const core_ident_t id = fpga_core_ident();
    if (!fpga_acq_core_ok())
        usb_debug_printf("core: no usable core; register writes asked for and not sent: %lu\r\n",
                         (unsigned long)fpga_acq_reg_writes_refused());
    if (!fpga_acq_design_ours()) { usb_send_str("core: NOT USABLE: the design in the FPGA was not loaded by this firmware; not asked, nothing is acquired\r\n"); return; }
    if (!id.valid) {
        usb_debug_printf("core: NOT USABLE: loaded by us but no valid identity (older than D5, or protocol %u unknown); nothing is acquired; reply %02X %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
                         (unsigned)id.proto, fboot.core_ident[0], fboot.core_ident[1], fboot.core_ident[2], fboot.core_ident[3],
                         fboot.core_ident[4], fboot.core_ident[5], fboot.core_ident[6], fboot.core_ident[7], fboot.core_ident[8]);
        return;
    }
    usb_debug_printf("core: tag %02X  protocol %u  record %u samples  caps %02X:%s%s%s%s%s\r\n",
                     (unsigned)id.tag, (unsigned)id.proto, 1u << id.depth_log2, (unsigned)id.caps,
                     core_ident_has(&id, FPGA_CORE_CAP_SEAM)    ? " seam"     : "",
                     core_ident_has(&id, FPGA_CORE_CAP_EDGE)    ? " edge"     : "",
                     core_ident_has(&id, FPGA_CORE_CAP_REAL50)  ? " real50"   : "",
                     core_ident_has(&id, FPGA_CORE_CAP_LOCKRST) ? " lock-rst" : "",
                     core_ident_has(&id, FPGA_CORE_CAP_FORCE)   ? " force"    : "");
    if (!fpga_acq_core_ok())
        usb_debug_printf("      NOT USABLE: this firmware needs caps %02X (seam edge real50 force); nothing is acquired\r\n",
                         (unsigned)CORE_IDENT_REQUIRED_CAPS);
    usb_debug_printf("      AUTO without a trigger: records held on request (op 09); requests %lu, forced records read %lu, not ready yet %lu\r\n",
                     (unsigned long)fpga_acq_force_count(0), (unsigned long)fpga_acq_force_count(1), (unsigned long)fpga_acq_force_count(2));
    if (id.tag != FPGA_CORE_TAG)
        usb_debug_printf("      (this firmware's header was made with tag %02X)\r\n", (unsigned)FPGA_CORE_TAG);
    usb_debug_printf("      reads by the op 05 header: held %lu, not held %lu; PC0 disagreed: held without an edge %lu, edge but not held %lu\r\n",
                     (unsigned long)fpga_acq_kind_count(0), (unsigned long)fpga_acq_kind_count(1),
                     (unsigned long)fpga_acq_kind_count(2), (unsigned long)fpga_acq_kind_count(3));
    usb_debug_printf("      captures restarted before a first read (RUN, SINGLE arm): %lu\r\n", (unsigned long)fpga_acq_fresh_restarts());
    const fpga_core_watch_t w = fpga_core_watch();
    if (!w.last.valid)
        usb_debug_printf("      status block: none read yet (%lu asked, %lu not a status block)\r\n", (unsigned long)w.polls, (unsigned long)w.bad);
    else
        usb_debug_printf("      status block: PLL %s, lock lost %u times, capture state %u; registers tb %02X src %02X level %02X edge %02X\r\n"
                         "      (firmware's copies: tb %02X src %02X level %02X edge %02X)\r\n"
                         "      read %lu times, %lu not a status block, %lu with the PLL unlocked\r\n",
                         w.last.locked ? "locked" : "NOT LOCKED", (unsigned)w.last.nlost, (unsigned)w.last.cap_state,
                         (unsigned)w.last.tb, (unsigned)w.last.src, (unsigned)w.last.lvl, (unsigned)w.last.edge,
                         (unsigned)fpga_acq_rate_idx_get(), (unsigned)fpga_acq_trig_src_code_get(),
                         (unsigned)fpga_acq_trig_code_get(), (unsigned)fpga_acq_edge_reg_get(),
                         (unsigned long)w.polls, (unsigned long)w.bad, (unsigned long)w.unlocked);
    usb_debug_printf("      registers rewritten (differed from the firmware's copy twice running; fix %s): tb %lu src %lu level %lu edge %lu\r\n",
                     w.check_on ? "on" : "off", (unsigned long)w.fixed[0], (unsigned long)w.fixed[1],
                     (unsigned long)w.fixed[2], (unsigned long)w.fixed[3]);
}

/* v x 100 rounded to the nearest integer. The shell's printf has no floats,
 * so values are printed as integers. */
static long x100(float v) { return (long)(v * 100.0f + (v < 0.0f ? -0.5f : 0.5f)); }

/* The three arguments for "%s%ld.%02ld": the sign apart, or -0.6 would print
 * as 0.60. */
#define F2(v)  (x100(v) < 0 ? "-" : ""), (long)((x100(v) < 0 ? -x100(v) : x100(v)) / 100), \
               (long)((x100(v) < 0 ? -x100(v) : x100(v)) % 100)

/* `fpga interleave cal [piecewise]`: converter-B self-calibration. Needs CH2
 * off, timebase <= 3 (interleave active) and a quiet CH1 input.
 *   1. Park the trigger level above the sweep.
 *   2. Step the CH1 offset DAC over five codes from this unit's calibration
 *      page (il_cal_dac_points: base+80, base+400, mid, upper-400, upper) and
 *      collect the op04 (A) and op05 (B) means at each.
 *   3. Put the DAC back to the position code and the trigger level back.
 *   4. Fit B = gain * A + offset (dsp/interleave_cal.c).
 *   5. Apply the line in RAM; with `piecewise`, also the knots.
 * Refuses, leaving the correction as it was, on a noisy input, rail-clipped
 * levels, a short span, a gain outside 0.5..2, a poor fit, or a channel or
 * timebase change during the sweep. */
static void cmd_fpga_interleave_cal(bool piecewise)
{
    scope_state_t *ss = scope_state_get();
    if (ss->ch2.enabled || scope_frontend_channel_mask() != 1u || !fpga_interleave_active()) {
        usb_send_str("refused: needs CH2 off (mask 1), timebase <= 3 and interleave ON "
                     "(set ch2.enabled=off; set tb.code <= 3)\r\n");
        return;
    }
    uint16_t base, upper, codes[5];
    if (!scope_vpos_ch1_endpoints(&base, &upper) || !il_cal_dac_points(base, upper, codes)) {
        usb_send_str("refused: needs a usable calibration page (cal page) with a 1000+ code span\r\n");
        return;
    }
    static il_cal_point_t pt[5];
    unsigned got = 0;
    /* Park the trigger level above every sweep level for the sweep. At the
     * user's level (0 = mid-screen) the middle level sits on it: noise
     * crossing it triggers those records and biases them, about 8 counts of
     * record-to-record drift on both converters, and the fit is refused. The
     * UI's level is untouched and written back after the sweep. */
    const bool parked = fpga_apply_trigger_level(100);
    usb_send_str("interleave cal: keep CH1 quiet (shorted or a steady DC); sweeping the CH1 offset DAC\r\n");
    for (unsigned i = 0; i < 5u; i++) {
        if (!scope_vpos_ch1_raw(codes[i])) break;
        vTaskDelay(pdMS_TO_TICKS(250));            /* the offset RC settles */
        fpga_ilcal_begin(2);
        if (!fpga_ilcal_collect(8, 8000, &pt[i])) {
            usb_debug_printf("  DAC %u: no records (acquisition not running?)\r\n", (unsigned)codes[i]);
            break;
        }
        got++;
        usb_debug_printf("  DAC %4u  A %s%ld.%02ld  B %s%ld.%02ld  p2p %u/%u  drift %s%ld.%02ld/%s%ld.%02ld%s\r\n",
                         (unsigned)codes[i], F2(pt[i].a_mean), F2(pt[i].b_mean),
                         (unsigned)pt[i].a_p2p, (unsigned)pt[i].b_p2p,
                         F2(pt[i].a_drift), F2(pt[i].b_drift),
                         pt[i].clipped ? "  CLIPPED (not used)" : "");
    }
    scope_vpos_apply_ch(1u, ss->ch1.vdiv_idx);     /* DAC back to the position code */
    if (parked && !fpga_apply_trigger_level(ss->trigger.level))
        usb_debug_printf("  WARNING: trigger level not restored (reg 0x08); `set trig.level %d`\r\n",
                         ss->trigger.level);
    {   /* apply_ch is silent when it cannot write (no clean bank after a
         * range change): say so rather than leave a sweep code. */
        uint16_t pos_code = 0;
        if (scope_vpos_active() &&
            (!scope_vpos_last(1u, NULL, NULL, &pos_code) || scope_trigger_dac_last() != pos_code))
            usb_debug_printf("  WARNING: CH1 offset DAC not restored (DAC %u, position code %u); "
                             "re-select the CH1 range or position\r\n",
                             (unsigned)scope_trigger_dac_last(), (unsigned)pos_code);
    }
    if (got < 5u) {
        usb_send_str("interleave cal: aborted, correction unchanged\r\n");
        return;
    }
    il_cal_params_t prm;
    il_cal_default_params(&prm);
    il_cal_fit_t fit;
    const il_cal_status_t st = il_cal_fit(pt, 5u, &prm, &fit);
    usb_debug_printf("  fit: B = gain %ld/10000 * A + offset %ld/100   used %u  span %s%ld.%02ld  "
                     "residual max %s%ld.%02ld rms %s%ld.%02ld\r\n",
                     (long)(fit.gain * 10000.0f + 0.5f), x100(fit.offset), (unsigned)fit.used,
                     F2(fit.a_span), F2(fit.resid_max), F2(fit.resid_rms));
    if (st != IL_CAL_OK) {
        usb_debug_printf("interleave cal: REFUSED, %s; correction unchanged\r\n", il_cal_status_str(st));
        return;
    }
    /* The sweep takes seconds: a CH2 long-press or a timebase change in the
     * meantime moves op05 off CH1 (or ends the merge), and the points no
     * longer describe converter B against A. */
    if (ss->ch2.enabled || scope_frontend_channel_mask() != 1u || !fpga_interleave_active()) {
        usb_send_str("interleave cal: REFUSED, channel/timebase changed during the sweep; "
                     "correction unchanged\r\n");
        return;
    }
    /* The line by default; setting it clears any knots. The knots pass
     * exactly through the five DC levels and so keep their ~0.5-count static
     * errors: measured on one unit with skew correction on and a 10 MHz tone,
     * the image was -52.0 dBc with the line and -45.2 dBc with the knots.
     * `cal piecewise` applies them on top of the line, for comparison. */
    fpga_interleave_corr_set(fit.gain, fit.offset);
    if (piecewise && fit.n_knots >= 2u &&
        fpga_interleave_knots_set(fit.n_knots, fit.knot_a, fit.knot_b))
        usb_debug_printf("interleave cal: applied, piecewise through %u levels (RAM; `cal user save` "
                         "keeps it). Skew: `fpga interleave skew measure` with a tone.\r\n",
                         (unsigned)fit.n_knots);
    else
        usb_send_str("interleave cal: applied, line only (RAM; `cal user save` keeps it). "
                     "Skew: `fpga interleave skew measure` with a tone.\r\n");
}

/* Print B's position as a fraction of a converter period ("0.451234"),
 * from its deviation from 0.5 in ppm of a period: 500000 + dev. */
static void print_skew_line(const char *what, int32_t dev)
{
    const int32_t t = 500000 + dev;
    usb_debug_printf("%s B at 0.%06ld of a converter period (%s%ld ppm from 0.5)\r\n",
                     what, (long)t, dev < 0 ? "-" : "+", (long)(dev < 0 ? -dev : dev));
}

#define SKEW_RECORDS   16u     /* fresh merged records per measurement */

#define SKEW_MIN_OK     8u     /* usable records needed to apply the median */

/* `fpga interleave skew measure`, and one pass of `fpga interleave tone`.
 * Needs CH2 off, timebase <= 3 and a steady, triggering tone of 5..50 MHz on
 * CH1 (a 10 MHz square serves).
 *   1. Take SKEW_RECORDS fresh merged records; in each, estimate B's delay
 *      after A (least-squares phase at the tone, dsp/interleave_cal.c).
 *   2. Apply the median. With a skew correction already on, the records read
 *      the residual, which is added: a second `measure` should read ~0.500.
 *   3. With with_gain, also set B's gain from the median B'/A amplitude ratio,
 *      keeping the offset.
 * Refuses, leaving the setting unchanged, on too few usable records, a wide
 * spread, or a channel, timebase or skew change during the measurement.
 * Returns true when everything asked for was applied. Uses shell_bus_scratch
 * for the record copy. */
static bool cmd_fpga_interleave_skew_measure(bool with_gain)
{
    scope_state_t *ss = scope_state_get();
    if (ss->ch2.enabled || scope_frontend_channel_mask() != 1u || !fpga_interleave_active()) {
        usb_send_str("refused: needs CH2 off (mask 1), timebase <= 3 and interleave ON "
                     "(set ch2.enabled=off; set tb.code <= 3)\r\n");
        return false;
    }
    const volatile uint8_t *c1 = fpga_get_ch1_buf();
    if (!c1) { usb_send_str("FPGA not initialized\r\n"); return false; }
    const int32_t cur = fpga_interleave_skew_get();
    float tau[SKEW_RECORDS];
    float gr[SKEW_RECORDS];             /* B'/A amplitude per usable record */
    unsigned ok = 0;
    uint8_t *rec = shell_bus_scratch;
    usb_send_str("interleave skew: keep a steady 5..50 MHz tone on CH1, triggering\r\n");
    for (unsigned r = 0; r < SKEW_RECORDS; r++) {
        /* One fresh, whole, merged record: an even generation past the last
         * one, unchanged across the copy (the acq task commits under it). */
        const uint32_t want = (fpga_acq_frame_generation() & ~1u) + 2u;
        const TickType_t t0 = xTaskGetTickCount();
        bool got = false;
        while (!got && (xTaskGetTickCount() - t0) < pdMS_TO_TICKS(2000)) {
            const uint32_t g0 = fpga_acq_frame_generation();
            if ((g0 & 1u) || (int32_t)(g0 - want) < 0 || !fpga_acq_record_interleaved()) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            for (uint32_t i = 0; i < FPGA_ADC_BUF_SIZE; i++) rec[i] = c1[i];
            got = (fpga_acq_frame_generation() == g0) && fpga_acq_record_interleaved();
        }
        if (!got) {
            usb_debug_printf("  %2u  no fresh merged record in 2 s (not triggering? STOP?)\r\n", r);
            continue;
        }
        il_skew_est_t e;
        const il_skew_status_t st = il_skew_estimate(rec, &e);
        const long khz = (long)(e.cycles * 125000.0f + 0.5f);
        const float tdev = (e.tau - 0.5f) * 1e6f;
        usb_debug_printf("  %2u  %-8s f %ld.%03ld MHz  amp A %s%ld.%02ld B %s%ld.%02ld  B %ld ppm from 0.5%s%s\r\n",
                         r, st == IL_SKEW_OK ? "ok" : "refused", khz / 1000, khz % 1000,
                         F2(e.amp_a), F2(e.amp_b), (long)(tdev + (tdev < 0.0f ? -0.5f : 0.5f)),
                         st == IL_SKEW_OK ? "" : "  ", st == IL_SKEW_OK ? "" : il_skew_status_str(st));
        if (st == IL_SKEW_OK) {
            gr[ok] = (e.amp_a > 1.0f) ? e.amp_b / e.amp_a : 0.0f;
            tau[ok++] = e.tau;
        }
    }
    if (ok < SKEW_MIN_OK) {
        usb_debug_printf("interleave skew: REFUSED, %u of %u records usable (need %u); skew unchanged\r\n",
                         ok, SKEW_RECORDS, SKEW_MIN_OK);
        return false;
    }
    for (unsigned i = 1; i < ok; i++)                    /* insertion sort, <= 16 */
        for (unsigned j = i; j > 0 && tau[j - 1] > tau[j]; j--) { float t = tau[j]; tau[j] = tau[j - 1]; tau[j - 1] = t; }
    const float med = (ok & 1u) ? tau[ok / 2u] : 0.5f * (tau[ok / 2u - 1u] + tau[ok / 2u]);
    /* The middle half's spread: a 10 MHz tone scatters ~0.005 per record on
     * a typical unit; a spread past 0.02 is not a steady tone. */
    const float iqr = tau[(3u * ok) / 4u] - tau[ok / 4u];
    const int32_t dev = (int32_t)((med - 0.5f) * 1e6f + (med < 0.5f ? -0.5f : 0.5f));
    print_skew_line("  median:", dev);
    usb_debug_printf("  spread (middle half) %ld ppm over %u records; correction was %ld ppm\r\n",
                     (long)(iqr * 1e6f + 0.5f), ok, (long)cur);
    if (iqr > 0.02f) {
        usb_send_str("interleave skew: REFUSED, records disagree (not a steady tone); skew unchanged\r\n");
        return false;
    }
    if (ss->ch2.enabled || scope_frontend_channel_mask() != 1u || !fpga_interleave_active() ||
        fpga_interleave_skew_get() != cur) {
        usb_send_str("interleave skew: REFUSED, channel/timebase/skew changed during the measurement; "
                     "skew unchanged\r\n");
        return false;
    }
    if (!fpga_interleave_skew_set(cur + dev)) {
        usb_debug_printf("interleave skew: REFUSED, %ld ppm is beyond +-%ld; skew unchanged\r\n",
                         (long)(cur + dev), (long)IL_SKEW_PPM_MAX);
        return false;
    }
    usb_send_str("interleave skew: applied (RAM; `cal user save` keeps it)\r\n");
    if (!with_gain) return true;

    /* Converter B's gain at the tone. The records are already corrected,
     * B' = (B - off) / g, so the true B/A ratio is g x median(B'/A). Measured
     * on one unit at 10 MHz: the image is lowest at a gain of 0.982..0.986,
     * the DC sweep gives 0.980, the tone's own ratio 0.984. */
    for (unsigned i = 1; i < ok; i++)
        for (unsigned k = i; k > 0 && gr[k - 1] > gr[k]; k--) { float t = gr[k]; gr[k] = gr[k - 1]; gr[k - 1] = t; }
    const float gmed = (ok & 1u) ? gr[ok / 2u] : 0.5f * (gr[ok / 2u - 1u] + gr[ok / 2u]);
    const float gspread = gr[(3u * ok) / 4u] - gr[ok / 4u];
    float g_cur = 1.0f, o_cur = 0.0f;
    fpga_interleave_corr_get(&g_cur, &o_cur);
    const float g_new = g_cur * gmed;
    usb_debug_printf("  gain: B'/A median %ld/10000 (spread %ld/10000) -> B gain %ld/10000 (was %ld), offset kept %ld/100\r\n",
                     (long)(gmed * 10000.0f + 0.5f), (long)(gspread * 10000.0f + 0.5f),
                     (long)(g_new * 10000.0f + 0.5f), (long)(g_cur * 10000.0f + 0.5f), x100(o_cur));
    if (gspread > 0.01f || g_new < 0.90f || g_new > 1.10f) {
        usb_send_str("interleave gain: REFUSED (records disagree or out of 0.90..1.10); gain unchanged\r\n");
        return false;
    }
    fpga_interleave_corr_set(g_new, o_cur);     /* line; clears knots, keeps skew */
    usb_send_str("interleave gain: applied (RAM; `cal user save` keeps it)\r\n");
    return true;
}

/* The skew line `fpga interleave skew measure` ends with. The skew is B's
 * deviation from half a converter period, in ppm of a period, signed (B at
 * 0.451 of a period is -49000); 0 means no correction. It is set with the
 * il.skew property. */
static void interleave_skew_print_status(void)
{
    const int32_t ppm = fpga_interleave_skew_get();
    if (ppm == 0) usb_send_str("interleave skew: correction OFF (B taken at 0.500)\r\n");
    else          print_skew_line("interleave skew: correcting", ppm);
}

/* The status block every `fpga interleave ...` ends with: the single-channel
 * merge and its converter-B correction (B' = (B - off) / gain, record units),
 * which are the properties il.enabled, il.gain, il.offset, il.knots, il.skew. */
static void interleave_print_status(void)
{
    float g, o;
    fpga_interleave_corr_get(&g, &o);
    int16_t ka[IL_KNOTS_MAX], kb[IL_KNOTS_MAX];
    const uint8_t nk = fpga_interleave_knots_get(ka, kb);
    const int32_t g4 = (int32_t)(g * 10000.0f + 0.5f);
    const int32_t o2 = (int32_t)(o * 100.0f + (o < 0 ? -0.5f : 0.5f));
    usb_debug_printf("interleave %s, %s; last record %s; B corr gain %ld/10000 offset %ld/100; "
                     "(triggered and forced records are merged)\r\n",
                     fpga_interleave_get() ? "ON" : "OFF",
                     fpga_interleave_active() ? "active (mask 1, tb <= 3)" : "not active",
                     fpga_acq_record_interleaved() ? "merged" : "single converter",
                     (long)g4, (long)o2);
    if (nk >= 2u) {   /* the knots override the line */
        usb_send_str("  B map, piecewise (A <- B, x100):");
        for (uint8_t i = 0; i < nk; i++)
            usb_debug_printf(" %d<-%d", (int)ka[i], (int)kb[i]);
        usb_send_str("\r\n");
    }
    const int32_t sk = fpga_interleave_skew_get();
    if (sk) print_skew_line("  skew: correcting", sk);
    else    usb_send_str("  skew: correction OFF\r\n");
}

/* `fpga interleave [cal [piecewise]|tone|skew measure]`: the status block and
 * the procedures, which drive the front end over many records and then write
 * the fit they found. The settings are the il.* properties. */
void cmd_fpga_interleave(const char *args)
{
    while (*args == ' ') args++;
    if (strncmp(args, "cal", 3) == 0) {
        cmd_fpga_interleave_cal(strstr(args + 3, "piecewise") != NULL);
        interleave_print_status();
        return;
    }
    if (strncmp(args, "tone", 4) == 0) {
        /* Skew and B's gain from a 5..50 MHz tone, twice: the gain error
         * biases the skew estimate and the skew the amplitudes. The offset
         * stays the DC sweep's (`cal`). */
        for (unsigned pass = 1; pass <= 2u; pass++) {
            usb_debug_printf("interleave tone: pass %u of 2\r\n", pass);
            if (!cmd_fpga_interleave_skew_measure(true)) break;
        }
        interleave_print_status();
        return;
    }
    if (strncmp(args, "skew", 4) == 0) {
        const char *p = args + 4;
        while (*p == ' ') p++;
        if (strncmp(p, "measure", 7) == 0) {
            (void)cmd_fpga_interleave_skew_measure(false);
            interleave_skew_print_status();
            return;
        }
    }
    if (*args) {
        usb_send_str("usage: fpga interleave [cal [piecewise]|tone|skew measure]  (no argument: status)\r\n"
                     "  the settings are properties: set il.enabled on|off; set il.knots=0 il.gain=<g> "
                     "il.offset=<o>; set il.skew <ppm>\r\n");
        return;
    }
    interleave_print_status();
}

/* The trigger: the settings and the registers in force, then what the
 * acquisition did (trig_policy.h): the status, the badge, the published
 * record's kind (none, triggered, forced) and the SINGLE state; then the
 * level code the boot reconcile wrote and where the last drawn window put
 * the trigger point. */
static void print_trigger_status(void)
{
    const scope_state_t *ss = scope_state_get();
    static const char *const kind[] = { "none", "triggered", "forced" };
    const uint8_t k = fpga_acq_record_kind();
    const trig_status_t st = (trig_status_t)fpga_trigger_status();
    usb_debug_printf("trigger mode %s edge %s source %s level %d code 0x%02X reg06 0x%02X reg07 0x%02X\r\n",
                     trigger_mode_labels[ss->trigger.mode], trigger_edge_labels[ss->trigger.edge],
                     trigger_source_labels[ss->trigger.source], (int)ss->trigger.level,
                     (unsigned)fpga_acq_trig_code_get(), (unsigned)fpga_acq_trig_src_code_get(),
                     (unsigned)fpga_acq_edge_reg_get());
    usb_debug_printf("trigger status %s badge %s record %s single %s\r\n",
                     trig_status_word(st), trig_status_label(st),
                     k < 3u ? kind[k] : "?",
                     ss->trigger.mode != TRIG_SINGLE ? "-" :
                     fpga_acq_single_done() ? "captured" : "armed");
    /* What no property carries: the level code the boot reconcile wrote,
     * and where the last drawn window put the trigger point. */
    const uint8_t a = (uint8_t)scope_ui_trig_anchor();
    usb_debug_printf("trigger level %d -> reg 0x08 code 0x%02X in force (boot reconcile wrote 0x%02X)\r\n",
                     (int)ss->trigger.level, (unsigned)fpga_acq_trig_code_get(),
                     (unsigned)fpga_trigger_reconcile_code());
    usb_debug_printf("trigger position: asked column %d; last window landed at %d (anchor %s)\r\n",
                     (int)prop_get(P(TRIG_X)), (int)scope_ui_trig_x_actual(),
                     a == 2 ? (scope_ui_trig_anchor_exact() ? "hardware, the core's index" : "hardware, searched") : a == 1 ? "soft" : "none");
}

/* `fpga scope trigger`: print_trigger_status(). The settings themselves
 * are the trig.* properties. */
void cmd_fpga_scope_trigger(const char *args)
{
    (void)args;
    print_trigger_status();
}

/* `fpga scope range <n> <1|2|both>`: drive coarse frontend range n (0-9) on
 * one channel or, with the explicit `both` keyword, on both. The two channels
 * have independent relay banks (CH1 PC12/PE4/PE5/PE6, CH2 PA15/PB11/PB10/PA10)
 * driven from the same 10-case table, whose values come from the stock
 * firmware. Coupling (PD12/PD13, set in the frontend init) is not touched.
 * Raw relay drive only: the display state and the volts-per-count are not
 * updated.
 *
 * The channel argument is mandatory and 1-based. Anything else, a `0`
 * included, is an error rather than a guess, so a mistyped channel can never
 * drive both banks while printing a success line. */
void cmd_fpga_scope_range(const char *args)
{
    static const char USAGE[] =
        "Usage: fpga scope range <0-9> <1|2|both>\r\n"
        "  channel is MANDATORY and 1-BASED: 1 = CH1, 2 = CH2, both = CH1+CH2\r\n";
    uint32_t n = 0;
    uint32_t chn = 0;
    uint8_t ch_sel;
    const char *space;

    if (args == NULL || *args == '\0' || parse_int(args, &n) != 0 || n > 9) {
        usb_send_str(USAGE);
        return;
    }

    space = strchr(args, ' ');
    if (space == NULL) {
        usb_send_str("ERR: no channel given — refusing to assume.\r\n");
        usb_send_str(USAGE);
        return;
    }
    while (*space == ' ') space++;

    if (strcmp(space, "both") == 0) {
        ch_sel = 0xFF;
    } else if (parse_int(space, &chn) == 0 && (chn == 1 || chn == 2)) {
        ch_sel = (uint8_t)(chn - 1);
    } else {
        usb_debug_printf("ERR: bad channel '%s' — 1, 2 or both (there is no channel 0)\r\n",
                         space);
        usb_send_str(USAGE);
        return;
    }

    fpga_scope_set_range_diag_ch(ch_sel, (uint8_t)n);
    usb_send_str("(raw relay drive — display state and counts->volts k NOT "
                 "updated; use `set chN.vdiv`)\r\n");
    usb_debug_printf("scope frontend range = %lu on %s\r\n",
                     (unsigned long)n,
                     ch_sel == 0 ? "CH1" : (ch_sel == 1 ? "CH2" : "CH1+CH2"));
}

/* ---- fpga scope center: per-range DC-offset centring ----
 *
 * Each frontend range has its own DC operating point, so before per-range
 * gain can be calibrated the channel's vertical-offset reference must put a
 * quiet input at mid-scale (record code ~128). For CH1 the reference is DAC1
 * (PA4); for CH2 it is the TMR13 channel 1 PWM-DAC (PA6). The search runs
 * with the acquisition task live and reads its RAM buffer, never SPI3, so
 * there is no bus contention; the task must keep running to refresh that
 * buffer at each new operating point. The input must be quiet or DC.
 */
#define SCOPE_CENTER_TARGET   128u   /* mid-scale record code to centre on */

#define SCOPE_CENTER_AVG      2u     /* buffer medians averaged per step    */

#define SCOPE_CENTER_ITERS    11u    /* binary-search steps over 0..4095    */

#define SCOPE_CENTER_SETTLE_MS 480u  /* analog settle after a DAC move
                                        (~350 ms measured), before waiting
                                        for fresh commits                   */
#define SCOPE_CENTER_COMMIT_TIMEOUT_MS 4000u /* bound on waiting for fresh
                                        commits; 0x12 AUTO fallback ~1.5 s   */
static uint32_t scope_center_stale_steps;  /* steps that timed out waiting */

/*
 * Median of the acquisition task's RAM buffer for channel `ch` (2 = CH2,
 * anything else CH1), via an 8-bit histogram. Returns 128 when there is no
 * buffer.
 *
 * Median, not mean: a clipped AC signal biases the mean toward the open
 * rail, so a mean-driven search raises the DAC, clips more and runs away to a
 * rail. The median equals the DC baseline until more than half the samples
 * clip, and stays monotonic in the DAC, so the binary search converges at
 * any amplitude.
 *
 * The live buffer, not a direct SPI3 read with acquisition paused: with
 * acquisition paused nothing re-arms the capture, so after a DAC step the
 * capture buffer freezes holding a mix of old and new levels and no statistic
 * recovers the new operating point. The running task re-arms and commits
 * continuously, so its buffer tracks the DAC.
 */
uint32_t scope_center_buf_median(uint8_t ch)
{
    const volatile uint8_t *b = (ch == 2) ? fpga_get_ch2_buf()
                                          : fpga_get_ch1_buf();
    if (b == NULL) return 128;
    uint16_t hist[256] = {0};
    for (uint32_t i = 0; i < FPGA_ADC_BUF_SIZE; i++)
        hist[b[i]]++;
    uint32_t half = FPGA_ADC_BUF_SIZE / 2u, cum = 0;
    for (uint32_t v = 0; v < 256; v++) {
        cum += hist[v];
        if (cum >= half) return v;
    }
    return 128;
}

/* Wait until the acquisition task has committed `n` records after this call.
 * The frame generation is a seqlock: +2 per commit, odd while a commit is in
 * progress. Returns false after SCOPE_CENTER_COMMIT_TIMEOUT_MS.
 *
 * A fixed delay is not enough: the RAM buffer changes only on a commit, and
 * with no trigger crossing AUTO commits one free-run record per fallback
 * budget, 3 x fill + 230 ms = 475 ms at timebase 0x10 and ~1.46 s at 0x12.
 * A median taken before that commit describes the level before the DAC step.
 * The first commit after the write may have been read before the DAC settled,
 * so the caller asks for two. */
static bool scope_center_wait_commits(uint32_t n)
{
    uint32_t g = fpga_acq_frame_generation() & ~1u;
    uint32_t target = g + 2u * n;
    TickType_t t0 = xTaskGetTickCount();
    while ((int32_t)((fpga_acq_frame_generation() & ~1u) - target) < 0) {
        if ((xTaskGetTickCount() - t0) > pdMS_TO_TICKS(SCOPE_CENTER_COMMIT_TIMEOUT_MS))
            return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

/* The settled level after a DAC step: wait SCOPE_CENTER_SETTLE_MS for the
 * analog settle, then for two fresh commits, then average SCOPE_CENTER_AVG
 * buffer medians, one commit apart, to knock down per-record noise. A commit
 * wait that times out increments scope_center_stale_steps; the median is
 * still taken, and the caller reports the result as unreliable. */
static uint32_t scope_center_settle_level(uint8_t ch)
{
    vTaskDelay(pdMS_TO_TICKS(SCOPE_CENTER_SETTLE_MS));
    if (!scope_center_wait_commits(2u)) scope_center_stale_steps++;
    uint32_t acc = 0;
    for (uint32_t k = 0; k < SCOPE_CENTER_AVG; k++) {
        if (k > 0 && !scope_center_wait_commits(1u)) scope_center_stale_steps++;
        acc += scope_center_buf_median(ch);
    }
    return acc / SCOPE_CENTER_AVG;
}

/* Binary-search channel `ch`'s offset reference (2 = CH2's TMR13 PWM-DAC,
 * anything else CH1's DAC1) for a settled median of SCOPE_CENTER_TARGET.
 * Expects the range already applied and the acquisition task running. Leaves
 * the reference at the best code found and returns that code and its median.
 *
 * The search assumes the level rises with the code. That was measured for
 * DAC1 on CH1 (500 -> ~0, 2500 -> ~140, 3500 -> ~255). For CH2 it is assumed:
 * TMR13's channel polarity is active-low (CCTRL C1P set), so the sense could
 * invert. If a CH2 search converges on a rail, check the direction with
 * `trig2 raw` by hand before suspecting the pin. */
static void scope_center_one(uint8_t ch, uint16_t *out_dac, uint32_t *out_mean)
{
    uint16_t lo = 0, hi = 4095;
    uint16_t best_dac = 2048;
    uint32_t best_mean = 0;
    uint32_t best_err = 0xFFFFFFFFu;

    for (uint32_t it = 0; it < SCOPE_CENTER_ITERS; it++) {
        uint16_t mid = (uint16_t)((lo + hi) / 2);
        if (ch == 2) scope_trigger_ch2_raw(mid);
        else         scope_trigger_dac_raw(mid);
        uint32_t level = scope_center_settle_level(ch);
        uint32_t err = (level > SCOPE_CENTER_TARGET)
                       ? level - SCOPE_CENTER_TARGET
                       : SCOPE_CENTER_TARGET - level;
        if (err < best_err) {
            best_err = err;
            best_dac = mid;
            best_mean = level;
        }
        if (level < SCOPE_CENTER_TARGET) lo = mid;   /* need a higher code */
        else                             hi = mid;   /* need a lower code  */
        if (hi - lo <= 1) break;
    }
    /* Leave the reference at the code that is reported, not at the last
     * probe. */
    if (ch == 2) scope_trigger_ch2_raw(best_dac);
    else         scope_trigger_dac_raw(best_dac);
    *out_dac = best_dac;
    *out_mean = best_mean;
}

/* The timebase in force, for `status`: what the boot reconcile did, then the
 * code (the property tb.code), reg 0x01, the sample rate and the s/div label.
 * after_set: a code was just set, so the published record is still the
 * previous one. */
void timebase_print_status(bool after_set)
{
    const scope_state_t *ss = scope_state_get();
    static const char *const recon[] = {
        "NEVER RAN — this build's arm path does not call it",
        "pushed the persisted code to the FPGA",
        "pulled the display down to the arm block's 0x08",
    };
    const uint8_t ra = fpga_timebase_reconcile_action();
    usb_debug_printf("boot reconcile: %s\r\n", recon[ra < 3u ? ra : 0u]);

    char lbl[12];
    /* The label the status bar shows: x2 when merged, times the fast-code
     * zoom. After a set, ask what the new code will produce
     * (fpga_interleave_active) rather than scope_ui_view_merged(), which
     * reads the previous record. */
    scope_timebase_view_label(ss->timebase_idx,
                              after_set ? fpga_interleave_active()
                                        : scope_ui_view_merged(),
                              lbl, sizeof(lbl));
    usb_debug_printf("timebase 0x%02X (reg 0x01 = 0x%02X)  %lu S/s  %s/div\r\n",
                     (unsigned)ss->timebase_idx,
                     (unsigned)fpga_acq_rate_idx_get(),
                     (unsigned long)(scope_timebase_sample_rate(ss->timebase_idx) + 0.5f),
                     lbl);
}

/* One roll point of channel `ch` into `out`: its code and its volts to the
 * millivolt ("code 140 = +0.123 V"), or the code and the zero when the range
 * is uncalibrated. The same zero and gain the Y-T badges use. */
static void fmt_roll_point(char *out, size_t n, uint8_t ch, uint8_t code)
{
    const scope_state_t *ss = scope_state_get();
    const channel_state_t *c = (ch == 1u) ? &ss->ch1 : &ss->ch2;
    const uint8_t zero = scope_measure_zero_for(scope_vpos_active(), c->position).v;
    const float vpc = scope_cal_volts_per_count(ch, c->vdiv_idx);
    if (!(vpc > 0.0f)) {
        snprintf(out, n, "code %u (zero %u, no cal)", (unsigned)code, (unsigned)zero);
        return;
    }
    const float v = scope_roll_point_volts((rec_code_t){ code }, (rec_code_t){ zero },
                                           (volts_per_count_t){ vpc }).v;
    long mv = (long)(v * 1000.0f + (v >= 0.0f ? 0.5f : -0.5f));
    const char sign = (mv < 0) ? '-' : '+';
    if (mv < 0) mv = -mv;
    snprintf(out, n, "code %u = %c%ld.%03ld V", (unsigned)code, sign, mv / 1000, mv % 1000);
}

/* Roll mode status: setting, step, reg 0x01, the point FIFO and read counts,
 * the last op03 frame exactly as received (so the live slots can be
 * checked), and the newest point of each channel against its expected 0 V
 * and the offset DAC last written. */
static void roll_print_status(void)
{
    const scope_state_t *ss = scope_state_get();
    fpga_roll_stats_t st;
    fpga_roll_stats_get(&st);
    char lbl[12];
    scope_roll_label(ss->roll_step, lbl, sizeof(lbl));
    const int ov = fpga_roll_code_override_get();
    usb_debug_printf("roll: %s  step %u = %s/div, %lu ms/point  (ui %s, acq %s)\r\n",
                     ss->roll ? "ON" : "off", (unsigned)ss->roll_step, lbl,
                     (unsigned long)scope_roll_period_ms(ss->roll_step).ms,
                     ss->roll ? "roll" : "Y-T",
                     fpga_roll_active() ? "op03" : "04/05");
    usb_debug_printf("reg 0x01 = 0x%02X (last apply; entry byte %s%02X)\r\n",
                     (unsigned)fpga_acq_rate_idx_get(),
                     ov >= 0 ? "override 0x" : "stock 0x",
                     (unsigned)(ov >= 0 ? (unsigned)ov : scope_roll_stock_code(ss->roll_step)));
    const scope_roll_fifo_t *f = fpga_roll_fifo();
    usb_debug_printf("fifo %u/%u points, %lu pushed; reads %lu rejects %lu late %lu\r\n",
                     (unsigned)f->count, (unsigned)SCOPE_ROLL_POINTS,
                     (unsigned long)f->total, (unsigned long)st.reads,
                     (unsigned long)st.rejects, (unsigned long)st.late);
    usb_debug_printf("last op03 rx: %02X %02X [CH1 %02X] %02X [CH2 %02X]\r\n",
                     st.rx[0], st.rx[1], st.rx[2], st.rx[3], st.rx[4]);
    usb_debug_printf("trigger: mode %u (0 AUTO 1 NORM 2 SINGLE), roll hits %lu, run %u\r\n",
                     (unsigned)ss->trigger.mode, (unsigned long)st.trig_hits,
                     (unsigned)ss->running);
    uint8_t c1, c2;
    if (scope_roll_fifo_get(f, 0u, 0u, &c1) && scope_roll_fifo_get(f, 1u, 0u, &c2)) {
        char a[40], b[40];
        fmt_roll_point(a, sizeof(a), 1u, c1);
        fmt_roll_point(b, sizeof(b), 2u, c2);
        usb_debug_printf("newest: CH1 %s  CH2 %s\r\n", a, b);
        /* Where each channel's 0 V is expected and what the offset DAC was
         * last told: the zero in the lines above is the cal-page formula
         * (100 + pos), so a channel whose DAC was never written (or written
         * for another range) reads off by exactly the difference. */
        for (uint8_t ch = 1u; ch <= 2u; ch++) {
            const channel_state_t *cs = (ch == 1u) ? &ss->ch1 : &ss->ch2;
            const uint8_t z = scope_measure_zero_for(scope_vpos_active(), cs->position).v;
            uint8_t bk = 0, rg = 0;
            uint16_t dc = 0;
            const bool w = scope_vpos_last(ch, &bk, &rg, &dc);
            const uint8_t c = (ch == 1u) ? c1 : c2;
            usb_debug_printf("CH%u: en %u pos %d range %u zero %u, newest %u = %+d cnt; "
                             "dac %s",
                             (unsigned)ch, (unsigned)cs->enabled, (int)cs->position,
                             (unsigned)cs->vdiv_idx, (unsigned)z, (unsigned)c,
                             (int)c - (int)z, w ? "" : "never written\r\n");
            if (w)
                usb_debug_printf("bank %u range %u code %u\r\n", (unsigned)bk,
                                 (unsigned)rg, (unsigned)dc);
        }
    }
}

/* `fpga roll [code <hex>|code stock]`: roll mode status. Roll itself is the
 * property tb.roll and its step tb.rollstep. `code` overrides the reg-0x01
 * byte written on entering roll (the stock value is 0x14 + step; 0x14 is the
 * only code with a measured rate); `code stock` removes the override. It is a
 * debug setting of fpga_acq.c, not a property. When roll is on, the step is
 * requested again so roll is re-entered with the new byte. */
void cmd_fpga_roll(const char *args)
{
    while (*args == ' ') args++;
    if (strncmp(args, "code", 4) == 0) {
        const char *q = args + 4;
        while (*q == ' ') q++;
        uint32_t v;
        if (strncmp(q, "stock", 5) == 0) {
            fpga_roll_code_override_set(-1);
        } else if (*q && parse_hex32(q, &v) == 0 && v <= 0xFFu) {
            fpga_roll_code_override_set((int)v);
        } else {
            usb_send_str("usage: fpga roll code <hex 00-FF>|stock\r\n");
            return;
        }
        /* Rolling: re-enter with the new byte, by asking the input task for
         * the step in force again (the setter re-enters roll with it). */
        if (prop_get(P(TB_ROLL))) {
            const prop_pair_t p = { PROP_ID_TB_ROLLSTEP, prop_get(P(TB_ROLLSTEP)) };
            model_result_t res;
            const prop_status_t st = prop_request(&p, 1, ORIGIN_SHELL, prop_request_timeout(&p, 1), &res);
            if (st != PS_OK && st != PS_CLAMPED) {
                usb_debug_printf("%s tb.rollstep (%s): the entry byte is set, roll was not re-entered\r\n",
                                 prop_status_name(st), res.note.text[0] ? res.note.text : "no reason given");
                return;
            }
        }
    } else if (*args) {
        usb_send_str("usage: fpga roll [code <hex 00-FF>|code stock]  (no argument: status)\r\n"
                     "  roll is a property: set tb.rollstep=<0-8> tb.roll=on; set tb.roll off\r\n");
        return;
    }
    roll_print_status();
}

/* `fpga scope measure [reps]`: print what the measurement badges compute,
 * raw and machine-parseable, for badge validation on the bench. One header
 * line, then per pass (reps, default 5, clamped to 1..50, 60 ms apart) an
 * `M` line and a `P` line from the live record buffers. It calls the same
 * functions the badges call (scope_measure_record, scope_cal_volts_per_count,
 * scope_timebase_sample_rate, scope_freq_estimate, scope_params_compute)
 * rather than reimplementing them, so the shell and the screen cannot
 * disagree.
 *
 * Integer-only output (the shell's printf has no %f): voltages in microvolts,
 * duty in permille, period and edge times in samples x100, frequency in mHz.
 * A field the instrument refuses (k = 0, no rate, estimator declined) prints
 * "-", so a refusal can never be confused with a measured zero. */
void cmd_fpga_scope_measure(const char *args)
{
    uint32_t reps = 5;
    /* An unparseable argument is an error, not a silent single pass. */
    if (args && *args && parse_int(args, &reps) != 0) {
        usb_send_str("Usage: fpga scope measure [reps 1-50]\r\n");
        return;
    }
    if (reps == 0u) reps = 1u;
    if (reps > 50u) reps = 50u;

    const scope_state_t *ss = scope_state_get();
    const float fs_tab = scope_timebase_sample_rate(ss->timebase_idx);
    const uint8_t in_force = fpga_acq_rate_idx_get();
    /* The published record's rate (x2 for an interleaved record) when the
     * hardware runs the code the UI shows, else the table rate; `fs` in the
     * header and the Freq estimate both use it, so an interleaved record is
     * not read at half. */
    const float fs = (in_force == ss->timebase_idx) ? fpga_record_sample_rate()
                                                    : fs_tab;
    const bool tb_ok = (in_force == ss->timebase_idx) && (fs > 0.0f);
    const float k1 = scope_cal_volts_per_count(1u, ss->ch1.vdiv_idx);
    const float k2 = scope_cal_volts_per_count(2u, ss->ch2.vdiv_idx);

    /* k1/k2 are probe-TIP volts per count (scope_cal applies the probe
     * factor); probeN says which factor, so a bench script can tell a 10x
     * setting from a calibration change. The probe fields come last, so
     * parsers of the earlier fields are not affected. */
    usb_debug_printf("badge sources: rng1=%u k1_uV=%lu  rng2=%u k2_uV=%lu  "
                     "tb=0x%02X inforce=0x%02X fs=%lu  probe1=x%u probe2=x%u\r\n",
                     (unsigned)ss->ch1.vdiv_idx,
                     (unsigned long)(k1 * 1e6f + 0.5f),
                     (unsigned)ss->ch2.vdiv_idx,
                     (unsigned long)(k2 * 1e6f + 0.5f),
                     (unsigned)ss->timebase_idx, (unsigned)in_force,
                     (unsigned long)(fs + 0.5f),
                     (unsigned)(scope_cal_probe_factor(1u) + 0.5f),
                     (unsigned)(scope_cal_probe_factor(2u) + 0.5f));
    if (in_force != ss->timebase_idx)
        usb_send_str("TB MISMATCH: frequency suppressed (see fpga scope freq)\r\n");

    const volatile uint8_t *ch1 = fpga_get_ch1_buf();
    const volatile uint8_t *ch2 = fpga_get_ch2_buf();
    if (!ch1) { usb_send_str("FPGA not initialized\r\n"); return; }

    for (uint32_t i = 0; i < reps; i++) {
        scope_measure_t m1, m2;
        scope_measure_record((const uint8_t *)ch1, FPGA_ADC_BUF_SIZE, &m1);
        scope_measure_record(ch2 ? (const uint8_t *)ch2 : NULL,
                             ch2 ? FPGA_ADC_BUF_SIZE : 0u, &m2);

        char vpp1[12] = "-", vrms1[12] = "-", per[12] = "-", f[16] = "-";
        float freq_hz = 0.0f;
        char vpp2[12] = "-";
        if (k1 > 0.0f) {
            snprintf(vpp1, sizeof vpp1, "%lu",
                     (unsigned long)((float)m1.pp_robust * k1 * 1e6f + 0.5f));
            snprintf(vrms1, sizeof vrms1, "%lu",
                     (unsigned long)(m1.ac_rms * k1 * 1e6f + 0.5f));
        }
        if (m1.period_valid)
            snprintf(per, sizeof per, "%lu",
                     (unsigned long)(m1.period_samples * 100.0f + 0.5f));
        if (tb_ok) {
            scope_freq_t fr;
            if (scope_freq_estimate((const uint8_t *)ch1, FPGA_ADC_BUF_SIZE,
                                    fs, &fr)) {
                freq_hz = fr.hz;
                snprintf(f, sizeof f, "%lu",
                         (unsigned long)(fr.hz * 1000.0f + 0.5f));
            }
        }
        if (m2.valid && k2 > 0.0f)
            snprintf(vpp2, sizeof vpp2, "%lu",
                     (unsigned long)((float)m2.pp_robust * k2 * 1e6f + 0.5f));

        /* Edge timing in samples x100. "-" when no clean edge was found,
         * same convention as per1. Kept in samples, not seconds: multiply by
         * 1/fs on the host when a rate is in force. */
        char rise[12] = "-", fall[12] = "-";
        if (m1.rise_valid)
            snprintf(rise, sizeof rise, "%lu",
                     (unsigned long)(m1.rise_samples * 100.0f + 0.5f));
        if (m1.fall_valid)
            snprintf(fall, sizeof fall, "%lu",
                     (unsigned long)(m1.fall_samples * 100.0f + 0.5f));

        usb_debug_printf("M %2lu pp1=%u ppr1=%u Vpp1_uV=%s Vrms1_uV=%s duty1_pm=%lu "
                         "per1_smp100=%s f1_mHz=%s rise1_smp100=%s fall1_smp100=%s "
                         "pp2=%u Vpp2_uV=%s\r\n",
                         (unsigned long)i, (unsigned)m1.pp,
                         (unsigned)m1.pp_robust, vpp1, vrms1,
                         (unsigned long)(m1.level_valid
                                         ? m1.duty_pct * 10.0f + 0.5f : 0.0f),
                         per, f, rise, fall, (unsigned)m2.pp, vpp2);

        /* All 12 parameters (the stock set), for CH1, through the same
         * scope_params_compute() the badges use. "--" = refused. Hidden or
         * not, all 12 are listed: selection only decides what the screen
         * shows. */
        {
            scope_prm_ctx_t ctx;
            scope_params_t pp;
            /* Only the code in force describes the record in hand; with a
             * mismatch the widths stay in samples, like Freq stays "-". */
            ctx.sample_rate.hz  = tb_ok ? fs : 0.0f;
            ctx.record_whole    = fpga_acq_record_time_ordered();
            ctx.volts_per_count.vpc = k1;
            ctx.freq_hz         = freq_hz;
            ctx.zero_count      = scope_measure_zero_for(scope_vpos_active(), ss->ch1.position);
            scope_params_compute(&m1, &ctx, &pp);
            usb_debug_printf("P %2lu", (unsigned long)i);
            for (unsigned id = 0; id < SCOPE_PRM_COUNT; id++) {
                char b[16];
                scope_prm_format(&pp.v[id], b, sizeof b);
                usb_debug_printf(" %s=%s", scope_prm_label(id), b);
            }
            usb_send_str("\r\n");
        }
        usb_delay_ms(60);
    }
}

/* For `ui dump`: which of the 12 measurements each channel shows (chN.meas,
 * bit i = parameter i in stock's order: Freq Peri Duty+ Duty- Wid+ Wid- Vmax
 * Vmin Vp-p Amp Vrms Avg) and long PRM's hide (view.meashide), as the strip
 * draws them. With more than MSTRIP_MAX_PER_CH (6) on, the strip shows the
 * ones measure_strip_shown() picks (levels first); the others are listed
 * "(no slot)". */
void prm_print_status(void)
{
    const scope_state_t *ss = scope_state_get();
    usb_debug_printf("prm hidden=%u\r\n", (unsigned)ss->prm_hidden);
    for (uint8_t c = 0; c < 2u; c++) {
        const uint16_t m = meas_select_get(c);
        usb_debug_printf("CH%u mask=0x%03X shown:", (unsigned)c + 1u, (unsigned)m);
        uint8_t ids[SCOPE_PRM_COUNT];
        const unsigned n = scope_prm_select(m, ids, SCOPE_PRM_COUNT);
        const uint16_t shown = measure_strip_shown(m);
        for (unsigned i = 0; i < n; i++)
            usb_debug_printf(" %s%s", scope_prm_label(ids[i]),
                             (shown >> ids[i]) & 1u ? "" : "(no slot)");
        usb_send_str("\r\n");
    }
}

/* `fpga scope cursor`: the cursor readouts as the screen computes them:
 * X1 X2 X1-X2 1/|dX| from the PUBLISHED record's rate, Y1 Y2 Y1-Y2 from the
 * last drawn band. Y1/Y2 are "--" unless the true-scale graticule is on (the
 * autofit render re-anchors the vertical every frame). Move the cursors first
 * with the CUR key; the cursors' mode does not matter here. */
void cmd_fpga_scope_cursor(const char *args)
{
    (void)args;
    char x1[16], x2[16], dx[16], ix[16], y1[16], y2[16], dy[16];
    float fs = 0.0f;
    scope_ui_cursor_report(x1, x2, dx, ix, y1, y2, dy, &fs);
    /* fs is the column rate, 1000 x record rate / R: record rate x zoom L,
     * or the selected code's rate on a re-sampled held record (L = 0). */
    usb_debug_printf("cursor fs=%lu L=%u R=%lu X1=%s X2=%s X1-X2=%s Y1=%s Y2=%s Y1-Y2=%s 1/dX=%s\r\n",
                     (unsigned long)(fs + 0.5f), (unsigned)scope_ui_view_factor(),
                     (unsigned long)scope_ui_view_ratio(), x1, x2, dx, y1, y2, dy, ix);
}

/* `fpga scope center [ch1|ch2] [0-9]`: centre the channel's vertical-offset
 * reference for one range (number given) or for every range 0..9, one report
 * line per range (scope_center_one). The channel defaults to CH1 and is a
 * named token (`ch1`, `ch2`), not a number, so it cannot be mistaken for the
 * range. Writes only the offset reference and the range relays; the
 * reference stays at the last range's result. */
void cmd_fpga_scope_center(const char *args)
{
    char buf[32];
    uint8_t ch = 1;

    if (args == NULL) args = "";
    if (strlen(args) >= sizeof(buf)) {
        usb_send_str("Usage: fpga scope center [ch1|ch2] [0-9]\r\n");
        return;
    }
    strcpy(buf, args);

    char *saveptr = NULL;
    char *t1 = strtok_r(buf, " \t", &saveptr);
    if (t1 != NULL && (strcmp(t1, "ch1") == 0 || strcmp(t1, "ch2") == 0)) {
        ch = (t1[2] == '2') ? 2u : 1u;
        t1 = strtok_r(NULL, " \t", &saveptr);
    }

    bool all = (t1 == NULL || *t1 == '\0');
    uint32_t n = 0;

    if (!all && (parse_int(t1, &n) != 0 || n > 9)) {
        usb_send_str("Usage: fpga scope center [ch1|ch2] [0-9]\r\n");
        return;
    }

    /* The acquisition task stays running: it keeps the RAM buffer fresh at
     * each new operating point (see scope_center_buf_median), and the search
     * reads RAM, never SPI3.
     *
     * The reference's peripheral must be initialised before raw writes take
     * effect; without it the writes are inert and the search wanders around
     * a fixed operating point. `trig raw` initialises it the same way. */
    if (ch == 2) scope_trigger_ch2_init();
    else         scope_trigger_dac_init();

    uint32_t first = all ? 0 : n;
    uint32_t last  = all ? 9 : n;
    for (uint32_t r = first; r <= last; r++) {
        uint16_t dac;
        uint32_t mean;
        /* Applies the range to both relay banks whichever channel is
         * centred. The two frontends are independent, so the other channel's
         * relays do not affect the result. */
        fpga_scope_set_range_diag((uint8_t)r);
        vTaskDelay(pdMS_TO_TICKS(20));     /* relay/frontend settle */
        scope_center_stale_steps = 0;
        scope_center_one(ch, &dac, &mean);
        /* Name the channel and its reference in the output, so a log line
         * cannot be attributed to the wrong pin. */
        usb_debug_printf("CH%u range %lu: center %s=%u (median=%lu)%s\r\n",
                         (unsigned)ch, (unsigned long)r,
                         (ch == 2) ? "TMR13_C1DT" : "DAC1",
                         dac, (unsigned long)mean,
                         scope_center_stale_steps
                             ? "  STALE: no fresh commit on some steps -- result unreliable"
                             : "");
    }
}

/* ---- autoset, baseline correction, 50 % centre ----
 * The shell only queues a run; the key task's service does it, exactly as
 * for the AUTO / long AUTO / long OK keys (scope_selfcal_run.h). Poll with
 * `fpga scope selfcal`. Nothing here writes flash. */
static const char *selfcal_kind_str(scr_kind_t k)
{
    return k == SCR_AUTOSET ? "autoset" : k == SCR_BASECAL ? "baseline" : "idle";
}

void cmd_fpga_scope_autoset(void)
{
    if (current_mode != MODE_OSCILLOSCOPE) { usb_send_str("refused: not in scope mode\r\n"); return; }
    usb_send_str(scope_selfcal_request_autoset()
                 ? "autoset queued (poll: fpga scope selfcal)\r\n" : "busy\r\n");
}

/* FFT AUTO as the AUTO key asks for it in a spectrum view (fft.h): the
 * display task fits the span to the record of its next spectrum frame. */
void cmd_fft_auto(void)
{
#if FEATURE_FFT
    extern void fft_auto_request(void);
    extern bool fft_is_initialized(void);
    if (!fft_is_initialized()) { usb_send_str("refused: no spectrum view (set view.mode fft|split|waterfall)\r\n"); return; }
    fft_auto_request();
    usb_send_str("fft auto requested (next spectrum frame)\r\n");
#else
    usb_send_str("refused: not in this build\r\n");
#endif
}

/* `fpga scope basecal [banks]`: queue a baseline correction over the banks
 * in the bit mask (bit b = bank b, default 3). Needs scope mode and the
 * calibration page in use; the inputs must be open or grounded. */
void cmd_fpga_scope_basecal(const char *args)
{
    while (*args == ' ') args++;
    uint32_t banks = 3u;
    if (*args && (parse_hex32(args, &banks) != 0 || banks == 0u || banks > 7u)) {
        usb_send_str("usage: fpga scope basecal [banks 1-7, bit b = bank b; default 3]\r\n");
        return;
    }
    if (current_mode != MODE_OSCILLOSCOPE) { usb_send_str("refused: not in scope mode\r\n"); return; }
    if (!scope_vpos_active()) { usb_send_str("refused: calibration page not in use (cal page)\r\n"); return; }
    if (!scope_selfcal_request_basecal((uint8_t)banks)) { usb_send_str("busy\r\n"); return; }
    usb_debug_printf("baseline correction queued, banks 0x%lX; input must be open/grounded "
                     "(poll: fpga scope selfcal; cancel: fpga scope selfcal cancel)\r\n",
                     (unsigned long)banks);
}

/* `fpga scope center50`: queue the 50 % centre, wait for the key task to do
 * it, then print the positions, trigger column and level it left. */
void cmd_fpga_scope_center50(void)
{
    if (current_mode != MODE_OSCILLOSCOPE) { usb_send_str("refused: not in scope mode\r\n"); return; }
    if (!scope_selfcal_request_center50()) { usb_send_str("busy\r\n"); return; }
    vTaskDelay(pdMS_TO_TICKS(250));   /* the key task acts within 100 ms */
    const scope_state_t *ss = scope_state_get();
    usb_debug_printf("50%% centre: pos CH1 %d CH2 %d, trig_x %d, level %d (code %u)\r\n",
                     (int)ss->ch1.position, (int)ss->ch2.position, (int)ss->trig_x,
                     (int)ss->trigger.level, (unsigned)fpga_acq_trig_code_get());
}

/* `fpga scope selfcal [cancel]`: progress and result of the autoset or
 * baseline run; after a finished baseline run, each refused entry with its
 * reason and the per-channel counts. `cancel` asks the running one to stop. */
void cmd_fpga_scope_selfcal(const char *args)
{
    while (*args == ' ') args++;
    if (strncmp(args, "cancel", 6) == 0) {
        scope_selfcal_request_cancel();
        vTaskDelay(pdMS_TO_TICKS(250));
    } else if (*args) {
        usb_send_str("usage: fpga scope selfcal [cancel]\r\n");
        return;
    }
    scope_selfcal_status_t st;
    scope_selfcal_get_status(&st);
    const scope_state_t *ss = scope_state_get();
    usb_debug_printf("selfcal: %s", selfcal_kind_str(st.kind));
    if (st.kind == SCR_BASECAL)
        usb_debug_printf(" %s b%u r%u", st.prechecking ? "pre-check" : "walk",
                         (unsigned)st.bank, (unsigned)st.range);
    usb_debug_printf("  last: %s %s  records %lu  timeouts %lu  torn %lu\r\n",
                     selfcal_kind_str(st.last_kind), st.last_result,
                     (unsigned long)st.records, (unsigned long)st.timeouts,
                     (unsigned long)st.torn);
    if (st.kind == SCR_BASECAL || st.last_kind == SCR_BASECAL)
        usb_debug_printf("baseline: %u ok %u fail (last fail: CH%u b%u r%u %s)\r\n",
                         (unsigned)st.n_ok, (unsigned)st.n_fail, (unsigned)st.last_fail_ch + 1u,
                         (unsigned)st.last_fail_bank, (unsigned)st.last_fail_range,
                         st.last_fail ? st.last_fail : "-");
    if (st.kind == SCR_IDLE && st.last_kind == SCR_BASECAL) {
        if (st.open_mask)
            usb_debug_printf("baseline: input open/noisy on%s%s (refused, not corrected)\r\n",
                             (st.open_mask & 1u) ? " CH1" : "", (st.open_mask & 2u) ? " CH2" : "");
        /* One line per refused entry (<= 60), as `ch b r: reason`; corrected ones are counted. */
        char why[96];
        for (unsigned ch = 0; ch < 2u; ch++) {
            unsigned okn = 0, bad = 0;
            for (unsigned bk = 0; bk < 3u; bk++)
                for (unsigned r = 0; r < 10u; r++) {
                    uint8_t es = 0;
                    if (!scope_selfcal_basecal_entry((uint8_t)ch, (uint8_t)bk, (uint8_t)r,
                                                     &es, why, sizeof(why)) || es == 0u) continue;
                    if (es == 1u) { okn++; continue; }
                    bad++;
                    usb_debug_printf("  refused CH%u b%u r%u: %s\r\n", ch + 1u, bk, r, why);
                }
            usb_debug_printf("  CH%u: %u corrected, %u refused\r\n", ch + 1u, okn, bad);
        }
    }
    usb_debug_printf("corrections in RAM: %u   now: CH1 r%u pos %d  CH2 r%u pos %d  tb 0x%02X  "
                     "trig %s src CH%u level %d\r\n",
                     (unsigned)st.corr_count, (unsigned)ss->ch1.vdiv_idx, (int)ss->ch1.position,
                     (unsigned)ss->ch2.vdiv_idx, (int)ss->ch2.position, (unsigned)ss->timebase_idx,
                     trigger_mode_labels[ss->trigger.mode],
                     ss->trigger.source == TRIG_SRC_CH2 ? 2u : 1u, (int)ss->trigger.level);
}

/* ---- `mask`: waveform pass/fail (docs/specs/scope/mask-pass-fail.md) ----
 *
 * Every mutation is a request applied by the acquisition task (scope_mask.h).
 * It is posted on the input task, where the keys and the mask's properties
 * post theirs (model_fw_mask_request, through input_call_sync), so a shell
 * request and a key never interleave; each command then waits for the
 * acknowledgement and SAYS when it did not arrive, instead of reporting the
 * intent as the result. */
typedef struct { uint8_t kind, a, b; uint16_t n; uint32_t seq; } mask_call_t;

static void mask_call_run(void *vc)
{
    mask_call_t *c = (mask_call_t *)vc;
    c->seq = model_fw_mask_request(c->kind, c->a, c->b, c->n);
}

/* Post mask request k (arguments a, b, n) on the input task and wait for
 * the acquisition task's acknowledgement. Returns true when it was applied;
 * otherwise prints which step failed. */
static bool mask_post_wait(scope_mask_req_t k, uint8_t a, uint8_t b, uint16_t n)
{
    mask_call_t c = { (uint8_t)k, a, b, n, 0u };
    if (!input_call_sync(mask_call_run, &c, 1000u)) {
        usb_send_str("NOT POSTED: the input task did not take the request in 1 s "
                     "(gallery / countdown / self-cal?).\r\n");
        return false;
    }
    for (int i = 0; i < 30; i++) {                  /* 300 ms here; 400 ms with the input task's 100 */
        if (scope_mask_acked(c.seq)) return true;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    usb_send_str("NOT APPLIED: the acquisition loop did not pick the request up in 400 ms\r\n"
                 "(not a live-scope build, scope not initialised, or acq parked).\r\n");
    return false;
}

static const char *mask_state_str(mask_pf_state_t s)
{
    return s == MASK_PF_READY ? "READY" : s == MASK_PF_TEACHING ? "TEACHING" : "EMPTY";
}

static const char *mask_verdict_str(mask_pf_verdict_t v)
{
    return v == MASK_PF_V_PASS ? "PASS" : v == MASK_PF_V_FAIL ? "FAIL" :
           v == MASK_PF_V_SKIP ? "SKIP" : "-";
}

static void mask_print_cond_diff(uint8_t d)
{
    if (d & MASK_PF_C_RANGE1)     usb_send_str(" CH1-range");
    if (d & MASK_PF_C_RANGE2)     usb_send_str(" CH2-range");
    if (d & MASK_PF_C_COUPLING)   usb_send_str(" coupling");
    if (d & MASK_PF_C_TIMEBASE)   usb_send_str(" timebase");
    if (d & MASK_PF_C_TRIG_LEVEL) usb_send_str(" trig-level");
    if (d & MASK_PF_C_TRIG_EDGE)  usb_send_str(" trig-edge");
    if (d & MASK_PF_C_TRIG_SRC)   usb_send_str(" trig-source");
}

/* The mask's state, teach progress, the conditions it was taught under,
 * the test counters with skip reasons, the last verdict and last failure,
 * stop-on-fail, the move focus and the free heap. */
static void mask_print_status(void)
{
    scope_mask_summary_t s;
    scope_mask_summary(&s);
    usb_debug_printf("mask %s  chans %s%s  span 320 from anchor-%d  tol v=%u cnt h=%u smp%s\r\n",
                     mask_state_str(s.state), (s.chans & 1u) ? "CH1" : "",
                     (s.chans & 2u) ? ((s.chans & 1u) ? "+CH2" : "CH2") : "",
                     (int)s.pre, (unsigned)s.tol_v, (unsigned)s.tol_h,
                     s.rail_limited ? "  RAIL-LIMITED" : "");
    if (s.state == MASK_PF_TEACHING || s.teach_rejects) {
        usb_debug_printf("teach %u/%u  rejected %lu", (unsigned)s.teach_got,
                         (unsigned)s.teach_target, (unsigned long)s.teach_rejects);
        if (s.teach_last_reject != MASK_PF_R_OK)
            usb_debug_printf(" (last: %s)", mask_pf_reason_str(s.teach_last_reject));
        usb_send_str("\r\n");
    }
    if (s.state == MASK_PF_READY) {
        usb_debug_printf("taught under: range %u/%u coupling %u/%u tb 0x%02X trig code %u %s src CH%u  spread %u cnt\r\n",
                         s.cond.range[0], s.cond.range[1], s.cond.coupling[0], s.cond.coupling[1],
                         s.cond.timebase, s.cond.trig_code, s.cond.trig_edge ? "falling" : "rising",
                         (unsigned)s.cond.trig_src + 1u, (unsigned)s.spread);
    }
    usb_debug_printf("tested %lu  pass %lu  fail %lu  skipped %lu  missed %lu  max-consec-fail %lu\r\n",
                     (unsigned long)s.tested, (unsigned long)s.passed, (unsigned long)s.failed,
                     (unsigned long)s.skipped, (unsigned long)s.missed,
                     (unsigned long)s.max_consec_fail);
    const mask_pf_t *m = scope_mask_raw();
    if (s.skipped && m != NULL) {
        usb_send_str("skips:");
        for (unsigned r = 1; r < MASK_PF_R_COUNT; r++)
            if (m->skipped[r]) usb_debug_printf(" %s=%lu", mask_pf_reason_str((mask_pf_reason_t)r),
                                                (unsigned long)m->skipped[r]);
        usb_send_str("\r\n");
    }
    usb_debug_printf("last %s gen %lu", mask_verdict_str(s.last.verdict), (unsigned long)s.last.gen);
    if (s.last.verdict == MASK_PF_V_SKIP) {
        usb_debug_printf(" (%s", mask_pf_reason_str(s.last.reason));
        if (s.last.reason == MASK_PF_R_SETTINGS) mask_print_cond_diff(s.last.cond_diff);
        usb_send_str(")");
    }
    usb_send_str("\r\n");
    if (s.failed)
        usb_debug_printf("last FAIL gen %lu: %u samples out, first CH%d col %d, worst %d cnt\r\n",
                         (unsigned long)s.last_fail.gen, (unsigned)s.last_fail.violations,
                         (int)s.last_fail.first_ch + 1, (int)s.last_fail.first_col,
                         (int)s.last_fail.worst);
    usb_debug_printf("stop-on-fail %s%s\r\n", s.stop_on_fail ? "ON" : "OFF",
                     s.hold ? "  -- HOLDING a failed record (mask run / OK to resume)" : "");
    usb_debug_printf("move-focus %s\r\n", key_focus_name((key_focus_t)key_focus_get()));
    usb_debug_printf("heap free %lu (min ever %lu)\r\n", (unsigned long)xPortGetFreeHeapSize(),
                     (unsigned long)xPortGetMinimumEverFreeHeapSize());
}

/* `mask [status|teach [n] [ch1|ch2|both]|clear|reset|run|dump]`. teach,
 * clear, reset and run are posted with mask_post_wait; dump reads the
 * finished mask directly. Tolerances and stop-on-fail are mask.* properties. */
void cmd_mask(const char *args)
{
    while (*args == ' ') args++;
    if (*args == 0 || strncmp(args, "status", 6) == 0) {
        mask_print_status();
        return;
    }
    if (strncmp(args, "teach", 5) == 0) {
        const char *p = args + 5;
        uint32_t n = 0;
        uint8_t ch = 0;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            if (strncmp(p, "ch1", 3) == 0)       ch = 1;
            else if (strncmp(p, "ch2", 3) == 0)  ch = 2;
            else if (strncmp(p, "both", 4) == 0) ch = 3;
            else if (parse_int(p, &n) != 0 || n == 0 || n > MASK_PF_TEACH_MAX) {
                usb_debug_printf("usage: mask teach [1..%u] [ch1|ch2|both]\r\n", MASK_PF_TEACH_MAX);
                return;
            }
            while (*p && *p != ' ') p++;
        }
        if (!mask_post_wait(SCOPE_MASK_REQ_TEACH, ch, 0, (uint16_t)n)) return;
        const char *why = scope_mask_teach_refusal();
        if (why) { usb_debug_printf("teach REFUSED: %s\r\n", why); return; }
        mask_print_status();
        return;
    }
    if (strncmp(args, "clear", 5) == 0) {
        if (mask_post_wait(SCOPE_MASK_REQ_CLEAR, 0, 0, 0)) usb_send_str("mask cleared\r\n");
        return;
    }
    if (strncmp(args, "reset", 5) == 0) {
        if (mask_post_wait(SCOPE_MASK_REQ_RESET, 0, 0, 0)) mask_print_status();
        return;
    }
    if (strncmp(args, "run", 3) == 0) {
        if (mask_post_wait(SCOPE_MASK_REQ_RELEASE, 0, 0, 0)) usb_send_str("hold released\r\n");
        return;
    }
    if (strncmp(args, "dump", 4) == 0) {
        /* One line per column: col lo hi [lo2 hi2] [*] -- * = outside in the
         * last FAIL. Machine-readable for bench scripts. */
        const mask_pf_t *m = scope_mask_raw();
        if (m == NULL || m->state != MASK_PF_READY) { usb_send_str("no finished mask\r\n"); return; }
        usb_debug_printf("mask dump pre %d chans %u\r\n", (int)m->pre, (unsigned)m->chans);
        for (unsigned i = 0; i < MASK_PF_SPAN; i++) {
            uint8_t l1 = 0, h1 = 0, l2 = 0, h2 = 0;
            (void)mask_pf_bounds(m, 0, (uint16_t)i, &l1, &h1);   /* 0 0 = not in mask */
            (void)mask_pf_bounds(m, 1, (uint16_t)i, &l2, &h2);
            usb_debug_printf("%u %u %u %u %u%s%s\r\n", i, l1, h1, l2, h2,
                             mask_pf_fail_col(m, 0, (uint16_t)i) ? " *1" : "",
                             mask_pf_fail_col(m, 1, (uint16_t)i) ? " *2" : "");
        }
        usb_send_str("end\r\n");
        return;
    }
    usb_send_str("usage: mask [status|teach [n] [ch1|ch2|both]|clear|reset|run|dump]\r\n"
                 "  the tolerance and stop-on-fail are properties: set mask.tolv=<counts> mask.tolh=<samples>; "
                 "set mask.stopfail on|off\r\n");
}
