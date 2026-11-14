/*
 * Host tests for the signal generator screen (src/ui/siggen_screen.c;
 * ARCH-1 B3): the real screen, the real router (screen.c), the real
 * frequency editor, the real property model and the real generator
 * (signal_gen.c), with fake hardware ops and a logging DAC driver.
 *
 * What is checked:
 *   - the key table of siggen_screen.h, every key, short and long, with the
 *     editor closed and open: the screen's answer and the owner calls made;
 *   - every change goes through a gen.* property (the op log is the only
 *     way the generator moved), is what prop_get answers at once, bumps the
 *     gen epoch and notes the settings;
 *   - a set from elsewhere while the screen (and the editor) is up is what
 *     the screen shows and what the next key steps from;
 *   - the editor's undo, and that leaving the screen drops the editor;
 *   - the epoch the display task gates the page on;
 *   - the DRIVER calls (dac_output_*: TMR7/DMA2/DAC2) of each key against
 *     the calls the pre-B3 key handlers made, in order.
 *
 * Build: make test-siggen-screen.
 */
#include "screen_test_env.h"
#include "siggen_screen.h"
#include "siggen_edit.h"

static int g_paints;
void siggen_ui_draw(void) { g_paints++; }

static const screen_t S_SCOPE = ENV_BASE("scope", SCR_SCOPE, MODE_OSCILLOSCOPE, 0);
static const screen_t S_HOME  = ENV_BASE("home",  SCR_HOME,  MODE_HOME, SCR_INSTRUMENT_NONE);

#define LONG(b) ((uint8_t)((b) | BTN_LONG_FLAG))

static void boot(void)
{
    env_fresh(MODE_SIGNAL_GEN);
    screen_reset();
    screen_register(&S_SCOPE); screen_register(&S_HOME); screen_register(&siggen_screen);
    screen_init(SCR_SIGGEN);
    g_paints = 0;
}

/* A state set the way the shell does it, then a clean log. */
static void given(const prop_desc_t *p, int32_t v)
{
    (void)prop_set_local(p, v, ORIGIN_SHELL, NULL);
    env_log_clear();
}

static const siggen_edit_t *ED(void) { return siggen_ui_editor(); }
static const char *described(void)
{
    static char d[32];
    d[0] = '\0';
    siggen_screen.describe(d, sizeof d);
    return d;
}
static int32_t G(const prop_desc_t *p) { return prop_get(p); }

/* One key from a fresh boot (+ an optional preset): answer and op log. */
static bool one(const prop_desc_t *pre, int32_t pre_v, bool editor, uint8_t raw,
                scr_res_t want_res, const char *want_log)
{
    boot();
    if (pre) given(pre, pre_v);
    if (editor) { (void)env_key(BTN_PRM); env_log_clear(); }
    const scr_res_t r = env_key(raw);
    const bool ok = r == want_res && env_log_is(want_log);
    if (!ok) printf("    key %u editor %d: answer %d (want %d), log [%s] (want [%s])\n",
                    raw, editor, r, want_res, env_log, want_log);
    return ok;
}

static void test_screen_entry(void)
{
    printf("the screen\n");
    CHECK("name siggen, id SCR_SIGGEN, MODE_SIGNAL_GEN, instrument 2 (gen), no flags",
          strcmp(siggen_screen.name, "siggen") == 0 && siggen_screen.id == SCR_SIGGEN &&
          siggen_screen.mode == MODE_SIGNAL_GEN && siggen_screen.instrument == 2 &&
          siggen_screen.flags == 0);
    const instrument_desc_t *in = model_instrument_at(siggen_screen.instrument);
    CHECK("its instrument is the model's generator", in && in->mode == MODE_SIGNAL_GEN);
    CHECK("it has open, close, key, draw, epoch and describe",
          siggen_screen.open && siggen_screen.close && siggen_screen.key && siggen_screen.draw &&
          siggen_screen.epoch && siggen_screen.describe);
    CHECK("it shows gen, sys and ui (a `set gen.*` repaints it)",
          siggen_screen.groups == (uint16_t)((1u << PG_GEN) | (1u << PG_SYS) | (1u << PG_UI)));
    boot();
    CHECK("defaults: sine 1000 Hz 3.0 V 50 %, output off, boot off; editor closed, edit=off",
          G(P(GEN_WAVE)) == 0 && G(P(GEN_FREQ)) == 1000 && G(P(GEN_AMP)) == 30 && G(P(GEN_DUTY)) == 50 &&
          G(P(GEN_OUT)) == 0 && G(P(GEN_BOOTOUT)) == 0 && !ED()->active && strcmp(described(), "edit=off") == 0);
}

