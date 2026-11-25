/*
 * OpenScope 2C53T - oscilloscope screen: cursors
 *
 * The cursor lines, their readout and the cursor row.
 */

#include "scope_ui_parts.h"
#include "canvas.h"

/* The time axis from the state and the record (view_time_axis: no draw
 * needed), the active channel's band as the last frame drew it. */
void scope_ui_cursor_axes(scope_cursor_axes_t *ax)
{
    const scope_state_t *ss = scope_state_get();
    view_time_axis_t ta;
    view_time_axis(ss, &ta);
    memset(ax, 0, sizeof(*ax));
    ax->roll    = ta.roll;
    ax->per_col = ta.per_col;
    ax->ref_col = ta.ref_col;
    ax->t_conf  = ta.conf;
    ax->cols    = (int16_t)SCOPE_GEOM_COLS;
    const uint8_t ch = active_channel ? 1u : 0u;
    const vband_t *b = &g_vband[ch];
    ax->channel  = (uint8_t)(ch + 1u);
    ax->level_ok = b->on;
    if (!b->on) return;
    ax->map            = b->map;
    ax->zero_code      = b->zero_code;
    ax->volts_per_code = b->volts_per_code;
    ax->v_conf         = b->conf;
    ax->row_top        = b->map.y_top;
    ax->row_end        = b->row_end;
}

/* The time pair the source on the screen keeps: roll's or Y-T's. */
static const int32_t *time_pair(const cursor_state_t *c, const scope_cursor_axes_t *ax)
{
    return ax->roll ? c->roll_ms : c->t_tick;
}

/* Shell/bench view of the cursors (`fpga scope cursor`): X1 X2 X1-X2 Y1 Y2
 * Y1-Y2 and 1/|dX|, from the SAME functions the screen uses. Strings are the
 * formatted readings, "--" where the instrument refuses. */
void scope_ui_cursor_report(char x1[16], char x2[16], char dx[16], char ix[16],
                            char y1[16], char y2[16], char dy[16], float *fs_out)
{
    const cursor_state_t *c = &scope_state_get()->cursor;
    scope_cursor_axes_t ax;
    scope_ui_cursor_axes(&ax);
    const int32_t *t = time_pair(c, &ax);
    const microvolts_t l1 = { c->level_uv[0] }, l2 = { c->level_uv[1] };
    /* The readouts carry stock's labels (X1-X2, Y1-Y2), so they are FIRST
     * minus SECOND: the on-screen dt/dV are second minus first and read
     * with the opposite sign. */
    const cursor_order_t ord = CUR_FIRST_MINUS_SECOND;
    scope_cursor_reading_t r;

    r = scope_cursor_x(&ax, t[0]);                  scope_cursor_format(&r, x1, 16);
    r = scope_cursor_x(&ax, t[1]);                  scope_cursor_format(&r, x2, 16);
    r = scope_cursor_dt(&ax, t[0], t[1], ord);      scope_cursor_format(&r, dx, 16);
    r = scope_cursor_inv_dt(&ax, t[0], t[1], ord);  scope_cursor_format(&r, ix, 16);
    r = scope_cursor_y(&ax, l1);                    scope_cursor_format(&r, y1, 16);
    r = scope_cursor_y(&ax, l2);                    scope_cursor_format(&r, y2, 16);
    r = scope_cursor_dv(&ax, l1, l2, ord);          scope_cursor_format(&r, dy, 16);
    if (fs_out) *fs_out = scope_cursor_col_rate(&ax);
}

/* The line of a cursor that is not selected is dashed: CURSOR_DASH_LEN
 * pixels on, as many off, along the line. */
#define CURSOR_DASH_LEN     4
#define CURSOR_DASH_ON(at)  (((at) & (2 * CURSOR_DASH_LEN - 1)) < CURSOR_DASH_LEN)

