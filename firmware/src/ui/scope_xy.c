/*
 * OpenScope 2C53T - oscilloscope screen: X-Y view
 *
 * CH1 against CH2 in a square as tall as its pane, with its title in the
 * left margin. One drawer for a pane of any height (xy_pane_draw): the full
 * view on the whole band with its trail in the persistence buffer
 * (draw_xy_screen), and a half of split with its trail in the FFT pool's
 * slot (xy_half_draw). The trail fades on wall time, so the tail is the same
 * length full and in a half, at any frame rate.
 */

#include "scope_ui_parts.h"
#include "canvas.h"
#include "shared_mem.h"     /* the FFT pool's slot: the trail of a half */

/* Autoscale (no calibrated volts/div, or true scale off): the record's
 * extremes this many pixels inside each edge of the square, and a span of
 * at least this many codes, so a flat record is not stretched into noise. */
#define XY_AUTOSCALE_MARGIN_PX  6
#define XY_AUTOSCALE_MIN_SPAN   8

/* The "no signal" note: its left edge this far left of the centre (about
 * half its width in font_small) and its top this far above the centre. */
#define XY_NOSIG_LEFT_PX        30
#define XY_NOSIG_UP_PX          16

/* The title in the left margin: font_small lines from this column, the
 * first this far under the pane's top, with this gap between lines. A line
 * that does not fit in the pane is left out. */
#define XY_TITLE_LEFT_PX        4
#define XY_TITLE_TOP_PX         2
#define XY_TITLE_GAP_PX         2

/* Tries for a copy of both records that no acquisition write crossed. */
#define XY_SNAPSHOT_TRIES       4

/* Full X-Y draws its trail into the persistence buffer, a screen column per
 * byte and the band's rows; a half's square is at most as tall as split's
 * upper pane on the whole band (the strips hidden, no cursors), and its trail
 * (side x side bytes) must fit the FFT pool's slot: 103 x 103 = 10609 of
 * 20480 bytes. */
_Static_assert(PERSIST_WIDTH == LCD_WIDTH && PERSIST_HEIGHT == SCOPE_H,
               "full X-Y's square is the persistence buffer's rows, at screen columns");
#define XY_HALF_SIDE_MAX        PLOT_PANE_UPPER_ROWS(SCOPE_H)
_Static_assert(XY_HALF_SIDE_MAX * XY_HALF_SIDE_MAX <= SHMEM_FFT_SLOT_SIZE,
               "the trail of the tallest X-Y half does not fit the FFT pool's slot");

/* A trail's fade on wall time: persist_fade's remainder and when it last ran.
 * It used to fade 16 steps a frame in the mode X-Y set (about 0.8 s at the
 * 20 frames a second of a running record, 3.2 s at the 5 a second of the
 * animation when stopped); it fades by the mode's time constant now (500 ms
 * in PERSIST_XY_TRAIL_MODE), the same full and in a half, at any rate. */
typedef struct {
    uint32_t rem;
    uint32_t last_ms;
    bool     ticked;            /* last_ms is set */
} xy_fade_t;

/* Where a square's trail is: side rows of stride bytes, the square starting
 * at byte col0 of each row; the fade runs over all n bytes in mode. buf
 * NULL: no trail (the square says "no signal"). */
typedef struct {
    uint8_t       *buf;
    uint32_t       n;
    uint16_t       stride, col0;
    persist_mode_t mode;
    xy_fade_t     *fade;
} xy_trail_t;

/* Fade the trail by the wall time since its last fade. */
static void xy_trail_fade(const xy_trail_t *t)
{
    const uint32_t now = (uint32_t)xTaskGetTickCount() * (1000u / configTICK_RATE_HZ);
    if (t->buf != NULL && t->fade->ticked)
        persist_fade(t->buf, t->n, t->mode, now - t->fade->last_ms, &t->fade->rem);
    t->fade->last_ms = now;
    t->fade->ticked = true;
}

/* Bresenham segment written into the trail (square coords, inside it). */
static void xy_trail_line(const xy_trail_t *t, int x0, int y0, int x1, int y1)
{
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = (dx > dy ? dx : -dy) / 2, e2;
    for (;;) {
        t->buf[(uint32_t)y0 * t->stride + t->col0 + (uint32_t)x0] = 255u;
        if (x0 == x1 && y0 == y1) break;
        e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 <  dy) { err += dx; y0 += sy; }
    }
}

/* One channel's X-Y axis: code -> pixels from the centre. True scale where
 * the range has a calibrated volts/div (a graticule division of the pane per
 * division, 0 V at position 0 on the axis, as in the Y-T plot); otherwise
 * autoscaled to the record's min and max. */
