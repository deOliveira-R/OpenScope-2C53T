/*
 * OpenScope 2C53T - acquisition from the capture core (SPI3)
 *
 * The acquisition task and the record it publishes; the core's registers it
 * keeps (timebase, trigger level, source and edge); single-channel
 * interleave; roll mode; the pause handshake other SPI3 users take.
 * Shared items: fpga_internal.h.
 */

#include "fpga_internal.h"

fpga_acq_state_t acq;

/* The published record. Other files reach it through this file's functions,
 * under the generation count (fpga_acq_frame_generation). */
static volatile uint8_t acq_ch1_buf[FPGA_ADC_BUF_SIZE];
static volatile uint8_t acq_ch2_buf[FPGA_ADC_BUF_SIZE];
static volatile uint32_t acq_pc0_last_tick;  /* tick of the last PC0 edge */

TaskHandle_t      acq_task_handle = NULL;

static volatile bool data_ready = false;

void fpga_scope_delay_ms(uint32_t ms)
{
    if (ms == 0) return;

    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        vTaskDelay(pdMS_TO_TICKS(ms));
    } else {
        systick_delay_ms(ms);
    }
}

/* Pause handshake of the acquisition task (fpga_acq_pause). The requester
 * sets req, clears ack, then waits for a fresh ack. The task raises ack only
 * at its park point, never inside a CS frame, and raises it again on every
 * parked cycle. */
static volatile bool acq_pause_req = false;

static volatile bool acq_pause_ack = false;

/* ── Staged and published record ─────────────────────────────────────────
 * The SPI read lands in two staging buffers (heap, allocated in
 * fpga_create_tasks), never in the published acq_ch1_buf/ch2_buf that the
 * lower-priority display task reads. An accepted record is copied over in
 * fpga_acq_frames_commit, bracketed by acq_frame_gen: odd while a commit is
 * in progress. Readers that need exact numbers re-read until the generation
 * is even and unchanged. A rejected read never touches the published
 * buffers. If the staging allocation failed, reads land in the published
 * buffers directly and the generation never moves. */
static volatile uint32_t acq_frame_gen = 0;

uint8_t *acq_stage_ch1 = NULL;

uint8_t *acq_stage_ch2 = NULL;

uint32_t fpga_acq_frame_generation(void)
{
    return acq_frame_gen;
}

static volatile uint8_t *acq_write_ch1(void)
{
    return acq_stage_ch1 ? acq_stage_ch1 : acq_ch1_buf;
}

static volatile uint8_t *acq_write_ch2(void)
{
    return acq_stage_ch2 ? acq_stage_ch2 : acq_ch2_buf;
}

/* Whether the staged / published record is in time order (rotated at its
 * seam). Published inside the commit's generation window, so the flag always
 * describes the record in acq_ch1_buf. The same holds for every acq_pub_*
 * below. */
static volatile bool acq_stage_ordered = false;

static volatile bool acq_pub_ordered = false;

/* ...and is a triggered record: its trigger crossing is at index 512. */
static volatile bool acq_stage_ordered_core = false;

static volatile bool acq_pub_ordered_core = false;


/* Single-channel 250 MS/s (dsp/interleave.h): the staged / published CH1
 * record is the merge of both converters. Published with the record, like
 * `ordered`, so the rate a reader applies always matches the buffer. */
static volatile bool acq_stage_il = false;

static volatile bool acq_pub_il = false;

/* The reg 0x01 code the published record was captured at (0xFF: none yet).
 * acq_rate_idx follows the timebase keys even when stopped; the paused zoom
 * view and the measurements must use the held record's own rate. */
static volatile uint8_t acq_pub_idx = 0xFF;

static volatile uint8_t acq_rate_idx;       /* defined (initialised) below */

/* What the published record is (trig_acq_t, trig_policy.h): triggered, or
 * AUTO's forced record. Staged and published with the record, so the badge
 * always describes the record on screen. */
static volatile uint8_t  acq_stage_kind = TRIG_ACQ_HOLD;

static volatile uint8_t  acq_pub_kind = TRIG_ACQ_HOLD;

static volatile uint32_t acq_last_trig_tick = 0;     /* tick the last triggered record was published */

/* SINGLE one-shot: the acq task's single_done, published, and the re-arm
 * request the UI raises (OK / the shell), consumed by the task. */
static volatile bool     acq_single_done_pub = false;

static volatile uint32_t acq_single_arm_req = 0;

/* The capture configuration (dsp/acq_cfg.h) of the staged record, set when
 * the task admits a read, and of the published one. Mask 0 = no record
 * committed yet: every channel then counts as present. */
static acq_cfg_t acq_stage_cfg;

static acq_cfg_t acq_pub_cfg;

static volatile uint32_t acq_cfg_drops;      /* records refused: configuration not settled */

static volatile uint16_t acq_cfg_settle_cmd;  /* 0 = ACQ_CFG_SETTLE_MS, 0xFFFF = rule off, else ms */

void fpga_acq_cfg_snapshot(acq_cfg_t *c);    /* defined with the registers it reads, below */

static void fpga_acq_frames_commit(void)
{
    if (acq_stage_ch1 == NULL || acq_stage_ch2 == NULL)
        return;   /* in-place fallback: nothing to copy */
    acq_frame_gen++;                                   /* odd: committing */
    memcpy((void *)acq_ch1_buf, acq_stage_ch1, FPGA_ADC_BUF_SIZE);
    memcpy((void *)acq_ch2_buf, acq_stage_ch2, FPGA_ADC_BUF_SIZE);
    acq_pub_ordered = acq_stage_ordered;
    acq_pub_ordered_core = acq_stage_ordered_core;
    acq_pub_il = acq_stage_il;
    acq_pub_idx = acq_rate_idx;
    acq_pub_kind = acq_stage_kind;
    acq_pub_cfg = acq_stage_cfg;
    acq_frame_gen++;                                   /* even: stable */
}

/* AUTO's wait for a trigger, set from the shell (`fpga autowait <ms>`).
 * 0 = derived from the timebase (fpga_acq_auto_wait_get). */
static volatile uint16_t acq_auto_wait_ms = 0;

/* The trigger level in force: reg 0x08, a raw ADC code. The boot arm burst
 * writes 0xAD (the value captured from stock), above most signals;
 * fpga_reconcile_trigger_after_arm() then sets the UI's level. At runtime
 * fpga_apply_trigger_level() changes it; the core watch and the task's
 * capture restart only write the same value again. */
static volatile uint8_t acq_trig_code = 0xAD;

/* Every register write drops the core's capture in flight. The apply entry
 * points raise this; the task then clears capture_in_flight and restarts the
 * configuration's settle window. */
volatile bool acq_reprime_req = false;

/* acq_design_ours: the design running now was loaded by this firmware (set
 * by the upload in fpga_boot.c). acq_edge_reg: what edge register 0x07 holds
 * (every arm burst writes 0x00); the task rewrites it whenever it differs
 * from the UI's edge (dsp/trig_edge_reg.h). */
volatile bool    acq_design_ours = false;

/* The design in the FPGA is OpenScope's capture core: this firmware loaded
 * it, it identified itself and has every capability the firmware needs
 * (core_ident_usable). Without that nothing is acquired: the task idles and
 * the badge says NoCore. There is no other design this firmware can drive. */
static volatile bool    acq_core_ok     = false;

bool fpga_acq_core_ok(void) { return acq_core_ok; }

/* Record kind taken from the op 05 header (our core): [0] held, [1] not held,
 * [2] held but no PC0 edge seen, [3] PC0 edge but not held. */
static volatile uint32_t acq_kind_n[4];

/* The core watch: its status block (op E0, dsp/core_status.h), read between
 * pairs by fpga_core_watch_poll(). */
static fpga_core_watch_t acq_core_watch = { .check_on = true };

static uint8_t           acq_core_watch_prev_diff;

fpga_core_watch_t fpga_core_watch(void) { return acq_core_watch; }

void fpga_core_watch_fix_set(bool on)   { acq_core_watch.check_on = on; acq_core_watch_prev_diff = 0u; }

