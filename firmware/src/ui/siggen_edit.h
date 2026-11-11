/*
 * OpenScope 2C53T - signal generator frequency editor (parity G4).
 *
 * Stock edits the frequency with a digit cursor (ms+0xF6B high nibble,
 * V1.2.0 0x0800A456): UP/DOWN add or subtract 10^digit, the result is
 * clamped to 0..50000 and 0 means output off (0x0800A45A, 0x08029D5C).
 * Any value in range is reachable, not a preset list.
 *
 * This is the pure state machine; no hardware and no siggen_* calls, so it
 * builds on the host (make test-siggen-edit). The input handler feeds it
 * keys and pushes `value` to the generator after every SGE_CHANGED, so the
 * output follows the edit as in stock.
 */

#ifndef SIGGEN_EDIT_H
#define SIGGEN_EDIT_H

#include <stdint.h>
#include <stdbool.h>

#define SIGGEN_EDIT_DIGITS       5u       /* 0..50000 */
#define SIGGEN_EDIT_MAX_HZ       50000u
#define SIGGEN_EDIT_START_DIGIT  3u       /* 1 kHz: the usual first decade */

typedef enum {
    SGE_KEY_LEFT = 0,    /* cursor to the next more significant digit */
    SGE_KEY_RIGHT,       /* cursor to the next less significant digit */
    SGE_KEY_UP,          /* + 10^digit */
    SGE_KEY_DOWN,        /* - 10^digit */
    SGE_KEY_ACCEPT,      /* OK or PRM: keep the value, leave the editor */
    SGE_KEY_CANCEL       /* MENU: restore the value the edit began with */
} siggen_edit_key_t;

typedef enum {
    SGE_IGNORED = 0,     /* editor not active or key without effect */
    SGE_MOVED,           /* cursor moved or hit a limit; value unchanged */
    SGE_CHANGED,         /* value changed: apply it */
    SGE_ACCEPTED,        /* editor closed, value stays */
    SGE_CANCELLED        /* editor closed, value back to the original: apply it */
} siggen_edit_result_t;

typedef struct {
    bool     active;
    uint8_t  digit;      /* cursor: 0 = 1 Hz .. 4 = 10 kHz */
    uint32_t value;      /* working frequency, 0..50000 */
    uint32_t original;   /* value at siggen_edit_begin() */
} siggen_edit_t;

void siggen_edit_begin(siggen_edit_t *e, uint32_t current_hz);

siggen_edit_result_t siggen_edit_key(siggen_edit_t *e, siggen_edit_key_t key);

/* The one live editor, owned by siggen_ui.c (device build only). */
siggen_edit_t *siggen_ui_editor(void);

/* 10^digit (digit 0..4). */
uint32_t siggen_edit_step(uint8_t digit);

/* Number of the digit at `digit` of `value` (0..9). */
uint8_t siggen_edit_digit_of(uint32_t value, uint8_t digit);

#endif /* SIGGEN_EDIT_H */
