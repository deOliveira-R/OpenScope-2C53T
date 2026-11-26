/*
 * OpenScope 2C53T - the paint timings of the scope screen
 *
 * How often each timed paint ran and how long the last and the longest took,
 * in microseconds of the DWT cycle counter (started by fault.c at boot), since
 * boot. main.c holds the table; `ui dump` prints one line per entry. A paint
 * is timed where it is called (UI_TIMED), so an entry nested in another (the
 * transform inside the spectrum frame) is part of that one's time too.
 */
#ifndef UI_PERF_H
#define UI_PERF_H

#include <stdint.h>
#include "at32f403a_407.h"  /* the cycle counter (DWT) and system_core_clock */

typedef struct { uint32_t n, last_us, max_us; } ui_perf_t;

/* The entries, in the order `ui dump` prints them (ui_perf_name). A view's
 * frame counts its full paints and its live frames alike: entering the view
 * sets "longest", "last" is the steady frame. */
enum {
    UI_PERF_FULL,           /* Y-T full render */
    UI_PERF_STRIP,          /* the measurement strip's refresh */
    UI_PERF_CURSOR_ROW,     /* the cursor readout row */
    UI_PERF_XY,             /* the X-Y view's frame, its trail's fade included */
    UI_PERF_FFT,            /* the spectrum view's frame, the transform included */
    UI_PERF_SPLIT,          /* the split view's frame, both halves */
    UI_PERF_WATERFALL,      /* the waterfall view's frame, the transform included */
    UI_PERF_FFT_TRANSFORM,  /* fft_process alone, in whichever view runs it */
    UI_PERF_FFT_BUILD,      /* a spectrum pane's frame build: transform, scale, labels */
    UI_PERF_FFT_FURNITURE,  /* the spectrum's strip and cursor readout under the pane */
    UI_PERF_COUNT
};

extern volatile ui_perf_t ui_perf[UI_PERF_COUNT];
extern const char *const ui_perf_name[UI_PERF_COUNT];

#define UI_TIMED(slot, call) do { \
        const uint32_t c0_ = DWT->CYCCNT; \
        call; \
        const uint32_t us_ = (DWT->CYCCNT - c0_) / (system_core_clock / 1000000u); \
        ui_perf[slot].n++; ui_perf[slot].last_us = us_; \
        if (us_ > ui_perf[slot].max_us) ui_perf[slot].max_us = us_; \
    } while (0)

#endif /* UI_PERF_H */
