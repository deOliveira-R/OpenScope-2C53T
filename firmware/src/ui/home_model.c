/*
 * OpenScope 2C53T - home screen model. See home_model.h for the why.
 */
#include "home_model.h"

static uint8_t sanitize(uint8_t sel)
{
    return (sel < (uint8_t)HOME_ITEM_COUNT) ? sel : (uint8_t)HOME_ITEM_SCOPE;
}

void home_init(home_t *h)
{
    h->sel  = (uint8_t)HOME_ITEM_SCOPE;
    h->back = HOME_NO_BACK;
}

static const home_tile_t k_tiles[HOME_ITEM_COUNT] = {
#define X(id, token, label, icon, mode, inst) { token, label, (uint8_t)(icon), (uint8_t)(mode), (uint8_t)(inst) },
    HOME_TILES(X)
#undef X
};

const home_tile_t *home_tile(home_item_t it)
{
    return ((unsigned)it < (unsigned)HOME_ITEM_COUNT) ? &k_tiles[it] : NULL;
}

device_mode_t home_item_mode(home_item_t it)
{
    const home_tile_t *t = home_tile(it);
    return t ? (device_mode_t)t->mode : MODE_OSCILLOSCOPE;
}

home_item_t home_item_for_mode(device_mode_t m)
{
    for (unsigned i = 0; i < (unsigned)HOME_ITEM_COUNT; i++)
        if (k_tiles[i].mode == (uint8_t)m) return (home_item_t)i;
    return HOME_ITEM_COUNT;
}

void home_enter(home_t *h, device_mode_t from)
{
    const home_item_t it = home_item_for_mode(from);
    h->sel = sanitize(h->sel);
    if (it == HOME_ITEM_COUNT) return;        /* re-entered from home: keep it all */
    h->sel  = (uint8_t)it;
    h->back = (uint8_t)from;
}

home_item_t home_neighbour(uint8_t sel, int dir)
{
    const unsigned n = (unsigned)HOME_ITEM_COUNT;
    sel = sanitize(sel);
    if (dir < 0) return (home_item_t)((sel + n - 1u) % n);
    return (home_item_t)((sel + 1u) % n);
}

home_result_t home_key(home_t *h, home_key_t k)
{
    home_result_t r = { HOME_IGNORED, MODE_HOME };
    h->sel = sanitize(h->sel);

    switch (k) {
    case HOME_KEY_LEFT:
    case HOME_KEY_RIGHT:
        h->sel = (uint8_t)home_neighbour(h->sel, k == HOME_KEY_LEFT ? -1 : +1);
        r.act = HOME_MOVED;
        break;
    case HOME_KEY_OK:
        r.act  = HOME_ENTER;
        r.mode = home_item_mode((home_item_t)h->sel);
        break;
    case HOME_KEY_BACK:
        /* Ours: stock ignores BACK on home. Going back to where home was
         * opened from makes a MENU tap a reversible peek. */
        if (h->back != HOME_NO_BACK &&
            home_item_for_mode((device_mode_t)h->back) != HOME_ITEM_COUNT) {
            r.act  = HOME_ENTER;
            r.mode = (device_mode_t)h->back;
        }
        break;
    case HOME_KEY_UP:        /* stock: nothing on home */
    case HOME_KEY_DOWN:
    case HOME_KEY_OTHER:
    default:
        break;
    }
    return r;
}

const char *home_item_label(home_item_t it)
{
    const home_tile_t *t = home_tile(it);
    return t ? t->label : "";
}