typedef struct { bool fixed; int c0; float px_per_code; } xy_axis_t;

/* Fill *a for channel ch (0 = CH1, 1 = CH2); mn/mx are the record's extremes,
 * div the pane's division and half the plot half-extent in pixels used by the
 * autoscale. */
static void xy_axis(const scope_state_t *ss, uint8_t ch, uint8_t mn, uint8_t mx,
                    int div, int half, xy_axis_t *a)
{
    const uint8_t r = ch ? ss->ch2.vdiv_idx : ss->ch1.vdiv_idx;
    const float cpd = scope_cal_codes_per_div((uint8_t)(ch + 1u), r);
    if (ss->true_scale && cpd > 0.0f) {
        a->fixed = true;
        a->c0 = yt_centre_code();
        a->px_per_code = (float)div / cpd;
    } else {
        int span = (int)mx - (int)mn;
        if (span < XY_AUTOSCALE_MIN_SPAN) span = XY_AUTOSCALE_MIN_SPAN;
        a->fixed = false;
        a->c0 = ((int)mn + (int)mx) / 2;
        a->px_per_code = (float)(2 * half) / (float)span;
    }
}

/* The title, top to bottom. */
static const char *const xy_title[] = { "X-Y", "X=CH1", "Y=CH2" };
#define XY_TITLE_LINES  (sizeof xy_title / sizeof xy_title[0])

static const char xy_nosig[] = "no signal";

/* What one X-Y frame streams: the trail over a graticule of the pane's
 * divisions about the centre (0 V at position 0 on both axes in true scale),
 * the focus mark under it when the pane is marked, the title in the left
 * margin. A "no signal" note is composed into it. Small: it is on the
 * display task's stack under split's frame. */
typedef struct {
    const theme_t *th;
    const uint8_t *tb;          /* the trail at the square's (0, 0), or NULL */
    uint16_t stride;            /* the trail's bytes per row */
    int16_t  top, rows;         /* the pane: its first row, its rows (the square's side) */
    uint16_t x0;                /* the square's first screen column */
    int16_t  cxr, cyr, div;     /* its centre, in the square; the graticule's division */
    int16_t  text_x1;           /* the columns left of this hold the title */
    uint8_t  title_lines;       /* the title's lines that fit in the pane */
    bool     have;              /* a record was drawn into the trail */
    bool     mark;              /* the focus mark: split's half the keys are on */
    uint16_t title_c[XY_TITLE_LINES];
} xy_frame_t;
/* The display task had 315 words of stack free with split drawn (2026-11);
 * this frame is what an X-Y half adds to it. */
#define XY_FRAME_MAX_BYTES  40u
_Static_assert(sizeof(xy_frame_t) <= XY_FRAME_MAX_BYTES, "the X-Y frame grew on the display task's stack");

/* The focus mark covers screen pixel (x, y) of the pane. */
static bool xy_mark_px(const xy_frame_t *f, int x, int y)
{
    return f->mark && focus_mark_px(x, y, 0, f->top, LCD_WIDTH, f->rows);
}

/* Pixel (rx, ry) of the square, screen column x. */
static uint16_t xy_px(const xy_frame_t *f, int x, int rx, int ry)
{
    const theme_t *th = f->th;
    const int nsx = f->cxr - XY_NOSIG_LEFT_PX, nsy = f->cyr - XY_NOSIG_UP_PX;
    const int dxc = rx - f->cxr, dyc = ry - f->cyr;
    const bool vline = (dxc % f->div) == 0;
    const uint8_t inten = f->tb ? f->tb[(uint32_t)ry * f->stride + (uint32_t)rx] : 0u;
    if (!f->have && rx >= nsx && ry >= nsy && ry < nsy + (int)font_small.height &&
        font_text_px(xy_nosig, &font_small, rx - nsx, ry - nsy))
        return th->warning;
    if (inten)
        return persist_intensity_to_color_ch1(inten);
    if (xy_mark_px(f, x, f->top + ry))
        return th->text_primary;
    if (dxc == 0 || dyc == 0)
        return th->grid_center;
    if ((vline && (ry & 1) == 0) || ((dyc % f->div) == 0 && (rx & 1) == 0))
        return th->grid;
    return th->background;
}

