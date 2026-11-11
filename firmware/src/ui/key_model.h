/*
 * OpenScope 2C53T - key model (long presses and the stock key meanings)
 *
 * Two pure pieces, no FreeRTOS and no GPIO, so tests/test_key_model.c runs the
 * very code the ISR and the input task run:
 *
 *   key_scan_tick()  the per-key press state machine. button_scan.c's TMR3 ISR
 *                    reads the matrix and calls it once per 2 ms tick; it says
 *                    which keys produced a short or a long press this tick.
 *   key_map()        (context, key, short/long) -> what to do. input_handler.c
 *                    executes the answer. Anything this does not claim comes
 *                    back KA_LEGACY and the older per-mode handlers keep it.
 *
 * Stock evidence (V1.2.0): the scanner at 0x080403A4-0x080404DE keeps one
 * counter per key, fires a code at 70 ticks (0x08040 3CE: cmp #70), fires a
 * second code from 72 ticks on every tick (cmp #72, counter decremented back
 * so it re-fires) and a release code for a tap of 2..69 ticks. The key task
 * (0x08040008) does no counting of its own: it takes one byte from the queue
 * at 0x20002D70 and calls the handler in the table at 0x08046548. So the
 * duration a user feels as "long" is decided by the scan call rate, which is
 * not fixed at 500 Hz (TMR3's ARR follows the timebase for the slow codes).
 * That, and the code table at 0x08046528 (not recoverable from the vendor
 * image, the 2026-04-08 re-audit), is why KEY_LONG_TICKS below is OUR value,
 * not a stock constant. TODO(bench): time stock's long BACK with a stopwatch.
 *
 * Stock's key meanings are in the manual's key tables (section 1.1/2.1/3.1);
 * the key names there are BACK = our MENU and "Switch CH1/CH2 Adjustment" =
 * our SELECT, "Select Function for Directional Keys" = our MOVE.
 */

#ifndef KEY_MODEL_H
#define KEY_MODEL_H

#include <stdint.h>
#include <stdbool.h>
#include "ui.h"          /* button_id_t, device_mode_t, BTN_LONG_FLAG */

/* ── Scan timing (ticks of the 500 Hz scan = 2 ms) ────────────────── */
#define KEY_NUM              15
#define KEY_CONFIRM_TICKS    70    /* 140 ms: a press is real (stock: cmp #70) */
#define KEY_REPEAT_INITIAL   250   /* arrows: 500 ms after the confirm */
#define KEY_REPEAT_INTERVAL  75    /* arrows: then every 150 ms */
#ifndef KEY_LONG_TICKS
#define KEY_LONG_TICKS       500   /* 1.0 s from first contact; see header note */
#endif

/* Bits of the 15-bit raw scan word (button_scan.c). */
#define KEYBIT_POWER    0
#define KEYBIT_AUTO     1
#define KEYBIT_CH1      2
#define KEYBIT_MOVE     3
#define KEYBIT_SELECT   4
#define KEYBIT_TRIGGER  5
#define KEYBIT_PRM      6
#define KEYBIT_CH2      7
#define KEYBIT_SAVE     8
#define KEYBIT_MENU     9
#define KEYBIT_UP       10
#define KEYBIT_DOWN     11
#define KEYBIT_LEFT     12
#define KEYBIT_RIGHT    13
#define KEYBIT_OK       14

/* Keys that have a long-press action. For these the short press is delivered
 * on RELEASE (held 70..LONG-1 ticks) and the long press once, at LONG; a
 * short-at-confirm plus a long later would double-fire (long SELECT would
 * flip the probe on its way to the meter). The other keys fire at the confirm
 * exactly as before. POWER is never in the set: its handler polls the pin
 * itself for the shutdown countdown.
 *
 * AUTO/OK/SAVE are in the set because stock gives them long actions (baseline
 * correction, 50 % centre, gallery). Long AUTO and long OK are built (C10,
 * scope_selfcal); long SAVE still answers with a "not in this build" popup
 * rather than a surprise short action. */
#define KEY_LONG_MASK  ((1u << KEYBIT_AUTO)   | (1u << KEYBIT_CH1)    | \
                        (1u << KEYBIT_MOVE)   | (1u << KEYBIT_SELECT) | \
                        (1u << KEYBIT_TRIGGER)| (1u << KEYBIT_PRM)    | \
                        (1u << KEYBIT_CH2)    | (1u << KEYBIT_SAVE)   | \
                        (1u << KEYBIT_MENU)   | (1u << KEYBIT_OK))

/* Keys that auto-repeat while held (the arrows). */
#define KEY_REPEAT_MASK ((1u << KEYBIT_UP)   | (1u << KEYBIT_DOWN) | \
                         (1u << KEYBIT_LEFT) | (1u << KEYBIT_RIGHT))

typedef struct {
    uint16_t held[KEY_NUM];     /* ticks held, saturating (a byte would stop at 510 ms) */
    uint16_t rep[KEY_NUM];      /* ticks since the confirm, for the repeat */
} key_scan_t;

