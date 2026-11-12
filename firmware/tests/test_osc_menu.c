/*
 * Host tests for the Oscilloscope Settings page model (src/ui/osc_menu_model.c),
 * the per-channel measurement selection (src/ui/meas_select.c) and the menu
 * frame's scroll rule (src/ui/menu_frame.h). All pure: this is the code the
 * input task runs. Labels are measured with the real font tables, so a label
 * that would be cut on the LCD fails here (study section 4: no label wraps).
 *
 * Build: make test-osc-menu
 */
#include <stdio.h>
#include <string.h>
#include "osc_menu_model.h"
#include "meas_select.h"
#include "menu_frame.h"
#include "scope_state.h"
#include "scope_measure.h"
#include "math_channel.h"
#include "font.h"

static int run, passed;
#define CHECK(name, cond) do { run++; if (cond) { passed++; printf("  %-66s[PASS]\n", name); } \
                               else { printf("  %-66s[FAIL]\n", name); } } while (0)

/* The UI half (osc_menu_ui.c) is not linked: the model never calls it. */

static osc_action_t A;
static osc_result_t key(osc_key_t k) { return osc_menu_step(k, &A); }
static const osc_menu_state_t *S(void) { return osc_menu_state(); }

static unsigned text_w(const char *s, const font_t *f)
{
    unsigned w = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        uint8_t idx = (c < f->first_char || c > f->last_char) ? 0xFF : f->charmap[c - f->first_char];
        w += (idx != 0xFF && idx < f->num_glyphs) ? f->advances[idx] : f->height / 3u;
    }
    return w;
}

static void fresh(void)
{
    scope_state_init(scope_state_get());
    meas_select_defaults();
    osc_menu_close();
}

/* ── labels ─────────────────────────────────────────────────────────── */
static void test_labels_fit(void)
{
    printf("labels (one line, measured in the real fonts)\n");
    static const char *const rail[OSC_ROW_COUNT] = {
        "Display", "Cursors", "Measure", "CH1", "CH2", "Trigger", "Persist", "Math" };
    int ok = 1, fit = 1;
    for (int r = 0; r < OSC_ROW_COUNT; r++) {
        ok  &= strcmp(osc_menu_row_label((osc_row_t)r), rail[r]) == 0;
        fit &= text_w(osc_menu_row_label((osc_row_t)r), &font_medium) <= 96u - 8u - 1u;
    }
    CHECK("8 rail rows in stock order, study 2.3's one-line labels", ok && OSC_ROW_COUNT == 8);
    CHECK("every rail label fits the 96 px rail in font_medium", fit);
    fit = 1;
    for (int r = 0; r < OSC_ROW_COUNT; r++)
        fit &= text_w(osc_menu_row_title((osc_row_t)r), &font_medium) <= 218u - 4u;
    CHECK("every pane title fits the 218 px pane", fit);
    CHECK("pane titles are stock's names", strcmp(osc_menu_row_title(OSC_ROW_PARAM), "Parameter Measurement") == 0 &&
          strcmp(osc_menu_row_title(OSC_ROW_MATH), "Mathematics") == 0);

    fit = 1;
    for (int r = 0; r < OSC_ROW_COUNT; r++) {
        const osc_pane_layout_t *l = osc_menu_layout((osc_row_t)r);
        for (uint8_t i = 0; i < l->n_items; i++) {
            for (uint8_t ch = 0; ch < 2; ch++) {
                const char *s = osc_menu_item_label((osc_row_t)r, i, ch);
                fit &= s[0] != '\0';
                if (l->kind == OSC_PANE_RADIO)  fit &= text_w(s, &font_medium) <= 210u - 30u;
                if (l->kind == OSC_PANE_VALUES) fit &= text_w(s, &font_medium) <= 98u;
                if (r == OSC_ROW_PARAM && i != OSC_PRM_TAB) fit &= text_w(s, &font_small) <= 64u - 4u;
                if (r == OSC_ROW_PARAM && i == OSC_PRM_TAB) fit &= text_w(s, &font_medium) <= 64u - 4u;
                if (r == OSC_ROW_MATH)          fit &= text_w(s, &font_small) <= 99u - 4u;
            }
        }
    }
    CHECK("every item label is non-empty and fits its radio row / value row / chip", fit);
    CHECK("out-of-range item gives \"\"", osc_menu_item_label(OSC_ROW_CH1, 9, 0)[0] == '\0' &&
          osc_menu_row_label(OSC_ROW_COUNT)[0] == '\0');
    CHECK("Measure tab names the channel", strcmp(osc_menu_item_label(OSC_ROW_PARAM, OSC_PRM_TAB, 0), "CH1") == 0 &&
          strcmp(osc_menu_item_label(OSC_ROW_PARAM, OSC_PRM_TAB, 1), "CH2") == 0);
}

