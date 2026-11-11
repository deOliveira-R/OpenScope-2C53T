/*
 * scope_selfcal_run.c — device glue for autoset, baseline correction and
 * 50 % centre (parity C9/C10). See scope_selfcal_run.h for the threading
 * model and src/ui/scope_selfcal.h for the decisions.
 *
 * FRESH RECORDS (ADDENDUM_B S15, done here rather than trusted to timing)
 * ---------------------------------------------------------------------------
 * After anything that moves the analog front end — an offset code, a relay,
 * the timebase, the channel routing — the service waits >= SETTLE_MS (stock
 * reopens its servo gate after 39-40 ticks of ~10 ms, 0x08040580), then asks
 * for TWO commits counted from the end of the settle: the first may have
 * been read before the settle ended (the record in flight is dropped), the
 * second was read after it. A step that changed nothing needs one commit.
 * Commits are counted with the acquisition's seqlock generation (+2 per
 * commit), the same recipe `fpga scope center` was fixed with on 2026-09-22;
 * a read during a commit is retried. No fresh record within RECORD_TIMEOUT_MS
 * ends the run and restores the user's settings.
 *
 * The acquisition task keeps running throughout: a paused task freezes the
 * buffer (the `fpga scope center` median comment). The trigger mode is
 * forced to AUTO for the run because NORMAL/SINGLE hold the last record
 * instead of committing a free-run one on a quiet input.
 */
#include "scope_selfcal_run.h"
#include "scope_geom.h"

#include <stdio.h>
#include <string.h>

#include "task.h"

#include "fpga.h"
#include "scope_vpos.h"
#include "scope_cal.h"
#include "scope_record.h"
#include "scope_selfcal.h"
#include "scope_state.h"
#include "scope_timebase.h"
#include "settings_store.h"
#include "ui.h"

#define SETTLE_MS           450u    /* >= 400 ms: S15, and the bench's ~350 ms */
#define POPUP_EVERY_MS      1000u

/* Autoset's timebase span: 0x04 is the fastest rate (0x00..0x03 share it and
 * switch to the interleaved bank-2 regime with one channel); 0x13 is the
 * slowest before the roll codes, whose records take seconds. */
#define AS_TB_MIN    0x04u
#define AS_TB_MAX    0x13u
#define AS_TB_PROBE  0x10u   /* 12.5 kS/s: a 72 ms record holds 3+ mains periods */
#define AS_WINDOW    320u    /* samples across the screen at 1 sample per pixel */

enum { REQ_NONE = 0, REQ_AUTOSET, REQ_BASECAL, REQ_CENTER50 };
enum { W_NONE = 0, W_SETTLE, W_COMMITS };

typedef struct {
    uint8_t        range[2];
    int16_t        pos[2];
    bool           en[2];
    coupling_t     coupling[2];
    uint8_t        tb;
    trigger_mode_t trig_mode;
    trigger_source_t trig_src;
    int16_t        trig_level;
    bool           running;
    int16_t        trig_x;
    bool           interleave;
} snap_t;

static struct {
    volatile uint8_t req;
    volatile uint8_t req_banks;
    volatile bool    cancel;
    uint8_t    kind;
    uint8_t    last_kind;
    uint8_t    wait;
    TickType_t t_mark;
    uint32_t   gen_target;
    snap_t     snap;
    union {
        sc_autoset_t as;
        sc_basecal_t bc;
    } e;
    uint8_t    applied_phase;
    uint8_t    applied_range;
    TickType_t t_popup;
    const char *result;
    uint32_t   records, timeouts, torn;
} R = { .result = "" };

static channel_state_t *chan(unsigned i)
{
    scope_state_t *ss = scope_state_get();
    return i ? &ss->ch2 : &ss->ch1;
}

static void popup(QueueHandle_t dq, const char *t)
{
    scope_show_popup(t);
    if (dq != NULL) {
        uint8_t cmd = DCMD_REDRAW_ALL;
        (void)xQueueSend(dq, &cmd, 0);
    }
}

/* ── Requests ──────────────────────────────────────────────────────── */
bool scope_selfcal_busy(void)
{
    return R.kind != SCR_IDLE || R.req == REQ_AUTOSET || R.req == REQ_BASECAL;
}