static void test_keys_editor_closed(void)
{
    printf("every key, editor closed: answer and owner calls\n");
    CHECK("SELECT: gen.wave +1",                 one(NULL, 0, false, BTN_SELECT, SCR_DRAW, "gen_wave(1);"));
    CHECK("SELECT at the last waveform wraps",   one(P(GEN_WAVE), 12, false, BTN_SELECT, SCR_DRAW, "gen_wave(0);"));
    CHECK("UP: gen.amp +0.1 V",                  one(P(GEN_AMP), 15, false, BTN_UP, SCR_DRAW, "gen_amp(16);"));
    CHECK("UP at 3.0 V: no call (page asked, the gate drops it)",
                                                 one(NULL, 0, false, BTN_UP, SCR_DRAW, ""));
    CHECK("DOWN: gen.amp -0.1 V",                one(NULL, 0, false, BTN_DOWN, SCR_DRAW, "gen_amp(29);"));
    CHECK("DOWN at 0 V: no call",                one(P(GEN_AMP), 0, false, BTN_DOWN, SCR_DRAW, ""));
    CHECK("LEFT on a sine: duty is not its parameter, no call",
                                                 one(NULL, 0, false, BTN_LEFT, SCR_DRAW, ""));
    CHECK("RIGHT on a sine: no call",            one(NULL, 0, false, BTN_RIGHT, SCR_DRAW, ""));
    CHECK("RIGHT on a square: gen.duty +5 %",    one(P(GEN_WAVE), 1, false, BTN_RIGHT, SCR_DRAW, "gen_duty(55);"));
    CHECK("LEFT on a square: gen.duty -5 %",     one(P(GEN_WAVE), 1, false, BTN_LEFT, SCR_DRAW, "gen_duty(45);"));
    CHECK("LEFT on a sawtooth (symmetry): -5 %", one(P(GEN_WAVE), 2, false, BTN_LEFT, SCR_DRAW, "gen_duty(45);"));
    CHECK("RIGHT on a half wave: no call",       one(P(GEN_WAVE), 3, false, BTN_RIGHT, SCR_DRAW, ""));
    CHECK("OK: gen.out on",                      one(NULL, 0, false, BTN_OK, SCR_DRAW, "gen_out(1);"));
    CHECK("OK with the output on: off",          one(P(GEN_OUT), 1, false, BTN_OK, SCR_DRAW, "gen_out(0);"));
    CHECK("long OK: gen.out (Run/Stop)",         one(NULL, 0, false, LONG(BTN_OK), SCR_DRAW, "gen_out(1);"));
    CHECK("long AUTO: gen.bootout on",           one(NULL, 0, false, LONG(BTN_AUTO), SCR_DRAW, "gen_boot(1);"));
    CHECK("long AUTO again: off",                one(P(GEN_BOOTOUT), 1, false, LONG(BTN_AUTO), SCR_DRAW, "gen_boot(0);"));
    CHECK("PRM: opens the editor, no call",      one(NULL, 0, false, BTN_PRM, SCR_DRAW, ""));
    CHECK("MENU: Home, as one sys.mode set",     one(NULL, 0, false, BTN_MENU, SCR_HANDLED, "mode(4);"));
    int ok = 1;
    static const uint8_t full[] = { BTN_CH1, BTN_CH2, BTN_TRIGGER, BTN_MOVE };
    for (unsigned i = 0; i < sizeof full; i++) ok &= one(NULL, 0, false, full[i], SCR_DRAW_FULL, "");
    CHECK("CH1, CH2, TRIGGER, MOVE: a full repaint and nothing else (as before)", ok);
    ok = 1;
    static const uint8_t none[] = { BTN_AUTO, BTN_SAVE, BTN_POWER, LONG(BTN_CH1), LONG(BTN_CH2),
                                    LONG(BTN_PRM), LONG(BTN_MOVE), LONG(BTN_SELECT), LONG(BTN_TRIGGER),
                                    LONG(BTN_SAVE), LONG(BTN_MENU) };
    for (unsigned i = 0; i < sizeof none; i++) ok &= one(NULL, 0, false, none[i], SCR_HANDLED, "");
    CHECK("AUTO and the long keys with no meaning here: handled, nothing", ok);

    boot();
    (void)env_key(BTN_PRM);
    CHECK("PRM: editor on the 1 kHz digit, the live frequency, edit=3",
          ED()->active && ED()->digit == 3 && ED()->original == 1000 && strcmp(described(), "edit=3") == 0);
    boot();
    (void)env_key(BTN_MENU);
    CHECK("MENU: the mode and the router's base are Home; the generator is untouched",
          current_mode == MODE_HOME && screen_base() == SCR_HOME && G(P(GEN_FREQ)) == 1000 && G(P(GEN_OUT)) == 0);
    boot();
    env_mode_applies = false;
    (void)env_key(BTN_MENU);
    CHECK("... with the owner only logging: the screen wrote no mode itself",
          env_log_is("mode(4);") && current_mode == MODE_SIGNAL_GEN && screen_base() == SCR_SIGGEN);
}

