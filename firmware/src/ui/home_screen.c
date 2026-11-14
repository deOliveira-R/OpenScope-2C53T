/*
 * OpenScope 2C53T - the home screen. See home_screen.h.
 */
#include "home_screen.h"
#include "home_model.h"
#include "model.h"

#include <stdio.h>

/* UI state: the selection and where BACK returns to. Written by the input
 * task (open, key); the display task and the shell read `sel`. */
static home_t s_home = HOME_INIT;

uint8_t home_screen_selected(void)
{
    const uint8_t s = s_home.sel;
    return s < (uint8_t)HOME_ITEM_COUNT ? s : (uint8_t)HOME_ITEM_SCOPE;
}

/* ── input task ────────────────────────────────────────────────────── */

/* Entering Home from another base preselects that screen's entry and keeps
 * it for BACK. From nothing (boot with Startup on Boot = Home) the boot
 * selection stays: Oscilloscope, nothing to go back to. */
static void hs_open(screen_id_t from)
{
    const screen_t *f = screen_get(from);
    home_enter(&s_home, f ? f->mode : MODE_HOME);
}

/* Activate an entry: bind the screen and keys to its instrument, or show
 * its page. One set through the model; the mode switch, the posture and the
 * repaint are the setter's (props_sys.c -> the mode op). */
static void hs_activate(home_item_t it)
{
    const home_tile_t *t = home_tile(it);
    if (!t) return;
    if (t->instrument != HOME_NO_INSTRUMENT)
        (void)prop_set_local(P(SYS_INSTRUMENT), (int32_t)t->instrument, ORIGIN_KEY, NULL);
    else
        (void)prop_set_local(P(SYS_MODE), (int32_t)t->mode, ORIGIN_KEY, NULL);
}

/* Home takes every key that reaches it (POWER, SAVE and the long mode keys
 * are the router's globals): LEFT/RIGHT move, OK enters, short MENU goes
 * back; UP/DOWN and the rest do nothing, as in stock. No long key has a
 * meaning of its own here. */
static scr_res_t hs_key(scr_key_t k)
{
    home_key_t hk;
    if (k.is_long) return SCR_HANDLED;
    switch (k.btn) {
    case BTN_LEFT:  hk = HOME_KEY_LEFT;  break;
    case BTN_RIGHT: hk = HOME_KEY_RIGHT; break;
    case BTN_UP:    hk = HOME_KEY_UP;    break;
    case BTN_DOWN:  hk = HOME_KEY_DOWN;  break;
    case BTN_OK:    hk = HOME_KEY_OK;    break;
    case BTN_MENU:  hk = HOME_KEY_BACK;  break;
    default:        hk = HOME_KEY_OTHER; break;
    }
    const home_result_t r = home_key(&s_home, hk);
    if (r.act == HOME_MOVED) return SCR_DRAW;
    if (r.act == HOME_ENTER) hs_activate(home_item_for_mode(r.mode));
    return SCR_HANDLED;
}

/* ── display task ──────────────────────────────────────────────────── */

static void hs_draw(scr_paint_t how, uint32_t frame)
{
    (void)frame;
    if (how == SCR_PAINT_FULL) home_ui_draw(home_screen_selected());
    else                       home_ui_draw_selection(home_screen_selected());
}

/* ── any task ──────────────────────────────────────────────────────── */

/* `ui screen`: "home=<the selected entry's token>". */
static void hs_describe(char *out, size_t n)
{
    const home_tile_t *t = home_tile((home_item_t)home_screen_selected());
    snprintf(out, n, "home=%s", t ? t->token : "?");
}

/* It shows the bars only: the generator slot of the status bar and sys/ui
 * (theme, mode). No epoch: an UPDATE paint always repaints the selection
 * (a popup drawn over it is put back on top after one). */
#define HS_GROUPS ((uint16_t)((1u << PG_GEN) | (1u << PG_SYS) | (1u << PG_UI)))

const screen_t home_screen = {
    .name = "home", .id = SCR_HOME, .mode = MODE_HOME, .instrument = SCR_INSTRUMENT_NONE,
    .flags = 0,
    .open = hs_open, .close = NULL, .key = hs_key,
    .draw = hs_draw, .tick = NULL, .epoch = NULL,
    .describe = hs_describe, .groups = HS_GROUPS,
};
