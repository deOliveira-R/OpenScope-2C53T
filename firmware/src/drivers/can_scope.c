#include "can_scope.h"
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "fpga.h"
#include "scope_timebase.h"

/* Heap-held working state (static RAM is full; see can_scope.h). */
typedef struct {
    int16_t         diff[FPGA_ADC_BUF_SIZE];
    decode_result_t res;
    can_bus_stats_t stats;
} can_scope_work_t;

static can_scope_work_t *g_w;
static SemaphoreHandle_t g_lock;
static volatile bool     g_screen;
static uint32_t          g_last_gen = 0xFFFFFFFFu;
static can_bus_report_t  g_last_rep;

float can_scope_sample_rate(uint8_t code)
{
    float fs = scope_timebase_sample_rate(code);
    if (fs > 0.0f) return fs;
    /* Measured 2026-11-10 on a 500 kbit/s bus between two STM32 FDCAN nodes
     * whose bit time was read back from their NBTP registers (16 MHz / 32):
     * the decoded bit width was 10.0 / 5.0 / 2.50 samples. */
    switch (code) {
    case 0x08: return 5.0e6f;
    case 0x09: return 2.5e6f;
    case 0x0A: return 1.25e6f;
    default:   return 0.0f;
    }
}

static bool work_get(void)
{
    if (!g_lock) {
        g_lock = xSemaphoreCreateMutex();
        if (!g_lock) return false;
    }
    if (!g_w) {
        g_w = (can_scope_work_t *)pvPortMalloc(sizeof(*g_w));
        if (!g_w) return false;
        can_bus_stats_reset(&g_w->stats);
    }
    return true;
}

/* Caller holds g_lock. */
static bool capture_locked(can_bus_report_t *rep, uint32_t *gen)
{
    const volatile uint8_t *c1 = fpga_get_ch1_buf();
    const volatile uint8_t *c2 = fpga_get_ch2_buf();
    if (!c1 || !c2) return false;
    for (uint8_t tries = 0; tries < 8; tries++) {
        uint32_t g0 = fpga_acq_frame_generation();
        if (g0 & 1u) { vTaskDelay(pdMS_TO_TICKS(1)); continue; }   /* mid-commit */
        bool rotated = !fpga_acq_record_time_ordered();
        float fs = can_scope_sample_rate(fpga_acq_rate_idx_get());
        can_bus_analyse(c1, c2, FPGA_ADC_BUF_SIZE, fs, rotated ? 1u : 0u,
                        g_w->diff, rep, &g_w->res);
        if (fpga_acq_frame_generation() == g0) {     /* one capture on both channels */
            if (gen) *gen = g0;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

bool can_scope_capture(can_bus_report_t *rep, const decode_result_t **res, uint32_t *gen)
{
    if (!work_get()) return false;
    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    bool ok = capture_locked(rep, gen);
    if (ok && res) *res = &g_w->res;
    xSemaphoreGive(g_lock);
    return ok;
}

can_bus_stats_t *can_scope_stats(void)
{
    return work_get() ? &g_w->stats : NULL;
}

void can_scope_screen_set(bool on)
{
    if (on) {
        if (work_get()) {
            can_bus_stats_reset(&g_w->stats);
            g_last_gen = 0xFFFFFFFFu;
            memset(&g_last_rep, 0, sizeof(g_last_rep));
            g_screen = true;
        }
        return;
    }
    g_screen = false;
    if (g_lock && xSemaphoreTake(g_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (g_w) { vPortFree(g_w); g_w = NULL; }
        xSemaphoreGive(g_lock);
    }
}

bool can_scope_screen_on(void)
{
    return g_screen;
}

int can_scope_screen_line(char *out, uint32_t len)
{
    if (!g_screen || !work_get()) {
        strncpy(out, "CAN: no memory", len);
        out[len - 1] = '\0';
        return 0;
    }
    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        strncpy(out, "CAN: busy", len);
        out[len - 1] = '\0';
        return 0;
    }
    uint32_t gen = fpga_acq_frame_generation();
    if (!(gen & 1u) && gen != g_last_gen) {
        can_bus_report_t rep;
        uint32_t g = 0;
        if (capture_locked(&rep, &g) && g != g_last_gen) {
            can_bus_stats_add(&g_w->stats, &rep, &g_w->res);
            g_last_rep = rep;
            g_last_gen = g;
        }
    }
    int ok = can_bus_summary(&g_w->stats, &g_last_rep, out, len);
    xSemaphoreGive(g_lock);
    return ok;
}
