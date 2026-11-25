/*
 * OpenScope 2C53T - acquisition from the capture core (fpga_acq.c)
 *
 * The acquisition task reads records from the core over SPI3 and publishes
 * them to the display. A record is FPGA_ADC_BUF_SIZE samples of each
 * converter. This header also has the settings that are registers of the
 * core (timebase, trigger), the single-channel interleave and roll.
 */
#ifndef FPGA_ACQ_H
#define FPGA_ACQ_H

#include <stdint.h>
#include <stdbool.h>
#include "../dsp/interleave_cal.h"
#include "../ui/scope_roll.h"
#include "../dsp/acq_cfg.h"
#include "../dsp/core_ident.h"
#include "../dsp/core_status.h"

#define FPGA_ADC_BUF_SIZE     1024   /* samples of one converter in a record */

/* Converter code to record unit offset. New code uses rec_from_raw() and
 * ADC_REC_OFFSET (util/units.h); `make check-adc-offset` counts the uses left. */
#define FPGA_ADC_OFFSET       (-28.0f)

/* ── the published record ───────────────────────────────────────────── */

/* A record has been published since boot. */
bool fpga_data_ready(void);
/* The published record's samples (NULL before fpga_init). */
const volatile uint8_t *fpga_get_ch1_buf(void);
const volatile uint8_t *fpga_get_ch2_buf(void);
/* Changes with every publication: even = stable, odd = a publication is in
 * progress. A reader that needs a whole record reads it, uses the buffers
 * and reads it again; unchanged and even means nothing was published
 * meanwhile. */
uint32_t fpga_acq_frame_generation(void);

/* The capture configuration (dsp/acq_cfg.h). snapshot: in force now (any
 * task). record_cfg: the published record's, copied with it (read it inside
 * a generation check). has_ch: the published record holds channel ch's own
 * samples (0 = CH1); with one channel routed, the other buffer is the second
 * converter's copy of it. drops: records refused because the configuration
 * had not settled over their window. */
void     fpga_acq_cfg_snapshot(acq_cfg_t *c);
void     fpga_acq_record_cfg(acq_cfg_t *out);
const acq_cfg_t *fpga_acq_record_cfg_ptr(void);   /* the renderer's per-sample read */
bool     fpga_acq_record_has_ch(uint8_t ch);
uint32_t fpga_acq_cfg_drops(void);
/* Settle margin of that rule: 0 = ACQ_CFG_SETTLE_MS, 0xFFFF = rule off
 * (every record admitted), else ms. `fpga cfgsettle`. */
void     fpga_acq_cfg_settle_set(uint16_t cmd);
uint16_t fpga_acq_cfg_settle_get(void);

uint8_t fpga_acq_record_kind(void);            /* trig_acq_t (dsp/trig_policy.h) */
float   fpga_record_sample_rate(void);         /* rate of the published record, 0 = none */
uint8_t fpga_acq_record_rate_idx(void);        /* the code the published record was captured at */
uint8_t fpga_acq_rate_idx_get(void);
bool    fpga_acq_record_interleaved(void);     /* published CH1 record is the merge of both converters */
/* The published record is in time order with the trigger at index 512, and
 * its start came from the core's own reply header. */
bool     fpga_acq_record_time_ordered(void);
bool     fpga_acq_record_core_ordered(void);

/* ── the task ───────────────────────────────────────────────────────── */

/* Park the acquisition task between two SPI3 frames so that the caller can
 * use the bus. true once the task is parked; false after about 1 s, and then
 * the bus must not be touched. Every pause needs its resume. */
bool fpga_acq_pause(void);
void fpga_acq_resume(void);

uint8_t fpga_trigger_status(void);             /* trig_status_t: the badge and `fpga scope trigger` */
bool    fpga_acq_single_done(void);            /* SINGLE holds its record */
void    fpga_acq_single_rearm(void);           /* SINGLE: take the next trigger */

/* ── the core ───────────────────────────────────────────────────────── */

