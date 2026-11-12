/*
 * OpenScope 2C53T - Oscilloscope Settings page: the pure model (UI round 1, R1-A)
 *
 * Stock V1.2.0's "Oscilloscope Settings" page (state 9, research/ui/UI_STUDY.md
 * sections 1.4 and 2.3; photos ~/Desktop/stock_ui/scope/scope_settings/): a
 * left rail of eight categories and a right pane of options.
 *
 *   rail:  Display, Cursors, Measure, CH1, CH2, Trigger, Persist, Math
 *          (stock's labels, made to fit one line: no "Cursor Mea/surement")
 *
 * This file is the state machine only: no LCD, no FPGA, no FreeRTOS, so
 * tests/test_osc_menu.c runs the code the input task runs (make test-osc-menu).
 * The one side effect it has is the Measure pane's selection, which is itself
 * pure (meas_select.c). Everything else the page changes is applied by
 * osc_menu_ui.c, from the action osc_menu_step() returns, through the existing
 * scope code (fpga_apply_*, trig_menu_change, persist_*, fft_*).
 *
 * KEYS (study 1.4; stock where seen, ours where stock is silent)
 *   rail focus:   UP/DOWN   previous/next row, wrapping (all 8 rows are
 *                           visible at 26 px, so no scroll: stock shows 6)
 *                 RIGHT/OK  enter the pane
 *                 LEFT      nothing
 *                 BACK      close the page (short MENU on stock: level 1 -> 0)
 *   pane focus, by pane kind:
 *     RADIO  (Display, Cursors, Persist): UP/DOWN move, OK picks, LEFT or
 *            BACK -> rail. RIGHT does nothing (picking applies, so it takes OK).
 *     VALUES (CH1, CH2, Trigger): UP/DOWN pick the row, LEFT/RIGHT change its
 *            value (-1/+1), OK = +1. BACK -> rail (LEFT is taken by the value).
 *     CHIPS  (Measure, Math): a grid. Arrows move, clamped at the edges;
 *            LEFT at the left column or BACK -> rail; OK toggles/picks.
 *            Measure: row 0 is [CHx tab][All][None] over stock's 3x4 grid;
 *            OK on the tab flips CH1/CH2, DOWN past CH1's last row continues
 *            into CH2 and UP above CH2's first row back into CH1 (stock
 *            scrolls one long list of both panels).
 *   osc_menu_open(row) while open jumps to that row, rail focus (another
 *   deep-link key pressed on the page goes to its row).
 */
#ifndef OSC_MENU_MODEL_H
#define OSC_MENU_MODEL_H

#include <stdint.h>
#include <stdbool.h>

/* ── Fixed interface (research/ui/ROUND1_BRIEF.md) ─────────────────── */
typedef enum { OSC_ROW_DISPLAY, OSC_ROW_CURSOR, OSC_ROW_PARAM, OSC_ROW_CH1, OSC_ROW_CH2,
               OSC_ROW_TRIGGER, OSC_ROW_PERSIST, OSC_ROW_MATH, OSC_ROW_COUNT } osc_row_t;
typedef enum { OSC_KEY_UP, OSC_KEY_DOWN, OSC_KEY_LEFT, OSC_KEY_RIGHT, OSC_KEY_OK, OSC_KEY_BACK } osc_key_t;
typedef enum { OSC_RES_NONE, OSC_RES_REDRAW, OSC_RES_CLOSE } osc_result_t;
void         osc_menu_open(osc_row_t row);   /* opens the page with that row selected */
bool         osc_menu_is_open(void);
osc_result_t osc_menu_key(osc_key_t k);      /* model step + apply (osc_menu_ui.c) */
void         osc_menu_draw(void);            /* full draw (osc_menu_ui.c), DCMD_DRAW_OSC_MENU */
void         osc_menu_close(void);

/* ── Pane layouts ───────────────────────────────────────────────────── */
typedef enum { OSC_PANE_RADIO = 0, OSC_PANE_VALUES, OSC_PANE_CHIPS } osc_pane_kind_t;

typedef struct {
    uint8_t kind;        /* osc_pane_kind_t */
    uint8_t n_items;     /* items in the pane (per channel page for Measure) */
    uint8_t cols;        /* CHIPS: grid columns; 1 otherwise */
    uint8_t span_first;  /* CHIPS: item 0 is alone on row 0, full width (Math "OFF") */
} osc_pane_layout_t;

const osc_pane_layout_t *osc_menu_layout(osc_row_t row);