static void test_keys_editor_open(void)
{
    printf("every key, editor open\n");
    CHECK("UP: gen.freq + 10^digit, at once",    one(NULL, 0, true, BTN_UP, SCR_DRAW, "gen_freq(2000);"));
    CHECK("DOWN: gen.freq - 10^digit (0 Hz = off)", one(NULL, 0, true, BTN_DOWN, SCR_DRAW, "gen_freq(0);"));
    CHECK("UP at 50000 Hz: no call",             one(P(GEN_FREQ), 50000, true, BTN_UP, SCR_DRAW, ""));
    CHECK("DOWN at 0 Hz: no call",               one(P(GEN_FREQ), 0, true, BTN_DOWN, SCR_DRAW, ""));
    CHECK("LEFT: the cursor, no call",           one(NULL, 0, true, BTN_LEFT, SCR_DRAW, ""));
    CHECK("RIGHT: the cursor, no call",          one(NULL, 0, true, BTN_RIGHT, SCR_DRAW, ""));
    CHECK("OK: closes, no call (not Run/Stop)",  one(NULL, 0, true, BTN_OK, SCR_DRAW, ""));
    CHECK("PRM: closes, no call",                one(NULL, 0, true, BTN_PRM, SCR_DRAW, ""));
    CHECK("MENU with nothing changed: closes, no call, no Home",
                                                 one(NULL, 0, true, BTN_MENU, SCR_DRAW, ""));
    CHECK("SELECT: still the waveform",          one(NULL, 0, true, BTN_SELECT, SCR_DRAW, "gen_wave(1);"));
    CHECK("long OK: still Run/Stop",             one(NULL, 0, true, LONG(BTN_OK), SCR_DRAW, "gen_out(1);"));
    CHECK("long AUTO: still the boot policy",    one(NULL, 0, true, LONG(BTN_AUTO), SCR_DRAW, "gen_boot(1);"));
    CHECK("CH1: still only a full repaint",      one(NULL, 0, true, BTN_CH1, SCR_DRAW_FULL, ""));
    CHECK("AUTO: nothing",                       one(NULL, 0, true, BTN_AUTO, SCR_HANDLED, ""));

    boot();
    (void)env_key(BTN_PRM);
    (void)env_key(BTN_LEFT);
    CHECK("LEFT moves to the 10 kHz digit (edit=4); LEFT again stays",
          ED()->digit == 4 && strcmp(described(), "edit=4") == 0 && env_key(BTN_LEFT) == SCR_DRAW && ED()->digit == 4);
    (void)env_key(BTN_UP);
    CHECK("UP there: 11000 Hz, read back from the property at once",
          env_log_is("gen_freq(11000);") && G(P(GEN_FREQ)) == 11000 && siggen_get_config()->frequency_hz == 11000);
    for (int i = 0; i < 4; i++) (void)env_key(BTN_RIGHT);
    (void)env_key(BTN_RIGHT);
    CHECK("RIGHT x5: the 1 Hz digit, and it stops there", ED()->digit == 0);
    env_log_clear();
    (void)env_key(BTN_UP); (void)env_key(BTN_UP); (void)env_key(BTN_SELECT); (void)env_key(LONG(BTN_OK));
    CHECK("the keys the editor leaves keep it open (SELECT, long OK)",
          ED()->active && env_log_is("gen_freq(11001);gen_freq(11002);gen_wave(1);gen_out(1);"));
    env_log_clear();
    CHECK("OK closes and keeps the value", env_key(BTN_OK) == SCR_DRAW && !ED()->active && env_log_is("") &&
          G(P(GEN_FREQ)) == 11002 && G(P(GEN_OUT)) == 1 && strcmp(described(), "edit=off") == 0);
    CHECK("the next OK is Run/Stop again", env_key(BTN_OK) == SCR_DRAW && env_log_is("gen_out(0);"));

    boot();
    (void)env_key(BTN_PRM); (void)env_key(BTN_UP); (void)env_key(BTN_UP);
    env_log_clear();
    CHECK("MENU after changes: gen.freq back to where the edit began, editor closed, still on the generator",
          env_key(BTN_MENU) == SCR_DRAW && env_log_is("gen_freq(1000);") && G(P(GEN_FREQ)) == 1000 &&
          !ED()->active && current_mode == MODE_SIGNAL_GEN);
    env_log_clear();
    CHECK("the next MENU opens Home", env_key(BTN_MENU) == SCR_HANDLED && env_log_is("mode(4);"));
}