static bool request(uint8_t what, uint8_t banks)
{
    if (scope_selfcal_busy() || R.req != REQ_NONE) return false;
    R.req_banks = banks;
    R.req = what;
    return true;
}

bool scope_selfcal_request_autoset(void)          { return request(REQ_AUTOSET, 0u); }
bool scope_selfcal_request_basecal(uint8_t banks) { return request(REQ_BASECAL, banks & 7u); }
bool scope_selfcal_request_center50(void)         { return request(REQ_CENTER50, 0u); }
void scope_selfcal_request_cancel(void)           { R.cancel = true; }

/* ── State snapshot ────────────────────────────────────────────────── */
static void snap_take(void)
{
    const scope_state_t *ss = scope_state_get();
    for (unsigned i = 0; i < 2u; i++) {
        const channel_state_t *c = i ? &ss->ch2 : &ss->ch1;
        R.snap.range[i] = c->vdiv_idx;
        R.snap.pos[i] = c->position;
        R.snap.en[i] = c->enabled;
        R.snap.coupling[i] = c->coupling;
    }
    R.snap.tb = ss->timebase_idx;
    R.snap.trig_mode = ss->trigger.mode;
    R.snap.trig_src = ss->trigger.source;
    R.snap.running = ss->running;
    R.snap.trig_level = ss->trigger.level;
    R.snap.trig_x = ss->trig_x;
    R.snap.interleave = fpga_interleave_get();
}

static void set_tb(uint8_t tb)
{
    scope_state_t *ss = scope_state_get();
    if (ss->timebase_idx == tb && fpga_acq_rate_idx_get() == tb) return;
    ss->timebase_idx = tb;
    (void)fpga_apply_timebase(tb);
}

static void set_range(unsigned i, uint8_t r)
{
    channel_state_t *c = chan(i);
    c->vdiv_idx = r;
    (void)fpga_apply_vdiv((uint8_t)(i + 1u), r);
}

/* Put back what the user had: routing, timebase, ranges, positions,
 * coupling, trigger mode/level, interleave. Overrides first, so every
 * re-apply below lands on the (now corrected) page codes. */
static void snap_restore(void)
{
    scope_state_t *ss = scope_state_get();
    scope_vpos_override(1u, false, 0u);
    scope_vpos_override(2u, false, 0u);
    fpga_interleave_set(R.snap.interleave);
    ss->ch1.enabled = R.snap.en[0];
    ss->ch2.enabled = R.snap.en[1];
    (void)fpga_apply_channel_enables();
    set_tb(R.snap.tb);
    for (unsigned i = 0; i < 2u; i++) {
        channel_state_t *c = chan(i);
        c->position = R.snap.pos[i];
        set_range(i, R.snap.range[i]);
        c->coupling = R.snap.coupling[i];
        (void)fpga_apply_coupling((uint8_t)i, c->coupling);
    }
    ss->trigger.mode = R.snap.trig_mode;
    ss->trigger.source = R.snap.trig_src;   /* a CH2-only phase moves it */
    ss->trigger.level = R.snap.trig_level;
    ss->running = R.snap.running;
    ss->trig_x = R.snap.trig_x;
    (void)fpga_apply_trigger_level(ss->trigger.level);
    scope_vpos_apply();
}

/* ── Waiting for a fresh record ────────────────────────────────────── */
static void wait_fresh(bool settle)
{
    R.t_mark = xTaskGetTickCount();
    if (settle) {
        R.wait = W_SETTLE;
    } else {
        R.wait = W_COMMITS;
        R.gen_target = (fpga_acq_frame_generation() & ~1u) + 2u;
    }
}

/* True once a record read after the last change is in the buffers. */
static bool fresh_ready(void)
{
    const TickType_t now = xTaskGetTickCount();
    if (R.wait == W_SETTLE) {
        if ((now - R.t_mark) < pdMS_TO_TICKS(SETTLE_MS)) return false;
        R.wait = W_COMMITS;
        R.t_mark = now;
        R.gen_target = (fpga_acq_frame_generation() & ~1u) + 4u;   /* two commits */
        return false;
    }
    return (int32_t)((fpga_acq_frame_generation() & ~1u) - R.gen_target) >= 0;
}