/* ── tables ─────────────────────────────────────────────────────────── */
static void test_tables(void)
{
    printf("stock grids\n");
    static const uint8_t want[12] = {
        SCOPE_PRM_FREQ, SCOPE_PRM_WID_POS, SCOPE_PRM_VPP, SCOPE_PRM_PERI, SCOPE_PRM_WID_NEG, SCOPE_PRM_AMP,
        SCOPE_PRM_DUTY_POS, SCOPE_PRM_VMAX, SCOPE_PRM_VRMS, SCOPE_PRM_DUTY_NEG, SCOPE_PRM_VMIN, SCOPE_PRM_AVG };
    int ok = 1; uint16_t seen = 0;
    for (uint8_t i = 0; i < 12; i++) { ok &= osc_menu_prm_of_chip(i) == want[i]; seen |= (uint16_t)(1u << osc_menu_prm_of_chip(i)); }
    CHECK("Measure chips = stock's 3x4 row-major Freq Wid+ Vp-p / Peri ...", ok);
    CHECK("the 12 chips cover all 12 measurements once", seen == SCOPE_PRM_ALL_MASK);
    CHECK("chip 12 is out of range", osc_menu_prm_of_chip(12) == 0xFF);
    CHECK("chip labels are scope_measure's", strcmp(osc_menu_item_label(OSC_ROW_PARAM, OSC_PRM_CHIP0 + 2, 0), "Vp-p") == 0);

    static const uint8_t mop[9] = { 0xFF, MATH_ADD, MATH_INV_A, MATH_SUB, MATH_INV_B,
                                    MATH_MUL, MATH_ABS_A, MATH_DIV, MATH_ABS_B };
    ok = 1;
    for (uint8_t i = 0; i < 9; i++) ok &= osc_menu_math_op(i) == mop[i];
    CHECK("Math grid = stock's 2x4 (CH1+CH2 -CH1 / CH1-CH2 -CH2 / ...) as math_op_t", ok);
    ok = 1;
    for (uint8_t op = 0; op < MATH_COUNT; op++) ok &= osc_menu_math_op(osc_menu_math_item(op)) == op;
    CHECK("math op -> item -> op round-trips; unknown op is OFF", ok && osc_menu_math_item(99) == OSC_MATH_OFF);
    CHECK("Math labels match their ops", strcmp(osc_menu_item_label(OSC_ROW_MATH, osc_menu_math_item(MATH_DIV), 0), "CH1/CH2") == 0 &&
          strcmp(osc_menu_item_label(OSC_ROW_MATH, osc_menu_math_item(MATH_ABS_B), 0), "|CH2|") == 0);

    const osc_pane_layout_t *l = osc_menu_layout(OSC_ROW_MATH);
    CHECK("pane kinds: Display/Cursors/Persist radio, CH/Trigger values, Measure/Math chips",
          osc_menu_layout(OSC_ROW_DISPLAY)->kind == OSC_PANE_RADIO && osc_menu_layout(OSC_ROW_CURSOR)->kind == OSC_PANE_RADIO &&
          osc_menu_layout(OSC_ROW_PERSIST)->kind == OSC_PANE_RADIO && osc_menu_layout(OSC_ROW_CH1)->kind == OSC_PANE_VALUES &&
          osc_menu_layout(OSC_ROW_CH2)->kind == OSC_PANE_VALUES && osc_menu_layout(OSC_ROW_TRIGGER)->kind == OSC_PANE_VALUES &&
          osc_menu_layout(OSC_ROW_PARAM)->kind == OSC_PANE_CHIPS && l->kind == OSC_PANE_CHIPS);
    uint8_t r, c;
    osc_menu_chip_pos(OSC_ROW_MATH, 0, &r, &c); ok = (r == 0 && c == 0);
    osc_menu_chip_pos(OSC_ROW_MATH, 1, &r, &c); ok &= (r == 1 && c == 0);
    osc_menu_chip_pos(OSC_ROW_MATH, 2, &r, &c); ok &= (r == 1 && c == 1);
    osc_menu_chip_pos(OSC_ROW_MATH, 8, &r, &c); ok &= (r == 4 && c == 1);
    osc_menu_chip_pos(OSC_ROW_PARAM, 14, &r, &c); ok &= (r == 4 && c == 2);
    CHECK("grid positions: Math OFF alone on row 0, 2x4 below; Measure 5x3", ok);
}