static void test_through_the_model(void)
{
    printf("every change is a property set: get, epoch, settings note\n");
    static const struct { const char *what; uint8_t pre_wave; uint8_t raw; const prop_desc_t *p; int32_t want; } T[] = {
        { "SELECT -> gen.wave",        0, BTN_SELECT,       P(GEN_WAVE),    1 },
        { "DOWN -> gen.amp",           0, BTN_DOWN,         P(GEN_AMP),     29 },
        { "RIGHT (square) -> gen.duty", 1, BTN_RIGHT,       P(GEN_DUTY),    55 },
        { "OK -> gen.out",             0, BTN_OK,           P(GEN_OUT),     1 },
        { "long OK -> gen.out",        0, LONG(BTN_OK),     P(GEN_OUT),     1 },
        { "long AUTO -> gen.bootout",  0, LONG(BTN_AUTO),   P(GEN_BOOTOUT), 1 },
    };
    int ok = 1;
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        boot();
        if (T[i].pre_wave) given(P(GEN_WAVE), T[i].pre_wave);
        env_notes = 0;
        const env_epochs_t e0 = env_epochs();
        (void)env_key(T[i].raw);
        const env_epochs_t e1 = env_epochs();
        const bool good = G(T[i].p) == T[i].want && env_epochs_moved(&e0, &e1) == (1u << PG_GEN) &&
                          env_notes == 1 && current_mode == MODE_SIGNAL_GEN;
        if (!good) printf("    %s: value %ld, groups 0x%X, notes %u\n", T[i].what, (long)G(T[i].p),
                          env_epochs_moved(&e0, &e1), env_notes);
        ok &= good;
    }
    CHECK("each: the property reads the new value, only the gen epoch moved, one settings note", ok);

    boot();
    (void)env_key(BTN_PRM);
    env_notes = 0;
    const env_epochs_t e0 = env_epochs();
    (void)env_key(BTN_UP);
    const env_epochs_t e1 = env_epochs();
    CHECK("the editor's UP: gen.freq reads 2000, gen epoch moved, one settings note",
          G(P(GEN_FREQ)) == 2000 && env_epochs_moved(&e0, &e1) == (1u << PG_GEN) && env_notes == 1);

    boot();
    env_notes = 0;
    const env_epochs_t c0 = env_epochs();
    (void)env_key(BTN_UP);                     /* 3.0 V is the top */
    (void)env_key(BTN_LEFT);                   /* a sine has no duty */
    (void)env_key(BTN_PRM); (void)env_key(BTN_LEFT); (void)env_key(BTN_OK);   /* cursor only */
    (void)env_key(BTN_CH1); (void)env_key(BTN_AUTO);
    const env_epochs_t c1 = env_epochs();
    CHECK("keys that change no generator value: no owner call, no epoch, no note",
          env_log_is("") && env_epochs_moved(&c0, &c1) == 0 && env_notes == 0);

    /* The other direction: a set from the shell while the screen is up. */
    boot();
    const uint32_t ep0 = siggen_screen.epoch();
    given(P(GEN_FREQ), 4321);
    const uint32_t ep1 = siggen_screen.epoch();
    CHECK("`set gen.freq 4321` with the screen up: its epoch moves (the page repaints)", ep0 != ep1);
    (void)env_key(BTN_PRM);
    given(P(GEN_FREQ), 7000);
    CHECK("... and with the editor open too", siggen_screen.epoch() != ep1 && ED()->active);
    (void)env_key(BTN_UP);
    CHECK("the editor's next UP steps from the value that was set (7000 -> 8000)",
          env_log_is("gen_freq(8000);") && G(P(GEN_FREQ)) == 8000);
    env_log_clear();
    (void)env_key(BTN_MENU);
    CHECK("MENU still undoes to the value the edit began with (4321)",
          env_log_is("gen_freq(4321);") && G(P(GEN_FREQ)) == 4321 && !ED()->active);
    given(P(GEN_AMP), 12);
    (void)env_key(BTN_UP);
    CHECK("`set gen.amp 1.2` then UP: steps from it (1.3 V)", env_log_is("gen_amp(13);"));
}

