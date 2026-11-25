#include "persistence.h"
#include "shared_mem.h"
#include <string.h>

/* The buffer is the shared pool while persistence owns it, and nothing
 * otherwise: another owner (the FFT of a spectrum view or of split) taking
 * the pool takes the buffer with it, so no reader keeps a pointer into
 * memory that is no longer persistence's. */
static uint8_t *persist_mem(void)
{
    return shared_mem_get(SHMEM_OWNER_PERSISTENCE);
}
static persist_mode_t current_mode = PERSIST_OFF;
static uint32_t decay_rem;     /* persist_decay_ms: fade carried between calls */

void persist_init(void)
{
    memset(shared_mem_acquire(SHMEM_OWNER_PERSISTENCE), 0, PERSIST_WIDTH * PERSIST_HEIGHT);
    current_mode = PERSIST_OFF;
    decay_rem = 0;
}

void persist_deinit(void)
{
    current_mode = PERSIST_OFF;
    if (persist_mem()) shared_mem_release();       /* only the pool it holds */
}

bool persist_is_initialized(void)
{
    return persist_mem() != 0;
}

void persist_clear(void)
{
    uint8_t *const persist_buf = persist_mem();
    if (persist_buf)
        memset(persist_buf, 0, PERSIST_WIDTH * PERSIST_HEIGHT);
}

void persist_set_mode(persist_mode_t mode)
{
    if (mode < PERSIST_COUNT) {
        if (mode != current_mode) decay_rem = 0;
        current_mode = mode;
    }
}

persist_mode_t persist_get_mode(void)
{
    return current_mode;
}

void persist_add_trace(const uint16_t *y_values, uint8_t color_index)
{
    uint16_t x, y;
    (void)color_index; /* Color is applied at render time, not stored in buffer */
    uint8_t *const persist_buf = persist_mem();
    if (!persist_buf) return;

    for (x = 0; x < PERSIST_WIDTH; x++) {
        y = y_values[x];
        if (y >= PERSIST_HEIGHT) {
            continue;
        }

        /* Full intensity at the trace point */
        persist_buf[y * PERSIST_WIDTH + x] = 255;

        /* Anti-aliased thickness: neighboring rows at reduced intensity */
        if (y > 0) {
            uint8_t *p = &persist_buf[(y - 1) * PERSIST_WIDTH + x];
            if (*p < 192) {
                *p = 192;
            }
        }
        if (y + 1 < PERSIST_HEIGHT) {
            uint8_t *p = &persist_buf[(y + 1) * PERSIST_WIDTH + x];
            if (*p < 192) {
                *p = 192;
            }
        }
    }
}

uint32_t persist_mode_tau_ms(persist_mode_t mode)
{
    switch (mode) {
    case PERSIST_LOW:    return 500u;
    case PERSIST_MEDIUM: return 1000u;
    case PERSIST_HIGH:   return 2000u;
    default:             return 0u;
    }
}

void persist_fade(uint8_t *buf, uint32_t n, persist_mode_t mode, uint32_t dt_ms, uint32_t *rem)
{
    const uint32_t tau_ms = persist_mode_tau_ms(mode);
    if (mode == PERSIST_OFF) {
        memset(buf, 0, n);
        return;
    }
    if (tau_ms == 0u) return;                       /* INFINITE */

    /* 255 * dt / tau, with the remainder kept. dt is capped at one tau, which
     * already fades everything, so the product cannot overflow. */
    if (dt_ms > tau_ms) dt_ms = tau_ms;
    const uint32_t acc = *rem + 255u * dt_ms;
    const uint32_t dec = acc / tau_ms;
    *rem = acc % tau_ms;
    if (dec == 0u) return;
    if (dec >= 255u) {
        memset(buf, 0, n);
        return;
    }
    for (uint32_t i = 0; i < n; i++)
        buf[i] = (buf[i] > dec) ? (uint8_t)(buf[i] - dec) : 0u;
}

void persist_decay_ms(uint32_t dt_ms)
{
    uint8_t *const persist_buf = persist_mem();
    if (!persist_buf) return;
    persist_fade(persist_buf, PERSIST_WIDTH * PERSIST_HEIGHT, current_mode, dt_ms, &decay_rem);
}

void persist_add_column(uint16_t x, int y_prev, int y)
{
    uint8_t *const persist_buf = persist_mem();
    if (!persist_buf || x >= PERSIST_WIDTH) return;
    if (y < 0 || y >= PERSIST_HEIGHT) return;
    int lo = y, hi = y;
    if (y_prev >= 0 && y_prev < PERSIST_HEIGHT) {
        if (y_prev < lo) lo = y_prev;
        if (y_prev > hi) hi = y_prev;
    }
    for (int v = lo; v <= hi; v++)
        persist_buf[v * PERSIST_WIDTH + x] = 255;
}

