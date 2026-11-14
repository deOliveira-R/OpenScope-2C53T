/*
 * fft_source.h - which channel the spectrum views analyse (pure).
 *
 * FFT, Split and Waterfall show the ACTIVE channel: the one MOVE selects and
 * UP/DOWN adjust (ui.ach). There is no separate "FFT source" setting; a host
 * reads view.mode and ui.ach to know what the spectrum shows.
 *
 * This file is the whole decision, with no LCD, RTOS or FPGA driver, so
 * tests/test_fft_source.c runs the code the display task runs
 * (make test-fft-source). The caller hands in a snapshot of the published
 * record's facts, read inside one generation check.
 *
 *   active channel off                     -> FFT_SEL_OFF   "CH2 is off"
 *   on, but the published record does not
 *   hold its samples yet (it was captured
 *   with the other routing: the buffer is
 *   the second converter's copy of the
 *   other channel)                         -> FFT_SEL_WAIT
 *   otherwise                              -> FFT_SEL_OK, its own buffer
 *
 * RATE. rec_rate_hz is the published record's rate as the Y-T view uses it:
 * the per-converter rate, doubled when the CH1 record is the two-converter
 * merge (CH1 alone at a fast timebase, 250 MS/s). The merge exists for CH1
 * only. CH1 takes rec_rate_hz as it is (the merged record when there is
 * one); CH2's own buffer is always one converter's samples, so it takes the
 * per-converter rate. 0 = not known: the axis is labelled in bins.
 */
#ifndef FFT_SOURCE_H
#define FFT_SOURCE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    FFT_SEL_OK = 0,      /* analyse buf at rate_hz */
    FFT_SEL_OFF,         /* the active channel is switched off */
    FFT_SEL_WAIT         /* on, no record of its own yet */
} fft_sel_state_t;

typedef struct {
    uint8_t active_ch;       /* 0 = CH1, 1 = CH2 (anything else counts as CH2) */
    bool    enabled[2];      /* chN.enabled */
    bool    rec_has[2];      /* the published record holds chN's own samples */
    bool    rec_merged;      /* the published CH1 record is the 250 MS/s merge */
    float   rec_rate_hz;     /* rate of the published record (x2 if merged); 0 = unknown */
} fft_source_in_t;

typedef struct {
    uint8_t state;           /* fft_sel_state_t */
    uint8_t ch;              /* the channel named on screen: 0 = CH1, 1 = CH2 */
    uint8_t buf;             /* FFT_SEL_OK: record buffer to read, 0 = CH1's, 1 = CH2's */
    bool    merged;          /* FFT_SEL_OK: buf is the two-converter merge */
    float   rate_hz;         /* FFT_SEL_OK: sample rate of buf; 0 = unknown */
    char    label[4];        /* "CH1" / "CH2" */
    char    note[12];        /* OFF: "CH2 is off"; WAIT: "CH2: wait"; OK: "" */
} fft_source_t;

void fft_source_select(const fft_source_in_t *in, fft_source_t *out);

/* History key: changes whenever spectra taken before and after must not be
 * averaged, max-held or scrolled together (another channel, or no channel).
 * 0 = nothing analysed; 1 = CH1; 2 = CH2. */
uint8_t fft_source_key(const fft_source_t *s);

#endif /* FFT_SOURCE_H */
