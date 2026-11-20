/*
 * OpenScope 2C53T - acquisition from the capture core (SPI3)
 *
 * The acquisition task, the staged and published record, the core's
 * registers (timebase, trigger), interleave and roll. Split out of fpga.c;
 * shared items: fpga_internal.h.
 */

#include "fpga_internal.h"

TaskHandle_t      acq_task_handle = NULL;

/* Auto/free-run acquisition (2026-08-14, analysis_v120/
 * trigger_regime_findings_2026-08-14.md Addendum 4). The raw capture buffer
 * (BSRAM_0/3, read via 0x04/0x05) free-runs at the sample clock regardless of
 * trigger state, and a direct poll returns a COHERENT waveform (bench: a 30 Hz
 * sine shows ~13 clean cycles across the 1024-sample buffer with PC0 never
 * pulsing). So a continuously-updating display is a READ-PACING choice, not a
 * fabric feature: in AUTO mode we free-run poll 0x04/0x05 rather than waiting
 * for a trigger edge that a quiet or below-threshold input never produces.
 *
 *   FPGA_AUTO_CADENCE_MS   free-run poll period in AUTO mode (~30 Hz). The
 *                          display's own ~50 ms frame loop is the real cap;
 *                          faster only costs SPI time (~275 us/pair at /2).
 *   FPGA_AUTO_TRIG_WAIT_MS how long AUTO waits for a real trigger edge before
 *                          falling through to a free-run read — small, so a
 *                          triggered capture is preferred WHEN available (a
 *                          stable trace) but the display never stalls without
 *                          one. This is exactly analog-scope "auto" behaviour.
 *   FPGA_NORMAL_TRIG_WAIT_MS  NORMAL/SINGLE edge-wait budget per iteration; on
 *                          timeout the loop HOLDS the last trace (no free-run
 *                          overwrite), which is correct triggered-mode behaviour. */
#ifndef FPGA_AUTO_CADENCE_MS

#define FPGA_AUTO_CADENCE_MS 30u

#endif

#ifndef FPGA_AUTO_TRIG_WAIT_MS

#define FPGA_AUTO_TRIG_WAIT_MS ACQ_AUTO_MIN_MS      /* dsp/acq_timing.h; a build may give another floor */

#endif

#ifndef FPGA_NORMAL_TRIG_WAIT_MS

#define FPGA_NORMAL_TRIG_WAIT_MS 300u

#endif

static volatile bool data_ready = false;

void fpga_scope_delay_ms(uint32_t ms);

void fpga_scope_delay_ms(uint32_t ms)
{
    if (ms == 0) return;

    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        vTaskDelay(pdMS_TO_TICKS(ms));
    } else {
        systick_delay_ms(ms);
    }
}

/* Cooperative pause handshake of the acquisition task. Protocol (see
 * fpga_acq_pause): requester sets req, clears ack, then waits for a FRESH
 * ack — the task raises ack only at its park point, never inside a CS frame,
 * and re-raises it every parked cycle so a stale ack from a previous pause
 * can never satisfy a new request. */
static volatile bool acq_pause_req = false;

static volatile bool acq_pause_ack = false;

/* ── Torn-frame fix (audit 2026-08-20, P0.2) ─────────────────────────────
 * ch1_buf/ch2_buf used to be written IN PLACE, byte-by-byte, over the whole
 * SPI read (~275 µs at /2, tens of ms at slow clocks) while the lower-
 * priority display task read them — a preempting capture spliced two frames
 * through every badge, freq estimate and rendered trace. Now the SPI read
 * lands in heap staging (allocated in fpga_create_tasks; heap not BSS — the
 * main-stack headroom over BSS is ~2 KB, see flash_fs.c's identical note)
 * and is committed by two memcpys (~10 µs) bracketed by acq_frame_gen:
 * odd = commit in progress. Readers that need exact numbers re-read until
 * the generation is even and unchanged; the renderer accepts the residual
 * ~10 µs window as a rare one-frame cosmetic. Side benefit: REJECTED frames
 * (validity gate, hw-timeout refusal) no longer touch the published buffers
 * at all. If staging allocation ever failed, writes fall back in place —
 * old behavior, generation counter simply never advances. */
static volatile uint32_t acq_frame_gen = 0;

uint8_t *acq_stage_ch1 = NULL;

uint8_t *acq_stage_ch2 = NULL;

uint32_t fpga_acq_frame_generation(void)
{
    return acq_frame_gen;
}

static volatile uint8_t *acq_write_ch1(void)
{
    return acq_stage_ch1 ? acq_stage_ch1 : fpga.ch1_buf;
}

static volatile uint8_t *acq_write_ch2(void)
{
    return acq_stage_ch2 ? acq_stage_ch2 : fpga.ch2_buf;
}

/* Whether the STAGED / PUBLISHED record is time-ordered (un-rotated at its
 * seam, trigger at 512). Published inside the commit's seqlock window so the
 * flag always describes the record in fpga.ch1_buf -- v18 read the flag of
 * the newest commit against a frame one commit older (2026-09-23). */
static volatile bool acq_stage_ordered = false;

static volatile bool acq_pub_ordered = false;

/* ...and the seam came from the core's reply header, not from a search in the
 * samples: the trigger is then at index 512 by construction (trig_edge.h). */
static volatile bool acq_stage_ordered_core = false;

static volatile bool acq_pub_ordered_core = false;

static bool acq_rot_from_hdr = false;        /* set by fpga_acq_unrotate_staging() */

/* Single-channel 250 MS/s (dsp/interleave.h): the staged / published CH1
 * record is the merge of both converters. Published with the record, like
 * `ordered`, so the rate a reader applies always matches the buffer. */
static volatile bool acq_stage_il = false;

static volatile bool acq_pub_il = false;

/* The reg-0x01 code the published record was captured at (0xFF: none yet, the
 * in-place fallback never commits). acq_rate_idx moves the moment a timebase
 * key is pressed, even STOPPED, while the held record keeps its own rate:
 * the zoom-while-paused view (C14, ui/scope_ui.c) and the measurement badges
 * must read the record's rate, not the selector's. */
static volatile uint8_t acq_pub_idx = 0xFF;

static volatile uint8_t acq_rate_idx;       /* defined (initialised) below */

/* What the published record is (trig_policy.h, parity C3): handed over,
 * free-run, or free-run with two non-simultaneous channels (#84). Staged and
 * published with the frame, like the two flags above, so the badge can never
 * describe a different record from the one on the glass. */
static volatile uint8_t  acq_stage_kind = TRIG_ACQ_HOLD;

static volatile uint8_t  acq_pub_kind = TRIG_ACQ_HOLD;