/* Where a cursor's label sits. V1/V2: at the top of the plot, starting
 * CURSOR_VLABEL_LEFT columns left of the line, or at column 0 when the line
 * is within CURSOR_VLABEL_MIN_X of the left edge. H1/H2: against the right
 * edge, its top CURSOR_HLABEL_ABOVE rows above the line (the font's 12 rows
 * and one clear), or CURSOR_HLABEL_BELOW under it when the line is within
 * CURSOR_HLABEL_BELOW rows of the plot's top. */
#define CURSOR_VLABEL_LEFT   10
#define CURSOR_VLABEL_MIN_X  12
#define CURSOR_HLABEL_ABOVE  13
#define CURSOR_HLABEL_BELOW  2

/* Build the cursor layer L: the vertical lines at columns vx with V1/V2
 * labels at the plot's top row `top`, the horizontal lines at rows hy with
 * H1/H2 labels at the right edge. A line whose position is negative is not
 * on the plot and is left out. The selected cursor is solid and highlighted,
 * the other dashed. Empty when cursors are off. */
static void cursor_layer_lines(const cursor_state_t *c, const int16_t vx[2], const int16_t hy[2],
                               int16_t top, const theme_t *th, cur_layer_t *L)
{
    memset(L, 0, sizeof(*L));
    if (c->mode == CURSOR_OFF) return;
    if (c->mode == CURSOR_VERTICAL || c->mode == CURSOR_BOTH) {
        L->v = true;
        for (uint8_t i = 0; i < 2u; i++) {
            const bool act = (c->active == (i ? CURSOR_SEL_V2 : CURSOR_SEL_V1));
            L->vx[i] = vx[i];
            L->vc[i] = act ? th->highlight : th->text_secondary;
            L->vsolid[i] = act;
            if (vx[i] < 0) continue;
            typeof(L->lab[0]) *t = &L->lab[L->nlab++];
            t->s[0] = 'V'; t->s[1] = (char)('1' + i); t->s[2] = '\0';
            t->w = (int16_t)font_string_width(t->s, &font_small);
            t->x = (int16_t)(L->vx[i] > CURSOR_VLABEL_MIN_X ? L->vx[i] - CURSOR_VLABEL_LEFT : 0);
            t->y = top;
            t->c = L->vc[i];
        }
    }
    if (c->mode == CURSOR_HORIZONTAL || c->mode == CURSOR_BOTH) {
        L->h = true;
        for (uint8_t i = 0; i < 2u; i++) {
            const bool act = (c->active == (i ? CURSOR_SEL_H2 : CURSOR_SEL_H1));
            L->hy[i] = hy[i];
            L->hc[i] = act ? th->highlight : th->text_secondary;
            L->hsolid[i] = act;
            if (hy[i] < 0) continue;
            typeof(L->lab[0]) *t = &L->lab[L->nlab++];
            t->s[0] = 'H'; t->s[1] = (char)('1' + i); t->s[2] = '\0';
            t->w = (int16_t)font_string_width(t->s, &font_small);
            t->x = (int16_t)(LCD_WIDTH - 1 - t->w);
            t->y = (int16_t)(L->hy[i] > top + CURSOR_HLABEL_BELOW ? L->hy[i] - CURSOR_HLABEL_ABOVE
                                                                  : L->hy[i] + CURSOR_HLABEL_BELOW);
            t->c = L->hc[i];
        }
    }
}

/* The cursor layer of the time views in pane p: each line where its time
 * falls on the time axis of the source on the screen, and where its level
 * falls on the active channel's band as drawn this frame (vband_note, before
 * this). A line off the plot, or with no conversion, is not drawn. */