/* ── open / close ───────────────────────────────────────────────────── */
static void test_open_close(void)
{
    printf("open / close / deep links\n");
    fresh();
    CHECK("closed after boot, keys ignored", !osc_menu_is_open() && key(OSC_KEY_DOWN) == OSC_RES_NONE);
    const uint8_t o0 = S()->opens;
    osc_menu_open(OSC_ROW_PARAM);
    CHECK("open(PARAM): open, Measure selected, rail focus", osc_menu_is_open() && S()->row == OSC_ROW_PARAM && !S()->in_pane);
    CHECK("each open bumps the repaint counter", S()->opens == (uint8_t)(o0 + 1u));
    key(OSC_KEY_RIGHT);
    osc_menu_open(OSC_ROW_TRIGGER);
    CHECK("another deep link while open jumps to its row, rail focus", S()->row == OSC_ROW_TRIGGER && !S()->in_pane);
    osc_menu_open((osc_row_t)42);
    CHECK("a bad row opens on Display", S()->row == OSC_ROW_DISPLAY);
    osc_menu_close();
    CHECK("close", !osc_menu_is_open());
}

/* ── rail ───────────────────────────────────────────────────────────── */
static void test_rail(void)
{
    printf("rail\n");
    fresh();
    osc_menu_open(OSC_ROW_DISPLAY);
    CHECK("UP from Display wraps to Math", key(OSC_KEY_UP) == OSC_RES_REDRAW && S()->row == OSC_ROW_MATH);
    CHECK("DOWN from Math wraps to Display", key(OSC_KEY_DOWN) == OSC_RES_REDRAW && S()->row == OSC_ROW_DISPLAY);
    key(OSC_KEY_DOWN); key(OSC_KEY_DOWN);
    CHECK("DOWN steps one row (Display -> Cursors -> Measure)", S()->row == OSC_ROW_PARAM);
    CHECK("LEFT on the rail does nothing", key(OSC_KEY_LEFT) == OSC_RES_NONE && !S()->in_pane);
    osc_menu_open(OSC_ROW_CURSOR);
    CHECK("RIGHT enters the pane and asks for the live value", key(OSC_KEY_RIGHT) == OSC_RES_REDRAW &&
          S()->in_pane && A.kind == OSC_ACT_ENTER && A.row == OSC_ROW_CURSOR);
    key(OSC_KEY_BACK);
    CHECK("BACK in the pane returns to the rail, page stays open", !S()->in_pane && osc_menu_is_open());
    CHECK("OK on the rail enters too", key(OSC_KEY_OK) == OSC_RES_REDRAW && S()->in_pane && A.kind == OSC_ACT_ENTER);
    key(OSC_KEY_BACK);
    CHECK("BACK on the rail closes the page", key(OSC_KEY_BACK) == OSC_RES_CLOSE && !osc_menu_is_open());
}