static volatile uint32_t acq_last_trig_tick = 0;     /* tick a handed-over record was published */

/* SINGLE one-shot: the acq task's single_done, published, and the re-arm
 * request the UI raises (OK / the shell), consumed by the task. */
static volatile bool     acq_single_done_pub = false;

static volatile uint32_t acq_single_arm_req = 0;

/* The capture configuration (dsp/acq_cfg.h) of the STAGED record, set by
 * the acquisition task when it admits a read, and of the PUBLISHED one,
 * copied in the commit's seqlock window like the flags above. Mask 0 = no
 * record committed yet (or the in-place fallback, which never commits):
 * every channel counts as present, as before this existed. 2 x 22 B .bss. */
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
    memcpy((void *)fpga.ch1_buf, acq_stage_ch1, FPGA_ADC_BUF_SIZE);
    memcpy((void *)fpga.ch2_buf, acq_stage_ch2, FPGA_ADC_BUF_SIZE);
    acq_pub_ordered = acq_stage_ordered;
    acq_pub_ordered_core = acq_stage_ordered_core;
    acq_pub_il = acq_stage_il;
    acq_pub_idx = acq_rate_idx;
    acq_pub_kind = acq_stage_kind;
    acq_pub_cfg = acq_stage_cfg;
    acq_frame_gen++;                                   /* even: stable */
}

/* NOTE: deliberately OUTSIDE the FPGA_WARM_HANDOFF_TEST guard below: the debug
 * shell references these in every build. The guarded part is only the
 * acquisition task's USE of them. */
/* EXP-41/42 (2026-09-14): the AUTO edge-wait budget, runtime-settable. PC0
 * is a real data-ready line: a READ starts a capture, the capture completes
 * when the ADC crosses the reg-0x08 trigger level, and completion pulses PC0
 * once (idle produces nothing; a reg-01 write produces nothing). The compiled
 * 25 ms budget is shorter than the capture at every code slower than 0x0E
 * (82 ms at 0x10), so the AUTO fallback always read mid-capture — the seam.
 * `fpga autowait <ms>`; 0 = the compiled default. */
static volatile uint16_t acq_auto_wait_ms = 0;

/* The digital trigger level in force: SPI3 reg 0x08, an unsigned ADC code.
 * The boot arm burst writes stock's captured 0xAD (173); every bench signal
 * since 2026-08 peaked near 160, so PC0 "never fired" for a month (EXP-41).
 * fpga_reconcile_trigger_after_arm() hands the register to the UI level
 * right after the arm; fpga_apply_trigger_level() is the ONLY runtime path. */
static volatile uint8_t acq_trig_code = 0xAD;

/* EXP-47: a reg-0x08 or reg-0x01 write drops the capture in flight (NORMAL
 * stayed frozen after level +100 -> 0). The apply entry points raise this;
 * the acq task clears capture_in_flight on it and re-primes. */
volatile bool acq_reprime_req = false;

/* Trigger edge register 0x07 (dsp/trig_edge_reg.h). acq_design_ours: the
 * design running now is our core (set where an upload ends, below);
 * acq_edge_reg: the value the register holds (every arm burst writes 0x00).
 * The acquisition task writes the register when the two disagree, so the edge
 * follows the UI and comes back by itself after any arm burst. */
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

/* The core's own account of its registers and PLL (op E0, dsp/core_status.h),
 * read by the acquisition task between pairs. A register that differs from
 * the firmware's copy in two successive readings is rewritten. */
static fpga_core_watch_t acq_core_watch = { .check_on = true };

static uint8_t           acq_core_watch_prev_diff;

fpga_core_watch_t fpga_core_watch(void) { return acq_core_watch; }

void fpga_core_watch_fix_set(bool on)   { acq_core_watch.check_on = on; acq_core_watch_prev_diff = 0u; }

/* AUTO without a trigger (op 09, dsp/trig_policy.h): [0] requests sent, [1]
 * forced records read, [2] reads after a request that found no record yet
 * (the memory was not all new since the last restart). */
static volatile uint32_t acq_force_n[3];

uint32_t fpga_acq_force_count(unsigned i) { return i < 3u ? acq_force_n[i] : 0u; }

static volatile uint32_t acq_fresh_restarts;     /* captures restarted before a first read (trig_fresh_action) */

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

/* Single-channel interleave (2026-11-10, bench B4 on unit #2): with mask 1
 * both converters sample CH1 half a sample apart; at timebase <= 3 (stock's
 * gate, 0x080252B2) the acq task merges them into a 250 MS/s record. Runtime
 * switch, default ON. Converter-B correction (level map + timing skew,
 * dsp/interleave.h) defaults to identity; a unit's own numbers come from
 * `fpga interleave cal` / `skew measure` or a saved user-cal record. */
static volatile bool acq_il_enable = true;

static interleave_corr_t acq_il_corr = { .gain = 1.0f, .offset = 0.0f };

/* Self-calibration collector (parity C2, `fpga interleave cal`): while `on`,
 * every pair the acq task reads (op04 = A, op05 = B, record units) adds the
 * means of its centre 512 samples (the window the merge uses). DC only: the
 * same-moment requirement of a merge does not apply to a flat input, so
 * untriggered roll reads count too. Shell side owns the DAC and the timing. */
static struct {
    volatile bool on;
    volatile uint8_t skip;          /* records to drop after begin (pipeline) */
    volatile uint8_t n;
    volatile uint8_t p2p_a, p2p_b;
    volatile bool clipped;
    float sum_a, sum_b, amin, amax, bmin, bmax;   /* of the per-record means */
} acq_ilcal;

static volatile uint16_t acq_poll_gap_ms = ACQ_POLL_GAP_MS;   /* dsp/acq_timing.h; `fpga pollgap` overrides */

static volatile bool     acq_unrotate = true;    /* 2026-09-22: seam-based (dev plan 2.3); `fpga unrotate off` for the raw record */

static volatile int16_t  acq_unrotate_offset = 0;

/* Held records put in time order by the seam in the core's reply header
 * (`fpga unrotate`): [0] ordered, [1] a held record without a seam (never
 * expected: the op 05 header of a pair always carries it). */
static volatile uint32_t acq_seam_n[2];

/* EXP-46 (2026-09-14): an explicit SPI3 clock for the task's reads (0..7 =
 * /2../256; 0xFF = leave whatever is set). `fpga acqbr`. */
static volatile uint8_t  acq_read_br = 0xFF;

