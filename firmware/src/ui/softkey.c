/*
 * Softkey bar -- see softkey.h and docs/specs/platform/softkey-ui.md.
 *
 * The table side: which slot a button is, a press, the validity rule and
 * the redraw hash. Pure (host tests link it); the drawing is
 * softkey_draw.c.
 */
#include "softkey.h"

int8_t softkey_slot_for_button(button_id_t b)
{
    switch (b) {
    case BTN_MOVE:    return 0;
    case BTN_SELECT:  return 1;
    case BTN_TRIGGER: return 2;
    case BTN_PRM:     return 3;
    default:          return -1;
    }
}

bool softkey_bar_valid(const softkey_bar_t *bar)
{
    for (int i = 0; i < SOFTKEY_N; i++) {
        const softkey_t *k = &bar->key[i];
        if (k->label && !k->press) return false;     /* a label that does nothing */
        if (!k->label && (k->press || k->value)) return false;  /* a hidden control */
    }
    return true;
}

bool softkey_bar_press(const softkey_bar_t *bar, int8_t slot)
{
    if (slot < 0 || slot >= SOFTKEY_N) return false;
    const softkey_t *k = &bar->key[slot];
    if (!k->label || !k->press) return false;
    k->press();
    return true;
}

uint32_t softkey_bar_epoch(const softkey_bar_t *bar)
{
    uint32_t h = 2166136261u;
    char v[SOFTKEY_VALUE_MAX];
    for (int i = 0; i < SOFTKEY_N; i++) {
        const softkey_t *k = &bar->key[i];
        h = (h ^ (uint32_t)(uintptr_t)k->label) * 16777619u;
        h = (h ^ (uint32_t)((k->active && k->active()) ? 1u : 0u)) * 16777619u;
        if (k->value) {
            v[0] = '\0';
            k->value(v, sizeof v);
            for (const char *p = v; *p; p++)
                h = (h ^ (uint8_t)*p) * 16777619u;
        }
    }
    return h;
}
