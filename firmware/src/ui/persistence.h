#ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include <stdint.h>
#include <stdbool.h>
#include "scope_state.h"   /* persist_geometry_key */

#define PERSIST_WIDTH  320
#define PERSIST_HEIGHT 206   /* Display area between status bars (y=18 to y=223) */

typedef enum {
    PERSIST_OFF = 0,
    PERSIST_LOW,        /* Fast decay -- shows recent history */
    PERSIST_MEDIUM,     /* Moderate decay */
    PERSIST_HIGH,       /* Slow decay -- long history */
    PERSIST_INFINITE,   /* No decay -- accumulate forever */
    PERSIST_COUNT
} persist_mode_t;

/* Initialize persistence buffer (claims shared memory pool) */
void persist_init(void);

/* Deinitialize persistence (releases shared memory pool) */
void persist_deinit(void);

/* Check if persistence buffer is initialized */
bool persist_is_initialized(void);

/* Clear persistence buffer */
void persist_clear(void);

/* Set persistence mode */
void persist_set_mode(persist_mode_t mode);

/* Get current mode */
persist_mode_t persist_get_mode(void);

/* Add a waveform trace to the persistence buffer.
 * y_values: array of PERSIST_WIDTH y-coordinates (0 to PERSIST_HEIGHT-1)
 * color_index: 0=CH1(yellow), 1=CH2(cyan) */
void persist_add_trace(const uint16_t *y_values, uint8_t color_index);

/* Add a single full-intensity point at buffer coords (x: 0..PERSIST_WIDTH-1,
 * y: 0..PERSIST_HEIGHT-1). For X-Y / arbitrary-locus plotting. */
void persist_add_point(uint16_t x, uint16_t y);

/* Apply decay to the persistence buffer (call once per frame) */
void persist_decay(void);

/* Time constant of a mode: milliseconds for an intensity-255 pixel to fade to
 * 0. 0 for OFF (cleared every frame) and INFINITE (never fades). Stock's menu
 * is OFF / 500 ms / 1 s / infinite (manual p.30); LOW and MEDIUM are the
 * first two, HIGH (2 s) is the extra step this firmware always had. */
uint32_t persist_mode_tau_ms(persist_mode_t mode);

/* Decay by elapsed wall time, independent of the frame rate: a pixel at 255
 * reaches 0 after persist_mode_tau_ms(). The sub-step remainder is carried
 * between calls so a fast frame rate does not round the fade away. */
void persist_decay_ms(uint32_t dt_ms);

/* One column of a LIVE trace, buffer coords: y is lit, and when y_prev >= 0
 * (the previous column's row) the span between the two is lit too, so a square
 * wave's edge is a line as on the glass. y outside 0..PERSIST_HEIGHT-1 is
 * skipped. The scope renderers call this as they draw each column. */
void persist_add_column(uint16_t x, int y_prev, int y);

/* The same for a whole row array (screen rows; row - y_origin is the buffer
 * row; one entry per column from column 0). */
void persist_add_rows(const int16_t *rows, uint16_t n, int y_origin);

/* RGB565 `color` scaled by intensity/255 (a phosphor tail in the trace's own
 * theme colour). */
uint16_t persist_scale_rgb565(uint16_t color, uint8_t intensity);

/* Get intensity at a pixel (0-255) */
uint8_t persist_get_intensity(uint16_t x, uint16_t y);

/* Get the persistence buffer pointer (for direct rendering) */
const uint8_t *persist_get_buffer(void);

/* Map intensity (0-255) to RGB565 color for CH1 (yellow) */
uint16_t persist_intensity_to_color_ch1(uint8_t intensity);

/* Map intensity (0-255) to RGB565 color for CH2 (cyan) */
uint16_t persist_intensity_to_color_ch2(uint8_t intensity);

/* ── Geometry guard ─────────────────────────────────────────────────
 * The buffer holds screen pixels, so a ghost is only right at the mapping
 * from signal to screen it was drawn with. The scope's draw paths call
 * persist_follow_geometry() once per frame, before the ghosts are read, with
 * a key of everything in that mapping (persist_geometry_key); the buffer is
 * cleared when the key moves, whoever moved it (a key handler, a shell set,
 * autoset, 50 % centre, a load), and when acquisition resumes after a stop
 * under a key other than the one it stopped with (the frozen record was
 * drawn at its own scale, the live one is not). */
/* view: the scope view (time, FFT, ...); merged: the record is the
 * two-converter 250 MS/s merge (twice the samples per division). */
uint32_t persist_geometry_key(const scope_state_t *ss, uint8_t view, bool merged);
/* true: the buffer was cleared now. */
bool     persist_follow_geometry(uint32_t key, bool running);

#endif
