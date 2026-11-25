/*
 * OpenScope 2C53T - the scope's views, as a table
 *
 * A view (the Display setting, view.mode) draws one or two DOMAINS: the time
 * record (Y-T, X-Y) or the spectrum computed from it (FFT, waterfall); split
 * draws both. Roll is not a view: it is a source of the time record (the
 * time-base property tb.roll), and only a view that says so takes it. What the keys, the measurements and the cursors mean
 * belongs to the domain, not to the picture: the arrows scale volts and time
 * or level and span, a cursor reads seconds and volts or hertz and dB. The
 * view only says which domains it shows and what it draws of them.
 *
 * So the key model, the settings page and the furniture ask this table
 * instead of testing for a particular view. An entry lists only what the
 * view's renderer really draws today: X-Y and the waterfall have no strip or
 * cursors yet (docs/design/display_architecture.md, steps 6 and 7). Split is
 * a pair of views in two panes (view.upper over view.lower), any two of the
 * views that can be a half (Y-T, X-Y, FFT): its entry is composed from
 * theirs (view_desc_of), so a view that can be a half brings its jobs, strip
 * and cursors with it.
 *
 * Pure: a header, so every host test that links the key model has it.
 */
#ifndef VIEW_TABLE_H
#define VIEW_TABLE_H

#include <stdbool.h>
#include <stdint.h>

