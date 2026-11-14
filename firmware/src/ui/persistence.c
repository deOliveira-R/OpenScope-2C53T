#include "persistence.h"
#include "shared_mem.h"
#include <string.h>

/* Persistence buffer uses shared memory pool (64KB of 150KB available) */
static uint8_t *persist_buf = 0;
static persist_mode_t current_mode = PERSIST_OFF;
static uint32_t decay_rem;     /* persist_decay_ms: fade carried between calls */

void persist_init(void)
{
    persist_buf = shared_mem_acquire(SHMEM_OWNER_PERSISTENCE);
    memset(persist_buf, 0, PERSIST_WIDTH * PERSIST_HEIGHT);
    current_mode = PERSIST_OFF;
    decay_rem = 0;
}

void persist_deinit(void)
{
    persist_buf = 0;
    current_mode = PERSIST_OFF;
    shared_mem_release();
}

bool persist_is_initialized(void)
{
    return persist_buf != 0;
}

void persist_clear(void)
{
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

void persist_add_point(uint16_t x, uint16_t y)
{
    if (!persist_buf || x >= PERSIST_WIDTH || y >= PERSIST_HEIGHT)
        return;
    persist_buf[y * PERSIST_WIDTH + x] = 255;
}

void persist_decay(void)
{
    uint8_t decay;
    uint32_t i;
    uint32_t total = PERSIST_WIDTH * PERSIST_HEIGHT;

    switch (current_mode) {
    case PERSIST_OFF:
        /* Clear everything each frame */
        memset(persist_buf, 0, PERSIST_WIDTH * PERSIST_HEIGHT);
        return;
    case PERSIST_LOW:
        decay = 16;
        break;
    case PERSIST_MEDIUM:
        decay = 4;
        break;
    case PERSIST_HIGH:
        decay = 1;
        break;
    case PERSIST_INFINITE:
        /* No decay */
        return;
    default:
        return;
    }

    for (i = 0; i < total; i++) {
        if (persist_buf[i] > decay) {
            persist_buf[i] -= decay;
        } else {
            persist_buf[i] = 0;
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

const char *persist_mode_name(persist_mode_t mode)
{
    switch (mode) {
    case PERSIST_OFF:      return "Off";
    case PERSIST_LOW:      return "500ms";
    case PERSIST_MEDIUM:   return "1s";
    case PERSIST_HIGH:     return "2s";
    case PERSIST_INFINITE: return "Inf";
    default:               return "?";
    }
}

void persist_decay_ms(uint32_t dt_ms)
{
    const uint32_t tau = persist_mode_tau_ms(current_mode);
    if (!persist_buf) return;
    if (current_mode == PERSIST_OFF) {
        memset(persist_buf, 0, PERSIST_WIDTH * PERSIST_HEIGHT);
        return;
    }
    if (tau == 0u) return;                          /* INFINITE */

    /* 255 * dt / tau, with the remainder kept. dt is capped at one tau, which
     * already fades everything, so the product cannot overflow. */
    if (dt_ms > tau) dt_ms = tau;
    uint32_t acc = decay_rem + 255u * dt_ms;
    uint32_t dec = acc / tau;
    decay_rem = acc % tau;
    if (dec == 0u) return;

    const uint32_t total = PERSIST_WIDTH * PERSIST_HEIGHT;
    if (dec >= 255u) {
        memset(persist_buf, 0, total);
        return;
    }
    for (uint32_t i = 0; i < total; i++)
        persist_buf[i] = (persist_buf[i] > dec) ? (uint8_t)(persist_buf[i] - dec) : 0u;
}

void persist_add_column(uint16_t x, int y_prev, int y)
{
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
    if (x >= PERSIST_WIDTH || y >= PERSIST_HEIGHT) {
        return 0;
    }
    return persist_buf[y * PERSIST_WIDTH + x];
}

const uint8_t *persist_get_buffer(void)
{
    return persist_buf;
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

uint32_t persist_geometry_key(const scope_state_t *ss, uint8_t view, bool merged)
{
    uint32_t h = 2166136261u;
    h = mix_ch(h, &ss->ch1);
    h = mix_ch(h, &ss->ch2);
    h = mix(h, (uint32_t)ss->timebase_idx | ((uint32_t)ss->roll << 8) | ((uint32_t)ss->roll_step << 9) |
               ((uint32_t)merged << 17) | ((uint32_t)view << 18) | ((uint32_t)ss->true_scale << 26));
    h = mix(h, (uint32_t)ss->trigger.edge | ((uint32_t)ss->trigger.source << 4) |
               ((uint32_t)ss->soft_trigger << 8));
    h = mix(h, (uint32_t)(uint16_t)ss->trigger.level | ((uint32_t)(uint16_t)ss->trig_x << 16));
    return h;
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