bool     fpga_acq_design_ours(void);           /* the running FPGA design is our core */
bool     fpga_acq_core_ok(void);               /* ...identified and complete; else nothing is acquired */
core_ident_t fpga_core_ident(void);            /* what the core said about itself when it was loaded (core_ident.h) */
/* The core's status block (op E0), read about once a second while records are read (core_status.h). */
typedef struct {
    core_status_t last;       /* the last valid reply (valid = false: none yet) */
    uint32_t polls;           /* replies read */
    uint32_t bad;             /* replies that were not a status block */
    uint32_t unlocked;        /* replies with the PLL not locked */
    uint32_t fixed[4];        /* registers rewritten: timebase, source, level, edge */
    bool     check_on;        /* rewrite a register that differs from the firmware's copy twice running */
} fpga_core_watch_t;
fpga_core_watch_t fpga_core_watch(void);
void     fpga_core_watch_fix_set(bool on);     /* off for bench scripts that write registers behind the firmware */
uint32_t fpga_acq_reg_writes_refused(void);   /* setting changes not sent: no usable core */

/* Counters for the shell. */
uint32_t fpga_acq_kind_count(unsigned i);      /* records by the op 05 header: 0 held, 1 not held, 2 held without a PC0 edge, 3 PC0 edge but not held */
uint32_t fpga_acq_force_count(unsigned i);     /* AUTO without a trigger, op 09: 0 requests, 1 forced records read, 2 reads that found the record not ready */
uint32_t fpga_acq_fresh_restarts(void);        /* core captures restarted on RUN / SINGLE arm so no stale record is shown */
uint32_t fpga_acq_seam_count(unsigned i);      /* held records: 0 ordered by the header's seam, 1 without a seam */

/* ── settings that are registers of the core ────────────────────────── */

/* Write a timebase code (reg 0x01) and record it as the one in force. The
 * only way to change the sample rate: the stored setting alone only relabels
 * the axis. false = the acquisition task would not park; nothing was written. */
bool    fpga_apply_timebase(uint8_t code);
/* The trigger level (reg 0x08) from the UI's -100..100. Same contract. */
bool    fpga_apply_trigger_level(int level);
uint8_t fpga_trigger_code_from_level(int level);
uint8_t fpga_acq_trig_code_get(void);          /* reg 0x08 value in force */
bool    fpga_apply_trigger_source(void);       /* reg 0x06 from the stored source (CH1 0x00, CH2 0x01) */
uint8_t fpga_acq_trig_src_code_get(void);      /* reg 0x06 value last written */
uint8_t fpga_acq_edge_reg_get(void);           /* reg 0x07 value last written (dsp/trig_edge_reg.h) */

/* Make the stored timebase and trigger level and the core's registers agree
 * at boot. main() calls them once the settings are loaded; they take no lock. */
void    fpga_reconcile_timebase_after_arm(void);
void    fpga_reconcile_trigger_after_arm(void);
/* For the shell. The timebase one's action: 0 = did not run, 1 = wrote the
 * stored code to the core, 2 = changed the stored code. The trigger one's
 * code: the reg 0x08 value it wrote. */
uint8_t fpga_timebase_reconcile_action(void);
uint8_t fpga_trigger_reconcile_code(void);

/* ── timing of the task, for the bench ──────────────────────────────── */

/* How long AUTO waits for a trigger before it asks for a record without one;
 * 0 = the value derived from the timebase (dsp/acq_timing.h). */
void     fpga_acq_auto_wait_set(uint16_t ms);
uint16_t fpga_acq_auto_wait_get(void);
bool    fpga_acq_auto_wait_is_override(void);
uint16_t fpga_acq_post_edge_get(void);          /* the wait after a record before the next poll (dsp/acq_timing.h) */
void     fpga_acq_poll_gap_set(uint16_t ms);   /* poll period, ms (0 = default) */
uint16_t fpga_acq_poll_gap_get(void);
void     fpga_acq_unrotate_set(bool on);       /* put records in time order (default on) */
bool     fpga_acq_unrotate_get(void);
void     fpga_acq_read_br_set(uint8_t br);     /* SPI3 divider for the task's reads */
uint8_t  fpga_acq_read_br_get(void);

/* ── single-channel interleave (dsp/interleave.h) ───────────────────── */