void persist_add_rows(const int16_t *rows, uint16_t n, int y_origin)
{
    if (n > PERSIST_WIDTH) n = PERSIST_WIDTH;
    int prev = -1;
    for (uint16_t x = 0; x < n; x++) {
        int y = (int)rows[x] - y_origin;
        persist_add_column(x, prev, y);
        prev = (y >= 0 && y < PERSIST_HEIGHT) ? y : -1;
    }
}

uint16_t persist_scale_rgb565(uint16_t color, uint8_t intensity)
{
    uint32_t r = (color >> 11) & 0x1Fu;
    uint32_t g = (color >> 5) & 0x3Fu;
    uint32_t b = color & 0x1Fu;
    r = r * intensity / 255u;
    g = g * intensity / 255u;
    b = b * intensity / 255u;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

uint8_t persist_get_intensity(uint16_t x, uint16_t y)
{
    const uint8_t *const persist_buf = persist_mem();
    if (!persist_buf || x >= PERSIST_WIDTH || y >= PERSIST_HEIGHT) {
        return 0;
    }
    return persist_buf[y * PERSIST_WIDTH + x];
}

uint8_t *persist_get_buffer(void)
{
    return persist_mem();
}

uint16_t persist_intensity_to_color_ch1(uint8_t intensity)
{
    if (intensity == 0) {
        return 0x0000; /* Black */
    }
    /* Dim yellow to bright yellow */
    uint8_t r = intensity >> 3;  /* 0-31 */
    uint8_t g = intensity >> 2;  /* 0-63 */
    return (uint16_t)(r << 11) | (uint16_t)(g << 5); /* RGB565 yellow */
}

uint16_t persist_intensity_to_color_ch2(uint8_t intensity)
{
    if (intensity == 0) {
        return 0x0000; /* Black */
    }
    /* Dim cyan to bright cyan */
    uint8_t g = intensity >> 2;  /* 0-63 */
    uint8_t b = intensity >> 3;  /* 0-31 */
    return (uint16_t)(g << 5) | b; /* RGB565 cyan */
}

/* ── Geometry guard (persistence.h) ─────────────────────────────────── */

static uint32_t mix(uint32_t h, uint32_t v)
{
    for (int i = 0; i < 4; i++) { h ^= (v >> (8 * i)) & 0xFFu; h *= 16777619u; }
    return h;
}

static uint32_t mix_ch(uint32_t h, const channel_state_t *c)
{
    h = mix(h, (uint32_t)c->enabled | ((uint32_t)c->vdiv_idx << 1) | ((uint32_t)c->coupling << 9) |
               ((uint32_t)c->probe << 13));
    return mix(h, (uint32_t)(uint16_t)c->position);
}

uint32_t persist_geometry_key(const scope_state_t *ss, uint8_t view, bool merged, uint16_t top,
                              uint16_t band_bot)
{
    uint32_t h = 2166136261u;
    h = mix_ch(h, &ss->ch1);
    h = mix_ch(h, &ss->ch2);
    h = mix(h, (uint32_t)ss->timebase_idx | ((uint32_t)ss->roll << 8) | ((uint32_t)ss->roll_step << 9) |
               ((uint32_t)merged << 17) | ((uint32_t)view << 18) | ((uint32_t)ss->true_scale << 26));
    h = mix(h, (uint32_t)ss->trigger.edge | ((uint32_t)ss->trigger.source << 4) |
               ((uint32_t)ss->soft_trigger << 8));
    h = mix(h, (uint32_t)(uint16_t)ss->trigger.level | ((uint32_t)(uint16_t)ss->trig_x << 16));
    h = mix(h, top);                /* the pane: the buffer's rows start at its top */
    return mix(h, band_bot);        /* a fitted trace is stretched to the strip's top */
}

/* The key the buffer was filled under, the one acquisition stopped with,
 * and whether it was running: 12 bytes. */
static uint32_t g_geom_key, g_geom_stop_key;
static bool     g_geom_seen, g_geom_running;

bool persist_follow_geometry(uint32_t key, bool running)
{
    bool clear = g_geom_seen && key != g_geom_key;
    if (g_geom_seen && running && !g_geom_running && key != g_geom_stop_key) clear = true;
    if (g_geom_seen && !running && g_geom_running) g_geom_stop_key = key;
    if (!g_geom_seen) g_geom_stop_key = key;
    g_geom_key = key;
    g_geom_running = running;
    g_geom_seen = true;
    if (clear) persist_clear();
    return clear;
}
