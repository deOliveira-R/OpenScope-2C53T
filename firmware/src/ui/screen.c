/*
 * OpenScope 2C53T - screen core. See screen.h. Pure: no hardware headers.
 *
 * The stack arrays are the only shared state and are touched under the
 * injected lock. Callbacks run with the lock released and are looked up again
 * by id afterwards, so a callback may push, pop or switch.
 */

#include "screen.h"
#include <stdio.h>
#include <string.h>

static const screen_t *g_reg[SCR_COUNT];
static screen_id_t     g_st[SCR_STACK_MAX];
static uint8_t         g_n;
static uint32_t        g_gen;
static screen_hooks_t  g_hk;

static void lock(void)   { if (g_hk.lock)   g_hk.lock(); }
static void unlock(void) { if (g_hk.unlock) g_hk.unlock(); }
static void changed(void) { if (g_hk.stack_changed) g_hk.stack_changed(); }

const screen_t *screen_get(screen_id_t id)
{
    return ((unsigned)id < (unsigned)SCR_COUNT) ? g_reg[id] : NULL;
}

static bool is_base(const screen_t *s) { return s && s->mode != MODE_COUNT; }

bool screen_register(const screen_t *s)
{
    if (!s || (unsigned)s->id >= (unsigned)SCR_COUNT || g_reg[s->id]) return false;
    g_reg[s->id] = s;
    return true;
}

void screen_set_hooks(const screen_hooks_t *h)
{
    memset(&g_hk, 0, sizeof g_hk);
    if (h) g_hk = *h;
}

void screen_reset(void)
{
    memset(g_reg, 0, sizeof g_reg);
    memset(g_st, 0, sizeof g_st);
    memset(&g_hk, 0, sizeof g_hk);
    g_n = 0;
    g_gen = 0;
}

/* A copy of the stack taken under the lock. */
static uint8_t copy_stack(screen_id_t out[SCR_STACK_MAX])
{
    lock();
    uint8_t n = g_n;
    memcpy(out, g_st, sizeof g_st);
    unlock();
    return n;
}

bool screen_init(screen_id_t base)
{
    const screen_t *s = screen_get(base);
    if (!is_base(s)) return false;
    lock();
    g_st[0] = base;
    g_n = 1;
    g_gen++;
    unlock();
    if (s->open) s->open(SCR_COUNT);
    changed();
    return true;
}

screen_id_t screen_top(void)
{
    screen_id_t st[SCR_STACK_MAX];
    uint8_t n = copy_stack(st);
    return n ? st[n - 1] : SCR_COUNT;
}

screen_id_t screen_base(void)
{
    screen_id_t st[SCR_STACK_MAX];
    uint8_t n = copy_stack(st);
    return n ? st[0] : SCR_COUNT;
}

screen_stack_t screen_snapshot(void)
{
    screen_stack_t s;
    memset(&s, 0, sizeof s);
    lock();
    s.n = g_n;
    memcpy(s.id, g_st, sizeof s.id);
    s.gen = g_gen;
    unlock();
    return s;
}

/* Pop the top layer; the caller posts the change. */
static bool pop_one(void)
{
    lock();
    if (g_n <= 1) { unlock(); return false; }
    screen_id_t gone = g_st[g_n - 1];
    screen_id_t below = g_st[g_n - 2];
    g_n--;
    g_gen++;
    unlock();
    const screen_t *s = screen_get(gone);
    if (s && s->close) s->close(below);
    return true;
}

void screen_pop(void)
{
    if (pop_one()) changed();
}

static bool is_open_above_base(screen_id_t id)
{
    lock();
    bool found = false;
    for (uint8_t i = 1; i < g_n; i++) if (g_st[i] == id) found = true;
    unlock();
    return found;
}

bool screen_close(screen_id_t id)
{
    if (!is_open_above_base(id)) return false;
    /* Whatever sits above a layer cannot outlive it: pop down through it. */
    while (is_open_above_base(id) && pop_one()) { }
    changed();
    return true;
}