/* Reg 0x01 value currently in force -- the ONE variable that mirrors the
 * hardware register. 0x08 is what the arm block writes at config time.
 *
 * 2026-08-19: that comment described an intent the code never implemented.
 * The UI's timebase button wrote scope_state.timebase_idx and nothing else;
 * this variable was reachable only from `fpga rate`. So the firmware carried
 * TWO notions of "current timebase" -- one that labelled the axis and one
 * that drove the hardware -- and they were observed diverging on a stock
 * boot (0x0A on the display, 0x08 in the register). It stayed invisible only
 * because both codes are in the uncalibrated band, so the label read "--".
 * Once scope_timebase.c gave the labels real measured rates, the same bug
 * would have printed a confident, wrong s/div. fpga_apply_timebase() below
 * is now the single entry point, and the button calls it. */
static volatile uint8_t acq_rate_idx     = 0x08;

void fpga_acq_auto_wait_set(uint16_t ms)  { acq_auto_wait_ms = ms; }

/* AUTO edge-wait budget in force. Override if set; else derived from the
 * timebase in force: 4x the 1024-sample fill at that rate (EXP-42: edges per
 * pair saturate by ~3.6x at 0x10), clamped to [compiled 25 ms, 1000 ms] —
 * codes slower than ~0x12 fall back to free-run reads past the cap rather
 * than freezing AUTO for seconds. Unmeasured codes (rate 0) get the compiled
 * value. */
uint16_t fpga_acq_auto_wait_get(void)
{
    if (acq_auto_wait_ms) return acq_auto_wait_ms;
    /* The numbers and their origins: dsp/acq_timing.h. */
    return acq_time_auto_wait_ms(scope_timebase_sample_rate(acq_rate_idx), (uint16_t)FPGA_AUTO_TRIG_WAIT_MS);
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

core_ident_t fpga_core_ident(void)
{
    uint8_t r[CORE_IDENT_REPLY_LEN];
    for (unsigned i = 0; i < CORE_IDENT_REPLY_LEN; i++) r[i] = fpga.core_ident[i];
    return core_ident_parse(r);
}

uint32_t fpga_acq_seam_count(unsigned i)      { return i < 2u ? acq_seam_n[i] : 0u; }

/* Un-rotate a strobed record into time order (dev plan 2.3, 2026-09-22).
 *
 * The committed record is one continuous 1024-sample segment rotated so its
 * seam sits at the FPGA's write pointer, with the trigger 512 samples after
 * the seam (EXP-53/54). The pointer is not recoverable from MCU timestamps
 * (EXP-53 postscript: PC0 strobes 6-9 us after the read, so the EXP-52
 * latency model this replaces measured nothing); the core names it in the
 * reply header of the two reads (below). Both channels share the pointer, so
 * both are rotated by it. After this, index 0 is the oldest sample and the
 * trigger is at index 512.
 *
 * Reads that are not a held record are left alone: the rolling buffer has
 * no trigger at 512 to put anywhere.
 *
 * Returns the rotation applied (0..1023), or -1 when none was. */
static int32_t fpga_acq_unrotate_staging(bool triggered)
{
    acq_rot_from_hdr = false;
    if (!triggered) return -1;
    volatile uint8_t *bufs[2] = { acq_write_ch1(), acq_write_ch2() };
    uint32_t k = 0;
    /* Our core (v3 on) says where the seam is, in the reply header of the two
     * reads just made. The op 05 header first: it always carries the pair's
     * pointer, the op 04 one is withheld when the record was held only as
     * that frame began. A record with no step at its seam (a whole number of
     * periods, a square, DC) is then ordered like any other; the search in
     * the samples could not order those (bench 2026-11-16: 9 of the 13
     * generator waveforms). */
    int hs = trig_seam_from_hdr(fpga.acq_hdr_ch2[FPGA_CORE_SEAM_I_HI], fpga.acq_hdr_ch2[FPGA_CORE_SEAM_I_LO]);
    if (hs < 0)
        hs = trig_seam_from_hdr(fpga.acq_hdr_ch1[FPGA_CORE_SEAM_I_HI], fpga.acq_hdr_ch1[FPGA_CORE_SEAM_I_LO]);
    if (hs < 0) {                                /* a held record always carries it: not reached */
        acq_seam_n[1]++;
        return -1;
    }
    k = (uint32_t)hs;
    acq_seam_n[0]++;
    acq_rot_from_hdr = true;
    if (!acq_unrotate) return (int32_t)k;        /* reported (status), not applied */
    if (k == 0) return 0;
    /* In place, no staging buffer (this used a 1 KB static tmp[]; RAM diet
     * 2026-11-11). Same result: index 0 becomes the old index k. */
    for (int c = 0; c < 2; c++)
        trig_edge_rotate_left(bufs[c], FPGA_ADC_BUF_SIZE, k);
    return (int32_t)k;
}

void fpga_acq_read_br_set(uint8_t br)     { acq_read_br = br; }

uint8_t fpga_acq_read_br_get(void)      { return acq_read_br; }

uint8_t fpga_acq_trig_code_get(void)      { return acq_trig_code; }

/* UI trigger level (-100..100, screen pixels about mid-scale, the renderer's
 * own convention: SCOPE_H px == 256 counts, SCOPE_H = LCD_HEIGHT-16-18 = 206
 * in scope_ui.c) -> reg 0x08 code. Keep the 206 in step with scope_ui.c. */
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
    return acq_il_enable && fpga.channel_mask == 1u && acq_rate_idx <= 3u;
}

/* The line also drops any knots: it is a complete level map of its own
 * (il.knots = 0, a v1 user-cal record, `cal user clear`). The
 * skew is independent and stays. */
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

/* Called by the acq task right after the op04/op05 reads; `ok` = no SPI3
 * hardware timeout during them. Cheap when off. */
/* One converter's window, robust to isolated glitch samples: on unit #2 a
 * quiet input at tb <= 3 reads +-5 counts plus interference bursts (10-40
 * samples of wild values, ~5 % of a record; bench 2026-11-11), which a plain
 * peak-to-peak counted as "not flat" and refused the whole cal. Pass 1 takes the mean; pass 2 drops samples
 * more than ILCAL_GLITCH counts from it and measures mean, spread and the
 * rails on the rest. More than ILCAL_GLITCH_MAX dropped samples is not a
 * glitch any more: the spread is reported as 255 (not flat). */
#define ILCAL_GLITCH      24

#define ILCAL_GLITCH_MAX  77u   /* of 512: 15 %. Unit #2 at tb <= 3 shows interference

                                 * BURSTS of 10-40 samples covering ~5 % of a record
                                 * (bench 2026-11-11); a real signal leaves far more
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
 * holds for two handover cycles: the FPGA hands a record over at most once
 * per fill + ~200 ms hold (EXP-72), so a shorter window would flicker
 * between handovers at the slow codes. */
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