void    fpga_interleave_set(bool on);
bool    fpga_interleave_get(void);
bool    fpga_interleave_active(void);          /* enabled, mask 1, timebase <= 3 */
void    fpga_interleave_corr_set(float gain, float offset);
void    fpga_interleave_corr_get(float *gain, float *offset);
/* Piecewise level map (1/100 record unit, ascending; n = 0 clears) and the
 * timing skew (B's deviation from half a converter period, ppm of a period;
 * 0 = off). The setters refuse invalid values and change nothing then. */
bool    fpga_interleave_knots_set(uint8_t n, const int16_t *a, const int16_t *b);
uint8_t fpga_interleave_knots_get(int16_t *a, int16_t *b);   /* a, b: IL_KNOTS_MAX each */
bool    fpga_interleave_skew_set(int32_t ppm);
int32_t fpga_interleave_skew_get(void);
/* Self-calibration collector (`fpga interleave cal`). begin() drops `skip`
 * records then starts averaging the task's A/B centre windows; collect()
 * waits for `records` of them (false on timeout) and stops. */
void    fpga_ilcal_begin(uint8_t skip);
bool    fpga_ilcal_collect(uint8_t records, uint32_t timeout_ms, il_cal_point_t *out);

/* ── roll (ui/scope_roll.h) ─────────────────────────────────────────── */

/* Apply the stored roll setting: park the task, write reg 0x01 (the roll
 * code, or the timebase code when leaving roll), clear the FIFO and switch
 * the task between reading records and reading one point per period (op 03).
 * false = the task would not park; nothing was written. */
bool    fpga_apply_roll(void);
bool    fpga_roll_active(void);                  /* the task is reading points */
const scope_roll_fifo_t *fpga_roll_fifo(void);   /* written by the task only */
/* Force the reg 0x01 byte written on entering roll (e.g. 0x14, the only code
 * with a measured rate) instead of 0x14 + step; -1 = no override. */
void    fpga_roll_code_override_set(int code);
int     fpga_roll_code_override_get(void);
typedef struct {
    uint32_t reads;        /* op03 frames that pushed a point */
    uint32_t rejects;      /* op03 frames refused (SPI3 timeout inside) */
    uint32_t late;         /* ticks resynchronised after falling a period behind */
    uint8_t  code;         /* reg-0x01 byte written by the last fpga_apply_roll() */
    uint8_t  step;         /* roll step in force */
    uint32_t trig_hits;    /* roll-trigger crossings found (NORMAL/SINGLE) */
    uint8_t  rx[5];        /* last op03 frame as received: [op, b1, CH1, b3, CH2] */
} fpga_roll_stats_t;
void    fpga_roll_stats_get(fpga_roll_stats_t *out);

void fpga_acq_counters_reset(void);   /* zero the record counters */

/* State and counters of the acquisition (fpga_acq.c writes; the display and the shell read). */
typedef struct {
    volatile uint16_t spi3_ok_count;  /* records read */
    volatile uint16_t spi3_total_timeouts;  /* records refused since boot: a transfer timed out during the read */
    volatile uint16_t spi3_hw_timeouts;  /* SPI3 byte transfers that timed out since boot; each returned 0xFF instead of bus data */
    volatile uint8_t  diag_ch1_raw[4];  /* first 4 bytes of the last CH1 read */
    volatile uint8_t  diag_ch2_raw[4];  /* first 4 bytes of the last CH2 read */
    volatile uint8_t  acq_hdr_ch1[3];  /* the 3 header bytes of the last op 04 reply */
    volatile uint8_t  acq_hdr_ch2[3];  /* the same of the last op 05 reply */
    volatile int32_t  acq_last_rot;  /* rotation applied to the last published record, -1 = none */
    volatile uint32_t acq_last_latency_ms;  /* PC0 edge to publication of the last held record */
    volatile uint32_t acq_poll_reads;  /* reads that found no held record, total */
    volatile uint32_t acq_polls_last;  /* such reads before the last held record */
    volatile uint32_t pc0_edges;  /* falling edges of PC0 (record ready), counted in EXINT0 */
} fpga_acq_state_t;

extern fpga_acq_state_t acq;   /* defined in fpga_acq.c */

#endif /* FPGA_ACQ_H */