/* The values of view.mode, in the property's order (lbl_view_mode). */
typedef enum {
    VIEW_MODE_YT = 0, VIEW_MODE_XY, VIEW_MODE_FFT, VIEW_MODE_SPLIT, VIEW_MODE_WATERFALL,
    VIEW_MODE_COUNT
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

/* The settings that hide a view's measurement rows (long PRM; the cursor
 * readout row stays while the cursors are on). The full views share one,
 * view.meashide; split has its own, view.splithide, so the results shown or
 * hidden in split are not the full views' and the other way round.
 * VIEW_RESULTS_COUNT: the view draws no strip. */
typedef enum { VIEW_RESULTS_FULL = 0, VIEW_RESULTS_SPLIT, VIEW_RESULTS_COUNT } view_results_t;

/* Split starts with its results hidden: its halves are half the plot each,
 * so the owner wants them whole until the rows are asked for (2026-11-25). */
#define VIEW_SPLIT_RESULTS_HIDDEN_DEFAULT  true

typedef struct {
    uint8_t domains;            /* VIEW_DOM_BIT of each domain on screen */
    uint8_t n_jobs;
    uint8_t jobs[KF_COUNT];     /* key_focus_t, in SELECT order; the first is where the view starts */
    uint8_t results;            /* view_results_t: the setting long PRM toggles to hide its
                                 * measurement strip; VIEW_RESULTS_COUNT: it draws none */
    bool    roll;               /* it takes the roll source (tb.roll): its renderer draws the
                                 * roll FIFO (scope_yt.c, view_build_roll). While the view
                                 * does not, tb.roll is off and cannot be set on. */
    bool    half;               /* it can be a half of split: it has a drawer for a pane of
                                 * any height (scope_split.c) */
    bool    pair;               /* split: its domains and jobs are its two halves' (view_desc_of) */
} view_desc_t;

#define VIEW_TIME_ONLY  VIEW_DOM_BIT(VIEW_DOM_TIME)
#define VIEW_FREQ_ONLY  VIEW_DOM_BIT(VIEW_DOM_FREQ)

/* X-Y is a half with its trail in the FFT pool's slot (scope_xy.c). The
 * waterfall cannot be one: its scratch line shares memory with the Y-T window
 * (ui_draw_scratch_t), which the other half would be using, and its history
 * is the slot the X-Y half's trail takes. */
static const view_desc_t view_table[VIEW_MODE_COUNT] = {
    [VIEW_MODE_YT]        = { VIEW_TIME_ONLY, 5, { KF_ZOOM, KF_CHMOVE, KF_TRIGMOVE, KF_CURSOR, KF_MASK },
                              VIEW_RESULTS_FULL, true, true, false },
    [VIEW_MODE_XY]        = { VIEW_TIME_ONLY, 3, { KF_ZOOM, KF_CHMOVE, KF_TRIGMOVE }, VIEW_RESULTS_COUNT,
                              false, true, false },
    /* The spectrum's span follows the timebase and its level the range, so
     * the spectrum views offer the input's scales as a second job. */
    [VIEW_MODE_FFT]       = { VIEW_FREQ_ONLY, 3, { KF_SPECTRUM, KF_ZOOM, KF_SPECTRUM_CURSOR },
                              VIEW_RESULTS_FULL, false, true, false },
    /* Two views, one above the other (view.upper, view.lower). The strip is
     * the one of the half the keys are on, hidden by split's own setting. It
     * does not take roll: that rule is kept for now, though a Y-T half could. */
    [VIEW_MODE_SPLIT]     = { 0u, 0, { 0 }, VIEW_RESULTS_SPLIT, false, false, true },
    [VIEW_MODE_WATERFALL] = { VIEW_FREQ_ONLY, 2, { KF_SPECTRUM, KF_ZOOM }, VIEW_RESULTS_COUNT,
                              false, false, false },
};

/* The halves of split, top to bottom. */
typedef enum { VIEW_HALF_UPPER = 0, VIEW_HALF_LOWER, VIEW_HALF_COUNT } view_half_t;

/* The pair split shows by default: the trace above its spectrum (the split
 * view before its halves could be chosen). */
#define VIEW_HALF_UPPER_DEFAULT  VIEW_MODE_YT
#define VIEW_HALF_LOWER_DEFAULT  VIEW_MODE_FFT

/* What is on screen: view.mode, and the views of split's halves
 * (view.upper, view.lower; read only when mode is split). */
typedef struct {
    uint8_t mode;
    uint8_t half[VIEW_HALF_COUNT];
} view_sel_t;

static inline view_sel_t view_sel(uint8_t mode, uint8_t upper, uint8_t lower)
{
    return (view_sel_t){ mode, { upper, lower } };
}

/* view.mode with split's default pair. */
static inline view_sel_t view_sel_mode(uint8_t mode)
{
    return view_sel(mode, VIEW_HALF_UPPER_DEFAULT, VIEW_HALF_LOWER_DEFAULT);
}

/* The table's entry of a view.mode value; Y-T's for a value out of range. */
static inline const view_desc_t *view_entry(uint8_t view_mode)
{
    return &view_table[view_mode < VIEW_MODE_COUNT ? view_mode : VIEW_MODE_YT];
}

/* The view can be a half of split. */
static inline bool view_is_half(uint8_t view_mode)
{
    return view_mode < VIEW_MODE_COUNT && view_table[view_mode].half;
}

/* The next view after view_mode that can be a half, round the table (a Display
 * menu row steps a half through them). */
static inline uint8_t view_half_next(uint8_t view_mode)
{
    for (uint8_t k = 1; k <= VIEW_MODE_COUNT; k++) {
        const uint8_t v = (uint8_t)((view_mode + k) % VIEW_MODE_COUNT);
        if (view_table[v].half) return v;
    }
    return view_mode;
}

/* Entry h has job f as a half: it lists f and shows f's domain (the
 * spectrum's second job, the input's scales, is the time half's own). */
static inline bool view_half_has_job(const view_desc_t *h, key_focus_t f)
{
    if ((h->domains & VIEW_DOM_BIT(key_focus_domain(f))) == 0u) return false;
    for (uint8_t i = 0; i < h->n_jobs; i++)
        if (h->jobs[i] == (uint8_t)f) return true;
    return false;
}

/* Append to d the jobs entry h has as a half (view_half_has_job), skipping a
 * job already listed. */
static inline void view_desc_take(view_desc_t *d, const view_desc_t *h)
{
    for (uint8_t i = 0; i < h->n_jobs; i++) {
        const key_focus_t f = (key_focus_t)h->jobs[i];
        bool listed = false;
        for (uint8_t k = 0; k < d->n_jobs; k++) listed |= d->jobs[k] == (uint8_t)f;
        if (!listed && view_half_has_job(h, f)) d->jobs[d->n_jobs++] = (uint8_t)f;
    }
    d->domains |= h->domains;
}

/* The view on screen as the keys and the furniture see it: the table's entry,
 * and for split the composition of its halves (the upper half's jobs, then
 * the lower half's). */
static inline view_desc_t view_desc_of(view_sel_t v)
{
    view_desc_t d = *view_entry(v.mode);
    if (d.pair)
        for (uint8_t h = 0; h < VIEW_HALF_COUNT; h++) view_desc_take(&d, view_entry(v.half[h]));
    return d;
}

/* The view's cursor job, or KF_COUNT when it draws no cursors. In split the
 * cursor job of the half the keys are on (job's domain), else the first. */
static inline key_focus_t view_cursor_job(view_sel_t v, key_focus_t job)
{
    const view_desc_t d = view_desc_of(v);
    key_focus_t first = KF_COUNT;
    for (uint8_t i = 0; i < d.n_jobs; i++) {
        const key_focus_t f = (key_focus_t)d.jobs[i];
        if (!key_focus_is_cursor(f)) continue;
        if (key_focus_domain(f) == key_focus_domain(job)) return f;
        if (first == KF_COUNT) first = f;
    }
    return first;
}

/* The view takes the roll source (tb.roll may be on). */
static inline bool view_takes_roll(uint8_t view_mode)
{
    return view_entry(view_mode)->roll;
}

static inline bool view_shows(view_sel_t v, view_domain_t d)
{
    return (view_desc_of(v).domains & VIEW_DOM_BIT(d)) != 0u;
}

/* The domain a key that has one meaning per domain (AUTO) acts on: the
 * job's, when the view shows it, else the one the view shows. */
static inline view_domain_t view_key_domain(view_sel_t v, key_focus_t job)
{
    const view_domain_t d = key_focus_domain(job);
    if (view_shows(v, d)) return d;
    return view_shows(v, VIEW_DOM_TIME) ? VIEW_DOM_TIME : VIEW_DOM_FREQ;
}

/* The half of split the keys are on: the half that has the job
 * (view_half_has_job); when both have it (two halves of the time record,
 * Y-T and X-Y: zoom, channel and trigger moves), the upper one; when neither
 * does, the half that shows the keys' domain. */
static inline view_half_t view_key_half(view_sel_t v, key_focus_t job)
{
    if (view_half_has_job(view_entry(v.half[VIEW_HALF_UPPER]), job)) return VIEW_HALF_UPPER;
    if (view_half_has_job(view_entry(v.half[VIEW_HALF_LOWER]), job)) return VIEW_HALF_LOWER;
    const view_domain_t d = view_key_domain(v, job);
    return (view_entry(v.half[VIEW_HALF_UPPER])->domains & VIEW_DOM_BIT(d)) ? VIEW_HALF_UPPER : VIEW_HALF_LOWER;
}

/* The domains whose measurement strip the view draws: a view's own when it
 * has a strip (view_desc_t.results); in split, those of its halves that
 * have one. A strip belongs to its domain, drawn by the view that shows it
 * with one: X-Y has none, so in split beside Y-T the time strip is Y-T's,
 * and beside the spectrum there is no time strip. */
static inline uint8_t view_strip_domains(view_sel_t v)
{
    const view_desc_t *e = view_entry(v.mode);
    if (!e->pair) return e->results != VIEW_RESULTS_COUNT ? e->domains : 0u;
    uint8_t s = 0u;
    for (uint8_t h = 0; h < VIEW_HALF_COUNT; h++) {
        const view_desc_t *x = view_entry(v.half[h]);
        if (x->results != VIEW_RESULTS_COUNT) s |= x->domains;
    }
    return s;
}

/* The domain whose measurement strip the view draws: the keys' domain when
 * the view has its strip (view_strip_domains), else VIEW_DOM_COUNT. In split
 * the keys on X-Y beside Y-T keep the time strip (it is the record's, and
 * the same whichever time half has the keys); beside the spectrum, none. */
static inline view_domain_t view_strip_domain(view_sel_t v, key_focus_t job)
{
    const view_domain_t d = view_key_domain(v, job);
    return (view_strip_domains(v) & VIEW_DOM_BIT(d)) != 0u ? d : VIEW_DOM_COUNT;
}

/* Where the band of the view's plot ends: the top of the tallest of the
 * strips it draws (view_strip_domains; top[d]: domain d's strip top as its
 * results setting and the cursors leave it), UINT16_MAX when it draws none.
 * A view of one domain ends at its strip. Split reserves the taller of its
 * halves' strips whichever half has the keys, so the panes keep their size
 * when the keys move; the keys' strip sits at the bottom of that space. Two
 * halves are never the same view, and only X-Y of the half views has no
 * strip, so split always has one. */
static inline uint16_t view_band_bot(view_sel_t v, const uint16_t top[VIEW_DOM_COUNT])
{
    const uint8_t strips = view_strip_domains(v);
    uint16_t bot = UINT16_MAX;
    for (uint8_t d = 0; d < VIEW_DOM_COUNT; d++)
        if ((strips & VIEW_DOM_BIT(d)) != 0u && top[d] < bot) bot = top[d];
    return bot;
}

/* A view of two domains marks the half the keys are on (the split view's
 * frame): the frame of rectangle (x0, y0, w, h), FOCUS_MARK_PX thick, covers
 * pixel (x, y). One pixel is enough to see on this panel and costs the
 * picture the least; the renderers draw it under the data, never over it. */
#define FOCUS_MARK_PX  1

static inline bool focus_mark_px(int x, int y, int x0, int y0, int w, int h)
{
    if (x < x0 || y < y0 || x >= x0 + w || y >= y0 + h) return false;
    return x < x0 + FOCUS_MARK_PX || x >= x0 + w - FOCUS_MARK_PX ||
           y < y0 + FOCUS_MARK_PX || y >= y0 + h - FOCUS_MARK_PX;
}

#endif /* VIEW_TABLE_H */
