/*
 * OpenScope 2C53T - Oscilloscope Settings page: the pure model (UI round 1, R1-A)
 *
 * Stock V1.2.0's "Oscilloscope Settings" page (state 9, research/ui/UI_STUDY.md
 * sections 1.4 and 2.3; photos ~/Desktop/stock_ui/scope/scope_settings/): a
 * left rail of eight categories and a right pane of options.
 *
 *   rail:  Display, Cursors, Measure, CH1, CH2, Trigger, Persist, Math
 *          (stock's labels, made to fit one line: no "Cursor Mea/surement"),
 *          then Spectrum, ours: the window, averaging and max hold of the
 *          spectrum views
 *
 * This file is the state machine only: no LCD, no FPGA, no FreeRTOS, so
 * tests/test_osc_menu.c runs the code the input task runs (make test-osc-menu).
 * The one side effect it has is the Measure pane's selection, which is itself
 * pure (meas_select.c). Everything else the page changes is one property
 * set, made by osc_menu_screen.c from the action osc_menu_step() returns
 * (its table binds each pane and row to a property; the setters hold the
 * rules).
 *
 * KEYS (study 1.4; stock where seen, ours where stock is silent)
 *   rail focus:   UP/DOWN   previous/next row, wrapping (all the rows are
 *                           visible, so no scroll: stock shows 6)
 *                 RIGHT/OK  enter the pane
 *                 LEFT      nothing
 *                 BACK      close the page (short MENU on stock: level 1 -> 0)
 *   pane focus, by pane kind:
 *     RADIO  (Display, Cursors, Persist): UP/DOWN move, OK picks, LEFT or
 *            BACK -> rail. RIGHT does nothing (picking applies, so it takes OK).
 *     VALUES (CH1, CH2, Trigger, Spectrum): UP/DOWN pick the row, LEFT/RIGHT change its
 *            value (-1/+1), OK = +1. BACK -> rail (LEFT is taken by the value).
 *     CHIPS  (Measure, Math): a grid. Arrows move, clamped at the edges;
 *            LEFT at the left column or BACK -> rail; OK toggles/picks.
 *            Measure: one panel per measurement set (meas_select.h: CH1,
 *            CH2, the spectrum), of the sets the view on screen has
 *            (osc_menu_set_meas_sets). Row 0 is [tab][All][None] over the
 *            set's grid (the channels': stock's 3x4). OK on the tab goes
 *            to the next panel, DOWN past a panel's last row continues into
 *            the next and UP above its first row back into the one before
 *            (stock scrolls one long list of both channels' panels).
 *   osc_menu_open(row) while open jumps to that row, rail focus (another
 *   deep-link key pressed on the page goes to its row).
 *
 * LOCKED PANE (osc_menu_open_locked; owner on stock V1.2.0, unit #2,
 * 2026-11-12): PRM and TRIGGER in the scope open the page directly inside
 * their pane (Measure, Trigger) and keep it there. The pane's keys work as
 * above, but nothing leads to the rail or another row: LEFT at the pane's
 * edge does nothing and BACK closes the page (the opener key closes it too,
 * key_model.c). Only short MENU opens the full page (osc_menu_open).
 */
#ifndef OSC_MENU_MODEL_H
#define OSC_MENU_MODEL_H

#include <stdint.h>
#include <stdbool.h>
#include "view_table.h"   /* view_mode_t: the Display items' values */

/* ── Fixed interface (research/ui/ROUND1_BRIEF.md) ─────────────────── */
typedef enum { OSC_ROW_DISPLAY, OSC_ROW_CURSOR, OSC_ROW_PARAM, OSC_ROW_CH1, OSC_ROW_CH2,
               OSC_ROW_TRIGGER, OSC_ROW_PERSIST, OSC_ROW_MATH, OSC_ROW_SPECTRUM,
               OSC_ROW_COUNT } osc_row_t;
typedef enum { OSC_KEY_UP, OSC_KEY_DOWN, OSC_KEY_LEFT, OSC_KEY_RIGHT, OSC_KEY_OK, OSC_KEY_BACK } osc_key_t;
typedef enum { OSC_RES_NONE, OSC_RES_REDRAW, OSC_RES_CLOSE } osc_result_t;
void         osc_menu_open(osc_row_t row);   /* opens the page with that row selected */
bool         osc_menu_is_open(void);
/* A key of the page: osc_menu_step() below, then the property set it asks
 * for (osc_menu_screen.c: the page is a screen of the router, table-bound to
 * the scope's properties). The brief's osc_menu_key() was that pair. */
void         osc_menu_draw(void);            /* the page's paint (osc_menu_ui.c), display task */
void         osc_menu_close(void);
/* Not in the brief, additive: the screen was cleared behind the page (e.g.
 * DCMD_REDRAW_ALL's lcd_clear), so the next osc_menu_draw() repaints all of
 * it, not just the elements (osc_menu_ui.c). */
void         osc_menu_invalidate(void);
/* Open the page inside `row`'s pane, locked to it (see LOCKED PANE above).
 * While open it switches to that pane, locked. The pane cursor starts on its
 * first item (Measure: Freq of CH1); no OSC_ACT_ENTER is issued, so it is
 * for panes that need no live-value start (PRM's Measure, TRIGGER's Trigger). */