typedef struct {
    uint8_t bit;                /* KEYBIT_* */
    bool    is_long;
} key_scan_event_t;

void key_scan_init(key_scan_t *s);

/* One scan tick. `raw` is the 15-bit pressed set. Writes the events of this
 * tick to out[] (room for KEY_NUM) and returns how many. */
uint8_t key_scan_tick(key_scan_t *s, uint16_t raw, key_scan_event_t out[KEY_NUM]);

/* ── What the arrows adjust (MOVE cycles it) ──────────────────────── */
typedef enum {
    KF_ZOOM = 0,    /* LEFT/RIGHT timebase, UP/DOWN volts/div */
    KF_CHMOVE,      /* LEFT/RIGHT trigger column, UP/DOWN channel vertical position */
    KF_TRIGMOVE,    /* LEFT/RIGHT trigger column, UP/DOWN trigger level */
    KF_CURSOR,      /* UP/DOWN move the cursor, LEFT/RIGHT pick one (only while cursors are on) */
    KF_MASK,        /* mask tolerances (only while a mask exists; ours, not stock's) */
    KF_COUNT
} key_focus_t;

key_focus_t key_focus_next(key_focus_t cur, bool cursors_on, bool mask_present);
/* Drop a stage that no longer exists (cursors turned off, mask cleared). */
key_focus_t key_focus_sanitize(key_focus_t cur, bool cursors_on, bool mask_present);
/* TRIGGER cycled the cursor mode: switching them on selects the cursor
 * stage (what the arrows did before this model), off leaves it. */
key_focus_t key_focus_after_cursor_change(key_focus_t cur, bool cursors_on);
const char *key_focus_name(key_focus_t f);     /* short shell word */
const char *key_focus_popup(key_focus_t f);    /* popup text */

/* ── Key -> action ────────────────────────────────────────────────── */
typedef struct {
    device_mode_t mode;
    int8_t        settings_depth;
    bool          scope_time_view;   /* scope mode and the time-domain view */
    key_focus_t   focus;
    bool          cursors_on;
    bool          mask_present;
    uint8_t       active_channel;    /* 0 = CH1, 1 = CH2 */
    bool          trig_menu_open;    /* the trigger panel (trig_menu.h) is up */
} key_ctx_t;

typedef enum {
    KA_LEGACY = 0,        /* not part of the model: the per-mode handlers decide */
    KA_NONE,              /* claimed, nothing to do */
    KA_ENTER_HOME,        /* long BACK: the settings list stands in for the home page */
    KA_ENTER_METER,       /* long SELECT */
    KA_ENTER_SCOPE,       /* long MOVE */
    KA_ENTER_SIGGEN,      /* long TRIGGER */
    KA_MEAS_HIDE_TOGGLE,  /* long PRM */
    KA_CH_ENABLE_TOGGLE,  /* long CH1/CH2: arg = channel (0/1) */
    KA_SIGGEN_BOOT_TOGGLE,/* long AUTO in the generator: output-at-boot policy (ours) */
    KA_SIGGEN_RUNSTOP,    /* long OK in the generator: output Run/Stop (manual 2.1) */
    KA_GALLERY_OPEN,      /* long SAVE in the scope: screenshot gallery (stock "nine palace grid") */
    KA_BASECAL,           /* long AUTO in the scope: baseline correction (stock 0x0800DF58) */
    KA_CENTER50,          /* long OK in the scope: 50 % centre (stock 0x0800DA3A) */
    KA_FOCUS_NEXT,        /* short MOVE */
    KA_ADJ_CH_SWITCH,     /* short SELECT: which channel the arrows act on */
    KA_PARAM_SELECT,      /* short PRM: measurement selection (hook; content is another item) */
    KA_TB_STEP,           /* arg = -1 faster, +1 slower */
    KA_VDIV_STEP,         /* arg = +1 UP */
    KA_CHPOS_STEP,        /* arg = +1 UP */
    KA_TRIGCOL_STEP,      /* arg = -1 left, +1 right */
    KA_TRIGLVL_STEP,      /* arg = +1 UP */
    KA_CURSOR_MOVE,       /* arg = -1 UP, +1 DOWN */
    KA_CURSOR_NEXT,
    KA_MASK_TOL_V,        /* arg = +1 UP */
    KA_MASK_TOL_H,        /* arg = +1 right */
    KA_MASK_STOPFAIL,     /* short SELECT in the mask stage */
    KA_TRIGMENU_OPEN,     /* short TRIGGER (time view): stock's "Trigger Settings" */
    KA_TRIGMENU_CLOSE,    /* TRIGGER/MENU/MOVE/SELECT/PRM while it is open */
    KA_TRIGMENU_ROW,      /* UP/DOWN while open: arg = -1 up, +1 down */
    KA_TRIGMENU_CHANGE    /* LEFT/RIGHT/OK while open: arg = -1 / +1 */
} key_action_t;

typedef struct {
    key_action_t act;
    int8_t       arg;
} key_cmd_t;

key_cmd_t key_map(const key_ctx_t *c, button_id_t btn, bool is_long);

#endif /* KEY_MODEL_H */