/* Pixel (x, ry) of the left margin: the title, the focus mark, background. */
static uint16_t xy_margin_px(const xy_frame_t *f, int x, int ry)
{
    const int pitch = (int)font_small.height + XY_TITLE_GAP_PX;
    if (x >= XY_TITLE_LEFT_PX && x < f->text_x1 && ry >= XY_TITLE_TOP_PX) {
        const int k = (ry - XY_TITLE_TOP_PX) / pitch, dy = (ry - XY_TITLE_TOP_PX) % pitch;
        if (k < (int)f->title_lines && dy < (int)font_small.height &&
            font_text_px(xy_title[k], &font_small, x - XY_TITLE_LEFT_PX, dy))
            return f->title_c[k];
    }
    return xy_mark_px(f, x, f->top + ry) ? f->th->text_primary : f->th->background;
}

/* The canvas's paint function: column x of the screen, the margins with
 * their title included, so nothing in the pane is left to another painter.
 * The strip is the pane's whole column (canvas.c steps around a hole when it
 * sends), so its row y is row y - top of the square. */
static void xy_paint(void *ctx, uint16_t x, const canvas_strip_t *s)
{
    const xy_frame_t *const f = ctx;
    const bool square = x >= f->x0 && x < f->x0 + (uint16_t)f->rows;
    for (int y = s->lo; y < s->hi; y++) {
        const int ry = y - f->top;
        s->px[y - s->lo] = square ? xy_px(f, (int)x, (int)x - (int)f->x0, ry) : xy_margin_px(f, (int)x, ry);
    }
}

/* The square's first screen column: centred on the glass. */
static uint16_t xy_square_x0(uint16_t side)
{
    return (uint16_t)((LCD_WIDTH - side) / 2u);
}

/*
 * Draw X-Y into pane p: CH1 on X, CH2 on Y, in a square of the pane's height
 * at the centre of the glass, through the pane's canvas over its whole band
 * (the margins and the title too): only the columns that changed are sent,
 * and the popup box is a hole. all: send every column. t: the square's
 * trail. Returns true when the frame kept a hole for the popup box.
 *
 * Steps: fade the trail, snapshot both records (re-copied when the frame
 * generation moved), find each axis scale, join consecutive samples with
 * lines into the trail, then draw the square from the trail over a
 * graticule.
 */
static bool xy_pane_draw(const plot_pane_t *p, bool all, const xy_trail_t *t)
{
    const theme_t *th = theme_get();
    const scope_state_t *ss = scope_state_get();
    const uint16_t side = (uint16_t)(p->band_bot - p->top);
    const uint16_t x0 = xy_square_x0(side);
    const int cxr = side / 2, cyr = p->grid_mid - p->top;    /* centre in the square */

    const volatile uint8_t *xb = fpga_get_ch1_buf();
    const volatile uint8_t *yb = fpga_get_ch2_buf();
    bool have = fpga_data_ready() && xb != NULL && yb != NULL && t->buf != NULL;

    /* Snapshot buffers on the FreeRTOS heap, not in .bss; allocated on the
     * first frame with data and kept. */
    static uint8_t *xy_xs, *xy_ys;
    if (have && (xy_xs == NULL || xy_ys == NULL)) {
        if (xy_xs == NULL) xy_xs = pvPortMalloc(FPGA_ADC_BUF_SIZE);
        if (xy_ys == NULL) xy_ys = pvPortMalloc(FPGA_ADC_BUF_SIZE);
        if (xy_xs == NULL || xy_ys == NULL) have = false;
    }

    xy_trail_fade(t);

    if (have) {
        uint8_t *const xs = xy_xs;
        uint8_t *const ys = xy_ys;
        for (int k = 0; k < XY_SNAPSHOT_TRIES; k++) {     /* coherent snapshot */
            uint32_t g0 = fpga_acq_frame_generation();
            memcpy(xs, (const void *)xb, FPGA_ADC_BUF_SIZE);
            memcpy(ys, (const void *)yb, FPGA_ADC_BUF_SIZE);
            if ((g0 & 1u) == 0u && fpga_acq_frame_generation() == g0)
                break;
        }
        uint8_t xmin = 255, xmax = 0, ymin = 255, ymax = 0;
        for (uint16_t i = 0; i < FPGA_ADC_BUF_SIZE; i++) {
            if (xs[i] < xmin) xmin = xs[i];
            if (xs[i] > xmax) xmax = xs[i];
            if (ys[i] < ymin) ymin = ys[i];
            if (ys[i] > ymax) ymax = ys[i];
        }
        const int half = side / 2 - XY_AUTOSCALE_MARGIN_PX;
        xy_axis_t ax, ay;
        xy_axis(ss, 0u, xmin, xmax, p->div_px, half, &ax);
        xy_axis(ss, 1u, ymin, ymax, p->div_px, half, &ay);
        /* Every sample joined to the next: a square's fast edge is a faint
         * line between its two levels, as on an analog X-Y display. */
        int pbx = 0, pby = 0;
        for (uint16_t i = 0; i < FPGA_ADC_BUF_SIZE; i++) {
            int bx = cxr + (int)lroundf((float)((int)xs[i] - ax.c0) * ax.px_per_code);
            int by = cyr - (int)lroundf((float)((int)ys[i] - ay.c0) * ay.px_per_code);
            if (bx < 0) bx = 0; else if (bx > side - 1) bx = side - 1;
            if (by < 0) by = 0; else if (by > side - 1) by = side - 1;
            if (i == 0) { pbx = bx; pby = by; }
            xy_trail_line(t, pbx, pby, bx, by);
            pbx = bx; pby = by;
        }
    }

    xy_frame_t f = { .th = th, .tb = t->buf ? t->buf + t->col0 : NULL, .stride = t->stride,
                     .top = p->top, .rows = (int16_t)side, .x0 = x0,
                     .cxr = (int16_t)cxr, .cyr = (int16_t)cyr, .div = p->div_px,
                     .have = have, .mark = p->marked,
                     .title_c = { th->text_secondary, th->ch1, th->ch2 } };
    const int pitch = (int)font_small.height + XY_TITLE_GAP_PX;
    f.text_x1 = 0;
    f.title_lines = 0;
    for (uint8_t k = 0; k < XY_TITLE_LINES; k++) {
        if (XY_TITLE_TOP_PX + k * pitch + (int)font_small.height > (int)side) break;
        const int x1 = XY_TITLE_LEFT_PX + (int)font_string_width(xy_title[k], &font_small);
        if (x1 > f.text_x1) f.text_x1 = (int16_t)x1;
        f.title_lines = (uint8_t)(k + 1u);
    }

    const bool hole = popup_hole_in(p->top, p->band_bot);
    const canvas_rect_t popup = { POPUP_X, POPUP_Y, POPUP_W, POPUP_H };
    const canvas_frame_t cf = {
        .id = p->canvas,
        .rect = { 0, (uint16_t)p->top, LCD_WIDTH, side },
        .holes = &popup, .nholes = hole ? 1u : 0u,
        .all = all,
        .paint = xy_paint, .ctx = &f,
    };
    (void)canvas_draw_columns(&cf);
    return hole;
}