/* Trigger source -> SPI3 reg 0x06 (parity C3). Only the boot arm wrote it
 * (0x00), so CH2 as the source moved the soft trigger and the label while the
 * FPGA kept firing on CH1. Stock sends op06 = ms[0x16] on every source change
 * (0x0800C890, 0x0800C96C) and after a channel switch-off (0x080235E8);
 * EXP-72 saw 0x01 (or 0xFF) move the trigger to CH2 (zero handovers with CH2
 * undriven). Like reg 0x08, the write drops the capture in flight. */
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

/* See fpga.h. Push a timebase code to the hardware NOW and record it as the
 * value in force, so the axis label and the sample rate cannot disagree.
 *
 * Parking the acquisition task first is the same discipline every shell
 * command that touches SPI3 uses -- an interleaved CS frame corrupts both
 * transfers. If the task will not park we skip the register write and report
 * failure rather than risk the bus; the caller keeps the old rate, which is
 * honest, where a torn write would leave the hardware at an unknown rate the
 * label could not describe. */
void fpga_reconcile_timebase_after_arm(void);                 /* below */

/* One CS-framed scope-engine register write: opcode byte, value byte.
 * Same shape as the five arm writes (fpga.c, FPGA_CONFIG_B_ARM block) — these
 * are runtime opcodes on the configured user design, NOT Gowin config-port
 * opcodes, and are bench-proven safe against a live design.
 * Defined OUTSIDE the FPGA_WARM_HANDOFF_TEST block deliberately: its callers
 * (fpga_apply_timebase below) exist in every build, and in non-warmtest
 * builds only reach it behind fpga_acq_pause(), whose constant-false stub
 * dead-codes the call — leaving the definition inside the block produced a
 * "used but never defined" warning on app/emu/guest builds. */
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

/* Roll mode state (C11). acq_roll_on is the acquisition task's path switch;
 * it changes only inside a park (fpga_apply_roll / fpga_apply_timebase), so
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
    c->mask      = fpga.channel_mask;
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

bool fpga_acq_record_has_ch(uint8_t ch)
{
    return acq_pub_cfg.mask == 0u ? ch <= 1u : acq_cfg_has_ch(&acq_pub_cfg, ch);
}

uint32_t fpga_acq_cfg_drops(void)        { return acq_cfg_drops; }

static volatile uint8_t  acq_roll_step = 0;

static int               acq_roll_code_override = -1;

static volatile fpga_roll_stats_t acq_roll_stats;

bool fpga_apply_timebase(uint8_t code)
{
    if (!fpga_acq_pause())
        return false;
    fpga_scope_write_reg(FPGA_CORE_OP_TIMEBASE, code);
    acq_rate_idx = code;
    acq_roll_on = false;          /* a Y-T code is in force: back to records */
    /* The timebase picks the calibration bank (>= 5 -> 0, 4 -> 1, <= 3 ->
     * 1 or 2 by mask): crossing 4/5 moves the offset references. DAC/TMR13
     * writes only, no SPI3, so inside or outside the park is equivalent. */
    scope_vpos_apply();
    acq_reprime_req = true;
    fpga_acq_resume();
    return true;
}

/* See fpga.h. Stock's timebase-change routine (0x0800B6B2..0x0800B798) does
 * the same three things for tb >= 0x14: arms the pacing timer, resets the
 * point counter (ms[0xDB4] = 301) and queues op01, whose handler
 * (0x0803E54C) writes ms[0x2D] = 0x14..0x1C to reg 0x01 unmodified. What the
 * FPGA does with 0x15..0x1C is unmeasured (0x14 = 500 S/s is, EXP-18); op03
 * reads a live point either way, so the shell can force 0x14 for an A/B.
 *
 * The FIFO is cleared on every step change. Stock keeps its points and
 * draws them at the new spacing; a trace whose left part claims the old
 * time per point is a confident wrong axis, so we start empty instead. */
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
    /* Stock's bank rule is on ms[0x2D] (>= 5 -> bank 0, SYNTHESIS 1.2), so
     * a roll code keeps the offset references of the slow Y-T codes. */
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

/* Trigger sibling of fpga_apply_timebase (EXP-41, 2026-09-14): write the
 * digital trigger level (reg 0x08, unsigned ADC code) and record what is in
 * force. The level is the UI's -100..100; the code is what the FPGA compares
 * the ADC against to complete a capture and pulse PC0. Returns false if the
 * acquisition task would not park, in which case nothing was written. */
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

/* (Pause-handshake flags acq_pause_req/ack live above fpga_acq_publish now —
 * shared by both acquisition tasks since 2026-08-20, audit P0.1.) */

/* PC0 (FPGA data-ready, active LOW) falling-edge counter. A polled level
 * read cannot see a short deassert pulse, which is exactly what made every
 * prior PC0 observation regime-blind; EXINT0 catches edges of any width.
 * No FreeRTOS calls here, so any NVIC priority is safe. */
void EXINT0_IRQHandler(void)
{
    exint_flag_clear(EXINT_LINE_0);
    fpga.pc0_edges++;
    fpga.pc0_last_tick = xTaskGetTickCountFromISR();
}

/* ─── Reading one record (2026-08-12) ────────────────────────────────
 * The real per-channel read protocol from the issue-#18 stock capture,
 * bench-proven on this board by Stlkv's port: PC0 data-ready (active LOW),
 * then ONE 1026-byte CS-LOW window per channel — the opcode byte (0x04=CH1,
 * 0x05=CH2) plus ONE dummy byte, then 1024 samples.
 *
 * CORRECTED 2026-08-15 from stock's own dispatch (handlers 0x0803E5A4 for
 * op 0x04 and 0x0803E68C for op 0x05, decoded in
 * analysis_v120/spi3_runtime_dispatch_2026-08-15.md): stock discards exactly
 * TWO bytes — the opcode echo and one dummy — then captures 1024 samples into
 * state[0x5B0 + 0..1023] (CH1) / [1024..2047] (CH2). We had been discarding
 * three and keeping 1023, so every sample array was shifted one byte late and
 * one sample short. The old third "header" byte was in fact sample[0]; the
 * status line showed it as 0x7C/0x79, i.e. mid-scale sample values, never the
 * 0x01 "buffer-valid flag" it had been read as. Strictly read-only
 * on the FPGA: scope-engine opcodes only, never the Gowin config port
 * (0x11/0x41 there desynchronise a configured part — Exp L). Feeds the same
 * buffers/flags the scope UI already consumes, so success shows on the LCD:
 * the synthetic demo trace disappears on the first real frame (scope_ui.c
 * latch), and spi3_ok_count drives redraws (main.c new-data detector).
 *
 * NOTE the 0x05 hazard: as a 2-byte CS frame, 0x05 would be byte-identical
 * to the config prelude's ERASE_SRAM step. The 1026-byte frame shape is what
 * makes it a CH2 read — do not "shorten" this window. */
