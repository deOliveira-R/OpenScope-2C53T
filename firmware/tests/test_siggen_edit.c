/*
 * Host tests for the generator's frequency digit editor (src/ui/siggen_edit.c,
 * parity G4): stock edits by +/- 10^digit with a clamp at 50000 and a floor
 * at 0 (0x0800A45A, 0x080454B0). Run: make test-siggen-edit
 */

#include <stdio.h>
#include <string.h>
#include "siggen_edit.h"

static int run, passed;
#define CHECK(name, cond) do { run++; if (cond) { passed++; printf("  %-66s[PASS]\n", name); } \
                               else { printf("  %-66s[FAIL]\n", name); } } while (0)

static siggen_edit_t E;
static void begin(uint32_t hz) { siggen_edit_begin(&E, hz); }
static siggen_edit_result_t key(siggen_edit_key_t k) { return siggen_edit_key(&E, k); }
static void cursor(uint8_t d)
{
    while (E.digit < d) key(SGE_KEY_LEFT);
    while (E.digit > d) key(SGE_KEY_RIGHT);
}

int main(void)
{
    printf("siggen frequency editor\n");

    memset(&E, 0, sizeof E);
    CHECK("inactive editor ignores every key", key(SGE_KEY_UP) == SGE_IGNORED && E.value == 0);

    begin(1000);
    CHECK("begin: active, 1 kHz digit, value kept",
          E.active && E.digit == SIGGEN_EDIT_START_DIGIT && E.value == 1000 && E.original == 1000);

    /* every digit steps by its own power of ten */
    for (uint8_t d = 0; d < SIGGEN_EDIT_DIGITS; d++) {
        begin(20000); cursor(d);
        siggen_edit_result_t r = key(SGE_KEY_UP);
        uint32_t step = siggen_edit_step(d);
        char nm[80];
        snprintf(nm, sizeof nm, "digit %u: UP adds %lu", d, (unsigned long)step);
        CHECK(nm, r == SGE_CHANGED && E.value == 20000 + step);
        snprintf(nm, sizeof nm, "digit %u: DOWN subtracts back", d);
        CHECK(nm, key(SGE_KEY_DOWN) == SGE_CHANGED && E.value == 20000);
    }

    /* any value reachable, not presets: 12345 from 0 */
    begin(0);
    static const uint8_t want[SIGGEN_EDIT_DIGITS] = { 5, 4, 3, 2, 1 };
    int ok = 1;
    for (uint8_t d = 0; d < SIGGEN_EDIT_DIGITS; d++) {
        cursor(d);
        for (int i = 0; i < want[d]; i++) key(SGE_KEY_UP);
    }
    ok = (E.value == 12345);
    CHECK("any value is reachable (12345 from 0)", ok);

    /* clamp at the top, no wrap */
    begin(49999); cursor(0);
    CHECK("UP at 49999 reaches 50000", key(SGE_KEY_UP) == SGE_CHANGED && E.value == 50000);
    CHECK("UP at 50000 does not change", key(SGE_KEY_UP) == SGE_MOVED && E.value == 50000);
    begin(45000); cursor(4);
    CHECK("10 kHz step past the top clamps to 50000",
          key(SGE_KEY_UP) == SGE_CHANGED && E.value == 50000);
    begin(60000);
    CHECK("begin clamps an out-of-range input", E.value == 50000);

    /* floor at 0 = output off */
    begin(5); cursor(1);
    CHECK("DOWN past zero saturates to 0", key(SGE_KEY_DOWN) == SGE_CHANGED && E.value == 0);
    CHECK("DOWN at 0 does not change", key(SGE_KEY_DOWN) == SGE_MOVED && E.value == 0);
    begin(10); cursor(0);
    CHECK("exactly to zero", key(SGE_KEY_DOWN) == SGE_CHANGED && E.value == 9);

    /* cursor limits */
    begin(1000); cursor(4);
    CHECK("LEFT stops at the 10 kHz digit", key(SGE_KEY_LEFT) == SGE_IGNORED && E.digit == 4);
    cursor(0);
    CHECK("RIGHT stops at the 1 Hz digit", key(SGE_KEY_RIGHT) == SGE_IGNORED && E.digit == 0);

    /* leaving */
    begin(1000); key(SGE_KEY_UP);
    CHECK("OK keeps the value and closes",
          key(SGE_KEY_ACCEPT) == SGE_ACCEPTED && !E.active && E.value == 2000);
    CHECK("closed editor ignores keys again", key(SGE_KEY_UP) == SGE_IGNORED);
    begin(1000); key(SGE_KEY_UP); key(SGE_KEY_UP);
    CHECK("MENU restores the original and says so",
          key(SGE_KEY_CANCEL) == SGE_CANCELLED && !E.active && E.value == 1000);
    begin(1000);
    CHECK("MENU with nothing changed is just a close",
          key(SGE_KEY_CANCEL) == SGE_ACCEPTED && !E.active);

    /* digit extraction used by the screen */
    CHECK("digit_of reads each decade",
          siggen_edit_digit_of(40302, 4) == 4 && siggen_edit_digit_of(40302, 3) == 0 &&
          siggen_edit_digit_of(40302, 2) == 3 && siggen_edit_digit_of(40302, 1) == 0 &&
          siggen_edit_digit_of(40302, 0) == 2);

    printf("%d/%d passed\n", passed, run);
    return passed == run ? 0 : 1;
}