/* The full X-Y view: the pane of the whole band (X-Y has no strip), its
 * trail in the persistence buffer, which scope_view_select
 * (tasks/scope_apply.c) claims and clears when the view is entered, on the
 * input task; without the buffer the square says "no signal". It fades in
 * the persistence mode in force (PERSIST_XY_TRAIL_MODE from the entry, or
 * view.persist's while persistence is on). full: every column and the popup
 * box. */
void draw_xy_screen(bool full)
{
    static xy_fade_t fade;
    const plot_pane_t pane = plot_pane_make(CANVAS_PLOT, SCOPE_TOP, SCOPE_BOT, SCOPE_BOT);
    const xy_trail_t t = {
        .buf = persist_is_initialized() ? persist_get_buffer() : NULL,
        .n = (uint32_t)PERSIST_WIDTH * PERSIST_HEIGHT, .stride = PERSIST_WIDTH,
        .col0 = xy_square_x0(SCOPE_H), .mode = persist_get_mode(), .fade = &fade,
    };
    popup_hole_last = xy_pane_draw(&pane, full, &t);
    if (full) draw_popup(theme_get());
}

/* X-Y as a half of split, in pane p: its trail in the FFT pool's slot (split
 * holds the pool for its spectrum, shared_mem.h), side x side bytes. Started
 * afresh when the slot was not this trail's (the half entered: its bring-up
 * releases the slot, scope_view_half_enter; another tenant or another pool
 * owner had it) and when the pane's height changed (the strips shown or
 * hidden, the cursors on or off). Display task. */
bool xy_half_draw(const plot_pane_t *p, bool all)
{
    static xy_fade_t fade;
    static uint16_t side_was;
    const uint16_t side = (uint16_t)(p->band_bot - p->top);
    bool changed;
    uint8_t *const slot = shared_mem_fft_slot(SHMEM_SLOT_XY_HALF, &changed);
    if (slot != NULL && (changed || side != side_was)) {
        memset(slot, 0, (size_t)side * side);
        fade.rem = 0u;
        side_was = side;
    }
    const xy_trail_t t = { .buf = slot, .n = (uint32_t)side * side, .stride = side, .col0 = 0u,
                           .mode = PERSIST_XY_TRAIL_MODE, .fade = &fade };
    return xy_pane_draw(p, all, &t);
}
