/*
 * OpenScope 2C53T - oscilloscope screen: X-Y view
 *
 * CH1 against CH2, with its axis labels.
 */

#include "scope_ui_parts.h"

/* Text laid out in column order: ctext_add records a string with its
 * position and colours (used by the spectrum and waterfall views), and
 * ctext_px answers which pixel a streamed column takes from it. Strings that
 * are empty or that do not fit in the set (CTEXT_MAX, CTEXT_LEN) are dropped.
 * opaque: the glyph box is filled with bg, else only the glyph pixels count. */
void ctext_add(ctext_set_t *T, int x, int y, const char *s,
                      uint16_t fg, uint16_t bg, bool opaque)
{
    if (T->n >= CTEXT_MAX || s == NULL || s[0] == '\0') return;
    typeof(T->t[0]) *e = &T->t[T->n++];
    strncpy(e->s, s, CTEXT_LEN - 1);
    e->s[CTEXT_LEN - 1] = '\0';
    e->x = (int16_t)x;
    e->y = (int16_t)y;
    e->w = (int16_t)font_string_width(e->s, &font_small);
    e->fg = fg;
    e->bg = bg;
    e->opaque = opaque;
}

/* The colour of the text pixel at (x, y), written to *c; false when no string
 * covers that pixel (the caller then draws its own background). */
bool ctext_px(const ctext_set_t *T, int x, int y, uint16_t *c)
{
    for (uint8_t k = 0; k < T->n; k++) {
        const int dx = x - T->t[k].x, dy = y - T->t[k].y;
        if (dx < 0 || dx >= T->t[k].w || dy < 0 || dy >= (int)font_small.height) continue;
        if (font_text_px(T->t[k].s, &font_small, dx, dy)) { *c = T->t[k].fg; return true; }
        if (T->t[k].opaque) { *c = T->t[k].bg; return true; }
    }
    return false;
}

/* Bresenham segment written into the persistence buffer (buffer coords). */
static void xy_line_persist(int x0, int y0, int x1, int y1)
{
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = (dx > dy ? dx : -dy) / 2, e2;
    for (;;) {
        persist_add_point((uint16_t)x0, (uint16_t)y0);
        if (x0 == x1 && y0 == y1) break;
        e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 <  dy) { err += dx; y0 += sy; }
    }
}

/* One channel's X-Y axis: code -> pixels from the centre. True scale where
 * the range has a calibrated volts/div (SCOPE_DIV_PX per division, 0 V at
 * position 0 on the axis, as in the Y-T plot); otherwise autoscaled to the
 * record's min and max (minimum span 8 codes). */
typedef struct { bool fixed; int c0; float px_per_code; } xy_axis_t;

/* Fill *a for channel ch (0 = CH1, 1 = CH2); mn/mx are the record's extremes
 * and half is the plot half-extent in pixels used by the autoscale. */
static void xy_axis(const scope_state_t *ss, uint8_t ch, uint8_t mn, uint8_t mx,
                    int half, xy_axis_t *a)
{
    const uint8_t r = ch ? ss->ch2.vdiv_idx : ss->ch1.vdiv_idx;
    const float cpd = scope_cal_codes_per_div((uint8_t)(ch + 1u), r);
    if (ss->true_scale && cpd > 0.0f) {
        a->fixed = true;
        a->c0 = yt_centre_code();
        a->px_per_code = (float)SCOPE_DIV_PX / cpd;
    } else {
        int span = (int)mx - (int)mn;
        if (span < 8) span = 8;
        a->fixed = false;
        a->c0 = ((int)mn + (int)mx) / 2;
        a->px_per_code = (float)(2 * half) / (float)span;
    }
}

/*
 * Draw the X-Y view: CH1 on X, CH2 on Y, in a square at the centre of the
 * glass. Streams the whole square column by column, so there is no blank
 * phase. `full` also paints the side margins and the title text.
 *
 * Steps: snapshot both records (re-copied when the frame generation moved),
 * find each axis scale, join consecutive samples with lines into the
 * persistence buffer, then stream the square from the buffer over a graticule.
 */