/*
 * CORRECTED AGAIN 2026-09-22 (EXP-58): the record starts at the THIRD byte
 * after the opcode, not the second. Reading held records twice over (2,060
 * bytes) on unit #1, the byte at wire index 2 + 1024n (n = 1, 2) continued
 * smoothly from the byte BEFORE it (residual 0-3 counts, 12/12, same as an
 * interior control byte) and broke into the byte AFTER it (8-35 counts): each
 * 1024-byte block ends at wire 2 + 1024n. So wire byte 2 is the tail of a
 * previous block -- a stale byte -- and the record is wire 3..1026. With the
 * 2-byte discard, "sample 0" was that stale byte (a stray value in 13/20
 * records) and the true last sample was never read. The 2026-08-15 change to
 * stock's 2-byte discard matched stock's CODE; at our read timing the first
 * byte out is stale. The window is now 1027 bytes: opcode, dummy, stale, then
 * 1024 samples. Longer windows (1043, 2063 bytes) caused no fault in EXP-58.
 */
static uint8_t fpga_acq_read_channel(uint8_t opcode, volatile uint8_t *buf)
{
    SPI3_CS_ASSERT();
    uint8_t s0 = spi3_xfer(opcode);        /* MISO during opcode: 0x80 marker
                                              expected in the first window
                                              after data-ready (Stlkv) */
    uint8_t h1 = spi3_xfer(0xFF);          /* the dummy byte stock discards */
    uint8_t h2 = spi3_xfer(0xFF);          /* EXP-58: stale, end of a previous block */
    {
        volatile uint8_t *hdr = (opcode == FPGA_CORE_OP_REC_A) ? fpga.acq_hdr_ch1
                                                 : fpga.acq_hdr_ch2;
        hdr[0] = s0; hdr[1] = h1; hdr[2] = h2;
    }
    if (opcode == FPGA_CORE_OP_REC_A)
        fpga.spi3_first_byte = s0;         /* debug overlay "1:" field */

    for (int i = 0; i < 1024; i++) {
        uint8_t raw = spi3_xfer(0xFF);
        if (i < 4) {
            if (opcode == FPGA_CORE_OP_REC_A) fpga.diag_ch1_raw[i] = raw;
            else                fpga.diag_ch2_raw[i] = raw;
        }
        int16_t cal = (int16_t)raw + (int16_t)FPGA_ADC_OFFSET;
        if (cal < 0)   cal = 0;
        if (cal > 255) cal = 255;
        buf[i] = (uint8_t)cal;
    }
    SPI3_CS_DEASSERT();
    return s0;
}

/* ─── Roll point reader (parity C11) ────────────────────────────────────
 * One op-0x03 frame per point, paced from the RTOS tick. Stock's TMR3 ISR
 * (0x0802E71C) queues op03 once per (ARR + 1)/10 kHz; the handler
 * (0x0803E9F2..0x0803EB38) clocks four 0xFF after the opcode and keeps
 * received byte 2 (CH1) and byte 4 (CH2). Our TMR3 is the key scan, so the
 * cadence comes from the tick instead (scope_roll.h: every stock period is a
 * whole number of ms). All five received bytes are kept for the shell, so
 * the bench can confirm which slots are live.
 *
 * Returns to the task loop at least every 10 ms so the park point above
 * still acks within fpga_acq_pause()'s budget at 800 ms per point. STOP
 * (ss->running false) reads nothing and freezes the FIFO, as stock's ISR
 * does on ms[0x2C]. A tick that falls more than a period behind (a park, a
 * STOP) resynchronises instead of bursting points that were never sampled
 * on time. */
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
    uint16_t hw_to_before = fpga.spi3_hw_timeouts;
    uint8_t rx[5];
    SPI3_CS_ASSERT();
    rx[0] = spi3_xfer(FPGA_CORE_OP_LIVE);
    for (unsigned i = 1; i < 5u; i++)
        rx[i] = spi3_xfer(0xFF);
    SPI3_CS_DEASSERT();
    for (unsigned i = 0; i < 5u; i++)
        acq_roll_stats.rx[i] = rx[i];

    if (fpga.spi3_hw_timeouts != hw_to_before) {
        acq_roll_stats.rejects++;        /* a fabricated 0xFF is not a point */
        fpga.spi3_total_timeouts++;
        return;
    }
    /* Record codes, the Y-T read's domain (raw + FPGA_ADC_OFFSET); the unit's
     * amplitude correction g = (m-28)/150 is applied in volts, exactly where
     * the Y-T trace gets it (scope_roll.h). */
    scope_roll_fifo_push(&acq_roll_fifo,
                         scope_roll_record_code((adc_raw_t){ rx[2] }),
                         scope_roll_record_code((adc_raw_t){ rx[4] }));
    acq_roll_stats.reads++;
    fpga.spi3_ok_count++;                /* the display's new-data signal */
    fpga.spi3_timeout_count = 0;

    /* Stock's roll trigger (0x0803EC2E..0x0803ED18, scope_roll.h): once the
     * FIFO is full and the mode is not AUTO, one crossing check in the middle
     * of it after every point. SINGLE stops (RUN = 0) and the full FIFO is the
     * frozen frame, the crossing at its centre; the OK key re-arms through the
     * Y-T path's own `!running` test. NORMAL empties the FIFO (stock's 32-bit
     * store at 0x08040000 zeroes the width ms[0xDB6]): the trace refills from
     * the right and the check re-arms 301 points later.
     * The level is the register in force (acq_trig_code, raw) lifted into the
     * record domain like every other comparison against a record; the source
     * and edge are the trigger settings. The column shift is 0: our trigger
     * column (ss->trig_x) places the Y-T trigger point and has no roll
     * meaning. */
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

/* The acquisition task's capture-configuration tracker (dsp/acq_cfg.h):
 * static, not on the task's 256-word stack. 36 B. */
static acq_cfg_track_t s_acq_cfg_trk;

/* One reading of the core's status block, in the acquisition task. Returns
 * true when a register was rewritten (the write restarted the capture). */
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

