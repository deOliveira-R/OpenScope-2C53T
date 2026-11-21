/*
 * OpenScope 2C53T - the scope's views, as a table
 *
 * A view (the Display setting, view.mode) draws one or two DOMAINS: the time
 * record (Y-T, roll, X-Y) or the spectrum computed from it (FFT, waterfall);
 * split draws both. What the keys, the measurements and the cursors mean
 * belongs to the domain, not to the picture: the arrows scale volts and time
 * or level and span, a cursor reads seconds and volts or hertz and dB. The
 * view only says which domains it shows and what it draws of them.
 *
 * So the key model, the settings page and the furniture ask this table
 * instead of testing for a particular view. An entry lists only what the
 * view's renderer really draws today: X-Y and the waterfall get a strip and
 * cursors when they are display types on the canvas (docs/design/
 * display_architecture.md, steps 6 and 7).
 *
 * Pure: a header, so every host test that links the key model has it.
 */
#ifndef VIEW_TABLE_H
#define VIEW_TABLE_H

#include <stdbool.h>
#include <stdint.h>

/* The values of view.mode, in the property's order (lbl_view_mode). */
typedef enum {
    VIEW_MODE_YT = 0, VIEW_MODE_ROLL, VIEW_MODE_XY, VIEW_MODE_FFT, VIEW_MODE_SPLIT,
    VIEW_MODE_WATERFALL, VIEW_MODE_COUNT
} view_mode_t;

typedef enum { VIEW_DOM_TIME = 0, VIEW_DOM_FREQ, VIEW_DOM_COUNT } view_domain_t;
#define VIEW_DOM_BIT(d)  ((uint8_t)(1u << (d)))

/* What the arrows adjust: a job. Short SELECT steps through the jobs of the
 * view, in the order its entry lists them. */
typedef enum {
    KF_ZOOM = 0,    /* LEFT/RIGHT timebase, UP/DOWN volts/div */
    KF_CHMOVE,      /* LEFT/RIGHT trigger column, UP/DOWN channel vertical position */
    KF_TRIGMOVE,    /* LEFT/RIGHT trigger column, UP/DOWN trigger level */
    KF_CURSOR,      /* UP/DOWN move the cursor, LEFT/RIGHT pick one (only while cursors are on) */
    KF_MASK,        /* mask tolerances (only while a mask exists; ours, not stock's) */
    KF_SPECTRUM,    /* LEFT/RIGHT the spectrum's span, UP/DOWN its reference level */
    KF_SPECTRUM_CURSOR,   /* the cursor job on a spectrum: the lines move in frequency and level */
    KF_COUNT
} key_focus_t;

/* The domain a job works on. */
static inline view_domain_t key_focus_domain(key_focus_t f)
{
    return (f == KF_SPECTRUM || f == KF_SPECTRUM_CURSOR) ? VIEW_DOM_FREQ : VIEW_DOM_TIME;
}

/* A job that moves the cursors (on offer only while cursors are on). */
static inline bool key_focus_is_cursor(key_focus_t f)
{
    return f == KF_CURSOR || f == KF_SPECTRUM_CURSOR;
}

typedef struct {
    uint8_t domains;            /* VIEW_DOM_BIT of each domain on screen */
    uint8_t n_jobs;
    uint8_t jobs[KF_COUNT];     /* key_focus_t, in SELECT order; the first is where the view starts */
    bool    strip;              /* it draws the measurement strip (long PRM hides it) */
} view_desc_t;

#define VIEW_TIME_ONLY  VIEW_DOM_BIT(VIEW_DOM_TIME)
#define VIEW_FREQ_ONLY  VIEW_DOM_BIT(VIEW_DOM_FREQ)

static const view_desc_t view_table[VIEW_MODE_COUNT] = {
    [VIEW_MODE_YT]        = { VIEW_TIME_ONLY, 5, { KF_ZOOM, KF_CHMOVE, KF_TRIGMOVE, KF_CURSOR, KF_MASK }, true },
    [VIEW_MODE_ROLL]      = { VIEW_TIME_ONLY, 5, { KF_ZOOM, KF_CHMOVE, KF_TRIGMOVE, KF_CURSOR, KF_MASK }, true },
    [VIEW_MODE_XY]        = { VIEW_TIME_ONLY, 3, { KF_ZOOM, KF_CHMOVE, KF_TRIGMOVE }, false },
    /* The spectrum's span follows the timebase and its level the range, so
     * the spectrum views offer the input's scales as a second job. */
    [VIEW_MODE_FFT]       = { VIEW_FREQ_ONLY, 3, { KF_SPECTRUM, KF_ZOOM, KF_SPECTRUM_CURSOR }, true },
    /* One ring over both halves: the job says which half the arrows are on. */
    [VIEW_MODE_SPLIT]     = { VIEW_TIME_ONLY | VIEW_FREQ_ONLY, 4,
                              { KF_ZOOM, KF_CHMOVE, KF_TRIGMOVE, KF_SPECTRUM }, false },
    [VIEW_MODE_WATERFALL] = { VIEW_FREQ_ONLY, 2, { KF_SPECTRUM, KF_ZOOM }, false },
};

/* The entry of a view.mode value; Y-T's for a value out of range. */
static inline const view_desc_t *view_desc(uint8_t view_mode)
{
    return &view_table[view_mode < VIEW_MODE_COUNT ? view_mode : VIEW_MODE_YT];
}

/* The view's cursor job, or KF_COUNT when it draws no cursors. */
static inline key_focus_t view_cursor_job(uint8_t view_mode)
{
    const view_desc_t *v = view_desc(view_mode);
    for (uint8_t i = 0; i < v->n_jobs; i++)
        if (key_focus_is_cursor((key_focus_t)v->jobs[i])) return (key_focus_t)v->jobs[i];
    return KF_COUNT;
}

static inline bool view_shows(uint8_t view_mode, view_domain_t d)
{
    return (view_desc(view_mode)->domains & VIEW_DOM_BIT(d)) != 0u;
}

/* The domain a key that has one meaning per domain (AUTO) acts on: the
 * job's, when the view shows it, else the one the view shows. */
static inline view_domain_t view_key_domain(uint8_t view_mode, key_focus_t job)
{
    const view_domain_t d = key_focus_domain(job);
    if (view_shows(view_mode, d)) return d;
    return view_shows(view_mode, VIEW_DOM_TIME) ? VIEW_DOM_TIME : VIEW_DOM_FREQ;
}

#endif /* VIEW_TABLE_H */