void cursor_layer_build(const scope_state_t *ss, const theme_t *th, const plot_pane_t *p, cur_layer_t *L)
{
    const cursor_state_t *c = &ss->cursor;
    scope_cursor_axes_t ax;
    scope_ui_cursor_axes(&ax);
    const int32_t *t = time_pair(c, &ax);
    int16_t vx[2] = { -1, -1 }, hy[2] = { -1, -1 };
    for (uint8_t i = 0; i < 2u; i++) {
        int32_t px;
        const cursor_sel_t sv = i ? CURSOR_SEL_V2 : CURSOR_SEL_V1, sh = i ? CURSOR_SEL_H2 : CURSOR_SEL_H1;
        if (scope_cursor_px(&ax, sv, t[i], &px) && scope_cursor_on_plot(&ax, sv, px)) vx[i] = (int16_t)px;
        if (scope_cursor_px(&ax, sh, c->level_uv[i], &px) && scope_cursor_on_plot(&ax, sh, px)) hy[i] = (int16_t)px;
    }
    cursor_layer_lines(c, vx, hy, p->top, th, L);
}

/* The cursor layer of a spectrum: the lines where their bins and levels fall
 * on the axes ax; a line outside the span or the level range is not drawn. */
void cursor_layer_build_spectrum(const scope_state_t *ss, const fft_axes_t *ax, const theme_t *th,
                                 cur_layer_t *L)
{
    const spectrum_cursor_t *sc = &ss->spectrum_cursor;
    int16_t vx[2] = { -1, -1 }, hy[2] = { -1, -1 };
    if (ax->on)
        for (uint8_t i = 0; i < 2u; i++) {
            vx[i] = (int16_t)fft_axes_x(ax, sc->bin[i]);
            hy[i] = (int16_t)fft_axes_y(ax, (float)sc->level[i] / (float)SPECTRUM_CURSOR_LEVEL_UNIT);
        }
    cursor_layer_lines(&ss->cursor, vx, hy, (int16_t)ax->y_top, th, L);
}

/* The layer's colour at pixel (x, y) into *c, or false. Labels win over
 * lines; where two lines cross, V1, H1, V2, H2 in that order. This is the
 * rule; the plot paints with cursor_layer_paint and `fpga scope livecheck`
 * compares the two. */
bool cursor_layer_px(const cur_layer_t *L, int x, int y, uint16_t *c)
{
    for (uint8_t k = 0; k < L->nlab; k++) {
        const int dx = x - L->lab[k].x, dy = y - L->lab[k].y;
        if (dx >= 0 && dx < L->lab[k].w && dy >= 0 && dy < (int)font_small.height &&
            font_text_px(L->lab[k].s, &font_small, dx, dy)) {
            *c = L->lab[k].c;
            return true;
        }
    }
    for (uint8_t i = 0; i < 2u; i++) {
        if (L->v && L->vx[i] >= 0 && x == L->vx[i] && (L->vsolid[i] || CURSOR_DASH_ON(y))) { *c = L->vc[i]; return true; }
        if (L->h && L->hy[i] >= 0 && y == L->hy[i] && (L->hsolid[i] || CURSOR_DASH_ON(x))) { *c = L->hc[i]; return true; }
    }
    return false;
}

/* Paint what the layer has in column x into strip s: only the rows a line or
 * a label can occupy. Same picture as cursor_layer_px gives pixel by pixel,
 * so it paints in the reverse of that function's priority: H2, V2, H1, V1,
 * then the labels, last one first. */
void cursor_layer_paint(const cur_layer_t *L, uint16_t x, const canvas_strip_t *s)
{
    for (int i = 1; i >= 0; i--) {
        if (L->h && L->hy[i] >= 0 && (L->hsolid[i] || CURSOR_DASH_ON(x)))
            strip_put(s, L->hy[i], L->hc[i]);
        if (L->v && (int)x == L->vx[i])
            for (int y = s->lo; y < s->hi; y++)
                if (L->vsolid[i] || CURSOR_DASH_ON(y)) s->px[y - s->lo] = L->vc[i];
    }
    for (int k = (int)L->nlab - 1; k >= 0; k--) {
        const int dx = (int)x - L->lab[k].x;
        if (dx < 0 || dx >= L->lab[k].w) continue;
        for (int dy = 0; dy < (int)font_small.height; dy++)
            if (font_text_px(L->lab[k].s, &font_small, dx, dy))
                strip_put(s, L->lab[k].y + dy, L->lab[k].c);
    }
}