void fpga_acq_task(void *pv)
{
    (void)pv;
    /* EXP-43 (2026-09-14): a READ is what starts a capture (EXP-41), so a
     * triggered mode that waits for an edge before its first read waits
     * forever — NORMAL froze at level 0 on the bench with zero edges and
     * zero reads. capture_in_flight says whether a read has started a
     * capture that has not yet been consumed; when it is false in
     * NORMAL/SINGLE we do a PRIMING read (staging only, never published) to
     * start one. single_done implements SINGLE: one triggered record, then
     * hold until the mode is re-selected. */
    bool capture_in_flight = false;
    bool single_done = false;
    uint32_t last_handover_tick = 0;   /* EXP-54: tick of the last PC0-strobed read */
    uint32_t last_read_tick = 0;
    uint32_t last_commit_tick = 0;
    uint32_t polls_since_handover = 0;
    trigger_mode_t last_mode = TRIG_AUTO;
    uint32_t single_arm_seen = acq_single_arm_req;
    /* The next read's record is dropped, whatever it is: set when SINGLE is
     * (re-)armed. While we were not reading the FPGA may have finished and
     * be HOLDING a record from a crossing before the arm; its handover would
     * pass for the one-shot. Stock's SINGLE path reads the write pointer
     * instead (op09/0A, 0x080215EE); dropping one read is our equivalent. */
    bool drop_next = false;
    /* The task has not been reading records (STOP, a SINGLE record held,
     * roll): what the FPGA holds by now is from before (trig_fresh_action). */
    bool was_holding = false;
    uint32_t core_watch_tick = xTaskGetTickCount();
    /* op 09 (CAP_FORCE): the last triggered record or restart, from which
     * AUTO's budget runs; a request is out; when it was sent. */
    uint32_t last_trig_tick = xTaskGetTickCount();
    for (;;) {
        if (!fpga.initialized || fpga.bus_released) {
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
         * record stays in the buffers -- the same hold SINGLE uses. */
        scope_mask_service();
        if (scope_mask_hold_active()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Roll (C11): paced op03 points instead of the 04/05 record poll.
         * Nothing below applies -- no record, no handover, no re-arm. */
        if (acq_roll_on) {
            fpga_roll_service();
            was_holding = true;
            continue;
        }
        roll_tick_armed = false;

        /* EDGE-PACED ready wait — rewritten 2026-08-14 after the June-capture
         * re-read (analysis_v120/trigger_regime_findings_2026-08-14.md).
         *
         * Stock's runtime never reads on a LEVEL: it waits out a fresh
         * data-ready event per 04/05 pair (median 18 ms, zero partial
         * buffers in 348 windows) and sends nothing else — the paced read
         * pair itself completes the capture cycle. The old level-gated loop
         * here re-read whenever PC0 sat low, which works in the free-run
         * regime (quiet input) and collects stale/partial buffers the moment
         * real trigger crossings begin. Pace on EXINT0 falling edges instead:
         * proceed only when a data-ready edge has fired since our last pair.
         *
         * The probe-read fallback below stays, for two reasons: a STOPPED
         * engine emits no edges (the 2026-08-12 hard-gate deadlock lesson),
         * and if free-run turns out not to pulse PC0 per cycle (unmeasured —
         * the pc0_edges counter in `status` now answers this on the bench),
         * this build degrades to ~4 probe pairs/s instead of deadlocking. */
        /* Ready-wait policy branches on the scope trigger mode (Addendum 4).
         *
         *   AUTO   — free-run display. Wait only a short budget for a real
         *            trigger edge (so a triggered, stable capture is used WHEN
         *            available), then read the free-running buffer anyway. The
         *            buffer holds a coherent waveform regardless of triggering,
         *            so this updates smoothly on any input, including a quiet or
         *            below-threshold one that never pulses PC0.
         *   NORMAL — triggered display. Wait a longer budget for an edge; on
         *   /SINGLE  timeout, HOLD the last trace (skip the read) rather than
         *            overwriting it with an untriggered free-run buffer. That is
         *            the correct behaviour: no trigger => trace frozen.
         *
         * DAC1 (PA4) is the vertical OFFSET (Addendum 1), not a trigger
         * comparator reference, so it must NOT be slammed to mid on every
         * free-run poll — that would override the UI vertical-position control.
         * The defensive re-arm is now gated on a genuine dry spell (many
         * consecutive rejected reads), the only case where a dropped reference
         * is plausible; a normal AUTO free-run read is a SUCCESS, not a
         * timeout. */
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
        /* STOP holds the published record (stock: ms[0x2E] = 0 at 0x0800BE5E,
         * and a stopped timebase/range key re-renders instead of re-arming,
         * 0x0800B6AA; docs/re/parity2_static.md section 3). Before this the
         * task never tested `running` (trigger-modes.md D7): AUTO kept
         * committing free-run reads and NORMAL every handover, so a key press
         * while STOPPED drew a NEW record, and after a timebase key the next
         * commit relabelled acq_pub_idx -- the zoom-while-paused view (C14)
         * collapsed to ratio 1000 within one poll. Read nothing, like the
         * SINGLE hold above: reg 0x01 may still follow the keys, the held
         * record and its rate stay. Autoset/baseline set `running` first. */
        if (ss_acq != NULL && !ss_acq->running) {
            was_holding = true;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        /* Reading (again). A record the FPGA captured while nobody read, or
         * before SINGLE was armed, must not pass for a new one: bench
         * 2026-11-17, RUN after a STOP published one record from before the
         * STOP in 6 of 6 (generator off since), and so did leaving a finished
         * SINGLE. The core is restarted by a register write: the level, with
         * the value it has (trig_fresh_restart). */
        if (trig_fresh_restart(drop_next, was_holding)) {
            fpga_scope_write_reg(FPGA_CORE_OP_LEVEL, acq_trig_code);
            acq_fresh_restarts++;
            drop_next = false;
            /* the capture starts now: wait for it like after a hand-over,
             * and give AUTO its whole budget before a free-running read */
            const uint32_t t = xTaskGetTickCount();
            capture_in_flight = true;
            last_handover_tick = t;
            last_commit_tick = t;
            last_trig_tick = t;
        }
        was_holding = false;
        /* EXP-53/54 (2026-09-20..22) — PC0 is a HANDOVER strobe, not a
         * completion flag: it pulses 6–9 µs after an opcode-0x04 read that
         * finds a completed record (50 cycles at 0x10/0x12, never more). The
         * FPGA runs its own cycle: handover → ~189 ms hold → roll → crossing
         * of (reg-0x08 − 28), either edge → 512 post samples → HELD until the
         * next read. So this loop is a POLL, stock's 29 ms cadence: read; if
         * the read strobed PC0 its data IS the fresh record → commit; else it
         * is the rolling buffer → discard (AUTO: commit it as free-run once
         * the budget expires; NORMAL/SINGLE: hold). Sleep until the earliest
         * possible next handover (last + fill + poll-start margin), then poll
         * every acq_poll_gap_ms. Reads during the FPGA's cycle are harmless
         * (EXP-53: the cycle ran at the same rate with reads inside it).
         * This also fixes NORMAL freezing on slow signals: the single arming
         * read of EXP-47/50 landed before the crossing and nothing read again. */
        uint32_t now = xTaskGetTickCount();
        if (capture_in_flight) {
            uint32_t earliest = last_handover_tick + fpga_acq_post_edge_get();
            /* Sleep in slices and leave for the park point as soon as a
             * pause is asked: at a slow timebase the fill is seconds long
             * (2 s at 500 S/s), longer than fpga_acq_pause()'s 1 s budget, so
             * a register write (timebase, trigger source, level) was refused
             * about every other time (bench 2026-11-14, tb 0x14). Nothing is
             * read on the way out; the wait is taken up again after the
             * resume, from the same last_handover_tick. */
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
            spi3_set_br(acq_read_br);                       /* EXP-46 (b) */
        /* Ask our core what it holds (never between the op 04 and the op 05
         * of a pair, never a design that has not identified itself). */
        if ((uint32_t)(xTaskGetTickCount() - core_watch_tick) >= pdMS_TO_TICKS(ACQ_CORE_WATCH_MS)) {
            core_watch_tick = xTaskGetTickCount();
            if (fpga_core_watch_poll())
                continue;                                   /* a register was rewritten: capture restarted */
        }
        /* AUTO and no trigger for a whole budget, on a core with op 09: ask
         * it to hold a record as it is, just before reading. Between polls
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
        uint32_t edges_before = fpga.pc0_edges;
        uint16_t hw_to_before = fpga.spi3_hw_timeouts;
        /* Reads land in staging, not the published buffers (P0.2): a frame
         * the gate below rejects never reaches the display, and an accepted
         * one is committed atomically in fpga_acq_frames_commit(). */
        volatile uint8_t *w1 = acq_write_ch1();
        volatile uint8_t *w2 = acq_write_ch2();
        uint8_t s1 = fpga_acq_read_channel(FPGA_CORE_OP_REC_A, w1);
        uint8_t s2 = fpga_acq_read_channel(FPGA_CORE_OP_REC_B, w2);
        last_read_tick = xTaskGetTickCount();
        fpga_acq_cfg_snapshot(&cfg_now);                 /* ...and after it */
        (void)acq_cfg_note(&s_acq_cfg_trk, &cfg_now, last_read_tick * portTICK_PERIOD_MS);
        /* The core says it in the reply itself: the op 05 header carries the
         * seam (V set) exactly when the op 04 before it found a held record
         * and froze it for the pair (fpga/core/rtl/cmd.sv), and the record
         * was still held when op 05 began. PC0 signals the same; it is only
         * compared, for the bench (`fpga core`). */
        bool triggered = trig_seam_from_hdr(fpga.acq_hdr_ch2[FPGA_CORE_SEAM_I_HI], fpga.acq_hdr_ch2[FPGA_CORE_SEAM_I_LO]) >= 0;
        {
            const bool pc0 = (fpga.pc0_edges != edges_before);
            acq_kind_n[triggered ? 0 : 1]++;
            if (triggered && !pc0) acq_kind_n[2]++;
            if (!triggered && pc0) acq_kind_n[3]++;
        }
        /* `held`: the read is a record the FPGA held and handed over. On a
         * core with op 09 it may have been held on request (forced) instead
         * of by a trigger: handed over, time-ordered and simultaneous like
         * any record, but not `triggered`. */
        const bool held = triggered;
        const bool forced = held && trig_hdr_forced(fpga.acq_hdr_ch2[FPGA_CORE_SEAM_I_HI]);
        if (forced) { triggered = false; acq_force_n[1]++; }
        if (triggered) last_trig_tick = last_read_tick;
        acq_ilcal_feed(w1, w2, fpga.spi3_hw_timeouts == hw_to_before);
        bool first_read = !capture_in_flight;
        capture_in_flight = true;
        if (held || first_read) {
            last_handover_tick = last_read_tick;
            fpga.acq_polls_last = polls_since_handover;
            polls_since_handover = 0;
        } else {
            polls_since_handover++;
            fpga.acq_poll_reads++;
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
        /* Capture configuration (dsp/acq_cfg.h, bench 2026-11-14): a record
         * whose samples may predate the last change of routing, range,
         * coupling, position, probe, timebase, trigger, interleave or roll,
         * or overlap the settling after it, is dropped -- neither
         * configuration describes it. The published record (screen and
         * stream) stays the last good one, with its own configuration. */
        if (acq_cfg_settle_cmd != 0xFFFFu &&
            !acq_cfg_admit(&s_acq_cfg_trk, last_read_tick * portTICK_PERIOD_MS,
                           acq_cfg_fill_ms(scope_timebase_sample_rate(acq_rate_idx)),
                           acq_poll_gap_ms, acq_cfg_settle_cmd ? acq_cfg_settle_cmd : ACQ_CFG_SETTLE_MS)) {
            acq_cfg_drops++;
            continue;
        }
        /* Anchor the success flags on frame validity — a fully dead bus
         * reads 0xFF everywhere (pull-up idle / spi3_xfer timeout), which
         * after the -28 offset is a plausible flat 227, and one spurious
         * PC0 glitch would otherwise latch the scope UI's demo-kill flag on
         * garbage (the same unanchored-measurement trap as the /2 reads).
         * Accept if either window carried the 0x80 data-ready marker, or the
         * CH1 buffer is non-constant (the marker appears only in the FIRST
         * window after data-ready per Stlkv, so a marker-only gate could
         * false-negative; varying data is accepted on its own merits). */
        bool marker = (s1 == FPGA_CORE_REPLY_FIRST) || (s2 == FPGA_CORE_REPLY_FIRST);
        bool varies = false;
        for (int i = 1; i < 1024; i++) {
            if (w1[i] != w1[0]) { varies = true; break; }
        }
        /* The old third gate term tested acq_hdr_ch1[FPGA_CORE_SEAM_I_LO]==0x01 as a stock
         * "buffer-valid" flag. Refuted 2026-08-15 by stock's own dispatch:
         * stock discards only two bytes, so that byte is sample[0], and on
         * this board it reads mid-scale (0x7C/0x79) and never 0x01. The term
         * was therefore always false here — dropping it changes nothing. */
        /* Third gate term (audit 2026-08-20, P0.3): the marker/varies gate
         * catches an all-0xFF dead bus, but a poll expiry PARTWAY through a
         * read yields a frame that is half real data, half fabricated 0xFF —
         * which "varies" and would pass. If spi3_hw_timeouts moved during the
         * two reads, at least one byte is fabricated: reject. */
        if ((marker || varies) && fpga.spi3_hw_timeouts == hw_to_before) {
            fpga.acq_last_rot = fpga_acq_unrotate_staging(held);   /* dev plan 2.3: seam-based */
            acq_stage_ordered = acq_unrotate && fpga.acq_last_rot >= 0;
            /* a forced record is in time order but has no trigger at 512 */
            acq_stage_ordered_core = acq_stage_ordered && acq_rot_from_hdr && !forced;
            acq_stage_il = held && fpga_interleave_active() && acq_stage_ch1 && acq_stage_ch2;
            acq_stage_kind = (uint8_t)kind;
            if (acq_stage_il)
                interleave_merge(acq_stage_ch1, acq_stage_ch2, &acq_il_corr);
            acq_stage_cfg = s_acq_cfg_trk.last;          /* what it was captured with */
            fpga_acq_frames_commit();
            /* Judge (or teach from) the record just committed: exactly once,
             * from the published buffers, in the only task that writes them. */
            scope_mask_on_commit(fpga.ch1_buf, fpga.ch2_buf, triggered,
                                 acq_stage_ordered, fpga_acq_frame_generation());
            last_commit_tick = last_read_tick;
            fpga.spi3_ok_count++;
            fpga.spi3_timeout_count = 0;
            data_ready = true;
            if (triggered)
                fpga.acq_last_latency_ms = xTaskGetTickCount() - fpga.pc0_last_tick;
            if (triggered)
                acq_last_trig_tick = last_commit_tick;
            if (mode_now == TRIG_SINGLE && triggered) {
                single_done = true;            /* one triggered record, then hold */
                acq_single_done_pub = true;
            }
        } else {
            fpga.spi3_total_timeouts++;    /* rejected frame — shows in TO: */
            /* Dry-spell recovery only: after a run of rejected reads (a dead
             * bus / dropped reference, not the normal free-run path), restore
             * DAC1 to mid so a lost vertical reference cannot silently freeze
             * capture. Rare by construction, so it does not fight the UI
             * vertical-position control the way a per-poll re-arm would. */
            if (++fpga.spi3_timeout_count >= 32) {
                /* Mid-scale without a usable cal page (as before); with one,
                 * the calibrated codes, so recovery does not silently undo
                 * the vertical position. */
                scope_vpos_recover();
                fpga.spi3_timeout_count = 0;
            }
        }

        /* Bound the read rate lightly; the display's own 50 ms frame loop
         * caps rendering, so reading faster than it draws only costs SPI
         * time (~275 µs per CH1+CH2 pair at /2). */
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* See fpga.h. The acquisition task reads SPI3 continuously, so every shell
 * command that uses the bus parks it here first. */
bool fpga_acq_pause(void)
{
    if (acq_task_handle == NULL)
        return true;
    acq_pause_req = true;
    acq_pause_ack = false;      /* demand a FRESH ack from the park point */
    /* Worst-case ack latency = one full pass of the acquisition loop: the
     * trigger-mode edge wait (≤FPGA_NORMAL_TRIG_WAIT_MS) + two 1026-byte
     * reads + 10 ms. 1 s is a comfortable margin. */
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
 * Reconcile the display's timebase with the register the arm block just wrote.
 * Before 2026-08-19 nothing did this, so a stock boot came up with the display
 * on 0x0A and the hardware on 0x08 and no way to notice.
 *
 * MUST be called from EVERY arm path. There are two — the hardware-SPI one in
 * fpga_spi3_config_sequence() and the bit-bang one under FPGA_CONFIG_B_ARM —
 * and the first attempt patched only the first, which is dead code in the
 * shipping guest-coldtrace build. The device reported the mismatch unchanged;
 * that is the whole reason `fpga scope freq` prints both codes.
 *
 * The arm sequences themselves are NOT touched: they are the bench-proven
 * 2026-08-13 cold-boot bring-up and stay byte-identical. This runs after, and
 * only in one of two directions:
 *
 *   - persisted code has a MEASURED rate -> push it to the hardware, so a
 *     user's timebase choice survives a power cycle;
 *   - persisted code has no rate         -> pull the display down to the 0x08
 *     the arm block wrote. Neither code can be labelled, so moving the
 *     hardware would be change for its own sake on the one path that took
 *     months to get working.
 *
 * Caller must hold the SPI3 bus with a clock the FPGA accepts for writes.
 *
 * ALSO called from main() immediately after settings_store_init(). That is not
 * belt-and-braces, it is required: settings_store_init() runs AFTER fpga_init()
 * — deliberately, so the bench-validated config sequence keeps the electrical
 * environment it was validated in — and it overwrites timebase_idx with the
 * persisted value. So the in-fpga_init call can never win, and on the first
 * flash it did not: the reconcile ran, reported "pulled", and the display still
 * read 0x0A. The call after the restore is the authoritative one; the one in
 * the arm path keeps acq_rate_idx honest about what the arm block wrote in
 * case no settings record exists.
 *
 * Safe to call pre-scheduler: it takes no lock, and every caller runs before
 * fpga_create_tasks(), so no acquisition task can be mid-frame on SPI3.
 */
/* 0 = never ran, 1 = pushed the display's code to the FPGA, 2 = pulled the
 * display down to the arm block's 0x08. Reported by `status`.
 *
 * This exists because the first version of this fix was placed in ONE of the
 * two arm paths and the other is what the shipping build uses. It compiled,
 * it was correct, and it never executed. "Did this code path run" was only
 * answerable by inference from a symptom; now it is a readable fact. */
static volatile uint8_t fpga_tb_reconcile_action;

uint8_t fpga_timebase_reconcile_action(void)
{
    return fpga_tb_reconcile_action;
}

void fpga_reconcile_timebase_after_arm(void)
{
    scope_state_t *ss = scope_state_get();

    /* Roll (C11) survives a re-arm: the arm burst leaves reg 0x01 at its own
     * value, so put back the roll code the FPGA was given, not the Y-T one. */
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

/* Trigger sibling of the reconcile above (EXP-41): the arm burst leaves reg
 * 0x08 at stock's captured 0xAD, above every bench signal this project has
 * applied; hand it to the (possibly restored) UI level. Runs before the acq
 * task exists, so a bare write is correct here, as for the timebase. */
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
    return fpga.initialized ? fpga.ch1_buf : NULL;
}

const volatile uint8_t *fpga_get_ch2_buf(void)
{
    return fpga.initialized ? fpga.ch2_buf : NULL;
}