/* Item indices, per pane (the order the pane draws them). */
enum { OSC_DISP_YT = 0, OSC_DISP_ROLL, OSC_DISP_XY, OSC_DISP_FFT, OSC_DISP_WATERFALL, OSC_DISP_COUNT };
enum { OSC_CUR_OFF = 0, OSC_CUR_TIME, OSC_CUR_VOLT, OSC_CUR_BOTH, OSC_CUR_COUNT };
enum { OSC_CH_ENABLE = 0, OSC_CH_PROBE, OSC_CH_COUPLING, OSC_CH_FFT, OSC_CH_COUNT };
enum { OSC_TRIG_MODE = 0, OSC_TRIG_EDGE, OSC_TRIG_SOURCE, OSC_TRIG_COUNT };
enum { OSC_PERS_OFF = 0, OSC_PERS_500MS, OSC_PERS_1S, OSC_PERS_2S, OSC_PERS_INF, OSC_PERS_COUNT };
/* Measure: 0 = channel tab, 1 = All ("A-on"), 2 = None ("A-off"), 3.. = chips */
enum { OSC_PRM_TAB = 0, OSC_PRM_ALL, OSC_PRM_NONE, OSC_PRM_CHIP0, OSC_PRM_COUNT = OSC_PRM_CHIP0 + 12 };
/* Math: 0 = OFF, 1..8 = stock's 2x4 grid (osc_menu_math_op) */
enum { OSC_MATH_OFF = 0, OSC_MATH_COUNT = 9 };

/* ── Model state ────────────────────────────────────────────────────── */
typedef struct {
    bool    open;
    uint8_t row;         /* osc_row_t: the rail selection */
    bool    in_pane;     /* false = rail focus */
    uint8_t item;        /* pane item under the cursor */
    uint8_t meas_ch;     /* Measure pane: 0 = CH1 page, 1 = CH2 */
    uint8_t opens;       /* bumped on every osc_menu_open(): the drawer repaints all */
} osc_menu_state_t;

const osc_menu_state_t *osc_menu_state(void);

/* What a key asks the UI layer to apply (model -> osc_menu_ui.c). */
typedef enum {
    OSC_ACT_NONE = 0,
    OSC_ACT_ENTER,       /* focus moved into the pane: put item on the current value */
    OSC_ACT_SELECT,      /* RADIO / Math: pick `item` */
    OSC_ACT_CHANGE,      /* VALUES: change row `item` by `dir` */
} osc_act_kind_t;

typedef struct {
    uint8_t kind;        /* osc_act_kind_t */
    uint8_t row;         /* osc_row_t */
    uint8_t item;
    int8_t  dir;         /* OSC_ACT_CHANGE: -1 / +1 */
} osc_action_t;

/* One key through the model. Returns NONE (nothing changed), REDRAW, or CLOSE
 * (the model is closed already). *act says what to apply; never NULL. The
 * Measure pane's All/None/chip toggles are applied here (meas_select_*). */
osc_result_t osc_menu_step(osc_key_t k, osc_action_t *act);

/* The UI's answer to OSC_ACT_ENTER: start the pane cursor on `item` (the live
 * value, e.g. the current persistence mode). Clamped to the pane. */
void osc_menu_set_item(uint8_t item);

/* True when picking `item` in `row` should close the page: the cursor modes,
 * so the arrows move the cursors at once (study 2.3). */
bool osc_menu_select_closes(osc_row_t row, uint8_t item);

/* ── Labels (pure, so a host test can check they fit) ───────────────── */
const char *osc_menu_row_label(osc_row_t row);    /* "Display" ... "Math" (rail) */
const char *osc_menu_row_title(osc_row_t row);    /* "Display Mode" ... (pane title) */
/* Item label for RADIO / CHIPS panes and the row label of VALUES panes
 * ("Channel", "Probe", ...); Measure chips use the stock measurement labels.
 * `ch` matters only for the Measure tab ("CH1"/"CH2"). "" out of range. */
const char *osc_menu_item_label(osc_row_t row, uint8_t item, uint8_t ch);

/* Measure chip `chip` (0..11, grid order) -> scope_prm_id_t. Stock's grid is
 * row-major Freq Wid+ Vp-p / Peri Wid- Amp / Duty+ Vmax Vrms / Duty- Vmin Avg. */
uint8_t osc_menu_prm_of_chip(uint8_t chip);
/* Math item 1..8 -> math_op_t (stock's 2x4: CH1+CH2 -CH1 / CH1-CH2 -CH2 /
 * CH1*CH2 |CH1| / CH1/CH2 |CH2|); 0xFF for OFF or out of range. */
uint8_t osc_menu_math_op(uint8_t item);
/* math_op_t -> Math item (1..8); OSC_MATH_OFF if unknown. */
uint8_t osc_menu_math_item(uint8_t op);

/* Grid geometry of a CHIPS item: row and column (span_first counted). */
void osc_menu_chip_pos(osc_row_t row, uint8_t item, uint8_t *r, uint8_t *c);

#endif /* OSC_MENU_MODEL_H */
