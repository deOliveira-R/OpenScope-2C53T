/*
 * OpenScope 2C53T - Oscilloscope Settings page: the fixed interface of UI
 * round 1 (research/ui/ROUND1_BRIEF.md). R1-A owns this file and the model
 * behind it; this copy is the brief's text verbatim so the key wiring (R1-K)
 * builds on its own. At merge, R1-A's version wins.
 */
#ifndef OSC_MENU_MODEL_H
#define OSC_MENU_MODEL_H

#include <stdint.h>
#include <stdbool.h>

typedef enum { OSC_ROW_DISPLAY, OSC_ROW_CURSOR, OSC_ROW_PARAM, OSC_ROW_CH1, OSC_ROW_CH2,
               OSC_ROW_TRIGGER, OSC_ROW_PERSIST, OSC_ROW_MATH, OSC_ROW_COUNT } osc_row_t;
typedef enum { OSC_KEY_UP, OSC_KEY_DOWN, OSC_KEY_LEFT, OSC_KEY_RIGHT, OSC_KEY_OK, OSC_KEY_BACK } osc_key_t;
typedef enum { OSC_RES_NONE, OSC_RES_REDRAW, OSC_RES_CLOSE } osc_result_t;

void         osc_menu_open(osc_row_t row);   /* opens the page with that row selected */
bool         osc_menu_is_open(void);
osc_result_t osc_menu_key(osc_key_t k);      /* model step; applies settings through existing scope APIs */
void         osc_menu_draw(void);            /* full draw, called by the display task on DCMD_DRAW_OSC_MENU */
void         osc_menu_close(void);

#endif /* OSC_MENU_MODEL_H */
