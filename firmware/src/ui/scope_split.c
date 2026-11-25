/*
 * OpenScope 2C53T - oscilloscope screen: split, two views in two panes
 *
 * The band over the measurement strip is split into two panes, one above the
 * other with a divider row between them (plot_pane_split). Each is drawn by
 * the view the pair puts there (view.upper, view.lower: a view that can be a
 * half, view_table.h) through that view's own pane drawer, on the canvas of
 * its half, so each half sends only the columns that changed. Each half
 * draws its own cursor lines. The half the keys are on (the job's domain,
 * view_key_half) is framed (the focus mark, under the data) and owns the
 * furniture: its domain's measurement strip under the band and, while the
 * cursors are on, their readout in that strip's AUX row; the band ends at
 * that strip's top. The keys' domain is layout (scope_screen_epochs), so
 * moving the keys to the other half is a full redraw: the strip, the focus
 * mark and the panes' heights all follow it.
 */

#include "scope_ui_parts.h"
#include "canvas.h"
#include "key_model.h"      /* scope_view_sel, key_focus_get */

/* What the halves drew, for the furniture of the keys' half. */
typedef struct {
    fft_drawn_t fft;
} split_drawn_t;

/* A view's drawer for a pane: every column (all) or only the columns that
 * changed; true when it kept a hole for the popup box. */
typedef bool (*split_half_fn)(const plot_pane_t *p, bool all, uint32_t frame, split_drawn_t *d);

static bool half_yt(const plot_pane_t *p, bool all, uint32_t frame, split_drawn_t *d)
{
    (void)d;
    return scope_yt_draw(p, all, frame);
}

static bool half_fft(const plot_pane_t *p, bool all, uint32_t frame, split_drawn_t *d)
{
    (void)frame;
    return fft_pane_draw(p, all, &d->fft);
}

/* The pane drawer of every view that can be a half (view_desc_t.half): the
 * setter of view.upper / view.lower admits no other view. */
static const split_half_fn k_half_draw[VIEW_MODE_COUNT] = {
    [VIEW_MODE_YT]  = half_yt,
    [VIEW_MODE_FFT] = half_fft,
};

/* The top of domain d's measurement strip: the band ends there. */
static uint16_t split_band_bot(const scope_state_t *ss, view_domain_t d)
{
    return (d == VIEW_DOM_FREQ) ? fft_strip_top(ss) : strip_top(ss);
}

/* The divider row at y: in neither pane's canvas, so painted with every
 * frame; while a pane keeps a hole for the popup box, around the box, so the
 * row comes back with the panes when the box ends. */
static void split_divider_paint(int16_t y, uint16_t color)
{
    const uint16_t rows = PLOT_PANE_DIVIDER_ROWS;
    if (popup_hole_in(y, y + PLOT_PANE_DIVIDER_ROWS)) {
        lcd_fill_rect(0, (uint16_t)y, POPUP_X, rows, color);
        lcd_fill_rect(POPUP_X + POPUP_W, (uint16_t)y, LCD_WIDTH - (POPUP_X + POPUP_W), rows, color);
    } else {
        lcd_fill_rect(0, (uint16_t)y, LCD_WIDTH, rows, color);
    }
}

/* Split: both halves, the divider, then the furniture of the keys' half.
 * full: every column, the strip with its AUX row, and the popup box.
 * A live frame of the time half's strip is main.c's (its refresh, as in
 * Y-T); the spectrum's strip is drawn with each frame, as in the FFT view. */
void draw_split_screen(uint32_t frame, bool full)
{
    const scope_state_t *ss = scope_state_get();
    const theme_t *th = theme_get();
    const view_sel_t v = scope_view_sel();
    const key_focus_t job = (key_focus_t)key_focus_get();
    const view_domain_t dom = view_key_domain(v, job);
    const uint16_t band_bot = split_band_bot(ss, dom);

    plot_pane_t pane[VIEW_HALF_COUNT];
    plot_pane_split(SCOPE_TOP, (int16_t)band_bot, CANVAS_SPLIT_UPPER, CANVAS_SPLIT_LOWER,
                    (int)view_key_half(v, job), pane);
    split_drawn_t d;
    bool hole = false;
    for (uint8_t h = 0; h < VIEW_HALF_COUNT; h++)
        hole |= k_half_draw[v.half[h]](&pane[h], full, frame, &d);
    popup_hole_last = hole;

    split_divider_paint(pane[VIEW_HALF_UPPER].bot, th->grid_center);
    if (dom == VIEW_DOM_FREQ) {
        fft_furniture_draw(ss, &d.fft, band_bot, !full);
    } else if (full) {
        if (band_bot < SCOPE_BOT)
            lcd_fill_rect(0, band_bot, LCD_WIDTH, (uint16_t)(SCOPE_BOT - band_bot), th->background);
        draw_measurement_badges(ss, th, false);
    }
    if (full) draw_popup(th);
}