/* AUTO without a trigger (op 09, dsp/trig_policy.h): [0] requests sent, [1]
 * forced records read, [2] reads after a request that found no record yet
 * (the memory was not all new since the last restart). */
static volatile uint32_t acq_force_n[3];

uint32_t fpga_acq_force_count(unsigned i) { return i < 3u ? acq_force_n[i] : 0u; }

static volatile uint32_t acq_fresh_restarts;     /* captures restarted before a first read (trig_fresh_restart) */

uint32_t fpga_acq_fresh_restarts(void) { return acq_fresh_restarts; }

uint32_t fpga_acq_kind_count(unsigned i) { return i < 4u ? acq_kind_n[i] : 0u; }

void acq_core_ok_set(bool ok) { acq_core_ok = ok; }

/* Register writes asked for while no usable core was loaded (not sent). */
static volatile uint32_t acq_reg_writes_refused = 0;

uint32_t fpga_acq_reg_writes_refused(void) { return acq_reg_writes_refused; }

/* The firmware's own constants against the capture core's package (drivers/fpga_core_regs.h, generated):
 * a core with another record length, trigger position or timebase list does not build into this firmware. */
_Static_assert(TRIG_EDGE_REC_N == (1u << FPGA_CORE_ADDR_W), "record length differs from the core's");

_Static_assert(TRIG_EDGE_TRIG_POS == FPGA_CORE_TRIG_INDEX, "trigger position differs from the core's");

_Static_assert(SCOPE_TIMEBASE_CODE_COUNT == FPGA_CORE_TB_CODES, "timebase code count differs from the core's");

volatile uint8_t acq_edge_reg    = 0x00;

uint8_t fpga_acq_edge_reg_get(void) { return acq_edge_reg; }

/* Single-channel interleave: with channel mask 1 both converters sample CH1
 * half a sample apart, and at timebase codes <= 3 (stock's gate) the task
 * merges them into one 250 MS/s record. Runtime switch, default on. The
 * converter-B correction (level map and timing skew, dsp/interleave.h)
 * starts as identity; a unit's own numbers come from `fpga interleave cal`,
 * `skew measure` or a saved user-cal record. */
static volatile bool acq_il_enable = true;

static interleave_corr_t acq_il_corr = { .gain = 1.0f, .offset = 0.0f };

/* Self-calibration collector for `fpga interleave cal`. While `on`, every
 * pair the task reads (op 04 = converter A, op 05 = B, record units) adds
 * the means of its centre 512 samples, the window the merge uses. DC only:
 * a flat input needs no common moment, so reads that found no held record
 * count too. The shell side owns the DAC and the timing. */
static struct {
    volatile bool on;
    volatile uint8_t skip;          /* records to drop after begin (pipeline) */
    volatile uint8_t n;
    volatile uint8_t p2p_a, p2p_b;
    volatile bool clipped;
    float sum_a, sum_b, amin, amax, bmin, bmax;   /* of the per-record means */
} acq_ilcal;

static volatile uint16_t acq_poll_gap_ms = ACQ_POLL_GAP_MS;   /* dsp/acq_timing.h; `fpga pollgap` overrides */

static volatile bool     acq_unrotate = true;    /* put held records in time order; `fpga unrotate off` keeps them raw */


/* Held records put in time order by the seam in the core's reply header
 * (`fpga unrotate`): [0] ordered, [1] a held record without a seam (never
 * expected: the op 05 header of a pair always carries it). */
static volatile uint32_t acq_seam_n[2];

/* An explicit SPI3 clock for the task's reads (0..7 = /2 to /256; 0xFF =
 * leave whatever is set). `fpga acqbr`. */
static volatile uint8_t  acq_read_br = 0xFF;

/* The reg 0x01 code in force: the one variable that mirrors the hardware
 * register, set where the register is written (fpga_apply_timebase,
 * fpga_apply_roll, the reconcile after an arm burst). 0x08 is what the arm
 * burst writes. */
static volatile uint8_t acq_rate_idx     = 0x08;

void fpga_acq_auto_wait_set(uint16_t ms)  { acq_auto_wait_ms = ms; }

/* AUTO's wait for a trigger in force: the shell override if set, else
 * derived from the timebase in force (a few fills plus a margin, clamped;
 * the numbers and their origins are in dsp/acq_timing.h). A code with no
 * known rate gets ACQ_AUTO_MIN_MS (dsp/acq_timing.h). */
uint16_t fpga_acq_auto_wait_get(void)
{
    if (acq_auto_wait_ms) return acq_auto_wait_ms;
    return acq_time_auto_wait_ms(scope_timebase_sample_rate(acq_rate_idx), (uint16_t)ACQ_AUTO_MIN_MS);
}

bool fpga_acq_auto_wait_is_override(void) { return acq_auto_wait_ms != 0; }

/* The wait after a record before the next poll, derived from the timebase in
 * force. */
uint16_t fpga_acq_post_edge_get(void)
{
    /* One fill, never less than the poll gap: dsp/acq_timing.h. */
    return acq_time_post_record_ms(scope_timebase_sample_rate(acq_rate_idx), acq_poll_gap_ms);
}

void     fpga_acq_poll_gap_set(uint16_t ms)  { acq_poll_gap_ms = ms ? ms : ACQ_POLL_GAP_MS; }

uint16_t fpga_acq_poll_gap_get(void)         { return acq_poll_gap_ms; }

void fpga_acq_unrotate_set(bool on)           { acq_unrotate = on; }

bool fpga_acq_unrotate_get(void)              { return acq_unrotate; }

bool fpga_acq_record_time_ordered(void)       { return acq_pub_ordered; }

bool fpga_acq_record_core_ordered(void)       { return acq_pub_ordered_core; }

bool fpga_acq_design_ours(void)               { return acq_design_ours; }

/* `fpga diag clear`: zero the record counters and the raw-byte samples. */
void fpga_acq_counters_reset(void)
{
    taskENTER_CRITICAL();
    acq.spi3_ok_count = 0;
    acq.spi3_total_timeouts = 0;
    memset((void *)acq.diag_ch1_raw, 0, sizeof(acq.diag_ch1_raw));
    memset((void *)acq.diag_ch2_raw, 0, sizeof(acq.diag_ch2_raw));
    taskEXIT_CRITICAL();
}

core_ident_t fpga_core_ident(void)
{
    uint8_t r[CORE_IDENT_REPLY_LEN];
    for (unsigned i = 0; i < CORE_IDENT_REPLY_LEN; i++) r[i] = fboot.core_ident[i];
    return core_ident_parse(r);
}

uint32_t fpga_acq_seam_count(unsigned i)      { return i < 2u ? acq_seam_n[i] : 0u; }

/* Put a held record into time order. The core's ring is read out from
 * address 0, so the oldest sample sits at the seam s (the write pointer when
 * the record was held) and a trigger crossing 512 samples after it. The core
 * reports s in the reply header of the two reads. Both channels share it and
 * are rotated left by s: index 0 becomes the oldest sample and the trigger
 * lands at index 512. Reads with no held record are left alone.
 *
 * Returns the seam (0..1023), or -1 when there is none. With unrotation off
 * (`fpga unrotate off`) the seam is returned but not applied. */
static int32_t fpga_acq_unrotate_staging(bool triggered)
{
    if (!triggered) return -1;
    volatile uint8_t *bufs[2] = { acq_write_ch1(), acq_write_ch2() };
    uint32_t k = 0;
    /* The op 05 header first: it always carries the pair's seam. The op 04
     * one is withheld when the record was held only after that frame began. */
    int hs = trig_seam_from_hdr(acq.acq_hdr_ch2[FPGA_CORE_SEAM_I_HI], acq.acq_hdr_ch2[FPGA_CORE_SEAM_I_LO]);
    if (hs < 0)
        hs = trig_seam_from_hdr(acq.acq_hdr_ch1[FPGA_CORE_SEAM_I_HI], acq.acq_hdr_ch1[FPGA_CORE_SEAM_I_LO]);
    if (hs < 0) {                                /* a held record always carries it: not reached */
        acq_seam_n[1]++;
        return -1;
    }
    k = (uint32_t)hs;
    acq_seam_n[0]++;
    if (!acq_unrotate) return (int32_t)k;        /* reported (status), not applied */
    if (k == 0) return 0;
    /* In place: index 0 becomes the old index k. */
    for (int c = 0; c < 2; c++)
        trig_edge_rotate_left(bufs[c], FPGA_ADC_BUF_SIZE, k);
    return (int32_t)k;
}