/*
 * The cursor readout text into out (n bytes): mode, then dt and 1/dt, then
 * dV, as the mode has them. It is shown on one line in the measurement
 * strip's AUX row (cursor_row_paint), off the trace and off the badges.
 *
 * Straight from the stored lines (scope_cursor.c): the times are exact in
 * ticks or ms, the levels in microvolts of the active channel; no pixel
 * enters. dV names the channel its levels belong to, and reads "--" while
 * that channel's band has no volts to put them on.
 */
static void cursor_readout(const scope_state_t *ss, char *out, size_t n)
{
    const cursor_state_t *c = &ss->cursor;
    char buf[24];
    size_t k = 0;
    out[0] = '\0';
    if (c->mode == CURSOR_OFF) return;
    scope_cursor_axes_t ax;
    scope_ui_cursor_axes(&ax);
    k += (size_t)snprintf(out + k, n - k, "%s",
                          c->mode == CURSOR_VERTICAL ? "CUR:T" :
                          c->mode == CURSOR_HORIZONTAL ? "CUR:V" : "CUR:TV");
    if (k < n && (c->mode == CURSOR_VERTICAL || c->mode == CURSOR_BOTH)) {
        const int32_t *t = time_pair(c, &ax);
        scope_cursor_reading_t r = scope_cursor_dt(&ax, t[0], t[1], CUR_SECOND_MINUS_FIRST);
        scope_cursor_format(&r, buf, sizeof(buf));
        k += (size_t)snprintf(out + k, n - k, "  dt=%s", buf);
        if (k < n) {
            r = scope_cursor_inv_dt(&ax, t[0], t[1], CUR_SECOND_MINUS_FIRST);
            scope_cursor_format(&r, buf, sizeof(buf));
            k += (size_t)snprintf(out + k, n - k, "  1/dt=%s", buf);
        }
    }
    if (k < n && (c->mode == CURSOR_HORIZONTAL || c->mode == CURSOR_BOTH)) {
        const microvolts_t l1 = { c->level_uv[0] }, l2 = { c->level_uv[1] };
        const scope_cursor_reading_t dv = scope_cursor_dv(&ax, l1, l2, CUR_SECOND_MINUS_FIRST);
        scope_cursor_format(&dv, buf, sizeof(buf));
        if (dv.unit == SCOPE_CURSOR_UNIT_VOLTS)
            (void)snprintf(out + k, n - k, "  dV%u=%s", ax.channel, buf);
        else
            (void)snprintf(out + k, n - k, "  dV=%s", buf);
    }
}

static uint32_t g_cur_row_hash;   /* hash of the text the AUX row shows now */

/* Repaint the AUX row with the cursor readout when its text changed (by
 * hash), or always when force is set. */
