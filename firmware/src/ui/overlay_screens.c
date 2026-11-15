/*
 * OpenScope 2C53T - the POWER prompt and the logo as screens. See
 * overlay_screens.h.
 */
#include "overlay_screens.h"
#include "theme.h"

/* ── the POWER prompt ──────────────────────────────────────────────── */

/* Written by the input task; read by the display task and the shell. */
static volatile uint8_t pp_kind, pp_count, pp_shows;

/* What the box shows, then its layer (if it is not up yet): the display
 * task paints what it finds when it sees the layer. */
void power_prompt_show(power_prompt_kind_t kind, uint8_t count)
{
    pp_count = count;
    pp_shows++;
    pp_kind = (uint8_t)kind;
    (void)screen_push(SCR_POWER_PROMPT);    /* refused while it is up: one layer for all its shows */
}

void power_prompt_hide(void)
{
    (void)screen_close(SCR_POWER_PROMPT);
    pp_kind = POWER_PROMPT_NONE;
}

bool power_prompt_up(void)   { return screen_is_open(SCR_POWER_PROMPT); }

power_prompt_view_t power_prompt_view(void)
{
    power_prompt_view_t v;
    v.kind  = pp_kind;
    v.count = pp_count;
    v.shows = pp_shows;
    return v;
}

/* The box, whole, whatever the paint: it is small and the screen under it
 * is not this layer's to repaint. */
static void pp_draw(scr_paint_t how, uint32_t frame)
{
    (void)how; (void)frame;
    const power_prompt_view_t v = power_prompt_view();
    if (v.kind != POWER_PROMPT_NONE) power_prompt_ui_paint(v);
}

static uint32_t pp_epoch(void)
{
    const power_prompt_view_t v = power_prompt_view();
    return redraw_epoch_mix(REDRAW_EPOCH_SEED,
                            (uint32_t)v.kind | ((uint32_t)v.count << 8) | ((uint32_t)v.shows << 16)
                            | ((uint32_t)theme_get_id() << 24));
}

const screen_t power_prompt_screen = {
    .name = "power_prompt", .id = SCR_POWER_PROMPT, .mode = MODE_COUNT, .instrument = SCR_INSTRUMENT_NONE,
    .flags = SCR_F_OPAQUE | SCR_F_NO_BARS,
    .open = NULL, .close = NULL, .key = NULL,
    .draw = pp_draw, .tick = NULL, .epoch = pp_epoch,
    .describe = NULL, .groups = 0,
};

/* ── the logo ──────────────────────────────────────────────────────── */

static volatile uint8_t lg_shows, lg_painted;    /* painted: the show that is on the LCD */

bool logo_screen_show(bool painted)
{
    if (screen_is_open(SCR_LOGO)) return false;
    lg_shows++;
    if (painted) lg_painted = lg_shows;
    return screen_push(SCR_LOGO);
}

void    logo_screen_hide(void)         { (void)screen_close(SCR_LOGO); }
bool    logo_screen_up(void)           { return screen_is_open(SCR_LOGO); }
uint8_t logo_screen_shows(void)        { return lg_shows; }
bool    logo_screen_painted(void)      { return lg_painted == lg_shows; }
void    logo_screen_mark_painted(void) { lg_painted = lg_shows; }

/* Once per show: the picture does not change while it is up. Again when
 * something was drawn over it (FULL / RESTORE: the POWER prompt's box). */
static void lg_draw(scr_paint_t how, uint32_t frame)
{
    (void)frame;
    if (logo_screen_up() && (how != SCR_PAINT_UPDATE || !logo_screen_painted())) logo_ui_paint();
}

static uint32_t lg_epoch(void)
{
    return redraw_epoch_mix(REDRAW_EPOCH_SEED, (uint32_t)lg_shows << 8);
}

const screen_t logo_screen = {
    .name = "logo", .id = SCR_LOGO, .mode = MODE_COUNT, .instrument = SCR_INSTRUMENT_NONE,
    .flags = SCR_F_OPAQUE | SCR_F_NO_BARS,
    .open = NULL, .close = NULL, .key = NULL,
    .draw = lg_draw, .tick = NULL, .epoch = lg_epoch,
    .describe = NULL, .groups = 0,
};

/* ── the display task's bookkeeping ────────────────────────────────── */

bool layer_track_step(layer_track_t *t, const screen_t *now)
{
    if (now == t->was) return false;
    if (t->was != NULL) {
        if (now != NULL) t->whole = true;
        if (t->was == &power_prompt_screen) { if (now == NULL) t->restore = true; }
        else t->redraw = true;
    }
    t->was = now;
    return true;
}

bool layer_track_take_whole(layer_track_t *t)
{
    const bool w = t->whole;
    t->whole = false;
    return w;
}

layer_repaint_t layer_track_take(layer_track_t *t, bool base_restores)
{
    layer_repaint_t r = LAYER_REPAINT_NONE;
    if (t->redraw || (t->restore && !base_restores)) r = LAYER_REPAINT_ALL;
    else if (t->restore)                             r = LAYER_REPAINT_RESTORE;
    t->redraw = t->restore = t->whole = false;
    return r;
}

void layer_track_redrawn(layer_track_t *t)
{
    t->redraw = t->restore = false;
}