static void test_leaving(void)
{
    printf("leaving and entering\n");
    boot();
    (void)env_key(BTN_PRM); (void)env_key(BTN_UP);
    (void)prop_set_local(P(SYS_MODE), MODE_OSCILLOSCOPE, ORIGIN_KEY, NULL);   /* a long mode key */
    CHECK("leaving with the editor open: the editor is dropped, the value stays",
          !ED()->active && G(P(GEN_FREQ)) == 2000 && screen_base() == SCR_SCOPE);
    (void)prop_set_local(P(SYS_MODE), MODE_SIGNAL_GEN, ORIGIN_KEY, NULL);
    env_log_clear();
    CHECK("back on the generator: editor closed, UP is the amplitude again",
          !ED()->active && strcmp(described(), "edit=off") == 0 &&
          env_key(BTN_DOWN) == SCR_DRAW && env_log_is("gen_amp(29);"));
}

static void test_epoch_and_draw(void)
{
    printf("the epoch the page is gated on, and its paint\n");
    boot();
    uint32_t e = siggen_screen.epoch();
    int ok = 1;
#define MOVES(action) do { action; const uint32_t n = siggen_screen.epoch(); ok &= (n != e); e = n; } while (0)
    MOVES((void)env_key(BTN_SELECT));
    MOVES((void)env_key(BTN_DOWN));
    MOVES((void)env_key(BTN_RIGHT));                 /* square: duty */
    MOVES((void)env_key(BTN_OK));
    MOVES((void)env_key(LONG(BTN_AUTO)));
    MOVES((void)env_key(BTN_PRM));
    MOVES((void)env_key(BTN_LEFT));
    MOVES((void)env_key(BTN_UP));
    MOVES((void)env_key(BTN_MENU));                  /* undo + close */
    MOVES(theme_init(THEME_CLASSIC_GREEN));
#undef MOVES
    CHECK("it moves with the waveform, amplitude, duty, output, boot policy, the editor "
          "opening, its cursor, the frequency, the editor closing and the theme", ok);
    boot();
    e = siggen_screen.epoch();
    (void)env_key(BTN_UP); (void)env_key(BTN_LEFT); (void)env_key(BTN_AUTO);
    CHECK("it stays for keys that changed nothing (their in-place paint is dropped)",
          siggen_screen.epoch() == e);
    g_paints = 0;
    screen_draw(SCR_PAINT_FULL, 0);
    screen_draw(SCR_PAINT_UPDATE, 0);
    CHECK("FULL and UPDATE both paint the page (it has no partial paint)", g_paints == 2);
    CHECK("no tick: the page is static", screen_tick(0) == REDRAW_SKIP);
    char t[96];
    screen_snapshot_text(t, sizeof t);
    CHECK("the router's snapshot text: siggen ... | edit=off",
          strncmp(t, "siggen gen=", 11) == 0 && strstr(t, "| edit=off") != NULL);
}