bool screen_push(screen_id_t layer)
{
    const screen_t *s = screen_get(layer);
    if (!s || is_base(s)) return false;
    lock();
    bool ok = g_n >= 1 && g_n < SCR_STACK_MAX;
    for (uint8_t i = 0; ok && i < g_n; i++) if (g_st[i] == layer) ok = false;
    screen_id_t from = ok ? g_st[g_n - 1] : SCR_COUNT;
    if (ok) { g_st[g_n++] = layer; g_gen++; }
    unlock();
    if (!ok) return false;
    if (s->open) s->open(from);
    changed();
    return true;
}

void screen_switch(screen_id_t base)
{
    const screen_t *nb = screen_get(base);
    if (!is_base(nb)) return;
    screen_id_t cur = screen_base();
    if (cur == SCR_COUNT) { screen_init(base); return; }

    while (pop_one()) { }                         /* overlays and children, top-down */
    if (cur != base) {
        const screen_t *ob = screen_get(cur);
        if (ob && ob->close) ob->close(base);
        lock();
        g_st[0] = base;
        g_n = 1;
        g_gen++;
        unlock();
        if (nb->open) nb->open(cur);
    }
    changed();
}

/* ---- router ------------------------------------------------------------ */

uint8_t screen_route_key(button_id_t raw)
{
    uint8_t r = (uint8_t)raw;
    scr_key_t k = { (button_id_t)(r & (uint8_t)~BTN_LONG_FLAG), (r & BTN_LONG_FLAG) != 0 };
    scr_res_t res = SCR_UNHANDLED;

    /* 1. BTN_NONE (the model wake) touches nothing. */
    if (k.btn == BTN_NONE) return SCR_UNHANDLED;
    if (g_hk.touch) g_hk.touch();
    if (g_hk.click) g_hk.click();

    screen_id_t st[SCR_STACK_MAX];
    uint8_t n = copy_stack(st);

    /* 2. A grabbing top layer takes everything but POWER. */
    bool grabbed = false;
    if (n > 1 && k.btn != BTN_POWER) {
        const screen_t *t = screen_get(st[n - 1]);
        if (t && (t->flags & SCR_F_GRAB)) {
            grabbed = true;
            res = t->key ? t->key(k) : SCR_UNHANDLED;
            if (res == SCR_CLOSE) { screen_close(st[n - 1]); res = SCR_DRAW_FULL; }
            if (res != SCR_UNHANDLED) goto done;
        }
    }

    /* 3. Self-cal lock (not for POWER, not after a grab left the key). */
    if (!grabbed && k.btn != BTN_POWER && g_hk.locked && g_hk.locked(k.btn, k.is_long)) {
        res = SCR_HANDLED;
        goto done;
    }

    /* 4. Globals. */
    if (g_hk.globals && g_hk.globals(k.btn, k.is_long)) {
        res = SCR_HANDLED;
        goto done;
    }

    /* 5. Layers, top down, unless the grab layer already had its turn. A
     * layer that leaves the key passes it down; an OPAQUE or GRAB layer ends
     * the walk (grab layers only ever get keys in step 2). */
    if (!grabbed) {
        for (int i = (int)n - 1; i >= 0; i--) {
            const screen_t *s = screen_get(st[i]);
            if (!s) continue;
            if (s->flags & SCR_F_GRAB) break;
            res = s->key ? s->key(k) : SCR_UNHANDLED;
            if (res == SCR_CLOSE) {
                if (i > 0) screen_close(st[i]);
                res = (i > 0) ? SCR_DRAW_FULL : SCR_HANDLED;   /* a base cannot close */
            }
            if (res != SCR_UNHANDLED) break;
            if (s->flags & SCR_F_OPAQUE) break;
        }
    }

done:
    /* 6. Unhandled by everyone: nothing. 7. Handled: the settings service. */
    if (res != SCR_UNHANDLED && g_hk.after_key) g_hk.after_key(res);
    return (uint8_t)res;
}

/* ---- lookups ----------------------------------------------------------- */

screen_id_t screen_base_for_mode(device_mode_t m)
{
    for (int i = 0; i < SCR_COUNT; i++)
        if (g_reg[i] && is_base(g_reg[i]) && g_reg[i]->mode == m) return (screen_id_t)i;
    return SCR_COUNT;
}

