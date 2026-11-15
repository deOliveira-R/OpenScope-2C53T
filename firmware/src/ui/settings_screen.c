/*
 * OpenScope 2C53T - Settings as screens. See settings_screen.h.
 */
#include "settings_screen.h"
#include "settings_reset.h"
#include "startup_policy.h"     /* startup_mode_step: the Startup row's order */
#include "model.h"
#include "theme.h"

#include <stdio.h>

/* UI state. Written by the input task (open, key); the display task and the
 * shell read it. Which page is up is the router's stack. */
static volatile uint8_t s_row;          /* settings_item_t */
static volatile uint8_t s_sub;          /* row of Sound and Light */
static volatile bool    s_question;     /* "Reset all settings?" over the list */
static volatile uint8_t s_about_keys;   /* UP/DOWN on About: it is painted again (its live numbers) */

settings_view_t settings_screen_view(void)
{
    settings_view_t v;
    v.row = s_row;
    v.sub = s_sub;
    if (screen_is_open(SCR_SET_SOUND))      v.page = SET_PAGE_SOUND;
    else if (screen_is_open(SCR_SET_ABOUT)) v.page = SET_PAGE_ABOUT;
    else                                    v.page = s_question ? SET_PAGE_RESET : SET_PAGE_LIST;
    return v;
}

/* ── input task ────────────────────────────────────────────────────── */

/* One step of a property. OK goes round the ends, LEFT/RIGHT stop at them
 * (as the rows always did), whatever the property's own PF_WRAP says. */
static void step(const prop_desc_t *p, int dir, bool wrap)
{
    const int32_t v = prop_get(p);
    const bool at_end = (dir > 0) ? (v >= p->max) : (v <= p->min);
    if (!at_end)
        (void)prop_step_local(p, dir, ORIGIN_KEY, NULL);
    else if (wrap)
        (void)prop_set_local(p, (dir > 0) ? p->min : p->max, ORIGIN_KEY, NULL);
}

/* LEFT/RIGHT (wrap = false) or OK (dir = +1, wrap = true) on a row of the
 * list that holds a value. false: the row has none (a page or an action). */
static bool row_adjust(settings_item_t item, int dir, bool wrap)
{
    switch (item) {
    case SET_ITEM_AUTOOFF:      /* Off / 15 / 30 / 60 min */
        step(P(SYS_AUTOOFF), dir, wrap);
        return true;
    case SET_ITEM_STARTUP:      /* stock's row order (home, meter, scope, generator), then
                                 * Restore ("Last Used"); always round */
        (void)prop_set_local(P(SYS_STARTUP),
                             (int32_t)startup_mode_step((startup_mode_t)prop_get(P(SYS_STARTUP)), dir),
                             ORIGIN_KEY, NULL);
        return true;
    case SET_ITEM_METER_LINK:   /* With Screen <-> Always; the display task turns the link on or
                                 * off within a frame */
        (void)prop_step_local(P(METER_LINK), dir, ORIGIN_KEY, NULL);
        return true;
    case SET_ITEM_THEME:        /* always round */
        (void)prop_step_local(P(SYS_THEME), dir, ORIGIN_KEY, NULL);
        return true;
    default:
        return false;
    }
}

/* The keys that mean nothing on any Settings screen. true: `*res` is the
 * answer. Long keys do nothing; CH1, CH2, TRIGGER and MOVE repaint the
 * screen, as they always did here; AUTO, PRM and SELECT do nothing. */
static bool common_key(scr_key_t k, scr_res_t *res)
{
    if (k.is_long) { *res = SCR_HANDLED; return true; }
    switch (k.btn) {
    case BTN_CH1: case BTN_CH2: case BTN_TRIGGER: case BTN_MOVE:
        *res = SCR_DRAW_FULL;
        return true;
    case BTN_AUTO: case BTN_PRM: case BTN_SELECT:
        *res = SCR_HANDLED;
        return true;
    default:
        return false;
    }
}

static void list_open(screen_id_t from)
{
    (void)from;
    s_row = 0;
    s_sub = 0;
    s_question = false;
}

static void list_close(screen_id_t to)
{
    (void)to;
    s_question = false;
}