void fpga_acq_read_br_set(uint8_t br)     { acq_read_br = br; }

uint8_t fpga_acq_read_br_get(void)      { return acq_read_br; }

uint8_t fpga_acq_trig_code_get(void)      { return acq_trig_code; }

/* UI trigger level (-100..100, screen pixels about mid-scale) to a reg 0x08
 * code. The renderer's convention: SCOPE_H pixels = 256 counts, SCOPE_H =
 * LCD_HEIGHT - 16 - 18 = 206 in scope_ui.c. Keep the 206 in step with it. */
uint8_t fpga_trigger_code_from_level(int level)
{
    if (level < -100) level = -100;
    if (level >  100) level =  100;
    int code = 128 + (level * 256) / 206;
    if (code < 1)   code = 1;
    if (code > 254) code = 254;
    return (uint8_t)code;
}

uint8_t fpga_acq_rate_idx_get(void)   { return acq_rate_idx; }

void fpga_interleave_set(bool on) { acq_il_enable = on; acq_reprime_req = true; }

bool fpga_interleave_get(void)    { return acq_il_enable; }

bool fpga_interleave_active(void)
{
    return acq_il_enable && scope_frontend_channel_mask() == 1u && acq_rate_idx <= 3u;
}

/* A gain/offset line also clears any knots: it is a complete level map of
 * its own (a v1 user-cal record, `cal user clear`). The skew is separate and
 * stays. */
void fpga_interleave_corr_set(float gain, float offset)
{
    interleave_corr_set_knots(&acq_il_corr, 0, NULL, NULL);
    acq_il_corr.gain = gain;
    acq_il_corr.offset = offset;
}

bool fpga_interleave_knots_set(uint8_t n, const int16_t *a, const int16_t *b)
{
    return interleave_corr_set_knots(&acq_il_corr, n, a, b);
}

uint8_t fpga_interleave_knots_get(int16_t *a, int16_t *b)
{
    const uint8_t n = acq_il_corr.n_knots;
    for (uint8_t i = 0; i < n && i < IL_KNOTS_MAX; i++) {
        if (a) a[i] = acq_il_corr.knot_a[i];
        if (b) b[i] = acq_il_corr.knot_b[i];
    }
    return n;
}

bool fpga_interleave_skew_set(int32_t ppm) { return interleave_skew_set(&acq_il_corr, (ppm_period_t){ ppm }); }

int32_t fpga_interleave_skew_get(void)     { return acq_il_corr.skew.ppm; }

void fpga_interleave_corr_get(float *gain, float *offset)
{
    if (gain)   *gain = acq_il_corr.gain;
    if (offset) *offset = acq_il_corr.offset;
}

/* Start the collector; the first `skip` records (reads already under way)
 * are dropped. */
void fpga_ilcal_begin(uint8_t skip)
{
    acq_ilcal.on = false;
    acq_ilcal.skip = skip; acq_ilcal.n = 0;
    acq_ilcal.p2p_a = acq_ilcal.p2p_b = 0;
    acq_ilcal.clipped = false;
    acq_ilcal.sum_a = acq_ilcal.sum_b = 0.0f;
    acq_ilcal.amin = acq_ilcal.bmin = 1e9f;
    acq_ilcal.amax = acq_ilcal.bmax = -1e9f;
    acq_ilcal.on = true;
}

/* Wait up to timeout_ms for `records` records, stop the collector and
 * report. False when too few came. */