/* With no trigger crossing AUTO commits one free-run record per fallback
 * budget, 3 x fill + 230 ms (fpga.c; 475 ms at 0x10, ~1.5 s at 0x12, ~2.7 s
 * at 0x13). Two of them plus a second of slack; a code with no rate gets the
 * slow-code figure. */
static uint32_t record_timeout_ms(void)
{
    const float fs = scope_timebase_sample_rate(fpga_acq_rate_idx_get());
    const float fill_ms = (fs > 0.0f) ? 1000.0f * (float)FPGA_ADC_BUF_SIZE / fs : 1000.0f;
    return 2u * (uint32_t)(3.0f * fill_ms + 500.0f) + 1000u;
}

static bool timed_out(void)
{
    return R.wait == W_COMMITS &&
           (xTaskGetTickCount() - R.t_mark) > pdMS_TO_TICKS(record_timeout_ms());
}

/* A run changes saved settings (ranges, timebase, positions, trigger) from
 * this service, after the key's own settings hook has already run: tell the
 * store, exactly as settings_after_button() does for a key. */
static void settings_changed(void)
{
    const uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    settings_store_note_change(now);
    (void)settings_store_service(now);
}

/* Reduce both channel records; false if a commit landed meanwhile. */
static bool read_obs(sc_rec_stats_t obs[2])
{
    const uint32_t g1 = fpga_acq_frame_generation();
    if (g1 & 1u) return false;
    const volatile uint8_t *b1 = fpga_get_ch1_buf();
    const volatile uint8_t *b2 = fpga_get_ch2_buf();
    if (b1 == NULL || b2 == NULL) return false;
    /* The buffers only change inside a commit, which the generation
     * brackets, so reading them as plain bytes between two equal even
     * generations is the documented tear check (fpga.h). */
    (void)sc_rec_stats((const uint8_t *)b1, FPGA_ADC_BUF_SIZE, SCOPE_RECORD_HEAD_SKIP, &obs[0]);
    (void)sc_rec_stats((const uint8_t *)b2, FPGA_ADC_BUF_SIZE, SCOPE_RECORD_HEAD_SKIP, &obs[1]);
    if (fpga_acq_frame_generation() != g1) {
        R.torn++;
        return false;
    }
    R.records++;
    return true;
}

/* ── 50 % centre ───────────────────────────────────────────────────── */
static void do_center50(QueueHandle_t dq)
{
    scope_state_t *ss = scope_state_get();
    int16_t pos[2];
    int lvl = sc_center50(ss->ch1.enabled, ss->ch2.enabled,
                          ss->trigger.source == TRIG_SRC_CH2 ? 1u : 0u, pos);
    /* Without the page the position does not move the trace (scope_vpos is
     * inactive), so 0 V is not at raw 128 + pos: keep the level at mid. */
    if (!scope_vpos_active()) lvl = 0;
    (void)scope_vpos_set(1u, pos[0]);
    (void)scope_vpos_set(2u, pos[1]);
    ss->trig_x = SCOPE_GEOM_CENTER_COL;          /* stock zeroes the horizontal offsets */
    ss->trigger.level = (int16_t)lvl;
    const bool ok = fpga_apply_trigger_level(lvl);
    popup(dq, ok ? "50% centred" : "50% centred, trig NOT SET");
    settings_changed();
}

/* ── Autoset ───────────────────────────────────────────────────────── */
static void as_apply(void)
{
    const sc_autoset_t *a = &R.e.as;
    set_tb(a->tb);
    for (unsigned i = 0; i < 2u; i++) {
        if (!a->cfg.ch_on[i]) continue;
        channel_state_t *c = chan(i);
        if (a->cfg.pos_volts && c->position != a->pos[i]) c->position = a->pos[i];
        if (c->vdiv_idx != a->range[i]) set_range(i, a->range[i]);   /* re-applies vpos */
        else scope_vpos_apply_ch((uint8_t)(i + 1u), c->vdiv_idx);
    }
}