/* ── radio panes ────────────────────────────────────────────────────── */
static void test_radio(void)
{
    printf("radio panes (Display, Cursors, Persist)\n");
    fresh();
    osc_menu_open(OSC_ROW_PERSIST);
    key(OSC_KEY_RIGHT);
    osc_menu_set_item(OSC_PERS_1S);
    CHECK("the UI puts the cursor on the live value", S()->item == OSC_PERS_1S);
    osc_menu_set_item(77);
    CHECK("an out-of-range value lands on item 0", S()->item == 0);
    CHECK("UP from the first item wraps to the last (Infinite)", key(OSC_KEY_UP) == OSC_RES_REDRAW && S()->item == OSC_PERS_INF);
    CHECK("DOWN wraps back to Off", key(OSC_KEY_DOWN) == OSC_RES_REDRAW && S()->item == OSC_PERS_OFF);
    key(OSC_KEY_DOWN);
    CHECK("OK picks the item under the cursor", key(OSC_KEY_OK) == OSC_RES_REDRAW &&
          A.kind == OSC_ACT_SELECT && A.row == OSC_ROW_PERSIST && A.item == OSC_PERS_500MS);
    CHECK("moving does not apply (no action on UP/DOWN)", key(OSC_KEY_DOWN) == OSC_RES_REDRAW && A.kind == OSC_ACT_NONE);
    CHECK("RIGHT in a radio pane does nothing", key(OSC_KEY_RIGHT) == OSC_RES_NONE && A.kind == OSC_ACT_NONE);
    CHECK("LEFT returns to the rail", key(OSC_KEY_LEFT) == OSC_RES_REDRAW && !S()->in_pane);

    printf("cursors close the page when one is picked\n");
    osc_menu_open(OSC_ROW_CURSOR);
    key(OSC_KEY_RIGHT);
    osc_menu_set_item(OSC_CUR_OFF);
    CHECK("picking Off keeps the page", key(OSC_KEY_OK) == OSC_RES_REDRAW && osc_menu_is_open() && A.kind == OSC_ACT_SELECT);
    key(OSC_KEY_DOWN);
    CHECK("picking X1-X2 closes it, the action still to apply", key(OSC_KEY_OK) == OSC_RES_CLOSE &&
          !osc_menu_is_open() && A.kind == OSC_ACT_SELECT && A.row == OSC_ROW_CURSOR && A.item == OSC_CUR_TIME);
    CHECK("select_closes: cursors on only", osc_menu_select_closes(OSC_ROW_CURSOR, OSC_CUR_BOTH) &&
          !osc_menu_select_closes(OSC_ROW_CURSOR, OSC_CUR_OFF) && !osc_menu_select_closes(OSC_ROW_DISPLAY, OSC_DISP_FFT) &&
          !osc_menu_select_closes(OSC_ROW_CURSOR, OSC_CUR_COUNT));
}

/* ── value panes ────────────────────────────────────────────────────── */
static void test_values(void)
{
    printf("value panes (CH1, CH2, Trigger)\n");
    fresh();
    osc_menu_open(OSC_ROW_CH2);
    key(OSC_KEY_OK);
    CHECK("entering starts on the first row (Channel)", S()->item == OSC_CH_ENABLE);
    key(OSC_KEY_DOWN); key(OSC_KEY_DOWN);
    CHECK("RIGHT changes the row by +1", key(OSC_KEY_RIGHT) == OSC_RES_REDRAW &&
          A.kind == OSC_ACT_CHANGE && A.row == OSC_ROW_CH2 && A.item == OSC_CH_COUPLING && A.dir == +1);
    CHECK("LEFT changes by -1 (and stays in the pane)", key(OSC_KEY_LEFT) == OSC_RES_REDRAW &&
          A.kind == OSC_ACT_CHANGE && A.dir == -1 && S()->in_pane);
    CHECK("OK changes by +1", key(OSC_KEY_OK) == OSC_RES_REDRAW && A.kind == OSC_ACT_CHANGE && A.dir == +1);
    key(OSC_KEY_DOWN);
    CHECK("DOWN from FFT wraps to Channel", key(OSC_KEY_DOWN) == OSC_RES_REDRAW && S()->item == OSC_CH_ENABLE);
    CHECK("BACK returns to the rail", key(OSC_KEY_BACK) == OSC_RES_REDRAW && !S()->in_pane);
    osc_menu_open(OSC_ROW_TRIGGER);
    key(OSC_KEY_RIGHT); key(OSC_KEY_UP);
    CHECK("Trigger: three rows, UP from Mode wraps to Channel", S()->item == OSC_TRIG_SOURCE);
}