screen_id_t screen_base_for_instrument(uint8_t instrument)
{
    if (instrument == SCR_INSTRUMENT_NONE) return SCR_COUNT;
    for (int i = 0; i < SCR_COUNT; i++)
        if (g_reg[i] && is_base(g_reg[i]) && g_reg[i]->instrument == instrument) return (screen_id_t)i;
    return SCR_COUNT;
}

size_t screen_snapshot_text(char *out, size_t n)
{
    if (!out || n == 0) return 0;
    screen_stack_t s = screen_snapshot();
    size_t o = 0;
    out[0] = '\0';
    for (uint8_t i = 0; i < s.n; i++) {
        const screen_t *sc = screen_get(s.id[i]);
        int w = snprintf(out + o, n - o, "%s%s", i ? " > " : "", sc && sc->name ? sc->name : "?");
        if (w < 0 || (size_t)w >= n - o) return strlen(out);
        o += (size_t)w;
    }
    int w = snprintf(out + o, n - o, " gen=%lu", (unsigned long)s.gen);
    if (w < 0 || (size_t)w >= n - o) return strlen(out);
    o += (size_t)w;
    const screen_t *top = s.n ? screen_get(s.id[s.n - 1]) : NULL;
    if (top && top->describe && n - o > 4) {
        char d[96];
        d[0] = '\0';
        top->describe(d, sizeof d);
        if (d[0]) {
            w = snprintf(out + o, n - o, " | %s", d);
            if (w < 0 || (size_t)w >= n - o) return strlen(out);
            o += (size_t)w;
        }
    }
    return o;
}

/* ---- display side ------------------------------------------------------ */

/* Index of the lowest visible layer: the highest OPAQUE one, else the base. */
static uint8_t visible_from(const screen_id_t *st, uint8_t n)
{
    for (int i = (int)n - 1; i >= 1; i--) {
        const screen_t *s = screen_get(st[i]);
        if (s && (s->flags & SCR_F_OPAQUE)) return (uint8_t)i;
    }
    return 0;
}

void screen_draw(scr_paint_t how, uint32_t frame)
{
    screen_id_t st[SCR_STACK_MAX];
    uint8_t n = copy_stack(st);
    if (!n) return;
    uint8_t from = (how == SCR_PAINT_FULL) ? visible_from(st, n) : (uint8_t)(n - 1);
    for (uint8_t i = from; i < n; i++) {
        const screen_t *s = screen_get(st[i]);
        if (s && s->draw) s->draw(how, frame);
    }
}

redraw_action_t screen_tick(uint32_t frame)
{
    screen_id_t st[SCR_STACK_MAX];
    uint8_t n = copy_stack(st);
    redraw_action_t r = REDRAW_SKIP;
    if (!n) return r;
    for (uint8_t i = visible_from(st, n); i < n; i++) {
        const screen_t *s = screen_get(st[i]);
        if (s && s->tick) {
            redraw_action_t a = s->tick(frame);
            if (a > r) r = a;
        }
    }
    return r;
}

bool screen_popups_allowed(void)
{
    screen_id_t st[SCR_STACK_MAX];
    uint8_t n = copy_stack(st);
    return visible_from(st, n) == 0;
}

bool screen_bars_wanted(void)
{
    screen_id_t st[SCR_STACK_MAX];
    uint8_t n = copy_stack(st);
    for (uint8_t i = visible_from(st, n); i < n; i++) {
        const screen_t *s = screen_get(st[i]);
        if (s && (s->flags & SCR_F_NO_BARS)) return false;
    }
    return true;
}

uint32_t screen_epoch(void)
{
    screen_id_t st[SCR_STACK_MAX];
    uint8_t n = copy_stack(st);
    uint32_t h = 0x811C9DC5u;
    for (uint8_t i = visible_from(st, n); i < n; i++) {
        const screen_t *s = screen_get(st[i]);
        h = redraw_epoch_mix(h, (uint32_t)st[i]);
        if (s && s->epoch) h = redraw_epoch_mix(h, s->epoch());
    }
    return h;
}

uint16_t screen_groups(void)
{
    screen_id_t st[SCR_STACK_MAX];
    uint8_t n = copy_stack(st);
    uint16_t g = 0;
    for (uint8_t i = visible_from(st, n); i < n; i++) {
        const screen_t *s = screen_get(st[i]);
        if (s) g |= s->groups;
    }
    return g;
}