void cursor_row_paint(const scope_state_t *ss, bool force)
{
    char t[64];
    cursor_readout(ss, t, sizeof(t));
    uint32_t h = 2166136261u;                         /* FNV-1a */
    for (const char *p = t; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    if (!force && h == g_cur_row_hash) return;
    g_cur_row_hash = h;
    const theme_t *th = theme_get();
    const uint16_t y = strip_top(ss);                 /* the AUX row */
    lcd_fill_rect(0, y, LCD_WIDTH, MSTRIP_ROW_H, th->background);
    font_draw_string(4, (uint16_t)(y + 1), t, th->highlight, th->background, &font_small);
}

/* Main loop, every tick in the time view: the row follows the cursors and
 * the data (dV depends on the drawn scale) without a full repaint. */
void scope_ui_cursor_row_refresh(void)
{
    const scope_state_t *ss = scope_state_get();
    if (ss->cursor.mode == CURSOR_OFF || can_scope_screen_on()) return;
    cursor_row_paint(ss, false);
}

/* The readout of the cursors on a spectrum, into out (n bytes): the mode,
 * then the two frequencies and their difference, then the level difference
 * (with the level lines alone, the two levels as well). Frequencies are in
 * hertz when the axis is known, else bin numbers; levels are dB re one count
 * of the transform's input, the unit of the plot's own scale. Differences
 * are second minus first, as in the time views. */
static void spectrum_cursor_readout(const scope_state_t *ss, const fft_axes_t *ax, char *out, size_t n)
{
    const cursor_state_t *c = &ss->cursor;
    const spectrum_cursor_t *sc = &ss->spectrum_cursor;
    const bool v = (c->mode == CURSOR_VERTICAL || c->mode == CURSOR_BOTH);
    const bool h = (c->mode == CURSOR_HORIZONTAL || c->mode == CURSOR_BOTH);
    char a[16], b[16], d[16];
    size_t k = 0;
    out[0] = '\0';
    if (c->mode == CURSOR_OFF) return;
    k += (size_t)snprintf(out + k, n - k, "%s", !h ? "CUR:F" : !v ? "CUR:L" : "CUR:FL");
    if (v && k < n) {
        const int32_t db = (int32_t)sc->bin[1] - (int32_t)sc->bin[0];
        if (fft_live_axis_known(ax->sample_rate_hz) && ax->fft_size != 0u) {
            fft_live_format_hz(fft_live_bin_hz(ax->sample_rate_hz, ax->fft_size, sc->bin[0]), a, sizeof a);
            fft_live_format_hz(fft_live_bin_hz(ax->sample_rate_hz, ax->fft_size, sc->bin[1]), b, sizeof b);
            fft_live_format_hz(fft_live_bin_hz(ax->sample_rate_hz, ax->fft_size,
                                               (uint16_t)(db < 0 ? -db : db)), d, sizeof d);
            k += (size_t)snprintf(out + k, n - k, "  F1=%s F2=%s dF=%s%s", a, b, db < 0 ? "-" : "", d);
        } else {
            k += (size_t)snprintf(out + k, n - k, "  F1=bin %u F2=bin %u dF=%ld bins",
                                  (unsigned)sc->bin[0], (unsigned)sc->bin[1], (long)db);
        }
    }
    if (h && k < n) {
        const float unit = (float)SPECTRUM_CURSOR_LEVEL_UNIT;
        fft_live_format_db((float)(sc->level[1] - sc->level[0]) / unit, "dB", d, sizeof d);
        if (!v) {
            fft_live_format_db((float)sc->level[0] / unit, "dB", a, sizeof a);
            fft_live_format_db((float)sc->level[1] / unit, "dB", b, sizeof b);
            (void)snprintf(out + k, n - k, "  L1=%s L2=%s dL=%s", a, b, d);
        } else {
            (void)snprintf(out + k, n - k, "  dL=%s", d);
        }
    }
}

/* The spectrum strip's AUX row: the cursor readout, repainted when its text
 * changed or when force is set. y: the row's top. */
void spectrum_cursor_row_paint(const scope_state_t *ss, const fft_axes_t *ax, uint16_t y, bool force)
{
    static uint32_t shown;                            /* hash of the text the row shows now */
    char t[64];
    spectrum_cursor_readout(ss, ax, t, sizeof(t));
    uint32_t hsh = 2166136261u;                       /* FNV-1a */
    for (const char *p = t; *p; p++) hsh = (hsh ^ (uint8_t)*p) * 16777619u;
    if (!force && hsh == shown) return;
    shown = hsh;
    const theme_t *th = theme_get();
    lcd_fill_rect(0, y, LCD_WIDTH, MSTRIP_ROW_H, th->background);
    font_draw_string(4, (uint16_t)(y + 1), t, th->highlight, th->background, &font_small);
}
