/*
 * OpenScope 2C53T - generator properties (gen.*)
 *
 * signal_gen.c is the owner: it keeps siggen_config_t and pushes every change
 * to TMR7/DMA2/DAC2 itself, so the getters read siggen_get_config() and the
 * setters are its setters through model_hw() (they clamp and cannot fail).
 * The store hooks go through the same owner calls: there is no generator
 * state outside signal_gen.c to write. gen.out is a saved setting only while
 * gen.bootout is on (settings_store.c); M3's codec keeps that rule.
 */
#define MODEL_INTERNAL
#include "model.h"
#include "signal_gen.h"

#define HW model_hw()

_Static_assert(SIGGEN_WAVEFORM_COUNT == 13, "gen.wave 0..12 (model_table.h)");
_Static_assert(SIGGEN_FREQ_MAX_HZ == 50000u && SIGGEN_AMP_MAX_DV == 30u && SIGGEN_DUTY_MAX == 100u,
               "gen.freq / gen.amp / gen.duty ranges (model_table.h)");
_Static_assert(SIGGEN_DEFAULT_FREQ == 1000u && SIGGEN_DEFAULT_AMP_DV == 30u &&
               SIGGEN_DEFAULT_DUTY == 50u && SIGGEN_DEFAULT_WAVE == SIGGEN_SINE,
               "gen.* defaults = config_init_defaults / stock factory");

/* Token-safe names (siggen_wave_name()'s have spaces), same order. */
const char *const lbl_gen_wave[SIGGEN_WAVEFORM_COUNT] = {
    "sine", "square", "sawtooth", "halfwave", "fullwave", "step", "revstep",
    "exprise", "expfall", "dc", "multitone", "sinc", "lorentz",
};

static const siggen_config_t *cfg(void) { return siggen_get_config(); }

int32_t gen_wave_get(const prop_desc_t *p) { (void)p; return cfg()->waveform; }
void    gen_wave_store(const prop_desc_t *p, int32_t v) { (void)p; if (HW->gen_wave) HW->gen_wave((uint8_t)v); }
prop_status_t gen_wave_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    gen_wave_store(p, v);
    return PS_OK;
}

int32_t gen_freq_get(const prop_desc_t *p) { (void)p; return (int32_t)cfg()->frequency_hz; }
void    gen_freq_store(const prop_desc_t *p, int32_t v) { (void)p; if (HW->gen_freq) HW->gen_freq((uint32_t)v); }
prop_status_t gen_freq_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin;
    gen_freq_store(p, v);
    if (v == 0) model_note(note, "0 Hz: output off");
    return PS_OK;
}

int32_t gen_amp_get(const prop_desc_t *p) { (void)p; return cfg()->amplitude_dv; }
void    gen_amp_store(const prop_desc_t *p, int32_t v) { (void)p; if (HW->gen_amp) HW->gen_amp((uint8_t)v); }
prop_status_t gen_amp_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    gen_amp_store(p, v);
    return PS_OK;
}

int32_t gen_duty_get(const prop_desc_t *p) { (void)p; return cfg()->duty_pct; }
void gen_duty_store(const prop_desc_t *p, int32_t v)
{
    (void)p;
    percent_t d = { (uint8_t)v };
    if (HW->gen_duty) HW->gen_duty(d);
}
prop_status_t gen_duty_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin;
    gen_duty_store(p, v);
    if (cfg()->waveform != SIGGEN_SQUARE && cfg()->waveform != SIGGEN_SAWTOOTH)
        model_note(note, "duty: square/sawtooth only");
    return PS_OK;
}

int32_t gen_out_get(const prop_desc_t *p) { (void)p; return cfg()->output_enabled ? 1 : 0; }
void    gen_out_store(const prop_desc_t *p, int32_t v) { (void)p; if (HW->gen_out) HW->gen_out(v != 0); }
prop_status_t gen_out_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    gen_out_store(p, v);
    return PS_OK;
}

int32_t gen_bootout_get(const prop_desc_t *p) { (void)p; return cfg()->start_on ? 1 : 0; }
void    gen_bootout_store(const prop_desc_t *p, int32_t v) { (void)p; if (HW->gen_bootout) HW->gen_bootout(v != 0); }
prop_status_t gen_bootout_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    gen_bootout_store(p, v);
    return PS_OK;
}