void         osc_menu_open_locked(osc_row_t row);
bool         osc_menu_is_locked(void);

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
/* Display: the one place the scope's view is chosen. The items are the
 * values of the view.mode property, in its order (osc_menu_disp_view_mode).
 * Roll is not a view: the timebase ladder enters it in Y-T (tb.roll).
 * Split is Y-T on top and the spectrum below. The spectrum views (FFT,
 * Split, Waterfall) analyse the ACTIVE channel (MOVE; ui.ach), so there is
 * no per-channel FFT row. */
enum { OSC_DISP_YT = 0, OSC_DISP_XY, OSC_DISP_FFT, OSC_DISP_SPLIT, OSC_DISP_WATERFALL, OSC_DISP_COUNT };
enum { OSC_CUR_OFF = 0, OSC_CUR_TIME, OSC_CUR_VOLT, OSC_CUR_BOTH, OSC_CUR_COUNT };
enum { OSC_CH_ENABLE = 0, OSC_CH_PROBE, OSC_CH_COUPLING, OSC_CH_COUNT };
enum { OSC_TRIG_MODE = 0, OSC_TRIG_EDGE, OSC_TRIG_SOURCE, OSC_TRIG_COUNT };
/* Spectrum (ours): how the spectrum of the FFT, split and waterfall views
 * is computed. */
enum { OSC_SPEC_WINDOW = 0, OSC_SPEC_AVG, OSC_SPEC_MAXHOLD, OSC_SPEC_COUNT };
enum { OSC_PERS_OFF = 0, OSC_PERS_500MS, OSC_PERS_1S, OSC_PERS_2S, OSC_PERS_INF, OSC_PERS_COUNT };
/* Measure: 0 = the set's tab, 1 = All ("A-on"), 2 = None ("A-off"), 3.. = the
 * set's chips (osc_menu_layout gives how many the panel on screen has;
 * OSC_PRM_COUNT is a channel's panel, the largest). */
enum { OSC_PRM_TAB = 0, OSC_PRM_ALL, OSC_PRM_NONE, OSC_PRM_CHIP0, OSC_PRM_COUNT = OSC_PRM_CHIP0 + 12 };
/* Math: 0 = OFF, 1..8 = stock's 2x4 grid (osc_menu_math_op) */
enum { OSC_MATH_OFF = 0, OSC_MATH_COUNT = 9 };

/* ── Model state ────────────────────────────────────────────────────── */
typedef struct {
    bool    open;
    uint8_t row;         /* osc_row_t: the rail selection */
    bool    in_pane;     /* false = rail focus */
    uint8_t item;        /* pane item under the cursor */
    uint8_t meas_set;    /* Measure pane: the panel on screen (meas_set_t) */
    uint8_t meas_sets;   /* ... and the panels the view has, bit per meas_set_t */
    uint8_t opens;       /* bumped on every osc_menu_open*(): the drawer repaints all */
    bool    locked;      /* osc_menu_open_locked(): pane only, no rail */
} osc_menu_state_t;

const osc_menu_state_t *osc_menu_state(void);

/* What a key asks the screen to apply (model -> osc_menu_screen.c). */
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
 * ("Channel", "Probe", ...); Measure chips use the measurements' labels.
 * `set` matters only in the Measure pane: its tab ("CH1", "CH2", "FFT") and
 * its chips. "" out of range. */
const char *osc_menu_item_label(osc_row_t row, uint8_t item, uint8_t set);

/* Display item -> the view.mode value it sets (view_mode_t: yt, xy, fft,
 * split, waterfall), or -1 for an item that is no view. */
#define OSC_DISP_VIEW_MODES VIEW_MODE_COUNT
int8_t  osc_menu_disp_view_mode(uint8_t item);
/* view.mode value -> the Display item that shows it checked;
 * OSC_DISP_COUNT (no item) for a value out of range. */
uint8_t osc_menu_disp_item(int32_t view_mode);

/* Measure chip `chip` (grid order) of a set's panel -> the measurement's id
 * in the set; 0xFF for a chip the panel has not. The channels' grid is
 * stock's, row-major Freq Wid+ Vp-p / Peri Wid- Amp / Duty+ Vmax Vrms /
 * Duty- Vmin Avg; the spectrum's is its measurements in order. */
uint8_t osc_menu_meas_of_chip(uint8_t set, uint8_t chip);

/* The Measure pane's panels: the measurement sets the view on screen has
 * (bit per meas_set_t; none given: the channels), and the one to start on.
 * The screen says so before it opens the page (osc_menu_screen.c). */
void osc_menu_set_meas_sets(uint8_t sets, uint8_t first);
/* Math item 1..8 -> math_op_t (stock's 2x4: CH1+CH2 -CH1 / CH1-CH2 -CH2 /
 * CH1*CH2 |CH1| / CH1/CH2 |CH2|); 0xFF for OFF or out of range. */
uint8_t osc_menu_math_op(uint8_t item);
/* math_op_t -> Math item (1..8); OSC_MATH_OFF if unknown. */
uint8_t osc_menu_math_item(uint8_t op);

/* Grid geometry of a CHIPS item: row and column (span_first counted). */
void osc_menu_chip_pos(osc_row_t row, uint8_t item, uint8_t *r, uint8_t *c);

#endif /* OSC_MENU_MODEL_H */