void draw_xy_screen(bool full)
{
    const theme_t *th = theme_get();
    const scope_state_t *ss = scope_state_get();
    const uint16_t side = SCOPE_H;                 /* square side, 206 px */
    const uint16_t x0   = (LCD_WIDTH - side) / 2;  /* centred: 57 */
    const uint16_t y0   = SCOPE_TOP;               /* 18 */
    const int cxr = side / 2, cyr = SCOPE_GRID_MID_Y - SCOPE_TOP;   /* centre in the square */

    /* The phosphor trail lives in the shared persistence buffer, which
     * scope_view_select (tasks/scope_apply.c) claims and clears when the view
     * is entered, on the input task. Nothing is initialised here. Without the
     * buffer the square says "no signal". */

    if (full) {                                    /* margins and title, once */
        lcd_fill_rect(0, SCOPE_TOP, x0, SCOPE_H, th->background);
        lcd_fill_rect((uint16_t)(x0 + side), SCOPE_TOP,
                      (uint16_t)(LCD_WIDTH - (x0 + side)), SCOPE_H, th->background);
        font_draw_string(4, SCOPE_TOP + 2, "X-Y", th->text_secondary, th->background, &font_small);
        font_draw_string(4, SCOPE_TOP + 16, "X=CH1", th->ch1, th->background, &font_small);
        font_draw_string(4, SCOPE_TOP + 30, "Y=CH2", th->ch2, th->background, &font_small);
    }

    const volatile uint8_t *xb = fpga_get_ch1_buf();
    const volatile uint8_t *yb = fpga_get_ch2_buf();
    bool have = fpga_data_ready() && xb != NULL && yb != NULL && persist_is_initialized();

    /* Snapshot buffers on the FreeRTOS heap, not in .bss; allocated on the
     * first frame with data and kept. */
    static uint8_t *xy_xs, *xy_ys;
    if (have && (xy_xs == NULL || xy_ys == NULL)) {
        if (xy_xs == NULL) xy_xs = pvPortMalloc(FPGA_ADC_BUF_SIZE);
        if (xy_ys == NULL) xy_ys = pvPortMalloc(FPGA_ADC_BUF_SIZE);
        if (xy_xs == NULL || xy_ys == NULL) have = false;
    }

    persist_decay();                     /* fade the whole buffer once per frame */

    if (have) {
        uint8_t *const xs = xy_xs;
        uint8_t *const ys = xy_ys;
        for (int t = 0; t < 4; t++) {                /* coherent snapshot */
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
        const int half = side / 2 - 6;
        xy_axis_t ax, ay;
        xy_axis(ss, 0u, xmin, xmax, half, &ax);
        xy_axis(ss, 1u, ymin, ymax, half, &ay);
        /* Every sample joined to the next: a square's fast edge is a faint
         * line between its two levels, as on an analog X-Y display. */
        int pbx = 0, pby = 0;
        for (uint16_t i = 0; i < FPGA_ADC_BUF_SIZE; i++) {
            int bx = (int)x0 + cxr + (int)lroundf((float)((int)xs[i] - ax.c0) * ax.px_per_code);
            int by = cyr - (int)lroundf((float)((int)ys[i] - ay.c0) * ay.px_per_code);
            if (bx < (int)x0) bx = x0; else if (bx > (int)x0 + side - 1) bx = x0 + side - 1;
            if (by < 0) by = 0; else if (by > side - 1) by = side - 1;
            if (i > 0) xy_line_persist(pbx, pby, bx, by);
            else persist_add_point((uint16_t)bx, (uint16_t)by);
            pbx = bx; pby = by;
        }
    }

    /* Stream the square: the trail over a graticule of SCOPE_DIV_PX divisions
     * about the centre (0 V at position 0 on both axes in true scale), no
     * blank phase. A "no signal" note is composed into it, not drawn over it. */
    static const char nosig[] = "no signal";
    const int nsx = cxr - 30, nsy = cyr - 16;      /* in the square */
    const uint8_t *pb = persist_get_buffer();
    for (int rx = 0; rx < (int)side; rx++) {
        const int dxc = rx - cxr;
        const bool vline = (dxc % SCOPE_DIV_PX) == 0;
        lcd_set_window((uint16_t)(x0 + rx), y0, 1, side);
        for (int ry = 0; ry < (int)side; ry++) {
            const int dyc = ry - cyr;
            const uint8_t inten = pb ? pb[ry * PERSIST_WIDTH + (int)x0 + rx] : 0;
            uint16_t c;
            if (!have && rx >= nsx && ry >= nsy && ry < nsy + (int)font_small.height &&
                font_text_px(nosig, &font_small, rx - nsx, ry - nsy))
                c = th->warning;
            else if (inten)
                c = persist_intensity_to_color_ch1(inten);
            else if (dxc == 0 || dyc == 0)
                c = th->grid_center;
            else if ((vline && (ry & 1) == 0) || ((dyc % SCOPE_DIV_PX) == 0 && (rx & 1) == 0))
                c = th->grid;
            else
                c = th->background;
            lcd_write_data(c);
        }
    }
}