static bool as_start(QueueHandle_t dq)
{
    scope_state_t *ss = scope_state_get();
    if (current_mode != MODE_OSCILLOSCOPE) return false;
    snap_take();
    sc_as_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.ch_on[0] = ss->ch1.enabled;
    cfg.ch_on[1] = ss->ch2.enabled;
    cfg.pos_volts = scope_vpos_active();
    /* Without the page r0..r3 have no usable gain (scope_cal.c: they rail);
     * autoset does not land on a range it cannot label. */
    cfg.range_min = 0u;
    while (cfg.range_min < SCOPE_CAL_RANGE_COUNT - 1u &&
           !(scope_cal_true_scale_ok(1u, cfg.range_min) && scope_cal_true_scale_ok(2u, cfg.range_min)))
        cfg.range_min++;
    cfg.mv = scope_cal_nominal_table();
    cfg.rate = scope_timebase_rate_table();
    cfg.tb_min = AS_TB_MIN;
    cfg.tb_max = AS_TB_MAX;
    cfg.tb_probe = AS_TB_PROBE;
    cfg.window = AS_WINDOW;
    cfg.trig_src = (ss->trigger.source == TRIG_SRC_CH2) ? 1u : 0u;
    uint8_t r[2] = { ss->ch1.vdiv_idx, ss->ch2.vdiv_idx };
    int16_t p[2] = { ss->ch1.position, ss->ch2.position };
    sc_autoset_begin(&R.e.as, &cfg, r, p, ss->timebase_idx);
    /* Stock forces ms[0] = ms[2] = 0 at 0x0800CC38 before its walk. Ours:
     * AUTO commits free-run records, and RUN so the screen follows. */
    ss->trigger.mode = TRIG_AUTO;
    ss->running = true;
    R.kind = SCR_AUTOSET;
    wait_fresh(true);
    popup(dq, "Autoset...");
    R.t_popup = xTaskGetTickCount();
    return true;
}

static void as_end(QueueHandle_t dq, sc_as_status_t st)
{
    scope_state_t *ss = scope_state_get();
    sc_autoset_t *a = &R.e.as;
    if (st == SC_AS_NO_SIGNAL) {
        snap_restore();
        R.result = "no signal";
        popup(dq, "Autoset: no signal");
    } else {
        as_apply();
        if (st == SC_AS_DONE) {
            ss->trigger.source = a->trig_ch ? TRIG_SRC_CH2 : TRIG_SRC_CH1;
            ss->trigger.level = a->trig_level;
            (void)fpga_apply_trigger_level(a->trig_level);
            ss->trig_x = SCOPE_GEOM_CENTER_COL;
        }
        const bool over = a->res[0] == SC_ASC_OVER || a->res[1] == SC_ASC_OVER;
        R.result = (st == SC_AS_DC) ? "DC only" : over ? "done, over range" : "done";
        popup(dq, (st == SC_AS_DC) ? "Autoset: DC level" :
                  over ? "Autoset: over range" : "Autoset done");
    }
    R.last_kind = SCR_AUTOSET;
    R.kind = SCR_IDLE;
    settings_changed();
}

static void as_service(QueueHandle_t dq)
{
    sc_rec_stats_t obs[2];
    if (!read_obs(obs)) return;
    const sc_as_status_t st = sc_autoset_step(&R.e.as, obs);
    if (st != SC_AS_RUNNING) {
        as_end(dq, st);
        return;
    }
    as_apply();
    wait_fresh(true);
}

/* ── Baseline correction ───────────────────────────────────────────── */
static bool bc_pair(void *ctx, uint8_t ch, uint8_t bank, uint8_t range,
                    uint16_t *base, uint16_t *upper)
{
    (void)ctx;
    return scope_vpos_page_pair((uint8_t)(ch + 1u), bank, range, base, upper);
}

static void bc_commit(void *ctx, uint8_t ch, uint8_t bank, uint8_t range,
                      int16_t d_base, int16_t d_upper)
{
    (void)ctx;
    (void)scope_vpos_corr_set((uint8_t)(ch + 1u), bank, range, d_base, d_upper);
}