/* ── the driver side: what reaches dac_output.c, in order ──────────── */

/* The driver calls of `key` from a state, against the calls of the handler
 * the key had before B3 (`legacy`). */
typedef void (*legacy_fn)(void);
static void old_select(void)    { (void)siggen_cycle_waveform(); }
static void old_up(void)        { (void)siggen_amplitude_up(); }
static void old_down(void)      { (void)siggen_amplitude_down(); }
static void old_left(void)      { if (siggen_wave_uses_duty(siggen_get_config()->waveform)) (void)siggen_duty_cycle_down(); }
static void old_right(void)     { if (siggen_wave_uses_duty(siggen_get_config()->waveform)) (void)siggen_duty_cycle_up(); }
static void old_ok(void)        { siggen_enable(!siggen_get_config()->output_enabled); }
static void old_boot(void)      { siggen_set_start_on(!siggen_get_config()->start_on); }
static void old_ed_up(void)     { uint32_t v = siggen_get_config()->frequency_hz + 1000u;
                                  if (v > 50000u) v = 50000u;
                                  if (v != siggen_get_config()->frequency_hz) siggen_set_frequency(v); }
static void old_ed_down(void)   { uint32_t f = siggen_get_config()->frequency_hz, v = f > 1000u ? f - 1000u : 0u;
                                  if (v != f) siggen_set_frequency(v); }

typedef struct { uint8_t wave; uint32_t freq; uint8_t amp, duty; bool out; } gstate_t;

static void set_state(const gstate_t *g)
{
    boot();
    given(P(GEN_WAVE), g->wave); given(P(GEN_FREQ), (int32_t)g->freq); given(P(GEN_AMP), g->amp);
    given(P(GEN_DUTY), g->duty); given(P(GEN_OUT), g->out);
}

/* 0 = same driver calls and same generator state; else why. */
static const char *driver_diff(const gstate_t *g, bool editor, uint8_t raw, legacy_fn legacy,
                               char *got_old, char *got_new, size_t n)
{
    set_state(g);
    env_log_ops = false; env_log_dac = true; env_log_clear();
    legacy();
    snprintf(got_old, n, "%s", env_log);
    const siggen_config_t after_old = *siggen_get_config();

    set_state(g);
    if (editor) (void)env_key(BTN_PRM);
    env_log_ops = false; env_log_dac = true; env_log_clear();
    (void)env_key(raw);
    snprintf(got_new, n, "%s", env_log);
    const siggen_config_t after_new = *siggen_get_config();
    env_log_ops = true; env_log_dac = false;

    if (after_old.waveform != after_new.waveform || after_old.frequency_hz != after_new.frequency_hz ||
        after_old.amplitude_dv != after_new.amplitude_dv || after_old.duty_pct != after_new.duty_pct ||
        after_old.output_enabled != after_new.output_enabled || after_old.start_on != after_new.start_on)
        return "state";
    return strcmp(got_old, got_new) == 0 ? NULL : "calls";
}