static scr_res_t list_ok(void)
{
    switch (settings_reset_on_ok(s_question ? SET_PAGE_RESET : SET_PAGE_LIST, (int8_t)s_row)) {
    case SET_RESET_ASK:             /* the first OK only asks */
        s_question = true;
        return SCR_DRAW_FULL;
    case SET_RESET_DO:              /* OK on the question: yes; the screen stays in Settings */
        settings_env_reset();
        s_question = false;
        return SCR_DRAW_FULL;
    default:
        break;
    }
    const settings_item_t item = (settings_item_t)s_row;
    if (row_adjust(item, +1, true)) return SCR_DRAW_FULL;
    switch (item) {
    case SET_ITEM_SOUND_LIGHT:      /* stock's sub-page: volume, brightness */
        s_sub = 0;
        (void)screen_push(SCR_SET_SOUND);
        break;
    case SET_ITEM_ABOUT:
        (void)screen_push(SCR_SET_ABOUT);
        break;
    case SET_ITEM_USB_SHARE:
        settings_env_usb_share_toggle();
        break;
    case SET_ITEM_FW_UPDATE:
        settings_env_fw_update();
        break;
    default:
        break;
    }
    return SCR_DRAW_FULL;
}

static scr_res_t list_key(scr_key_t k)
{
    scr_res_t res;
    if (common_key(k, &res)) return res;
    switch (k.btn) {
    case BTN_MENU:
        if (s_question) {           /* no: back to the list, nothing changed */
            s_question = false;
            settings_env_page_left();
            return SCR_DRAW_FULL;
        }
        /* Short MENU at the top level opens Home (owner decision D2; stock's
         * BACK does nothing here). */
        (void)prop_set_local(P(SYS_MODE), (int32_t)MODE_HOME, ORIGIN_KEY, NULL);
        return SCR_HANDLED;         /* the mode switch repaints */
    case BTN_UP:
        if (!s_question && s_row > 0) s_row--;
        return SCR_DRAW;
    case BTN_DOWN:
        if (!s_question && s_row < SETTINGS_ITEM_COUNT - 1) s_row++;
        return SCR_DRAW;
    case BTN_LEFT:
    case BTN_RIGHT:
        if (!s_question && row_adjust((settings_item_t)s_row, k.btn == BTN_RIGHT ? +1 : -1, false))
            return SCR_DRAW;
        return SCR_HANDLED;
    case BTN_OK:
        return list_ok();
    default:
        return SCR_HANDLED;
    }
}

/* Sound and Light: sys.volume / sys.brightness. */
static void sound_adjust(int dir, bool wrap)
{
    if (s_sub == SET_SOUND_ROW_VOLUME) {
        step(P(SYS_VOLUME), dir, wrap);
        settings_env_click();           /* hear the new level */
    } else {
        step(P(SYS_BRIGHTNESS), dir, wrap);     /* the setter applies the backlight */
    }
}

static scr_res_t sound_key(scr_key_t k)
{
    scr_res_t res;
    if (common_key(k, &res)) return res;
    switch (k.btn) {
    case BTN_MENU:
        s_sub = 0;
        settings_env_page_left();
        return SCR_CLOSE;
    case BTN_UP:
        if (s_sub > 0) s_sub--;
        return SCR_DRAW;
    case BTN_DOWN:
        if (s_sub < SET_SOUND_ROWS - 1) s_sub++;
        return SCR_DRAW;
    case BTN_LEFT:
        sound_adjust(-1, false);
        return SCR_DRAW;
    case BTN_RIGHT:
        sound_adjust(+1, false);
        return SCR_DRAW;
    case BTN_OK:
        sound_adjust(+1, true);
        return SCR_DRAW_FULL;
    default:
        return SCR_HANDLED;
    }
}

static scr_res_t about_key(scr_key_t k)
{
    scr_res_t res;
    if (common_key(k, &res)) return res;
    switch (k.btn) {
    case BTN_MENU:
        settings_env_page_left();
        return SCR_CLOSE;
    case BTN_OK:                    /* OK goes back */
        return SCR_CLOSE;
    case BTN_UP:
    case BTN_DOWN:
        s_about_keys++;             /* painted again: the heap and pool lines are live */
        return SCR_DRAW;
    default:
        return SCR_HANDLED;
    }
}