static void bc_apply(void)
{
    const sc_basecal_t *b = &R.e.bc;
    scope_state_t *ss = scope_state_get();
    if (R.applied_phase != b->phase) {
        const sc_bc_phase_t *p = &sc_bc_phases[b->phase];
        ss->ch1.enabled = (p->mask & 1u) != 0u;
        ss->ch2.enabled = (p->mask & 2u) != 0u;
        (void)fpga_apply_channel_enables();
        set_tb(p->tb);
        R.applied_phase = b->phase;
        R.applied_range = 0xFFu;
    }
    /* Codes first, so the range's re-apply writes the servo's code. A
     * channel outside this phase's routing (bank 2 is one channel at a time)
     * keeps its page code rather than a stale servo code. */
    const uint8_t mask = sc_bc_phases[b->phase].mask;
    for (unsigned i = 0; i < 2u; i++)
        scope_vpos_override((uint8_t)(i + 1u), ((mask >> i) & 1u) != 0u, b->dac[i]);
    if (R.applied_range != b->range) {
        set_range(0u, b->range);
        set_range(1u, b->range);
        R.applied_range = b->range;
    }
}

static bool bc_start(QueueHandle_t dq, uint8_t banks)
{
    scope_state_t *ss = scope_state_get();
    if (current_mode != MODE_OSCILLOSCOPE) return false;
    if (!scope_vpos_active()) {
        R.result = "refused: no calibration page in use";
        popup(dq, "Baseline: no cal page");
        return false;
    }
    if (banks == 0u) banks = 0x03u;
    snap_take();
    /* DC relays (stock's cal setup), AUTO, and no interleave merge: bank 2
     * is servoed on the channel's own converter. */
    for (unsigned i = 0; i < 2u; i++) {
        chan(i)->coupling = COUPLING_DC;
        (void)fpga_apply_coupling((uint8_t)i, COUPLING_DC);
    }
    ss->trigger.mode = TRIG_AUTO;
    ss->running = true;
    fpga_interleave_set(false);
    sc_bc_cfg_t cfg = { banks, bc_pair, bc_commit, NULL };
    sc_basecal_begin(&R.e.bc, &cfg);
    R.applied_phase = 0xFFu;
    R.applied_range = 0xFFu;
    R.kind = SCR_BASECAL;
    bc_apply();
    wait_fresh(true);
    popup(dq, "Baseline: remove probe");
    R.t_popup = xTaskGetTickCount();
    return true;
}

static void bc_end(QueueHandle_t dq, sc_bc_status_t st)
{
    snap_restore();
    static char msg[24];
    const sc_basecal_t *b = &R.e.bc;
    switch (st) {
    case SC_BC_NOT_QUIET: popup(dq, "Remove probe! Refused"); break;
    case SC_BC_DC_INPUT:  popup(dq, "DC on input! Refused");  break;
    case SC_BC_FAILED:    popup(dq, "Baseline failed");       break;
    case SC_BC_OPEN:      popup(dq, "Input open/noisy!");     break;
    default:
        if (b->open_ch[0] || b->open_ch[1])   /* corrected the others, say which was refused */
            snprintf(msg, sizeof(msg), "CH%s open, ok %u",
                     (b->open_ch[0] && b->open_ch[1]) ? "1+2" : (b->open_ch[0] ? "1" : "2"),
                     (unsigned)b->n_ok);
        else
            snprintf(msg, sizeof(msg), "Baseline ok %u bad %u", (unsigned)b->n_ok, (unsigned)b->n_fail);
        popup(dq, msg);
        break;
    }
    R.result = sc_bc_status_str(st);
    R.last_kind = SCR_BASECAL;
    R.kind = SCR_IDLE;
}

static void bc_service(QueueHandle_t dq)
{
    sc_rec_stats_t obs[2];
    if (!read_obs(obs)) return;
    bool fresh = false;
    const sc_bc_status_t st = sc_basecal_step(&R.e.bc, obs, &fresh);
    if (st != SC_BC_RUNNING) {
        bc_end(dq, st);
        return;
    }
    bc_apply();
    wait_fresh(fresh);
}

/* ── The service ───────────────────────────────────────────────────── */
static void progress(QueueHandle_t dq)
{
    const TickType_t now = xTaskGetTickCount();
    if ((now - R.t_popup) < pdMS_TO_TICKS(POPUP_EVERY_MS)) return;
    R.t_popup = now;
    if (R.kind == SCR_AUTOSET) {
        popup(dq, "Autoset...");
    } else {
        static char msg[24];
        const sc_basecal_t *b = &R.e.bc;
        if (sc_basecal_prechecking(b))
            snprintf(msg, sizeof(msg), "Baseline: checking");
        else
            snprintf(msg, sizeof(msg), "Baseline b%u r%u",
                     (unsigned)sc_bc_phases[b->phase].bank, (unsigned)b->range);
        popup(dq, msg);
    }
}

