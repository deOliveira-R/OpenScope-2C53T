/*
 * OpenScope 2C53T - the shell's UI route: exact key replay (ARCH-1 M2)
 *
 *   ui press <KEY> [n]        n short presses (1..50)
 *   ui long <KEY>             the long event (KEY | BTN_LONG_FLAG); refused
 *                             for keys without one (key_model.h KEY_LONG_MASK)
 *   ui seq <tok> <tok> ...    tok = KEY | KEY*n | long:KEY | wait:<ms>
 *                             e.g.  ui seq MENU DOWN DOWN RIGHT UP*5
 *   ui screen                 one key=value line: where the UI is
 *
 * KEY = CH1 CH2 MOVE SELECT TRIGGER PRM AUTO SAVE MENU UP DOWN LEFT RIGHT OK
 * (any case). POWER is refused: its handlers shut down or enter the IAP.
 *
 * Each press goes into the queue the TMR3 key scan feeds
 * (input_inject_button), so it runs everything downstream of the physical
 * scan, and the command then WAITS until the input task has handled it (the
 * input_events_done counter, main.c), not a fixed sleep: replay is as fast
 * as the UI and deterministic. A press the input task does not finish in
 * UI_PRESS_TIMEOUT_MS answers TIMEOUT and stops the sequence; the shell
 * never hangs. A whole `ui seq` line is parsed before the first press, so a
 * bad token anywhere presses nothing.
 *
 * After a press/long/seq the shell prints one status line, then the
 * `ui screen` line:
 *   OK 7 events 412 ms
 *   INVALID token 3 'UPP': unknown key; nothing pressed
 *   REFUSED token 1 'POWER': shutdown / IAP; nothing pressed
 *   TIMEOUT at token 4 (MENU): input task busy (gallery/countdown/selfcal?); 3 of 9 events handled
 *   BUSY at token 2 (UP): key queue full; 1 of 5 events handled
 *
 * `btn` and `keys` are aliases kept for old bench scripts: they print
 * exactly the lines they printed before (ui_shell_btn / ui_shell_keys).
 *
 * The token parser and the one-press wait (ui_press_one) are pure
 * (tests/test_ui_shell.c, make test-ui-shell); the rest is firmware only.
 */
#ifndef UI_SHELL_H
#define UI_SHELL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UI_SEQ_MAX_REPEAT     50u     /* KEY*n and `ui press KEY n`: 1..50, as `btn`      */
#define UI_SEQ_MAX_WAIT_MS    5000u   /* wait:<ms>: 0..5000; longer settles go host-side */
/* One press, whole: a long MENU (mode switch) flushes the settings to the
 * W25Q (plan 2.8: 2 s per press). The budget covers the wait for an earlier
 * press that timed out AND this press's own wait, so a press never holds
 * the shell longer than this (plus one poll step). */
#define UI_PRESS_TIMEOUT_MS   2000u
#define UI_PRESS_POLL_MS      2u

typedef enum { UI_TOK_PRESS = 0, UI_TOK_LONG, UI_TOK_WAIT } ui_tok_kind_t;

typedef struct {
    uint8_t  kind;      /* ui_tok_kind_t                                  */
    uint8_t  key;       /* button_id_t (PRESS/LONG), without BTN_LONG_FLAG */
    uint16_t count;     /* events: n for KEY*n, 1 for long:KEY, 0 for wait */
    uint16_t ms;        /* WAIT only                                      */
} ui_tok_t;

typedef enum {
    UI_PARSE_OK = 0,
    UI_PARSE_END,       /* no token left                                  */
    UI_PARSE_BAD_KEY,   /* unknown key name                               */
    UI_PARSE_BAD_FORM,  /* bad repeat / wait syntax, or out of range      */
    UI_PARSE_POWER,     /* POWER: refused                                 */
    UI_PARSE_NO_LONG,   /* long:KEY for a key without a long event        */
} ui_parse_t;

/* Button id for a key name (case-insensitive, len chars), or -1. POWER
 * answers BTN_POWER: the callers refuse it. */
int         ui_key_find(const char *name, size_t len);
/* Upper-case name of a button id ("MENU"), "?" if none. */
const char *ui_key_name(uint8_t key);
/* The key has a long event (KEY_LONG_MASK). */
bool        ui_key_has_long(uint8_t key);

/* Next token of a `ui seq` line at *s (spaces/tabs between tokens). On
 * return *s is past the token (whatever the result) and tok[0..tn) is the
 * token's text for a message (truncated, NUL-terminated). */
ui_parse_t  ui_seq_next(const char **s, ui_tok_t *t, char *tok, size_t tn);

/* The whole line, before any press: UI_PARSE_OK with *events = the presses
 * it makes (wait tokens make none) and *ntok tokens (0 is UI_PARSE_END), or
 * the first error with *bad = its 1-based token index and tok its text. */
ui_parse_t  ui_seq_check(const char *s, unsigned *ntok, unsigned *events, unsigned *bad,
                         char *tok, size_t tn);

/* Message for a parse result: "unknown key", ... */
const char *ui_parse_text(ui_parse_t r);

/* ── one press (pure; the firmware binds FreeRTOS and the key queue) ── */

typedef struct {
    uint32_t (*now_ms)(void);
    uint32_t (*done)(void);              /* input_events_done: keys handled   */
    bool     (*inject)(uint8_t id);      /* into the key queue; false = full  */
    void     (*sleep_ms)(uint32_t ms);
} ui_press_port_t;

/* DONE: sent and handled. FULL: not sent (queue full). TIMEOUT: sent, not
 * handled in time. STALE: not sent, an earlier press is still unhandled. */
typedef enum { UI_PRESS_DONE = 0, UI_PRESS_FULL, UI_PRESS_TIMEOUT, UI_PRESS_STALE } ui_press_t;

/* One event in, handled, within UI_PRESS_TIMEOUT_MS in all. *target is the
 * handled count the last press waited for (modular): it stays ahead of
 * done() after a TIMEOUT, so the late completion of that press cannot
 * satisfy this one's wait. An earlier press still unhandled is waited for
 * first, out of the same budget; if it does not finish the count is
 * resynchronised (a press the boot logo swallowed is never counted) and
 * this one is not sent (STALE). */
ui_press_t  ui_press_one(const ui_press_port_t *port, uint32_t *target, uint8_t id);

/* usb_debug.c's rows and the two legacy handler bodies (firmware only). */
void cmd_ui_press(const char *args);
void cmd_ui_long(const char *args);
void cmd_ui_seq(const char *args);
void cmd_ui_screen(void);
void ui_shell_btn(const char *args);     /* `btn <name> [n|long]`, old output */
void ui_shell_keys(void);                /* `keys`, old output                */

#endif /* UI_SHELL_H */
