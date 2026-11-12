/*
 * Measurement strip layout. See measure_strip.h. Pure: no LCD, no FPGA.
 */
#include "measure_strip.h"

static unsigned popcount_capped(uint16_t mask, unsigned cap)
{
    unsigned c = 0;
    for (unsigned i = 0; i < MSTRIP_PRM_COUNT; i++)
        if (mask & (1u << i))
            c++;
    return (c > cap) ? cap : c;
}

static unsigned count_shown(uint16_t m1, bool en1, uint16_t m2, bool en2, bool hidden)
{
    if (hidden)
        return 0;
    return (en1 ? popcount_capped(m1, MSTRIP_MAX_PER_CH) : 0u) +
           (en2 ? popcount_capped(m2, MSTRIP_MAX_PER_CH) : 0u);
}

static unsigned rows_for(unsigned n)
{
    return (n + MSTRIP_COLS - 1u) / MSTRIP_COLS;
}

uint16_t measure_strip_top(uint16_t mask1, bool en1, uint16_t mask2, bool en2,
                           bool hidden, bool aux, uint16_t bottom)
{
    unsigned h = rows_for(count_shown(mask1, en1, mask2, en2, hidden)) * MSTRIP_ROW_H;
    if (aux)
        h += MSTRIP_ROW_H;
    return (h > bottom) ? 0u : (uint16_t)(bottom - h);
}

void measure_strip_layout(uint16_t mask1, bool en1, uint16_t mask2, bool en2,
                          bool hidden, bool aux, uint16_t bottom,
                          mstrip_layout_t *out)
{
    if (out == 0)
        return;
    out->n = 0;
    out->dropped = 0;
    out->aux = aux;
    out->bottom = bottom;

    const unsigned n = count_shown(mask1, en1, mask2, en2, hidden);
    out->rows = (uint8_t)rows_for(n);
    out->top = measure_strip_top(mask1, en1, mask2, en2, hidden, aux, bottom);
    out->aux_y = out->top;                       /* the AUX row is the top one */
    const uint16_t first_row = (uint16_t)(out->top + (aux ? MSTRIP_ROW_H : 0u));

    for (unsigned ch = 0; ch < 2u; ch++) {
        const uint16_t m = ch ? mask2 : mask1;
        const bool en = ch ? en2 : en1;
        if (!en || hidden)
            continue;
        unsigned taken = 0;
        for (unsigned i = 0; i < MSTRIP_PRM_COUNT; i++) {
            if (!(m & (1u << i)))
                continue;
            if (taken >= MSTRIP_MAX_PER_CH || out->n >= MSTRIP_MAX_CELLS) {
                out->dropped++;
                continue;
            }
            mstrip_cell_t *c = &out->cell[out->n];
            c->ch  = (uint8_t)ch;
            c->prm = (uint8_t)i;
            c->x   = (uint16_t)(MSTRIP_X0 + (out->n % MSTRIP_COLS) * MSTRIP_CELL_W);
            c->y   = (uint16_t)(first_row + (out->n / MSTRIP_COLS) * MSTRIP_ROW_H);
            out->n++;
            taken++;
        }
    }
}

bool measure_strip_place(uint16_t x, uint16_t label_w, uint16_t value_w,
                         uint16_t *label_x, uint16_t *value_x)
{
    const bool fits = label_w <= MSTRIP_LABEL_W && value_w <= MSTRIP_VALUE_W;
    if (label_x) *label_x = (uint16_t)(x + MSTRIP_PAD);
    if (value_x) {
        if (fits) {
            *value_x = (uint16_t)(x + MSTRIP_VALUE_X);
        } else {                                 /* value only, right-aligned */
            const unsigned inner = MSTRIP_CELL_W - 2u * MSTRIP_PAD;
            const unsigned vw = (value_w > inner) ? inner : value_w;
            *value_x = (uint16_t)(x + MSTRIP_CELL_W - MSTRIP_PAD - vw);
        }
    }
    return fits;
}

uint16_t measure_strip_dim(uint16_t fg, uint16_t bg)
{
    const unsigned r = (((fg >> 11) & 0x1Fu) + ((bg >> 11) & 0x1Fu)) / 2u;
    const unsigned g = (((fg >> 5)  & 0x3Fu) + ((bg >> 5)  & 0x3Fu)) / 2u;
    const unsigned b = (( fg        & 0x1Fu) + ( bg        & 0x1Fu)) / 2u;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

#ifndef MEASURE_STRIP_HOST
/* Weak stub until R1-A's meas_select.c is linked: today's displayed set, so
 * this branch shows what bench/parity showed (CH1 = the `prm` selection,
 * default Freq/Peri/Duty+/Vp-p/Vrms; CH2 = Vp-p). */
#include "scope_state.h"
#include "scope_measure.h"

__attribute__((weak)) uint16_t meas_select_get(uint8_t ch)
{
    return ch == 0u ? scope_state_get()->prm_mask
                    : (uint16_t)(1u << SCOPE_PRM_VPP);
}
#endif