void scope_selfcal_service(QueueHandle_t dq)
{
    if (R.cancel) {
        R.cancel = false;
        if (R.req == REQ_AUTOSET || R.req == REQ_BASECAL) R.req = REQ_NONE;
        if (R.kind != SCR_IDLE) {
            snap_restore();
            R.result = "cancelled";
            R.last_kind = R.kind;
            R.kind = SCR_IDLE;
            popup(dq, "Cancelled");
        }
        return;
    }
    const uint8_t req = R.req;
    if (req != REQ_NONE) {
        R.req = REQ_NONE;
        if (R.kind == SCR_IDLE) {
            R.records = 0;
            if (req == REQ_CENTER50) do_center50(dq);
            else if (req == REQ_AUTOSET) (void)as_start(dq);
            else if (req == REQ_BASECAL) (void)bc_start(dq, R.req_banks);
        }
        return;
    }
    if (R.kind == SCR_IDLE) return;

    if (!fresh_ready()) {
        if (timed_out()) {
            R.timeouts++;
            snap_restore();
            R.result = "no fresh record (timeout)";
            R.last_kind = R.kind;
            R.kind = SCR_IDLE;
            popup(dq, "No records: stopped");
            return;
        }
        progress(dq);
        return;
    }
    if (R.kind == SCR_AUTOSET) as_service(dq);
    else                       bc_service(dq);
}

void scope_selfcal_get_status(scope_selfcal_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = (scr_kind_t)R.kind;
    out->last_kind = (scr_kind_t)R.last_kind;
    out->last_result = R.result;
    out->records = R.records;
    out->timeouts = R.timeouts;
    out->torn = R.torn;
    out->corr_count = (uint16_t)scope_vpos_corr_count();
    /* The engines share storage: the basecal fields mean something only
     * while it runs or when it was the last run. */
    if (R.kind == SCR_BASECAL || (R.kind == SCR_IDLE && R.last_kind == SCR_BASECAL)) {
        const sc_basecal_t *b = &R.e.bc;
        out->bank = (b->phase < 4u) ? sc_bc_phases[b->phase].bank : 0u;
        out->range = b->range;
        out->prechecking = sc_basecal_prechecking(b);
        out->n_ok = b->n_ok;
        out->n_fail = b->n_fail;
        out->last_fail = sc_bc_fail_str(b->last_fail);
        out->last_fail_ch = b->last_fail_ch;
        out->last_fail_bank = b->last_fail_bank;
        out->last_fail_range = b->last_fail_range;
        out->open_mask = (uint8_t)((b->open_ch[0] ? 1u : 0u) | (b->open_ch[1] ? 2u : 0u));
    }
}

bool scope_selfcal_basecal_entry(uint8_t ch, uint8_t bank, uint8_t range,
                                 uint8_t *state, char *text, unsigned text_len)
{
    if (!(R.kind == SCR_BASECAL || (R.kind == SCR_IDLE && R.last_kind == SCR_BASECAL)))
        return false;
    uint8_t st, sp, dr;
    if (!sc_basecal_entry(&R.e.bc, ch, bank, range, &st, &sp, &dr)) return false;
    if (state) *state = st;
    if (text && text_len) {
        text[0] = '\0';
        if (st >= SC_BC_LOG_FAIL0) {
            const sc_bc_fail_t f = (sc_bc_fail_t)(st - SC_BC_LOG_FAIL0);
            if (sp == 255u)
                snprintf(text, text_len, "%s: >%u%% of samples off by >%d counts, drift %u.%02u",
                         sc_bc_fail_str(f), (unsigned)SC_TRIM_MAX_PCT, (int)SC_TRIM,
                         (unsigned)(dr / 4u), (unsigned)(dr % 4u) * 25u);
            else
                snprintf(text, text_len, "%s: spread %u (limit %u), drift %u.%02u",
                         sc_bc_fail_str(f), (unsigned)sp, sc_bc_flat_limit(range),
                         (unsigned)(dr / 4u), (unsigned)(dr % 4u) * 25u);
        }
    }
    return true;
}