/* ── display task ──────────────────────────────────────────────────── */

/* No partial paints: FULL, UPDATE and RESTORE are the same picture; an
 * UPDATE is gated on the epoch by the display task. */
static void list_draw(scr_paint_t how, uint32_t frame)
{
    (void)how; (void)frame;
    if (s_question) settings_ui_draw_reset_question(s_row);
    else            settings_ui_draw_list(s_row);
}

static void sound_draw(scr_paint_t how, uint32_t frame)
{
    (void)how; (void)frame;
    settings_ui_draw_sound(s_sub);
}

static void about_draw(scr_paint_t how, uint32_t frame)
{
    (void)how; (void)frame;
    settings_ui_draw_about();
}

/* What the list renders from: the row, the question, the values and the
 * theme. Not the USB Sharing label: it follows the host, and the row shows
 * the label in force whenever the list is painted. */
static uint32_t list_epoch(void)
{
    uint32_t h = REDRAW_EPOCH_SEED;
    h = redraw_epoch_mix(h, (uint32_t)theme_get_id() | ((uint32_t)s_row << 8) | ((uint32_t)s_question << 16));
    h = redraw_epoch_mix(h, (uint32_t)prop_get(P(SYS_AUTOOFF)) | ((uint32_t)prop_get(P(SYS_STARTUP)) << 8)
                            | ((uint32_t)prop_get(P(METER_LINK)) << 16) | ((uint32_t)prop_get(P(SYS_THEME)) << 24));
    return h;
}

static uint32_t sound_epoch(void)
{
    return redraw_epoch_mix(REDRAW_EPOCH_SEED,
                            (uint32_t)theme_get_id() | ((uint32_t)s_sub << 8)
                            | ((uint32_t)prop_get(P(SYS_VOLUME)) << 16) | ((uint32_t)prop_get(P(SYS_BRIGHTNESS)) << 24));
}

/* Not the live numbers (heap, pool): the page would repaint by itself. */
static uint32_t about_epoch(void)
{
    return redraw_epoch_mix(REDRAW_EPOCH_SEED, (uint32_t)theme_get_id() | ((uint32_t)s_about_keys << 8));
}

/* ── any task ──────────────────────────────────────────────────────── */

/* `ui screen`: "settings=<page>/<row>/<sub>". */
static void describe(char *out, size_t n)
{
    const settings_view_t v = settings_screen_view();
    snprintf(out, n, "settings=%u/%u/%u", (unsigned)v.page, (unsigned)v.row, (unsigned)v.sub);
}

/* Device settings are sys.* and meter.link, and the bars add the rest: a
 * repaint here is cheap, so every group repaints it. */
#define ST_GROUPS ((uint16_t)((1u << PG_COUNT) - 1u))
_Static_assert(PG_COUNT <= 16, "screen_t.groups holds one bit per property group");

const screen_t settings_screen = {
    .name = "settings", .id = SCR_SETTINGS, .mode = MODE_SETTINGS, .instrument = SCR_INSTRUMENT_NONE,
    .flags = 0,
    .open = list_open, .close = list_close, .key = list_key,
    .draw = list_draw, .tick = NULL, .epoch = list_epoch,
    .describe = describe, .groups = ST_GROUPS,
};

const screen_t settings_sound_screen = {
    .name = "set_sound", .id = SCR_SET_SOUND, .mode = MODE_COUNT, .instrument = SCR_INSTRUMENT_NONE,
    .flags = SCR_F_CHILD | SCR_F_OPAQUE,
    .open = NULL, .close = NULL, .key = sound_key,
    .draw = sound_draw, .tick = NULL, .epoch = sound_epoch,
    .describe = describe, .groups = ST_GROUPS,
};

const screen_t settings_about_screen = {
    .name = "set_about", .id = SCR_SET_ABOUT, .mode = MODE_COUNT, .instrument = SCR_INSTRUMENT_NONE,
    .flags = SCR_F_CHILD | SCR_F_OPAQUE,
    .open = NULL, .close = NULL, .key = about_key,
    .draw = about_draw, .tick = NULL, .epoch = about_epoch,
    .describe = describe, .groups = ST_GROUPS,
};