/* ── Measure pane ───────────────────────────────────────────────────── */
static void test_measure(void)
{
    printf("Measure pane (stock's per-channel A-on / A-off lists)\n");
    fresh();
    CHECK("defaults: CH1 and CH2 = the five badges",
          meas_select_get(0) == SCOPE_PRM_DEFAULT_MASK && meas_select_get(1) == SCOPE_PRM_DEFAULT_MASK);
    osc_menu_open(OSC_ROW_PARAM);
    key(OSC_KEY_RIGHT);
    CHECK("entering lands on Freq of CH1", S()->in_pane && S()->item == OSC_PRM_CHIP0 && S()->meas_ch == 0 &&
          A.kind == OSC_ACT_ENTER);
    key(OSC_KEY_OK);
    CHECK("OK on Freq turns it off in CH1 only", !(meas_select_get(0) & (1u << SCOPE_PRM_FREQ)) &&
          meas_select_get(1) == SCOPE_PRM_DEFAULT_MASK);
    key(OSC_KEY_OK);
    CHECK("OK again turns it back on", meas_select_get(0) == SCOPE_PRM_DEFAULT_MASK);
    key(OSC_KEY_RIGHT); key(OSC_KEY_OK);
    CHECK("RIGHT to Wid+ and OK adds Wid+", meas_select_get(0) == (SCOPE_PRM_DEFAULT_MASK | (1u << SCOPE_PRM_WID_POS)));
    key(OSC_KEY_RIGHT);
    CHECK("RIGHT at the last column is clamped", key(OSC_KEY_RIGHT) == OSC_RES_NONE &&
          S()->item == OSC_PRM_CHIP0 + 2);
    key(OSC_KEY_UP);
    CHECK("UP from the first chip row reaches None", S()->item == OSC_PRM_NONE);
    key(OSC_KEY_OK);
    CHECK("None clears CH1", meas_select_get(0) == 0);
    key(OSC_KEY_LEFT); key(OSC_KEY_OK);
    CHECK("All sets all 12", meas_select_get(0) == SCOPE_PRM_ALL_MASK);
    CHECK("UP on CH1's top row is clamped", key(OSC_KEY_UP) == OSC_RES_NONE);
    key(OSC_KEY_LEFT);
    CHECK("LEFT reaches the CH1 tab", S()->item == OSC_PRM_TAB);
    key(OSC_KEY_OK);
    CHECK("OK on the tab flips to CH2's panel", S()->meas_ch == 1 && S()->in_pane);
    key(OSC_KEY_OK);
    CHECK("... and back", S()->meas_ch == 0);
    CHECK("LEFT at the left column returns to the rail", key(OSC_KEY_LEFT) == OSC_RES_REDRAW && !S()->in_pane);

    key(OSC_KEY_RIGHT);                    /* Freq of CH1 */
    key(OSC_KEY_RIGHT);                    /* Wid+ */
    for (int i = 0; i < 3; i++) key(OSC_KEY_DOWN);
    CHECK("DOWN walks the column (Wid+ Wid- Vmax Vmin)", S()->item == OSC_PRM_CHIP0 + 10 && S()->meas_ch == 0);
    CHECK("DOWN past CH1's last row continues into CH2's top row, same column",
          key(OSC_KEY_DOWN) == OSC_RES_REDRAW && S()->meas_ch == 1 && S()->item == OSC_PRM_ALL);
    key(OSC_KEY_OK);
    CHECK("All on CH2 sets CH2 only", meas_select_get(1) == SCOPE_PRM_ALL_MASK && meas_select_get(0) == SCOPE_PRM_ALL_MASK);
    key(OSC_KEY_RIGHT); key(OSC_KEY_OK);
    CHECK("None on CH2 clears CH2 only", meas_select_get(1) == 0 && meas_select_get(0) == SCOPE_PRM_ALL_MASK);
    CHECK("UP above CH2's top row goes back to CH1's last row", key(OSC_KEY_UP) == OSC_RES_REDRAW &&
          S()->meas_ch == 0 && S()->item == OSC_PRM_CHIP0 + 11);
    key(OSC_KEY_DOWN);
    for (int i = 0; i < 4; i++) key(OSC_KEY_DOWN);
    CHECK("DOWN at CH2's last row is clamped", S()->meas_ch == 1 && S()->item == OSC_PRM_CHIP0 + 11 &&
          key(OSC_KEY_DOWN) == OSC_RES_NONE);
    CHECK("the Measure pane never asks the UI to apply anything", A.kind == OSC_ACT_NONE);
}

