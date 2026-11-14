/*
 * OpenScope 2C53T - Status bar (top), info bar (bottom) and splash screen
 *
 * Layout, slot by slot, in status_bar.h. UI round 1 (R1-B, UI_STUDY.md 2.1):
 * the brand text is gone (the boot screen and About say OpenScope, the case
 * says 2C53T); the bar carries what stock's does, plus the trigger state.
 *
 * Flicker: draw_status_bar() runs every second (main.c's 1 s timer), on
 * every full redraw and when status_bar_live_key() changes (the RUN tab and
 * the trigger dot, polled by the display loop). Each slot keeps a key of what it last painted and is
 * repainted only when that changes; status_bar_invalidate() (theme or mode
 * change, DCMD_REDRAW_ALL) forgets them all.
 */

#include "ui.h"
#include "status_bar.h"
#include "icons.h"
#include "key_model.h"          /* key_focus_t (read only) */
#include "lcd.h"
#include "font.h"
#include "theme.h"
#include "scope_state.h"
#include "scope_cal.h"
#include "scope_timebase.h"
#include "scope_roll.h"
#include "signal_gen.h"
#include "trig_policy.h"
#include "battery.h"
#include "at32f403a_407.h"
#include "fpga.h"
#include "units.h"              /* rec_rail_run */
#include <stdio.h>
#include <string.h>

/* R1-K exports these from key_model (UI round 1 brief); until that lands,
 * status_bar_stub.c supplies weak versions reading today's globals. */
extern uint8_t key_focus_get(void);
extern uint8_t key_active_channel_get(void);

/* Colours not (yet) in theme_t. TODO(R3-E): the `amber` token of UI_STUDY 4.2. */
#define SB_AMBER     0xFD20u
#define SB_TAB_TEXT  0x0000u    /* text/icon on a filled tab: black reads on every tab colour */

/* ═══════════════════════════════════════════════════════════════════
 * Small drawing helpers (icons.c rasterises, these blit a row at a time)
 * ═══════════════════════════════════════════════════════════════════ */

static void blit_icon(uint16_t x, uint16_t y, icon_id_t id, uint16_t fg, uint16_t bg)
{
    const icon_bitmap_t *ic = icon_bitmap(id);
    uint16_t line[ICON_MAX_W];
    if (ic == NULL) return;
    for (uint8_t r = 0; r < ic->h; r++) {
        icon_row(ic, r, fg, bg, line);
        lcd_blit_rect(x, (uint16_t)(y + r), ic->w, 1, line);
    }
}

/* The sheared tab: full width at the top, one pixel in per side every
 * ICON_TAB_SHEAR rows. The slot's background is painted around it. */
static void draw_tab(uint16_t x, uint16_t w, uint16_t fill, uint16_t bg)
{
    for (uint8_t r = 0; r < SB_H; r++) {
        const uint16_t in = icon_tab_inset(r);
        lcd_fill_rect(x, r, in, 1, bg);
        lcd_fill_rect((uint16_t)(x + in), r, (uint16_t)(w - 2u * in), 1, fill);
        lcd_fill_rect((uint16_t)(x + w - in), r, in, 1, bg);
    }
}

/* font_draw_string with bg == fg draws only the glyph pixels (font.h). */
static void text_over(uint16_t x, uint16_t y, const char *s, uint16_t fg)
{
    font_draw_string(x, y, s, fg, fg, &font_small);
}

static uint32_t mix(uint32_t h, uint32_t v)
{
    h ^= v;
    h *= 16777619u;             /* FNV-1a step */
    return h;
}

static uint32_t mix_str(uint32_t h, const char *s)
{
    while (*s) h = mix(h, (uint8_t)*s++);
    return mix(h, 0u);
}

/* ═══════════════════════════════════════════════════════════════════
 * Slot cache
 * ═══════════════════════════════════════════════════════════════════ */

enum { SL_FRAME, SL_GEN, SL_TB, SL_RUN, SL_FOCUS, SL_DOT, SL_VIEW, SL_BATT, SL_COUNT };
static uint32_t slot_key[SL_COUNT];
static bool     slot_valid[SL_COUNT];

/* true when the slot must repaint (and records the new key). */
static bool slot_changed(unsigned s, uint32_t key)
{
    if (slot_valid[s] && slot_key[s] == key)
        return false;
    slot_valid[s] = true;
    slot_key[s] = key;
    return true;
}

/* Title for the mode it was set in (status_bar_set_title). */
static char    s_title[SB_TITLE_MAX];
static uint8_t s_title_mode = 0xFFu;