bool fpga_ilcal_collect(uint8_t records, uint32_t timeout_ms, il_cal_point_t *out)
{
    const TickType_t t0 = xTaskGetTickCount();
    while (acq_ilcal.n < records) {
        if ((xTaskGetTickCount() - t0) >= pdMS_TO_TICKS(timeout_ms)) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    acq_ilcal.on = false;
    __asm volatile ("" ::: "memory");   /* read the sums only after the feed is off */
    if (acq_ilcal.n < records) return false;
    const float n = (float)acq_ilcal.n;
    out->a_mean = acq_ilcal.sum_a / n;
    out->b_mean = acq_ilcal.sum_b / n;
    out->a_p2p = acq_ilcal.p2p_a;
    out->b_p2p = acq_ilcal.p2p_b;
    out->a_drift = acq_ilcal.amax - acq_ilcal.amin;
    out->b_drift = acq_ilcal.bmax - acq_ilcal.bmin;
    out->clipped = acq_ilcal.clipped;
    return true;
}

/* One converter's window, robust to isolated glitch samples. On unit #2 a
 * quiet input at timebase codes <= 3 reads +-5 counts plus interference
 * bursts of 10-40 samples (about 5 % of a record; bench 2026-11-11), which a
 * plain peak-to-peak would call not flat. Pass 1 takes the mean; pass 2
 * drops samples more than ILCAL_GLITCH counts from it and measures mean,
 * spread and rails on the rest. More than ILCAL_GLITCH_MAX dropped samples
 * is a signal, not glitches: the spread is then reported as 255 (not flat). */
#define ILCAL_GLITCH      24

#define ILCAL_GLITCH_MAX  77u   /* of 512: 15 %. A real signal leaves far more
                                 * than 15 % outside +-24 codes of its mean. */
static void ilcal_window(const volatile uint8_t *w, float *mean, uint8_t *spread, bool *clip)
{
    uint32_t s = 0;
    for (unsigned i = INTERLEAVE_WIN_LO; i < INTERLEAVE_WIN_LO + 512u; i++) s += w[i];
    const int m = (int)((s + 256u) / 512u);
    uint32_t s2 = 0, n = 0;
    uint8_t mn = 255, mx = 0;
    for (unsigned i = INTERLEAVE_WIN_LO; i < INTERLEAVE_WIN_LO + 512u; i++) {
        const uint8_t x = w[i];
        const int d = (int)x - m;
        if (d > ILCAL_GLITCH || d < -ILCAL_GLITCH) continue;
        s2 += x; n++;
        if (x < mn) mn = x;
        if (x > mx) mx = x;
    }
    /* Rails in record units: fpga_acq_read_channel stores raw + FPGA_ADC_OFFSET
     * (-28) clamped, so raw 255 reads 227 and anything at raw <= 28 reads 0. */
    const uint8_t rail_hi = (uint8_t)(255 + (int)FPGA_ADC_OFFSET);
    *mean   = n ? (float)s2 / (float)n : (float)m;
    *spread = (512u - n > ILCAL_GLITCH_MAX || n == 0) ? 255u : (uint8_t)(mx - mn);
    *clip   = (n == 0) || mn == 0 || mx >= rail_hi;
}

/* Called by the task right after the op 04 / op 05 reads; `ok` = no SPI3
 * hardware timeout during them. Cheap when off. */
static void acq_ilcal_feed(const volatile uint8_t *a, const volatile uint8_t *b, bool ok)
{
    if (!acq_ilcal.on || !ok) return;
    if (acq_ilcal.skip) { acq_ilcal.skip--; return; }
    float ma, mb; uint8_t pa, pb; bool ca, cb;
    ilcal_window(a, &ma, &pa, &ca);
    ilcal_window(b, &mb, &pb, &cb);
    acq_ilcal.sum_a += ma; acq_ilcal.sum_b += mb;
    if (ma < acq_ilcal.amin) acq_ilcal.amin = ma;
    if (ma > acq_ilcal.amax) acq_ilcal.amax = ma;
    if (mb < acq_ilcal.bmin) acq_ilcal.bmin = mb;
    if (mb > acq_ilcal.bmax) acq_ilcal.bmax = mb;
    if (pa > acq_ilcal.p2p_a) acq_ilcal.p2p_a = pa;
    if (pb > acq_ilcal.p2p_b) acq_ilcal.p2p_b = pb;
    if (ca || cb) acq_ilcal.clipped = true;
    __asm volatile ("" ::: "memory");   /* sums land before the count the shell polls */
    acq_ilcal.n++;
}

bool fpga_acq_record_interleaved(void) { return acq_pub_il; }

uint8_t fpga_acq_record_kind(void)      { return acq_pub_kind; }

bool fpga_acq_single_done(void)         { return acq_single_done_pub; }

void fpga_acq_single_rearm(void)        { acq_single_arm_req++; }

/* The trigger status the badge and the shell show (trig_policy.h). "Trig'd"
 * holds while the last triggered record is younger than
 * acq_time_trigd_stale_ms (dsp/acq_timing.h), so it does not flicker between
 * records at slow timebases. */
uint8_t fpga_trigger_status(void)
{
    const scope_state_t *ss = scope_state_get();
    if (!acq_core_ok)
        return (uint8_t)TRIG_ST_NOCORE;          /* nothing is acquired: say so, whatever the mode */
    const uint32_t stale = acq_time_trigd_stale_ms(fpga_acq_post_edge_get(), fpga_acq_auto_wait_get());
    return (uint8_t)trig_status_eval((uint8_t)ss->trigger.mode, ss->running,
                                     acq_single_done_pub, (trig_acq_t)acq_pub_kind,
                                     xTaskGetTickCount() - acq_last_trig_tick, stale);
}

/* Trigger source: reg 0x06, 0 = CH1, 1 = CH2. Like every register write it
 * drops the capture in flight. */
static void fpga_scope_write_reg(uint8_t reg, uint8_t val);   /* defined below */

static volatile uint8_t acq_trig_src_code = 0x00;

uint8_t fpga_acq_trig_src_code_get(void) { return acq_trig_src_code; }

bool fpga_apply_trigger_source(void)
{
    const scope_state_t *ss = scope_state_get();
    uint8_t code = (ss->trigger.source == TRIG_SRC_CH2) ? 0x01u : 0x00u;
    if (!fpga_acq_pause())
        return false;
    fpga_scope_write_reg(FPGA_CORE_OP_SOURCE, code);
    acq_trig_src_code = code;
    acq_reprime_req = true;
    fpga_acq_resume();
    return true;
}

/* Rate of the PUBLISHED record: the timebase table's per-converter rate,
 * doubled when the record is the two-converter merge. 0 = no rate. */
float fpga_record_sample_rate(void)
{
    const uint8_t idx = (acq_pub_idx == 0xFFu) ? acq_rate_idx : acq_pub_idx;
    const float fs = scope_timebase_sample_rate(idx);
    return acq_pub_il ? 2.0f * fs : fs;
}


/* One CS-framed register write to the core: opcode byte, value byte, the
 * same frame as the boot arm burst (fpga_boot.c). Callers hold the bus: the
 * task itself, or code that parked it with fpga_acq_pause(). */
static void fpga_scope_write_reg(uint8_t reg, uint8_t val)
{
    /* The opcodes are our core's. A design that is not a usable core of ours
     * (acq_core_ok false: NoCore) is sent nothing; the caller's copy of the
     * setting still changes, so the value is in force once a core is loaded. */
    if (!acq_core_ok) { acq_reg_writes_refused++; return; }
    SPI3_CS_ASSERT();
    spi3_xfer(reg);
    spi3_xfer(val);
    SPI3_CS_DEASSERT();
}

/* Roll mode state. acq_roll_on is the acquisition task's path switch; it
 * changes only inside a park (fpga_apply_roll, fpga_apply_timebase), so
 * the task never sees a reg-0x01 code and a read path that disagree. */
static scope_roll_fifo_t acq_roll_fifo;

static volatile bool     acq_roll_on = false;

/* The capture configuration in force now (dsp/acq_cfg.h): what the hardware
 * was last set to (routing, reg 0x01/0x06/0x08, interleave, roll) and the
 * scope state the record's scale depends on. Any task; every field is one
 * aligned read. */
void fpga_acq_cfg_snapshot(acq_cfg_t *c)
{
    const scope_state_t *ss = scope_state_get();
    memset(c, 0, sizeof *c);
    c->mask      = scope_frontend_channel_mask();
    c->rate_idx  = acq_rate_idx;
    c->trig_code = acq_trig_code;
    c->trig_src  = acq_trig_src_code;
    c->il        = fpga_interleave_active() ? 1u : 0u;
    c->roll      = acq_roll_on ? 1u : 0u;
    if (ss != NULL) {
        c->vdiv[0] = ss->ch1.vdiv_idx;            c->vdiv[1] = ss->ch2.vdiv_idx;
        c->coupling[0] = (uint8_t)ss->ch1.coupling; c->coupling[1] = (uint8_t)ss->ch2.coupling;
        c->probe[0] = (uint8_t)ss->ch1.probe;     c->probe[1] = (uint8_t)ss->ch2.probe;
        c->pos[0] = ss->ch1.position;             c->pos[1] = ss->ch2.position;
        c->trig_edge = (uint8_t)ss->trigger.edge;
        c->trig_mode = (uint8_t)ss->trigger.mode;
        c->trig_level = ss->trigger.level;
    }
}

void fpga_acq_record_cfg(acq_cfg_t *out) { *out = acq_pub_cfg; }   /* read it inside the generation check */

const acq_cfg_t *fpga_acq_record_cfg_ptr(void) { return &acq_pub_cfg; }

void fpga_acq_cfg_settle_set(uint16_t cmd) { acq_cfg_settle_cmd = cmd; }

uint16_t fpga_acq_cfg_settle_get(void)     { return acq_cfg_settle_cmd; }

/* Whether the published record holds channel ch (0 = CH1, 1 = CH2). */
bool fpga_acq_record_has_ch(uint8_t ch)
{
    return acq_pub_cfg.mask == 0u ? ch <= 1u : acq_cfg_has_ch(&acq_pub_cfg, ch);
}

uint32_t fpga_acq_cfg_drops(void)        { return acq_cfg_drops; }

static volatile uint8_t  acq_roll_step = 0;

static int               acq_roll_code_override = -1;

static volatile fpga_roll_stats_t acq_roll_stats;

/* Write a timebase code to the core now and record it as the code in force. The task is parked first: a CS frame interleaved with its
 * reads corrupts both. If it will not park, nothing is written and false is
 * returned; the old code stays in force and the label still matches it. */
bool fpga_apply_timebase(uint8_t code)
{
    if (!fpga_acq_pause())
        return false;
    fpga_scope_write_reg(FPGA_CORE_OP_TIMEBASE, code);
    acq_rate_idx = code;
    acq_roll_on = false;          /* a Y-T code is in force: back to records */
    /* The timebase picks the calibration bank (calpage_bank_select: code
     * >= 5 bank 0; 4 bank 1; <= 3 bank 1 with both channels, else 2), so
     * crossing 4/5 moves the offset references. DAC and timer writes only,
     * no SPI3, so inside or outside the park is equivalent. */
    scope_vpos_apply();
    acq_reprime_req = true;
    fpga_acq_resume();
    return true;
}

/* Roll on: write the roll step's timebase code (stock's codes
 * 0x14..0x1C, scope_roll.h; our core runs every code above 0x14 as 0x14)
 * unless the shell overrides it. Roll off: the Y-T code. Roll points are
 * op 03 live reads paced by the firmware, so the code does not set their
 * spacing. The point FIFO is cleared on every change: points taken at the
 * old spacing would be drawn against the new time axis. */
bool fpga_apply_roll(void)
{
    scope_state_t *ss = scope_state_get();
    const bool on = ss->roll;
    const uint8_t step = (ss->roll_step < SCOPE_ROLL_STEPS) ? ss->roll_step
                                                            : (uint8_t)(SCOPE_ROLL_STEPS - 1u);
    uint8_t code;
    if (on)
        code = (acq_roll_code_override >= 0) ? (uint8_t)acq_roll_code_override
                                             : scope_roll_stock_code(step);
    else
        code = ss->timebase_idx;
    if (!fpga_acq_pause())
        return false;
    fpga_scope_write_reg(FPGA_CORE_OP_TIMEBASE, code);
    acq_rate_idx = code;
    acq_roll_step = step;
    acq_roll_stats.code = code;
    acq_roll_stats.step = step;
    scope_roll_fifo_reset(&acq_roll_fifo);
    acq_roll_on = on;
    /* The calibration bank follows the code (>= 5: bank 0), so a roll code
     * keeps the offset references of the slow Y-T codes. */
    scope_vpos_apply();
    acq_reprime_req = true;
    fpga_acq_resume();
    return true;
}

bool fpga_roll_active(void)                     { return acq_roll_on; }

const scope_roll_fifo_t *fpga_roll_fifo(void)   { return &acq_roll_fifo; }

void fpga_roll_code_override_set(int code)
{
    acq_roll_code_override = (code >= 0 && code <= 0xFF) ? code : -1;
}

int  fpga_roll_code_override_get(void)          { return acq_roll_code_override; }

void fpga_roll_stats_get(fpga_roll_stats_t *out)
{
    if (out == NULL)
        return;
    taskENTER_CRITICAL();
    out->reads   = acq_roll_stats.reads;
    out->rejects = acq_roll_stats.rejects;
    out->late    = acq_roll_stats.late;
    out->code    = acq_roll_stats.code;
    out->step    = acq_roll_stats.step;
    out->trig_hits = acq_roll_stats.trig_hits;
    for (unsigned i = 0; i < 5u; i++)
        out->rx[i] = acq_roll_stats.rx[i];
    taskEXIT_CRITICAL();
}

/* Write the trigger level (reg 0x08, a raw ADC code made from
 * the UI's -100..100) and record it as in force. The core compares each
 * stored sample of the source converter against it. Returns false, with
 * nothing written, if the task would not park. */
bool fpga_apply_trigger_level(int level)
{
    uint8_t code = fpga_trigger_code_from_level(level);
    if (!fpga_acq_pause())
        return false;
    fpga_scope_write_reg(FPGA_CORE_OP_LEVEL, code);
    acq_trig_code = code;
    acq_reprime_req = true;
    fpga_acq_resume();
    return true;
}

/* PC0 falling-edge counter. The core drives PC0 low for about 8 us when a
 * held record starts going out (FPGA_CORE_READY_PULSE_CYCLES); a polled read
 * could miss that, EXINT0 does not. The task uses it only for bench
 * statistics (acq_kind_n, the latency). Its one FreeRTOS call is a FromISR one,
 * so the priority (6, set in fpga.c) must stay within FreeRTOS's limit for
 * such calls. */
void EXINT0_IRQHandler(void)
{
    exint_flag_clear(EXINT_LINE_0);
    acq.pc0_edges++;
    acq_pc0_last_tick = xTaskGetTickCountFromISR();
}

/* ─── Reading one record ─────────────────────────────────────────────
 * One CS-low frame per converter, 1027 bytes (the core's cmd.sv): while the
 * opcode goes out (0x04 = converter A, 0x05 = B) the core sends reply byte 0,
 * 0x80 on every reply (FPGA_CORE_REPLY_FIRST); bytes 1-2 are the seam header,
 * kept in acq.acq_hdr_ch1/ch2; then 1024 samples from memory address 0.
 * Each sample is stored as raw + FPGA_ADC_OFFSET, clamped to 0..255.
 * Keep the frame whole: a 2-byte frame starting 0x05 would match the Gowin
 * configuration port's ERASE_SRAM command. */
static void fpga_acq_read_channel(uint8_t opcode, volatile uint8_t *buf)
{
    SPI3_CS_ASSERT();
    uint8_t s0 = spi3_xfer(opcode);        /* reply byte 0: 0x80 from a live core */
    uint8_t h1 = spi3_xfer(0xFF);          /* byte 1: seam flags and bits 9:8 */
    uint8_t h2 = spi3_xfer(0xFF);          /* byte 2: seam bits 7:0 */
    {
        volatile uint8_t *hdr = (opcode == FPGA_CORE_OP_REC_A) ? acq.acq_hdr_ch1
                                                 : acq.acq_hdr_ch2;
        hdr[0] = s0; hdr[1] = h1; hdr[2] = h2;
    }

    for (int i = 0; i < 1024; i++) {
        uint8_t raw = spi3_xfer(0xFF);
        if (i < 4) {
            if (opcode == FPGA_CORE_OP_REC_A) acq.diag_ch1_raw[i] = raw;
            else                acq.diag_ch2_raw[i] = raw;
        }
        int16_t cal = (int16_t)raw + (int16_t)FPGA_ADC_OFFSET;
        if (cal < 0)   cal = 0;
        if (cal > 255) cal = 255;
        buf[i] = (uint8_t)cal;
    }
    SPI3_CS_DEASSERT();
}

/* ─── Roll point reader ──────────────────────────────────────────────
 * One op 03 frame per point: reply byte 2 is converter A now, byte 4 is B
 * (fpga_core_regs.h). Paced from the RTOS tick at the roll step's period
 * (scope_roll.h; every stock period is a whole number of ms). All five reply
 * bytes are kept for the shell.
 *
 * Returns to the task loop at least every 10 ms, so the park point still
 * acks within fpga_acq_pause()'s budget at 800 ms per point. STOP
 * (ss->running false) reads nothing and freezes the FIFO. A tick that falls
 * more than a period behind (a park, a STOP) resynchronises instead of
 * bursting points that were never sampled on time. */
static uint32_t roll_next_tick;

static bool     roll_tick_armed = false;

static void fpga_roll_service(void)
{
    const scope_state_t *ss = scope_state_get();
    const uint32_t period = pdMS_TO_TICKS(scope_roll_period_ms(acq_roll_step).ms);
    if (ss == NULL || !ss->running || period == 0u) {
        roll_tick_armed = false;
        vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }
    uint32_t now = xTaskGetTickCount();
    if (!roll_tick_armed) {
        roll_next_tick = now;
        roll_tick_armed = true;
    }
    int32_t wait = (int32_t)(roll_next_tick - now);
    if (wait > 0) {
        vTaskDelay((TickType_t)(wait > 10 ? 10 : wait));
        return;
    }
    if ((uint32_t)(-wait) > period) {
        roll_next_tick = now;
        acq_roll_stats.late++;
    }
    roll_next_tick += period;

    if (acq_read_br != 0xFF)
        spi3_set_br(acq_read_br);
    uint16_t hw_to_before = acq.spi3_hw_timeouts;
    uint8_t rx[5];
    SPI3_CS_ASSERT();
    rx[0] = spi3_xfer(FPGA_CORE_OP_LIVE);
    for (unsigned i = 1; i < 5u; i++)
        rx[i] = spi3_xfer(0xFF);
    SPI3_CS_DEASSERT();
    for (unsigned i = 0; i < 5u; i++)
        acq_roll_stats.rx[i] = rx[i];

    if (acq.spi3_hw_timeouts != hw_to_before) {
        acq_roll_stats.rejects++;        /* a fabricated 0xFF is not a point */
        acq.spi3_total_timeouts++;
        return;
    }
    /* Record codes, the Y-T read's domain (raw + FPGA_ADC_OFFSET); the unit's
     * amplitude correction g = (m-28)/150 is applied in volts, exactly where
     * the Y-T trace gets it (scope_roll.h). */
    scope_roll_fifo_push(&acq_roll_fifo,
                         scope_roll_record_code((adc_raw_t){ rx[2] }),
                         scope_roll_record_code((adc_raw_t){ rx[4] }));
    acq_roll_stats.reads++;
    acq.spi3_ok_count++;                /* the display's new-data signal */

    /* Roll trigger, as stock's (scope_roll.h): once the FIFO is full and the
     * mode is not AUTO, one crossing check at its centre after every point.
     * SINGLE stops (running = false) and the full FIFO is the frozen frame;
     * the OK key re-arms through the Y-T path's own `!running` test. NORMAL
     * empties the FIFO: the trace refills from the right and the check
     * re-arms 301 points later. The level is the register in force (raw)
     * lifted into the record domain, like every comparison against a record.
     * The column shift is 0: ss->trig_x places the Y-T trigger point and has
     * no roll meaning. */
    acq_single_done_pub = false;         /* running again = armed */
    {
        scope_state_t *sw = scope_state_get();
        if (sw != NULL && sw->trigger.mode != TRIG_AUTO) {
            int lv = (int)acq_trig_code + (int)FPGA_ADC_OFFSET;
            if (lv < 0)   lv = 0;
            if (lv > 255) lv = 255;
            const scope_roll_trig_act_t act = scope_roll_trig_step(
                &acq_roll_fifo, (uint8_t)sw->trigger.mode,
                (sw->trigger.source == TRIG_SRC_CH2) ? 1u : 0u, 0,
                (rec_code_t){ (uint8_t)lv },
                (sw->trigger.edge == TRIG_FALLING) ? SCOPE_ROLL_EDGE_FALLING
                                                   : SCOPE_ROLL_EDGE_RISING);
            if (act != SCOPE_ROLL_TRIG_NONE) {
                acq_roll_stats.trig_hits++;
                if (act == SCOPE_ROLL_TRIG_STOP) {
                    sw->running = false;
                    acq_single_done_pub = true;
                }
            }
        }
    }
}

/* The task's capture-configuration tracker (dsp/acq_cfg.h): static, not on
 * the task's 256-word stack. */
static acq_cfg_track_t s_acq_cfg_trk;

/* One reading of the core's status block (op E0), in the task. A register
 * that differs from the firmware's copy in two successive valid readings is
 * rewritten. Returns true when one was (the write restarted the capture). */
static bool fpga_core_watch_poll(void)
{
    uint8_t r[CORE_STATUS_REPLY_LEN];
    SPI3_CS_ASSERT();
    r[0] = spi3_xfer(FPGA_CORE_OP_DBG_STAT);
    for (unsigned i = 1; i < CORE_STATUS_REPLY_LEN; i++) r[i] = spi3_xfer(0xFF);
    SPI3_CS_DEASSERT();
    const core_status_t st = core_status_parse(r);
    acq_core_watch.polls++;
    if (!st.valid) {
        acq_core_watch.bad++;
        acq_core_watch_prev_diff = 0u;
        return false;
    }
    acq_core_watch.last = st;
    if (!st.locked) acq_core_watch.unlocked++;
    const core_regs_copy_t copy = { acq_rate_idx, acq_trig_src_code, acq_trig_code, acq_edge_reg };
    const uint8_t diff = core_status_reg_diff(&st, &copy);
    const uint8_t fix = acq_core_watch.check_on ? core_status_to_fix(acq_core_watch_prev_diff, diff) : 0u;
    acq_core_watch_prev_diff = diff;
    if (fix == 0u)
        return false;
    if (fix & CORE_REG_DIFF_TB)   { fpga_scope_write_reg(FPGA_CORE_OP_TIMEBASE, copy.tb); acq_core_watch.fixed[0]++; }
    if (fix & CORE_REG_DIFF_SRC)  { fpga_scope_write_reg(FPGA_CORE_OP_SOURCE, copy.src);  acq_core_watch.fixed[1]++; }
    if (fix & CORE_REG_DIFF_LVL)  { fpga_scope_write_reg(FPGA_CORE_OP_LEVEL, copy.lvl);   acq_core_watch.fixed[2]++; }
    if (fix & CORE_REG_DIFF_EDGE) { fpga_scope_write_reg(FPGA_CORE_OP_EDGE, copy.edge);   acq_core_watch.fixed[3]++; }
    acq_core_watch_prev_diff = 0u;
    acq_reprime_req = true;
    return true;
}

/* The acquisition task. Each pass, in order:
 *   1. idle while the FPGA is not up, a pause is asked (the park point), no
 *      usable core is loaded, or a failed mask record is held;
 *   2. roll mode: read one paced op 03 point instead of a record;
 *   3. follow the trigger mode and edge; note writes that dropped the capture;
 *   4. read nothing while SINGLE holds its record or the scope is stopped;
 *      on reading again, restart the capture so an old record cannot pass;
 *   5. wait one fill after the last hand-over, then poll every acq_poll_gap_ms;
 *   6. now and then read the core's status block; in AUTO with no trigger
 *      for a while, ask for a forced record (op 09);
 *   7. read the pair (op 04, op 05); the op 05 header says whether it is a
 *      held record, and whether it was forced;
 *   8. show it or not (trig_policy.h); drop it if the configuration had not
 *      settled (acq_cfg.h);
 *   9. check it, put it in time order, merge it for interleave, commit it. */
void fpga_acq_task(void *pv)
{
    (void)pv;
    /* capture_in_flight: the core's capture has been running since
     * last_handover_tick (a read or a restart), so no new record can be held
     * before one fill after it. False at start and after a register write
     * dropped the capture: the next read then goes out at once.
     * single_done: SINGLE has its one record and holds it until re-armed. */
    bool capture_in_flight = false;
    bool single_done = false;
    uint32_t last_handover_tick = 0;   /* last hand-over, first read or restart */
    uint32_t last_read_tick = 0;
    uint32_t last_commit_tick = 0;
    uint32_t polls_since_handover = 0;
    trigger_mode_t last_mode = TRIG_AUTO;
    uint32_t single_arm_seen = acq_single_arm_req;
    /* Set when SINGLE is (re-)armed: the core may still hold a record from a
     * crossing before the arm, so the capture is restarted before the next
     * read (trig_fresh_restart). */
    bool drop_next = false;
    /* The task has not been reading records (no core, STOP, a SINGLE record
     * held, roll): what the core holds now is old (trig_fresh_restart). */
    bool was_holding = false;
    uint32_t core_watch_tick = xTaskGetTickCount();
    /* The last triggered record or restart: AUTO's wait for a trigger runs
     * from here. */
    uint32_t last_trig_tick = xTaskGetTickCount();
    for (;;) {
        if (!fpga.initialized || fboot.bus_released) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Park point for fpga_acq_pause(): always between CS frames, so a
         * shell-side bus user never interleaves with a half-read window. */
        if (acq_pause_req) {
            acq_pause_ack = true;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* No usable core in the FPGA (core_ident_usable): nothing to read.
         * The badge says NoCore; `fpga core` says why. A reload that brings
         * the core back sets the flag again. */
        if (!acq_core_ok) {
            was_holding = true;
            vTaskDelay(pdMS_TO_TICKS(ACQ_NO_CORE_IDLE_MS));
            continue;
        }

        /* Mask pass/fail (docs/specs/scope/mask-pass-fail.md): apply posted
         * requests here, in the one task that mutates the mask; and while a
         * failed record is held (stop-on-fail) read nothing, so the failing
         * record stays in the buffers, the same hold SINGLE uses. */
        scope_mask_service();
        if (scope_mask_hold_active()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Roll: paced op 03 points instead of the op 04/05 record poll.
         * Nothing below applies: no record, no hand-over, no re-arm. */
        if (acq_roll_on) {
            fpga_roll_service();
            was_holding = true;
            continue;
        }
        roll_tick_armed = false;

        const scope_state_t *ss_acq = scope_state_get();
        trigger_mode_t mode_now = (ss_acq == NULL) ? TRIG_AUTO : ss_acq->trigger.mode;
        if (mode_now != last_mode) {           /* re-selecting SINGLE re-arms it */
            single_done = false;
            drop_next = (mode_now == TRIG_SINGLE);
            last_mode = mode_now;
        }
        if (acq_single_arm_req != single_arm_seen) {   /* OK / trig.mode = single */
            single_arm_seen = acq_single_arm_req;
            if (mode_now == TRIG_SINGLE) {
                single_done = false;
                drop_next = true;
            }
        }
        acq_single_done_pub = single_done;
        {   /* trigger edge register: our core only (dsp/trig_edge_reg.h) */
            const uint8_t want = trig_edge_reg_code(
                                     ss_acq != NULL && ss_acq->trigger.edge == TRIG_FALLING);
            if (want != acq_edge_reg) {
                fpga_scope_write_reg(FPGA_CORE_OP_EDGE, want);
                acq_edge_reg = want;
                acq_reprime_req = true;        /* the write dropped the capture */
            }
        }
        if (acq_reprime_req) {                 /* a register write dropped the capture */
            acq_reprime_req = false;
            capture_in_flight = false;
            /* AUTO's budget is not restarted here: with no trigger about, a
             * setting that keeps changing (a held key) would otherwise show
             * nothing for a whole budget after every step. The request for a
             * forced record goes out again with the next read, and the core
             * still lets a trigger win while its memory refills. */
            /* ...and the records in flight describe the old configuration,
             * even when the snapshot ends where it began (dsp/acq_cfg.h) */
            acq_cfg_touch(&s_acq_cfg_trk, xTaskGetTickCount() * portTICK_PERIOD_MS);
        }
        if (mode_now == TRIG_SINGLE && single_done) {
            was_holding = true;
            vTaskDelay(pdMS_TO_TICKS(10));     /* hold the one record */
            continue;
        }
        /* STOP holds the published record, as stock does: read nothing, like
         * the SINGLE hold above. Reg 0x01 may still follow the timebase keys;
         * the held record and its rate (acq_pub_idx) stay, so the paused
         * zoom view keeps its scale. Autoset and baseline set `running`
         * first. */
        if (ss_acq != NULL && !ss_acq->running) {
            was_holding = true;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        /* Reading again. A record the core held while nobody read, or before
         * SINGLE was armed, must not pass for a new one (without this, RUN
         * after STOP showed a record from before the STOP, 6 times in 6 on
         * the bench). Any register write restarts the capture: write the
         * level with the value it has (trig_fresh_restart). */
        if (trig_fresh_restart(drop_next, was_holding)) {
            fpga_scope_write_reg(FPGA_CORE_OP_LEVEL, acq_trig_code);
            acq_fresh_restarts++;
            drop_next = false;
            /* the capture starts now: wait for it like after a hand-over,
             * and give AUTO its whole wait before it asks for a forced record */
            const uint32_t t = xTaskGetTickCount();
            capture_in_flight = true;
            last_handover_tick = t;
            last_commit_tick = t;
            last_trig_tick = t;
        }
        was_holding = false;
        /* Poll pacing. After a pair has been read (or the capture
         * restarted), the core fills its ring again before it can hold the
         * next record: sleep one fill (fpga_acq_post_edge_get), then read
         * every acq_poll_gap_ms until a read finds a held record. A read
         * while the core is filling only returns the ring as it is (never
         * shown) and does not disturb the capture. */
        uint32_t now = xTaskGetTickCount();
        if (capture_in_flight) {
            uint32_t earliest = last_handover_tick + fpga_acq_post_edge_get();
            /* Sleep in slices and leave for the park point as soon as a
             * pause is asked: at slow timebases the fill is seconds long
             * (2 s at 500 S/s), longer than fpga_acq_pause()'s 1 s budget.
             * Nothing is read on the way out; the wait resumes after the
             * pause from the same last_handover_tick. */
            bool pause_asked = false;
            if ((int32_t)(earliest - now) > 0) {
                uint32_t d = earliest - now;
                if (d > 5000u) d = 5000u;
                while (d > 0u && !(pause_asked = acq_pause_req)) {
                    uint32_t slice = d > ACQ_WAIT_SLICE_MS ? ACQ_WAIT_SLICE_MS : d;
                    vTaskDelay(pdMS_TO_TICKS(slice));
                    d -= slice;
                }
                now = xTaskGetTickCount();
            }
            uint32_t next_poll = last_read_tick + acq_poll_gap_ms;
            while (!pause_asked && (int32_t)(next_poll - now) > 0) {
                uint32_t d = next_poll - now;
                vTaskDelay(pdMS_TO_TICKS(d > ACQ_WAIT_SLICE_MS ? ACQ_WAIT_SLICE_MS : d));
                now = xTaskGetTickCount();
                pause_asked = acq_pause_req;
            }
            if (pause_asked) continue;               /* to the park point at the top */
        }
        if (acq_read_br != 0xFF)
            spi3_set_br(acq_read_br);                       /* `fpga acqbr` */
        /* Ask our core what it holds (never between the op 04 and the op 05
         * of a pair, never a design that has not identified itself). */
        if ((uint32_t)(xTaskGetTickCount() - core_watch_tick) >= pdMS_TO_TICKS(ACQ_CORE_WATCH_MS)) {
            core_watch_tick = xTaskGetTickCount();
            if (fpga_core_watch_poll())
                continue;                                   /* a register was rewritten: capture restarted */
        }
        /* AUTO and no trigger for a whole wait: ask the core to hold a
         * record as it is (op 09), just before reading. Between polls
         * the capture stays armed, so a trigger that comes is still caught
         * and its record read instead (the request does nothing to a record
         * already held). The hold happens at the next stored sample: wait
         * for one, and for the crossing between the two clocks. */
        bool forced_asked = false;
        if (trig_auto_ask((uint8_t)mode_now, (uint32_t)(xTaskGetTickCount() - last_trig_tick) * portTICK_PERIOD_MS,
                          fpga_acq_auto_wait_get())) {
            SPI3_CS_ASSERT();
            spi3_xfer(FPGA_CORE_OP_FORCE);
            SPI3_CS_DEASSERT();
            acq_force_n[0]++;
            forced_asked = true;
            vTaskDelay(pdMS_TO_TICKS(acq_time_force_settle_ms(scope_timebase_sample_rate(acq_rate_idx))));
        }
        /* The configuration before the read (dsp/acq_cfg.h): a change since
         * the last note restarts the settle window. */
        acq_cfg_t cfg_now;
        fpga_acq_cfg_snapshot(&cfg_now);
        (void)acq_cfg_note(&s_acq_cfg_trk, &cfg_now, xTaskGetTickCount() * portTICK_PERIOD_MS);
        uint32_t edges_before = acq.pc0_edges;
        uint16_t hw_to_before = acq.spi3_hw_timeouts;
        volatile uint8_t *w1 = acq_write_ch1();
        volatile uint8_t *w2 = acq_write_ch2();
        fpga_acq_read_channel(FPGA_CORE_OP_REC_A, w1);
        fpga_acq_read_channel(FPGA_CORE_OP_REC_B, w2);
        last_read_tick = xTaskGetTickCount();
        fpga_acq_cfg_snapshot(&cfg_now);                 /* ...and after it */
        (void)acq_cfg_note(&s_acq_cfg_trk, &cfg_now, last_read_tick * portTICK_PERIOD_MS);
        /* The core says it in the reply itself: the op 05 header carries the
         * seam (V set) exactly when the op 04 before it found a held record
         * and froze it for the pair (fpga/core/rtl/cmd.sv), and the record
         * was still held when op 05 began. PC0 signals the same; it is only
         * compared, for the bench (`fpga core`). */
        bool triggered = trig_seam_from_hdr(acq.acq_hdr_ch2[FPGA_CORE_SEAM_I_HI], acq.acq_hdr_ch2[FPGA_CORE_SEAM_I_LO]) >= 0;
        {
            const bool pc0 = (acq.pc0_edges != edges_before);
            acq_kind_n[triggered ? 0 : 1]++;
            if (triggered && !pc0) acq_kind_n[2]++;
            if (!triggered && pc0) acq_kind_n[3]++;
        }
        /* `held`: the read is a record the core held and handed over. It may
         * have been held on request (op 09, `forced`) instead of by a
         * trigger: time-ordered and simultaneous like any record, but not
         * `triggered`. */
        const bool held = triggered;
        const bool forced = held && trig_hdr_forced(acq.acq_hdr_ch2[FPGA_CORE_SEAM_I_HI]);
        if (forced) { triggered = false; acq_force_n[1]++; }
        if (triggered) last_trig_tick = last_read_tick;
        acq_ilcal_feed(w1, w2, acq.spi3_hw_timeouts == hw_to_before);
        bool first_read = !capture_in_flight;
        capture_in_flight = true;
        if (held || first_read) {
            last_handover_tick = last_read_tick;
            acq.acq_polls_last = polls_since_handover;
            polls_since_handover = 0;
        } else {
            polls_since_handover++;
            acq.acq_poll_reads++;
        }
        /* What this read is and whether it is shown: dsp/trig_policy.h. */
        const trig_acq_in_t pin = {
            .mode        = (uint8_t)mode_now,
            .triggered   = triggered,
            .forced      = forced,
            .single_done = single_done,
        };
        const trig_acq_t kind = trig_acq_decide(&pin);
        if (kind == TRIG_ACQ_HOLD) {
            /* Nothing held, or not for this mode. After a request for a
             * forced record: the memory was not all new yet (the core keeps
             * the request), or a trigger got there first. */
            if (forced_asked && !held) acq_force_n[2]++;
            continue;
        }
        /* Capture configuration (dsp/acq_cfg.h): a record whose samples may
         * predate the last change of routing, range, coupling, position,
         * probe, timebase, trigger, interleave or roll, or overlap the
         * settling after it, is dropped: neither configuration describes it.
         * The published record stays the last good one. */
        if (acq_cfg_settle_cmd != 0xFFFFu &&
            !acq_cfg_admit(&s_acq_cfg_trk, last_read_tick * portTICK_PERIOD_MS,
                           acq_cfg_fill_ms(scope_timebase_sample_rate(acq_rate_idx)),
                           acq_poll_gap_ms, acq_cfg_settle_cmd ? acq_cfg_settle_cmd : ACQ_CFG_SETTLE_MS)) {
            acq_cfg_drops++;
            continue;
        }
        /* A record is used only when no SPI3 transfer timed out during its
         * two reads: a timed-out transfer returns 0xFF instead of bus data. */
        if (acq.spi3_hw_timeouts == hw_to_before) {
            acq.acq_last_rot = fpga_acq_unrotate_staging(held);
            acq_stage_ordered = acq_unrotate && acq.acq_last_rot >= 0;
            /* a forced record is in time order but has no trigger at 512 */
            acq_stage_ordered_core = acq_stage_ordered && !forced;
            acq_stage_il = held && fpga_interleave_active() && acq_stage_ch1 && acq_stage_ch2;
            acq_stage_kind = (uint8_t)kind;
            if (acq_stage_il)
                interleave_merge(acq_stage_ch1, acq_stage_ch2, &acq_il_corr);
            acq_stage_cfg = s_acq_cfg_trk.last;          /* what it was captured with */
            fpga_acq_frames_commit();
            /* Judge (or teach from) the record just committed: exactly once,
             * from the published buffers, in the only task that writes them. */
            scope_mask_on_commit(acq_ch1_buf, acq_ch2_buf, triggered,
                                 acq_stage_ordered, fpga_acq_frame_generation());
            last_commit_tick = last_read_tick;
            acq.spi3_ok_count++;
            data_ready = true;
            if (triggered)
                acq.acq_last_latency_ms = xTaskGetTickCount() - acq_pc0_last_tick;
            if (triggered)
                acq_last_trig_tick = last_commit_tick;
            if (mode_now == TRIG_SINGLE && triggered) {
                single_done = true;            /* one triggered record, then hold */
                acq_single_done_pub = true;
            }
        } else {
            acq.spi3_total_timeouts++;
        }

        /* A short yield between passes. */
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* The acquisition task reads SPI3 continuously, so every other user of the
 * bus parks it here first. True at once when the task does not exist. */
bool fpga_acq_pause(void)
{
    if (acq_task_handle == NULL)
        return true;
    acq_pause_req = true;
    acq_pause_ack = false;      /* demand a FRESH ack from the park point */
    /* A running task reaches its park point within one wait slice
     * (ACQ_WAIT_SLICE_MS) plus one record pair, roll point or status read;
     * 100 x 10 ms = 1 s is a wide margin. It never acks while the FPGA is
     * not initialised or the bus is released (the check above the park
     * point), so the pause then fails. */
    for (int i = 0; i < 100; i++) {
        if (acq_pause_ack)
            return true;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    acq_pause_req = false;      /* don't leave a half-armed request behind */
    return false;
}

void fpga_acq_resume(void)
{
    acq_pause_req = false;
}

/*
 * Make the core's timebase register and the display's timebase agree after
 * an arm burst, which writes 0x08:
 *   - the display's code has a known rate: write it to the core, so the
 *     user's timebase survives a power cycle;
 *   - it has none: set the display to the 0x08 the burst wrote.
 * In roll mode the roll code is written back instead.
 *
 * Must be called after every arm burst: the upload in fpga_boot.c (boot and
 * fpga_reload) and the reverse-engineering toolkit's config sequence. main()
 * calls it again right after settings_store_init(), which runs after
 * fpga_init() and restores the persisted timebase_idx: at boot that later
 * call is the one that counts.
 *
 * Writes the register directly, without a park. The caller must hold SPI3
 * at a clock the core accepts, with the task not running: at boot it does
 * not exist yet, and fpga_reload() suspends it.
 */
/* 0 = never ran, 1 = wrote a code to the core, 2 = set the display to 0x08.
 * Shown by `status`. */
static volatile uint8_t fpga_tb_reconcile_action;

uint8_t fpga_timebase_reconcile_action(void)
{
    return fpga_tb_reconcile_action;
}

void fpga_reconcile_timebase_after_arm(void)
{
    scope_state_t *ss = scope_state_get();

    /* Roll survives a re-arm: put back the roll code the core was given,
     * not the Y-T one. */
    if (ss->roll && acq_roll_on) {
        const uint8_t rc = (acq_roll_code_override >= 0)
                               ? (uint8_t)acq_roll_code_override
                               : scope_roll_stock_code(acq_roll_step);
        SPI3_CS_ASSERT();
        spi3_xfer(FPGA_CORE_OP_TIMEBASE);
        spi3_xfer(rc);
        SPI3_CS_DEASSERT();
        acq_rate_idx = rc;
        fpga_tb_reconcile_action = 1u;
        return;
    }
    if (scope_timebase_sample_rate(ss->timebase_idx) > 0.0f) {
        SPI3_CS_ASSERT();
        spi3_xfer(FPGA_CORE_OP_TIMEBASE);
        spi3_xfer(ss->timebase_idx);
        SPI3_CS_DEASSERT();
        acq_rate_idx = ss->timebase_idx;
        fpga_tb_reconcile_action = 1u;
    } else {
        ss->timebase_idx = 0x08;
        acq_rate_idx     = 0x08;
        fpga_tb_reconcile_action = 2u;
    }
}

/* Trigger counterpart of the reconcile above: the arm burst leaves reg 0x08
 * at 0xAD and reg 0x06 at CH1; write the (possibly restored) UI level and
 * source. Called only from main(), before the task exists, so bare writes
 * are correct. fpga_reload() uses the apply functions instead. */
static volatile uint8_t fpga_trig_reconcile_code;

uint8_t fpga_trigger_reconcile_code(void) { return fpga_trig_reconcile_code; }

void fpga_reconcile_trigger_after_arm(void)
{
    scope_state_t *ss = scope_state_get();
    uint8_t code = fpga_trigger_code_from_level(ss->trigger.level);
    SPI3_CS_ASSERT();
    spi3_xfer(FPGA_CORE_OP_LEVEL);
    spi3_xfer(code);
    SPI3_CS_DEASSERT();
    acq_trig_code = code;
    fpga_trig_reconcile_code = code;
    /* The arm burst wrote source 0x00 (CH1); a persisted CH2 source must
     * reach reg 0x06 too, or the FPGA fires on CH1 under a "C2" label. */
    uint8_t src = (ss->trigger.source == TRIG_SRC_CH2) ? 0x01u : 0x00u;
    SPI3_CS_ASSERT();
    spi3_xfer(FPGA_CORE_OP_SOURCE);
    spi3_xfer(src);
    SPI3_CS_DEASSERT();
    acq_trig_src_code = src;
}

bool fpga_data_ready(void)
{
    return data_ready;
}

const volatile uint8_t *fpga_get_ch1_buf(void)
{
    return fpga.initialized ? acq_ch1_buf : NULL;
}

const volatile uint8_t *fpga_get_ch2_buf(void)
{
    return fpga.initialized ? acq_ch2_buf : NULL;
}