static void test_driver_calls(void)
{
    printf("driver calls (dac_output: TMR7 / DMA2 / DAC2) against the pre-B3 handlers\n");
    static const struct { const char *name; bool editor; uint8_t raw; legacy_fn old; } K[] = {
        { "SELECT",     false, BTN_SELECT,     old_select },
        { "UP",         false, BTN_UP,         old_up },
        { "DOWN",       false, BTN_DOWN,       old_down },
        { "LEFT",       false, BTN_LEFT,       old_left },
        { "RIGHT",      false, BTN_RIGHT,      old_right },
        { "OK",         false, BTN_OK,         old_ok },
        { "long OK",    false, LONG(BTN_OK),   old_ok },
        { "long AUTO",  false, LONG(BTN_AUTO), old_boot },
        { "editor UP",  true,  BTN_UP,         old_ed_up },
        { "editor DOWN", true, BTN_DOWN,       old_ed_down },
    };
    int cases = 0, same = 0, state_bad = 0, duty_limit = 0, other = 0;
    char o[256], nw[256], first[400] = "";
    static const uint8_t  waves[] = { 0, 1, 2, 9, 12 };
    static const uint32_t freqs[] = { 0, 1, 1000, 49500, 50000 };
    static const uint8_t  amps[]  = { 0, 15, 30 };
    static const uint8_t  duties[] = { 0, 3, 50, 98, 100 };
    for (unsigned w = 0; w < sizeof waves; w++)
    for (unsigned f = 0; f < sizeof freqs / sizeof freqs[0]; f++)
    for (unsigned a = 0; a < sizeof amps; a++)
    for (unsigned d = 0; d < sizeof duties; d++)
    for (int out = 0; out < 2; out++)
    for (unsigned k = 0; k < sizeof K / sizeof K[0]; k++) {
        const gstate_t g = { waves[w], freqs[f], amps[a], duties[d], out != 0 };
        const char *why = driver_diff(&g, K[k].editor, K[k].raw, K[k].old, o, nw, sizeof o);
        cases++;
        if (!why) { same++; continue; }
        if (strcmp(why, "state") == 0) { state_bad++; continue; }
        /* The one known difference: LEFT at 0 % / RIGHT at 100 % on a
         * square or sawtooth. The old handler called siggen_set_duty() with
         * the value it already had, which rebuilt the same cycle into the
         * DMA buffer; the property step does nothing at its limit. */
        const bool at_limit = siggen_wave_uses_duty(g.wave) &&
                              ((K[k].raw == BTN_LEFT && g.duty == 0) || (K[k].raw == BTN_RIGHT && g.duty == 100));
        if (at_limit && strcmp(o, "dac_buf;") == 0 && nw[0] == '\0') { duty_limit++; continue; }
        other++;
        if (!first[0])
            snprintf(first, sizeof first, "%s wave %u freq %lu amp %u duty %u out %d: old [%s] new [%s]",
                     K[k].name, g.wave, (unsigned long)g.freq, g.amp, g.duty, g.out, o, nw);
    }
    printf("    %d cases: %d identical, %d duty-at-limit\n", cases, same, duty_limit);
    if (first[0]) printf("    first other difference: %s\n", first);
    CHECK("every key, 750 generator states: the generator ends in the same state", state_bad == 0 && cases == 7500);
    CHECK("... and the driver sees the same calls in the same order", other == 0);
    CHECK("the one listed difference: duty at its limit no longer rewrites the same cycle",
          duty_limit == 2 * 2 * 5 * 3 * 2);   /* square+sawtooth x {LEFT@0, RIGHT@100} x freqs x amps x out */

    /* Spelled out for the common presses, output running at 1 kHz. */
    const gstate_t running = { 1, 1000, 30, 50, true };
    set_state(&running);
    env_log_dac = true; env_log_clear();
    (void)env_key(BTN_OK);
    CHECK("OK (stop): the op, then one driver stop", env_log_is("gen_out(0);dac_stop;"));
    env_log_clear();
    (void)env_key(BTN_OK);
    CHECK("OK (run): the op, then one driver apply", env_log_is("gen_out(1);dac_apply;"));
    env_log_clear();
    (void)env_key(BTN_SELECT);
    CHECK("SELECT while running: the cycle is rebuilt into the DMA buffer, the timer is not touched",
          env_log_is("gen_wave(2);dac_buf;"));
    env_log_clear();
    (void)env_key(BTN_PRM); (void)env_key(BTN_UP);
    CHECK("editor UP while running: the timer is re-applied, the cycle is not rebuilt",
          env_log_is("gen_freq(2000);dac_apply;"));
    env_log_dac = false;
}

int main(void)
{
    printf("signal generator screen\n");
    test_screen_entry();
    test_keys_editor_closed();
    test_keys_editor_open();
    test_through_the_model();
    test_leaving();
    test_epoch_and_draw();
    test_driver_calls();
    printf("\n%d/%d passed\n", passed, run);
    return passed == run ? 0 : 1;
}