void status_bar_set_title(const char *title)
{
    if (title == NULL || title[0] == '\0') {
        s_title_mode = 0xFFu;
        return;
    }
    s_title_mode = 0xFFu;               /* off while the text is replaced */
    strncpy(s_title, title, sizeof(s_title) - 1u);
    s_title[sizeof(s_title) - 1u] = '\0';
    s_title_mode = (uint8_t)current_mode;
}

static const char *title_for_mode(void)
{
    if (s_title_mode == (uint8_t)current_mode)
        return s_title;
    switch (current_mode) {
    case MODE_MULTIMETER: return "Multimeter";
    case MODE_SIGNAL_GEN: return "Signal Generator";
    case MODE_SETTINGS:   return "Settings";
    case MODE_HOME:       return "Home";
    default:              return "";
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Top bar slots
 * ═══════════════════════════════════════════════════════════════════ */

/* S8: the generator's waveform, green tab while the output is on. */
static void slot_gen(const theme_t *th)
{
    const siggen_config_t *g = siggen_get_config();
    const uint8_t wave = g ? g->waveform : 0u;
    const uint8_t duty = (g && siggen_wave_uses_duty(wave)) ? g->duty_pct : 50u;
    const bool on = g && g->output_enabled;
    if (!slot_changed(SL_GEN, (uint32_t)wave | ((uint32_t)duty << 8) | ((uint32_t)on << 16)))
        return;

    const uint16_t bg = th->status_bar_bg;
    const uint16_t tab = on ? th->success : bg;
    const uint16_t fg  = on ? SB_TAB_TEXT : th->text_secondary;
    if (on) draw_tab(SB_GEN_X, SB_GEN_W, tab, bg);
    else    lcd_fill_rect(SB_GEN_X, 0, SB_GEN_W, SB_H, bg);

    uint8_t pts[ICON_WAVE_W];
    uint16_t line[ICON_WAVE_W];
    icon_wave_points(wave, duty, pts);
    const uint16_t ix = SB_GEN_X + (SB_GEN_W - ICON_WAVE_W) / 2u;
    for (uint8_t r = 0; r < ICON_WAVE_H; r++) {
        icon_wave_row(pts, r, fg, tab, line);
        lcd_blit_rect(ix, (uint16_t)(SB_TEXT_Y + r), ICON_WAVE_W, 1, line);
    }
}

/* The view's seconds per division: the code's, /2 for a merged record, /zoom
 * at the fast codes -- the same quantity scope_timebase_view_label prints. */
static float view_s_per_div(const scope_state_t *ss, bool merged)
{
    const float spd = scope_timebase_seconds_per_div(ss->timebase_idx);
    const uint8_t L = scope_timebase_interp_factor(ss->timebase_idx, merged);
    return spd / ((merged ? 2.0f : 1.0f) * (float)(L ? L : 1u));
}

/* What the screen showed while running, for the paused-zoom suffix (2.4).
 * Updated on every draw while running; STOP's own full redraw reaches here
 * before any timebase key can, so it holds the captured view's s/div. */
static float s_spd_running;

/* S4 + 2.4: "H=100ns"; amber "H=50ns x2" while a held record is zoomed. */
static void slot_timebase(const theme_t *th, const scope_state_t *ss)
{
    char tb[12], buf[24];
    uint16_t col = th->text_primary;
    if (ss->roll) {
        scope_roll_label(ss->roll_step, tb, sizeof(tb));
        snprintf(buf, sizeof(buf), "H=%s", tb);
    } else {
        const bool merged = scope_ui_view_merged();
        scope_timebase_view_label(ss->timebase_idx, merged, tb, sizeof(tb));
        snprintf(buf, sizeof(buf), "H=%s", tb);
        const float now = view_s_per_div(ss, merged);
        if (ss->running) {
            s_spd_running = now;
        } else if (fpga_data_ready() && s_spd_running > 0.0f && now > 0.0f) {
            /* A held record (scope_ui.c view_held) re-sampled at another
             * timebase: say how far it is zoomed against the capture. */
            const float z = s_spd_running / now;
            if (z > 1.01f || z < 0.99f) {
                char sfx[10];
                const float m = (z >= 1.0f) ? z : 1.0f / z;
                const char op = (z >= 1.0f) ? 'x' : '/';
                const unsigned mi = (unsigned)(m + 0.5f);
                if (m >= 9.95f || (m - (float)mi < 0.05f && (float)mi - m < 0.05f))
                    snprintf(sfx, sizeof(sfx), " %c%u", op, mi);
                else
                    snprintf(sfx, sizeof(sfx), " %c%u.%u", op, (unsigned)m,
                             (unsigned)((m - (float)(unsigned)m) * 10.0f + 0.5f) % 10u);
                strncat(buf, sfx, sizeof(buf) - strlen(buf) - 1u);
                col = SB_AMBER;
            }
        }
    }
    if (!slot_changed(SL_TB, mix_str(col, buf)))
        return;
    lcd_fill_rect(SB_TB_X, 0, SB_TB_W, SB_H, th->status_bar_bg);
    font_draw_string(SB_TB_X + 2u, SB_TEXT_Y, buf, col, th->status_bar_bg, &font_small);
}

/* S3: RUN (green) / STOP (red) / ARM (amber, SINGLE armed), as a tab. */
static void slot_run(const theme_t *th, const scope_state_t *ss, trig_status_t st)
{
    uint8_t k;
    if (!ss->running || st == TRIG_ST_STOP || st == TRIG_ST_SINGLE_DONE) k = 1u;
    else if (st == TRIG_ST_ARMED)                                        k = 2u;
    else                                                                 k = 0u;
    if (!slot_changed(SL_RUN, k))
        return;
    static const char *const words[3] = { "RUN", "STOP", "ARM" };
    const uint16_t fill = (k == 1u) ? th->warning : (k == 2u) ? SB_AMBER : th->success;
    draw_tab(SB_RUN_X, SB_RUN_W, fill, th->status_bar_bg);
    const uint16_t w = font_string_width(words[k], &font_small);
    text_over((uint16_t)(SB_RUN_X + (SB_RUN_W - w) / 2u), SB_TEXT_Y, words[k], SB_TAB_TEXT);
}

/* S2: what the arrows adjust, in stock's glyphs and colours:
 *   KF_ZOOM      "V H"    V in the active channel's colour, H white
 *   KF_CHMOVE    ">  v"   both in the channel colour (lines.jpeg)
 *   KF_TRIGMOVE  "<  v"   both in the trigger colour (IMG_0324.jpeg)
 *   KF_CURSOR    "X1 Y1"  (or "X2 Y2" when the second pair is selected)
 *   KF_MASK      "Mask"   ours: stock has no mask stage */
static void slot_focus(const theme_t *th, const scope_state_t *ss)
{
    const uint8_t f  = key_focus_get();
    const uint8_t ch = key_active_channel_get() ? 1u : 0u;
    const uint8_t cs = (ss->cursor.active == CURSOR_SEL_V2 ||
                        ss->cursor.active == CURSOR_SEL_H2) ? 1u : 0u;
    if (!slot_changed(SL_FOCUS, (uint32_t)f | ((uint32_t)ch << 8) | ((uint32_t)cs << 9)))
        return;
    const uint16_t bg = th->status_bar_bg;
    const uint16_t cc = ch ? th->ch2 : th->ch1;
    const uint16_t x = SB_FOCUS_X;
    lcd_fill_rect(x, 0, SB_FOCUS_W, SB_H, bg);
    switch ((key_focus_t)f) {
    case KF_ZOOM:
        font_draw_string(x + 7u, SB_TEXT_Y, "V", cc, bg, &font_small);
        font_draw_string(x + 23u, SB_TEXT_Y, "H", th->text_primary, bg, &font_small);
        break;
    case KF_CHMOVE:
        blit_icon(x + 7u, 4u, ICON_ARROW_RIGHT, cc, bg);
        blit_icon(x + 22u, 6u, ICON_ARROW_DOWN, cc, bg);
        break;
    case KF_TRIGMOVE:
        blit_icon(x + 7u, 4u, ICON_ARROW_LEFT, th->trigger, bg);
        blit_icon(x + 22u, 6u, ICON_ARROW_DOWN, th->trigger, bg);
        break;
    case KF_CURSOR:
        font_draw_string_center(x + SB_FOCUS_W / 2u, SB_TEXT_Y, cs ? "X2 Y2" : "X1 Y1",
                                th->text_primary, bg, &font_small);
        break;
    case KF_MASK:
        font_draw_string_center(x + SB_FOCUS_W / 2u, SB_TEXT_Y, "Mask",
                                th->text_primary, bg, &font_small);
        break;
    default:
        break;
    }
}

/* S11: the trigger state as a dot (the plot's "Trig'd" word in colour). */
static void slot_dot(const theme_t *th, trig_status_t st)
{
    uint16_t c;
    switch (st) {
    case TRIG_ST_TRIGD:      c = th->success; break;
    case TRIG_ST_AUTO:
    case TRIG_ST_AUTO_SPLIT: c = SB_AMBER;    break;
    default:                 c = th->text_secondary; break;   /* wait, armed, stopped */
    }
    if (!slot_changed(SL_DOT, c))
        return;
    lcd_fill_rect(SB_DOT_X, 0, SB_DOT_W, SB_H, th->status_bar_bg);
    blit_icon(SB_DOT_X + 1u, 5u, ICON_DOT, c, th->status_bar_bg);
}

/* The scope's view word, or a title another screen set for the scope. */
static void slot_view(const theme_t *th, const scope_state_t *ss)
{
    const char *w;
    if (s_title_mode == (uint8_t)current_mode) {
        w = s_title;
    } else {
        switch (scope_view) {
        case SCOPE_VIEW_FFT:       w = "FFT";   break;
        case SCOPE_VIEW_SPLIT:     w = "SPLIT"; break;
        case SCOPE_VIEW_WATERFALL: w = "WFALL"; break;
        case SCOPE_VIEW_XY:        w = "X-Y";   break;
        default:                   w = ss->roll ? "ROLL" : "Y-T"; break;
        }
    }
    if (!slot_changed(SL_VIEW, mix_str(0u, w)))
        return;
    lcd_fill_rect(SB_VIEW_X, 0, SB_VIEW_W, SB_H, th->status_bar_bg);
    font_draw_string_center(SB_VIEW_X + SB_VIEW_W / 2u, SB_TEXT_Y, w,
                            th->text_secondary, th->status_bar_bg, &font_small);
}

/* Other modes: the title, centred over the scope's middle slots. */
static void slot_title(const theme_t *th)
{
    const char *t = title_for_mode();
    if (!slot_changed(SL_VIEW, mix_str(1u, t)))
        return;
    lcd_fill_rect(SB_TITLE_X, 0, SB_TITLE_W, SB_H, th->status_bar_bg);
    font_draw_string_center(SB_TITLE_X + SB_TITLE_W / 2u, SB_TEXT_Y, t,
                            th->text_primary, th->status_bar_bg, &font_small);
}

/* B1: 22x10 glyph + "%". On external power: bolt in the glyph, and the
 * percentage stays (the gauge takes the charging step off). Critical: red, blinking each second, "LOW". */
static void slot_battery(const theme_t *th)
{
    const uint16_t bg = th->status_bar_bg;
    const bool crit = battery_is_critical() != 0u;
    const bool chg  = !crit && battery_is_charging() != 0u;
    const uint8_t pct = battery_percent();
    const bool blink_on = crit ? ((uptime_seconds & 1u) != 0u) : true;
    if (!slot_changed(SL_BATT, (uint32_t)pct | ((uint32_t)crit << 8) | ((uint32_t)chg << 9)
                               | ((uint32_t)blink_on << 10)))
        return;

    uint16_t fill, fg = th->text_primary;
    char buf[8] = "";
    if (crit) {
        fill = th->warning;
        fg   = blink_on ? th->warning : bg;
        strcpy(buf, "LOW");
    } else if (chg) {
        fill = th->success;
        snprintf(buf, sizeof(buf), "%u%%", (unsigned)pct);
    } else {
        fill = (pct <= 15u) ? th->warning : (pct <= 30u) ? SB_AMBER : th->success;
        snprintf(buf, sizeof(buf), "%u%%", (unsigned)pct);
    }
    const uint8_t bars = crit ? 1u : icon_battery_bars(pct);

    lcd_fill_rect(SB_BATT_X, 0, SB_BATT_W, SB_H, bg);
    uint16_t line[ICON_BATT_W];
    for (uint8_t r = 0; r < ICON_BATT_H; r++) {
        icon_battery_row(r, bars, chg, blink_on ? fg : bg, blink_on ? fill : bg, bg, line);
        lcd_blit_rect(SB_BATT_X, (uint16_t)(3u + r), ICON_BATT_W, 1, line);
    }
    if (buf[0])
        font_draw_string_right(LCD_WIDTH - 2u, SB_TEXT_Y, buf,
                               crit ? fg : fill, bg, &font_small);
}

static uint32_t s_info_drawn_epoch;
static bool s_info_drawn;
static uint8_t s_info_drawn_clip;      /* info_clip() as last drawn */

/* Bit 0 CH1, bit 1 CH2: the channel is on and its last record holds an ADC
 * rail for REC_RAIL_RUN_MIN samples in a row (units.h rec_rail_run). Over
 * the record's valid part: samples 1..1023 (sample 0 is the one the seam
 * search also skips, a stray head value on unit #1, trig_edge.c). 0 before
 * the first record and in Roll, which draws its own point FIFO, not these
 * buffers. */
#define INFO_CLIP_FIRST 1u
static uint8_t info_clip(void)
{
    const scope_state_t *ss = scope_state_get();
    if (!fpga_data_ready() || ss->roll) return 0u;
    const uint32_t n = FPGA_ADC_BUF_SIZE - INFO_CLIP_FIRST;
    uint8_t m = 0u;
    if (ss->ch1.enabled && rec_rail_run(fpga_get_ch1_buf() + INFO_CLIP_FIRST, n, REC_RAIL_RUN_MIN)) m |= 1u;
    if (ss->ch2.enabled && rec_rail_run(fpga_get_ch2_buf() + INFO_CLIP_FIRST, n, REC_RAIL_RUN_MIN)) m |= 2u;
    return m;
}

/* Seconds of critical battery so far (the timer service task writes, the
 * display task reads). Like the counter it replaces, it never counts back
 * down when the battery recovers. */
static volatile uint8_t s_critical_seconds;

void status_bar_second_tick(void)
{
    if (battery_is_critical() && s_critical_seconds < 0xFFu)
        s_critical_seconds++;
}

uint32_t status_bar_live_key(void)
{
    if (current_mode != MODE_OSCILLOSCOPE)
        return 0u;
    return (uint32_t)fpga_trigger_status() | ((uint32_t)scope_state_get()->running << 8) | 0x10000u;
}

/* Draw the top status bar */
void draw_status_bar(void)
{
    const theme_t *th = theme_get();

    /* Full clear on first draw, invalidate, mode or theme change. Every
     * caller that gets here this way (boot, DCMD_REDRAW_ALL) draws the info
     * bar itself right after, so the self-repair below stands aside. */
    const bool full = slot_changed(SL_FRAME, (uint32_t)current_mode | ((uint32_t)theme_get_id() << 8));
    if (full) {
        for (unsigned s = 0; s < SL_COUNT; s++)
            if (s != SL_FRAME) slot_valid[s] = false;
        lcd_fill_rect(0, 0, LCD_WIDTH, SB_H, th->status_bar_bg);
    }

    slot_gen(th);
    if (current_mode == MODE_OSCILLOSCOPE) {
        const scope_state_t *ss = scope_state_get();
        const trig_status_t st = (trig_status_t)fpga_trigger_status();
        slot_timebase(th, ss);
        slot_run(th, ss, st);
        slot_focus(th, ss);
        slot_dot(th, st);
        slot_view(th, ss);
        /* The info bar's own inputs, for changes main.c's epoch does not
         * hash (the trigger level, a label from a new calibration). */
        if (!full && (scope_view == SCOPE_VIEW_TIME || scope_view == SCOPE_VIEW_FFT) &&
            (!s_info_drawn || status_bar_info_epoch() != s_info_drawn_epoch ||
             info_clip() != s_info_drawn_clip))
            draw_info_bar();
    } else {
        slot_title(th);
    }
    slot_battery(th);

    /* Auto-shutdown at critical battery — protect the LiPo cell. The seconds
     * are counted by status_bar_second_tick() (the 1 s timer), not per draw. */
    if (battery_is_critical()) {
        if (s_critical_seconds >= 30u) {  /* 30 seconds of critical = shutdown */
            lcd_fill_rect(40, 80, 240, 80, 0x0000);
            font_draw_string_center(160, 95, "Battery Critical",
                                    th->warning, 0x0000, &font_large);
            font_draw_string_center(160, 130, "Shutting down...",
                                    th->text_secondary, 0x0000, &font_medium);
            /* Brief delay so user can see the message */
            for (volatile uint32_t d = 0; d < 100000000; d++);
            GPIOC->clr = (1U << 9);  /* PC9 LOW = power off */
            while (1);
        }
    }
}

/* Force full redraw on next status bar update (call on theme/mode change) */
void status_bar_invalidate(void)
{
    for (unsigned s = 0; s < SL_COUNT; s++)
        slot_valid[s] = false;
    s_info_drawn = false;
}

/* ═══════════════════════════════════════════════════════════════════
 * Info bar (bottom)
 * ═══════════════════════════════════════════════════════════════════ */

static const char *const trig_mode_short[TRIG_COUNT] = { "Auto", "Norm", "Single" };

/* "1.37V X1 DC": volts/div (measured, probe included: scope_cal), the probe
 * factor (stock shows it always, S7) and the coupling. */
typedef struct { char vdiv[12]; const char *probe, *cpl; } ch_fields_t;

static void ch_fields(uint8_t ch, const channel_state_t *c, ch_fields_t *f)
{
    scope_cal_range_label(ch, c->vdiv_idx, f->vdiv, sizeof(f->vdiv));
    f->probe = (c->probe == PROBE_10X) ? "X10" : "X1";
    f->cpl = (unsigned)c->coupling < (unsigned)COUPLING_COUNT ? coupling_labels[c->coupling] : "?";
}

/* A channel badge: the number chip, then the three fields 3 px apart, all in
 * `col`. With `right` the badge ends at x; otherwise it starts there. If it
 * would not fit IB_CH_W the chip goes (the colour and side still say which). */
static void draw_ch_badge(uint16_t x, bool right, uint8_t ch, const channel_state_t *c,
                          uint16_t col, uint16_t bg, bool clipped, uint16_t warn)
{
    ch_fields_t f;
    ch_fields(ch, c, &f);
    const uint16_t tw = (uint16_t)(font_string_width(f.vdiv, &font_small) + IB_FIELD_GAP +
                                   font_string_width(f.probe, &font_small) + IB_FIELD_GAP +
                                   font_string_width(f.cpl, &font_small));
    const bool chip = (IB_CHIP_W + IB_CHIP_GAP + tw) <= IB_CH_W;
    const uint16_t w  = (uint16_t)((chip ? IB_CHIP_W + IB_CHIP_GAP : 0u) + tw);
    uint16_t x0 = right ? (uint16_t)(x - w) : x;
    if (chip) {
        /* A record at an ADC rail: the chip turns the warning colour. */
        const char num[2] = { (char)('0' + ch), '\0' };
        lcd_fill_rect(x0, IB_TEXT_Y, IB_CHIP_W, 12, clipped ? warn : col);
        text_over((uint16_t)(x0 + (IB_CHIP_W - font_string_width(num, &font_small) + 1u) / 2u),
                  IB_TEXT_Y, num, bg);
        x0 = (uint16_t)(x0 + IB_CHIP_W + IB_CHIP_GAP);
    }
    x0 = (uint16_t)(x0 + font_draw_string(x0, IB_TEXT_Y, f.vdiv, (clipped && !chip) ? warn : col, bg,
                                          &font_small) + IB_FIELD_GAP);
    x0 = (uint16_t)(x0 + font_draw_string(x0, IB_TEXT_Y, f.probe, col, bg, &font_small) + IB_FIELD_GAP);
    font_draw_string(x0, IB_TEXT_Y, f.cpl, col, bg, &font_small);
}

/* S5: (T) Auto [edge] CH1 [+20], centred in the badge slot. */
static void draw_trig_badge(const theme_t *th, const scope_state_t *ss, uint16_t bg)
{
    const trigger_state_t *t = &ss->trigger;
    const char *mode = (unsigned)t->mode < (unsigned)TRIG_COUNT ? trig_mode_short[t->mode] : "?";
    const char *src  = (t->source == TRIG_SRC_CH2) ? "CH2" : "CH1";
    const uint16_t scol = (t->source == TRIG_SRC_CH2) ? th->ch2 : th->ch1;
    const icon_bitmap_t *disc = icon_bitmap(ICON_TRIG_DISC);
    const icon_bitmap_t *edge = icon_bitmap(t->edge == TRIG_FALLING ? ICON_EDGE_FALL : ICON_EDGE_RISE);
    char lv[8] = "";
    if (t->level != 0)
        snprintf(lv, sizeof(lv), "%+d", (int)t->level);

    const uint16_t gap = 3u;
    const uint16_t wm = font_string_width(mode, &font_small);
    const uint16_t ws = font_string_width(src, &font_small);
    const uint16_t wl = lv[0] ? (uint16_t)(gap + font_string_width(lv, &font_small)) : 0u;
    const uint16_t w  = (uint16_t)(disc->w + gap + wm + gap + edge->w + gap + ws + wl);
    uint16_t x = (uint16_t)(IB_TRIG_X + (w < IB_TRIG_W ? (IB_TRIG_W - w) / 2u : 0u));

    uint16_t line[ICON_MAX_W];
    for (uint8_t r = 0; r < disc->h; r++) {
        icon_row(disc, r, th->trigger, bg, line);
        icon_row_over(icon_bitmap(ICON_TRIG_T), r, 0, SB_TAB_TEXT, line, disc->w);
        lcd_blit_rect(x, (uint16_t)(IB_TEXT_Y + r), disc->w, 1, line);
    }
    x = (uint16_t)(x + disc->w + gap);
    x = (uint16_t)(x + font_draw_string(x, IB_TEXT_Y, mode, th->text_primary, bg, &font_small) + gap);
    blit_icon(x, IB_TEXT_Y, t->edge == TRIG_FALLING ? ICON_EDGE_FALL : ICON_EDGE_RISE, scol, bg);
    x = (uint16_t)(x + edge->w + gap);
    x = (uint16_t)(x + font_draw_string(x, IB_TEXT_Y, src, scol, bg, &font_small));
    if (lv[0])
        font_draw_string((uint16_t)(x + gap), IB_TEXT_Y, lv, th->text_secondary, bg, &font_small);
}

uint32_t status_bar_info_epoch(void)
{
    const scope_state_t *ss = scope_state_get();
    ch_fields_t f;
    uint32_t h = 2166136261u;
    h = mix(h, (uint32_t)current_mode | ((uint32_t)theme_get_id() << 8) | ((uint32_t)scope_view << 16));
    ch_fields(1u, &ss->ch1, &f);
    h = mix(mix_str(mix_str(mix_str(h, f.vdiv), f.probe), f.cpl), ss->ch1.enabled);
    ch_fields(2u, &ss->ch2, &f);
    h = mix(mix_str(mix_str(mix_str(h, f.vdiv), f.probe), f.cpl), ss->ch2.enabled);
    h = mix(h, (uint32_t)ss->trigger.mode | ((uint32_t)ss->trigger.edge << 4)
               | ((uint32_t)ss->trigger.source << 8));
    h = mix(h, (uint32_t)(uint16_t)ss->trigger.level);
    h = mix(h, (uint32_t)key_active_channel_get());   /* the FFT bar names the active channel */
    return h;
}

/* Draw the bottom info bar */
void draw_info_bar(void)
{
    const theme_t *th = theme_get();
    /* The meter's bottom edge is its softkey bar (softkey UI P1). */
    if (current_mode == MODE_MULTIMETER) {
        meter_softkeys_draw();
        return;
    }
    /* G1: the generator page shows its own settings; no placeholder here. */
    if (current_mode == MODE_SIGNAL_GEN)
        return;
    lcd_fill_rect(0, IB_Y, LCD_WIDTH, SB_H, th->status_bar_bg);

    uint16_t ib = th->status_bar_bg;
    switch (current_mode) {
    case MODE_OSCILLOSCOPE:
#if FEATURE_FFT
        if (scope_view == SCOPE_VIEW_FFT) {
            const char *win_names[] = { "Rect", "Hann", "Hamm", "BHar", "Flat" };
            const fft_config_t *cfg = fft_get_config();
            font_draw_string(4, LCD_HEIGHT - 14, "FFT 4096pt",
                             th->ch1, ib, &font_small);
            font_draw_string(80, LCD_HEIGHT - 14,
                             (cfg->window < FFT_WINDOW_COUNT) ? win_names[cfg->window] : "?",
                             th->success, ib, &font_small);
            if (cfg->avg_count > 0)
                font_draw_string(120, LCD_HEIGHT - 14, "AVG",
                                 th->ch2, ib, &font_small);
            if (cfg->max_hold)
                font_draw_string(150, LCD_HEIGHT - 14, "MH",
                                 th->warning, ib, &font_small);
            /* UP/DOWN change the ACTIVE channel's V/div here too, and the
             * channel badges are not on this bar: show that channel's
             * measured V/div at the right (it replaces the "SEL:Window"
             * hint), and CLIP when that channel's record touches an ADC
             * rail, whose flat tops put false harmonics in the spectrum.
             * It is the channel the spectrum analyses (dsp/fft_source.h);
             * grey when it is switched off (the plot says "CHn is off"). */
            const scope_state_t *ss = scope_state_get();
            const uint8_t ach = key_active_channel_get() ? 2u : 1u;
            const channel_state_t *ac = (ach == 2u) ? &ss->ch2 : &ss->ch1;
            ch_fields_t fv;
            ch_fields(ach, ac, &fv);
            char lab[20];
            snprintf(lab, sizeof lab, "CH%u %s", (unsigned)ach, fv.vdiv);
            font_draw_string_right(LCD_WIDTH - 4u, LCD_HEIGHT - 14, lab,
                                   !ac->enabled ? th->text_secondary
                                   : ach == 2u ? th->ch2 : th->ch1, ib, &font_small);
            const uint8_t clip = info_clip();
            if (clip & (1u << (ach - 1u)))
                font_draw_string(176, LCD_HEIGHT - 14, "CLIP", th->warning, ib, &font_small);
            s_info_drawn_epoch = status_bar_info_epoch();
            s_info_drawn_clip = clip;
            s_info_drawn = true;
        } else
#endif
        {
            const scope_state_t *ss = scope_state_get();
            /*
             * Channel badges (S7): "1.37V X1 DC". The volts/div shown is
             * DERIVED from the bench-measured gain for this channel on this
             * range (scope_cal.h), not read out of vdiv_table: the nominal
             * labels there were never traced to a measurement and are
             * 2.7x-3.5x out on the three ranges cross-validated. Uncalibrated
             * ranges read "--", provisional ones carry a leading '~'. CH2's
             * frontend measures a different gain from CH1's on the same
             * range index, which is why the label is per channel. The
             * timebase moved to the top bar (S4), which frees the centre for
             * the trigger badge (S5).
             */
            const uint8_t clip = info_clip();
            draw_ch_badge(IB_CH1_X, false, 1u, &ss->ch1,
                          ss->ch1.enabled ? th->ch1 : th->text_secondary, ib, (clip & 1u) != 0u, th->warning);
            draw_trig_badge(th, ss, ib);
            draw_ch_badge(IB_CH2_X + IB_CH_W, true, 2u, &ss->ch2,
                          ss->ch2.enabled ? th->ch2 : th->text_secondary, ib, (clip & 2u) != 0u, th->warning);
            s_info_drawn_epoch = status_bar_info_epoch();
            s_info_drawn_clip = clip;
            s_info_drawn = true;
        }
        break;
    case MODE_SETTINGS:
        font_draw_string(4, LCD_HEIGHT - 14, "MENU/SELECT to navigate",
                         th->text_secondary, ib, &font_small);
        break;
    case MODE_HOME:
        font_draw_string(4, LCD_HEIGHT - 14, "LEFT/RIGHT select  OK enter  MENU back",
                         th->text_secondary, ib, &font_small);
        break;
    default:
        break;
    }
}

/* Boot splash.
 *
 * If a generated splash_image.h exists (see scripts/generate_splash.py — any
 * PNG/JPG dropped at firmware/assets/splash.png, letterboxed to 320x240 and
 * RLE-compressed at build-prep time), draw that. Otherwise fall back to the
 * original text splash, so the build never depends on the asset pipeline. */
#if defined(__has_include)
#  if __has_include("splash_image.h")
#    include "splash_image.h"
#  endif
#endif

void draw_splash(void)
{
#ifdef SPLASH_IMAGE_H
    /* Stream the RLE image straight into one full-screen window. Records are
     * (uint8 run, uint16 LE color); runs cross row boundaries freely. */
    lcd_set_window(0, 0, SPLASH_IMAGE_WIDTH, SPLASH_IMAGE_HEIGHT);
    const uint8_t *p   = splash_image_rle;
    const uint8_t *end = splash_image_rle + sizeof(splash_image_rle);
    while (p + 3 <= end) {
        uint8_t  run   = p[0];
        uint16_t color = (uint16_t)(p[1] | ((uint16_t)p[2] << 8));
        p += 3;
        while (run--)
            lcd_write_data(color);
    }
#else
    lcd_clear(COLOR_BLACK);

    /* Title - large font, centered */
    font_draw_string_center(LCD_WIDTH / 2, 50, "OpenScope 2C53T",
                            COLOR_CYAN, COLOR_BLACK, &font_large);

    /* Version */
    font_draw_string_center(LCD_WIDTH / 2, 85, "Custom Firmware v0.1",
                            COLOR_WHITE, COLOR_BLACK, &font_medium);

    /* Hardware info */
    font_draw_string_center(LCD_WIDTH / 2, 125, "AT32F403A | FreeRTOS",
                            COLOR_GRAY, COLOR_BLACK, &font_small);
    font_draw_string_center(LCD_WIDTH / 2, 142, "ST7789V 320x240",
                            COLOR_GRAY, COLOR_BLACK, &font_small);

    /* Repo */
    font_draw_string_center(LCD_WIDTH / 2, 190, "github.com/",
                            COLOR_DARK_GRAY, COLOR_BLACK, &font_small);
    font_draw_string_center(LCD_WIDTH / 2, 206, "DavidClawson/OpenScope-2C53T",
                            COLOR_DARK_GRAY, COLOR_BLACK, &font_small);
#endif
}
