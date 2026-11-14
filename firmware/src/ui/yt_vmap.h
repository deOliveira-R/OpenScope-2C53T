/*
 * Vertical transform of the Y-T plot: ADC record code -> screen row.
 *
 * One map per channel, built once per frame by scope_ui.c (yt_fits) and read
 * by every consumer: the trace in both renderers, the mask bounds, the
 * persistence feed, the ground / trigger tags, the trigger line and the
 * cursor scale. Two kinds, one formula:
 *
 *   row = y0 - ((code - mn) * h) / span          (C division, toward zero)
 *
 *   true scale  y0 = band middle, mn = 128, h = band height, span = 256:
 *               256 counts over the full plot (SCOPE_H = 206 px), so code 128
 *               sits on the centre row and 32 counts are 25.75 px, one 26 px
 *               division to within a pixel. Both channels share the full plot.
 *   autofit     y0 = y_bot - 2, mn = the window's minimum, span = its range
 *               (>= 8 counts), h = band height - 3: the record stretched to
 *               its band, split into halves when both channels autofit.
 *
 * The row is clipped to [y_top, y_bot). A held record's range rescale
 * (pv_vscale) is applied to the code by the caller, in true scale only.
 *
 * Pure C: host-built by `make test-yt-vmap`.
 */
#ifndef YT_VMAP_H
#define YT_VMAP_H

#include <stdbool.h>
#include <stdint.h>

#define YT_VMAP_CENTRE      128     /* the code true scale puts on the centre row */
#define YT_VMAP_MIN_SPAN    8       /* autofit: don't zoom pure noise to full band */

typedef struct {
    bool    on;
    int8_t  fx_ch;          /* true scale: channel 0/1; -1 = autofit */
    int16_t y_top, y_bot;   /* clip, y_bot exclusive */
    int16_t y0;             /* row of code mn */
    int     h, span;        /* h rows per span counts */
    uint8_t mn;
} yt_vmap_t;

static inline void yt_vmap_true(yt_vmap_t *a, int16_t y_top, int16_t y_bot, int8_t ch)
{
    a->on    = true;
    a->fx_ch = ch;
    a->y_top = y_top;
    a->y_bot = y_bot;
    a->y0    = (int16_t)((y_top + y_bot) / 2);
    a->h     = y_bot - y_top;
    a->span  = 256;
    a->mn    = YT_VMAP_CENTRE;
}

static inline void yt_vmap_autofit(yt_vmap_t *a, uint8_t mn, uint8_t mx,
                                   int16_t y_top, int16_t y_bot)
{
    int span = (int)mx - (int)mn;
    if (span < YT_VMAP_MIN_SPAN) span = YT_VMAP_MIN_SPAN;
    int h = (int)(y_bot - y_top) - 3;       /* usable height, small margin */
    if (h < 4) h = 4;
    a->on    = true;
    a->fx_ch = -1;
    a->y_top = y_top;
    a->y_bot = y_bot;
    a->y0    = (int16_t)(y_bot - 2);
    a->h     = h;
    a->span  = span;
    a->mn    = mn;
}

/* Unclipped: where a code lands, on or off the band (the tags use this). */
static inline int yt_vmap_raw(const yt_vmap_t *a, int v)
{
    return (int)a->y0 - ((v - (int)a->mn) * a->h) / a->span;
}

static inline int16_t yt_vmap_clip(const yt_vmap_t *a, int y)
{
    if (y < a->y_top)  y = a->y_top;
    if (y >= a->y_bot) y = a->y_bot - 1;
    return (int16_t)y;
}

/* ADC counts one row is worth (the cursor scale); 0 when nothing is drawn. */
static inline float yt_vmap_counts_per_px(const yt_vmap_t *a)
{
    return (a->on && a->h > 0) ? (float)a->span / (float)a->h : 0.0f;
}

/* Split bands only when both channels are on and both autofit. */
static inline bool yt_vmap_split(bool en1, bool en2, bool fx1, bool fx2)
{
    return en1 && en2 && !fx1 && !fx2;
}

/* Channel ch's band on the plot [top, bot): the full plot, or its half
 * (CH1 above the middle row, CH2 below, the middle row itself left free). */
static inline void yt_vmap_band(int16_t top, int16_t bot, bool split, uint8_t ch,
                                int16_t *t, int16_t *b)
{
    const int16_t mid = (int16_t)(top + (bot - top) / 2);
    *t = top;
    *b = bot;
    if (split) {
        if (ch) *t = (int16_t)(mid + 1);
        else    *b = (int16_t)(mid - 1);
    }
}

#endif /* YT_VMAP_H */
