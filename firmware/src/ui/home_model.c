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

device_mode_t home_item_mode(home_item_t it)
{
    switch (it) {
    case HOME_ITEM_METER:    return MODE_MULTIMETER;
    case HOME_ITEM_SIGGEN:   return MODE_SIGNAL_GEN;
    case HOME_ITEM_SETTINGS: return MODE_SETTINGS;
    case HOME_ITEM_SCOPE:
    default:                 return MODE_OSCILLOSCOPE;
    }
}

home_item_t home_item_for_mode(device_mode_t m)
{
    switch (m) {
    case MODE_MULTIMETER:   return HOME_ITEM_METER;
    case MODE_OSCILLOSCOPE: return HOME_ITEM_SCOPE;
    case MODE_SIGNAL_GEN:   return HOME_ITEM_SIGGEN;
    case MODE_SETTINGS:     return HOME_ITEM_SETTINGS;
    default:                return HOME_ITEM_COUNT;
    }
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
    switch (it) {
    case HOME_ITEM_METER:    return "Multimeter";
    case HOME_ITEM_SCOPE:    return "Oscilloscope";
    case HOME_ITEM_SIGGEN:   return "Signal Generator";
    case HOME_ITEM_SETTINGS: return "Settings";
    default:                 return "";
    }
}