/* ── Math pane ──────────────────────────────────────────────────────── */
static void test_math(void)
{
    printf("Math pane (OFF + 2x4)\n");
    fresh();
    osc_menu_open(OSC_ROW_MATH);
    key(OSC_KEY_RIGHT);
    osc_menu_set_item(OSC_MATH_OFF);
    CHECK("RIGHT on the full-width OFF does nothing", key(OSC_KEY_RIGHT) == OSC_RES_NONE);
    key(OSC_KEY_DOWN);
    CHECK("DOWN from OFF lands on CH1+CH2", S()->item == 1);
    key(OSC_KEY_RIGHT);
    CHECK("RIGHT to -CH1", S()->item == 2);
    key(OSC_KEY_DOWN);
    CHECK("DOWN to -CH2 (same column)", S()->item == 4);
    key(OSC_KEY_OK);
    CHECK("OK picks it", A.kind == OSC_ACT_SELECT && A.row == OSC_ROW_MATH && osc_menu_math_op(A.item) == MATH_INV_B);
    key(OSC_KEY_UP); key(OSC_KEY_UP);
    CHECK("UP from row 1 goes to OFF", S()->item == OSC_MATH_OFF);
    CHECK("UP at OFF is clamped", key(OSC_KEY_UP) == OSC_RES_NONE);
    for (int i = 0; i < 6; i++) key(OSC_KEY_DOWN);
    CHECK("DOWN clamps at the last row (CH1/CH2)", S()->item == 7);
    CHECK("LEFT at column 0 returns to the rail", key(OSC_KEY_LEFT) == OSC_RES_REDRAW && !S()->in_pane);
}

/* ── meas_select ────────────────────────────────────────────────────── */
static void test_meas_select(void)
{
    printf("meas_select\n");
    fresh();
    meas_select_set(0, 0xFFFF);
    CHECK("bits above the 12 are dropped", meas_select_get(0) == SCOPE_PRM_ALL_MASK);
    CHECK("CH1's mask is scope_state's prm_mask (one copy for the badges and the shell)",
          scope_state_get()->prm_mask == SCOPE_PRM_ALL_MASK);
    scope_state_get()->prm_mask = 0x00Fu;
    CHECK("... both ways", meas_select_get(0) == 0x00Fu);
    meas_select_set(1, 0x0F0u);
    meas_select_set(2, 0x111u);
    CHECK("CH2 has its own; ch > 1 is ignored", meas_select_get(1) == 0x0F0u && meas_select_get(0) == 0x00Fu &&
          meas_select_get(2) == 0);
    meas_select_toggle(1, SCOPE_PRM_AVG);
    meas_select_toggle(1, 12);
    CHECK("toggle flips one bit; prm >= 12 ignored", meas_select_get(1) == (0x0F0u | (1u << SCOPE_PRM_AVG)));
    CHECK("count", meas_select_count(1) == 5 && meas_select_count(0) == 4);
    meas_select_defaults();
    CHECK("defaults restore both", meas_select_get(0) == MEAS_SELECT_CH1_DEFAULT &&
          meas_select_get(1) == MEAS_SELECT_CH2_DEFAULT);
    CHECK("scope_state_init's CH1 default is the same set", MEAS_SELECT_CH1_DEFAULT == SCOPE_PRM_DEFAULT_MASK);
    CHECK("CH2's default is CH1's (Freq, Peri, Duty+, Vp-p, Vrms)",
          MEAS_SELECT_CH2_DEFAULT == MEAS_SELECT_CH1_DEFAULT);
}

/* ── menu_frame scroll ──────────────────────────────────────────────── */
static void test_frame_scroll(void)
{
    printf("menu_frame scroll rule (for System Settings' 14 rows)\n");
    const menu_frame_t f = { 0, 16, 320, 208, 96, 26, 8, NULL, NULL };
    CHECK("8 rows of 26 px fit 208 px: no scroll on this page", menu_frame_visible(&f) == 8 &&
          menu_frame_first(7, 8, 8, 0) == 0);
    CHECK("a selection already visible does not move the window", menu_frame_first(5, 14, 8, 3) == 3);
    CHECK("below the window: scroll just enough", menu_frame_first(10, 14, 8, 0) == 3);
    CHECK("above the window: it becomes the first row", menu_frame_first(1, 14, 8, 4) == 1);
    CHECK("a stale window past the end is clamped", menu_frame_first(13, 14, 8, 12) == 6);
    CHECK("pane geometry: x 102, w 218", menu_frame_pane_x(&f) == 102 && menu_frame_pane_w(&f) == 218);
}

int main(void)
{
    printf("test_osc_menu\n");
    test_labels_fit();
    test_tables();
    test_open_close();
    test_rail();
    test_radio();
    test_values();
    test_measure();
    test_math();
    test_meas_select();
    test_frame_scroll();
    printf("\n%d/%d passed\n", passed, run);
    return passed == run ? 0 : 1;
}
