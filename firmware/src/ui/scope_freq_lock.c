/*
 * Firmware-side lock for scope_freq's shared FFT scratch (fft_re/fft_im).
 *
 * The display task and the shell task both call scope_freq_estimate(); without
 * this a shell `scope freq` mid-frame corrupts the badge's transform (and vice
 * versa). The mutex is ~80 B of FreeRTOS heap (configSUPPORT_STATIC_ALLOCATION is 0). Kept out
 * of scope_freq.c so that file stays host-buildable.
 */

#include "scope_freq.h"
#include "FreeRTOS.h"
#include "semphr.h"

static SemaphoreHandle_t s_mutex;

static void lock(void)   { if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock(void) { if (s_mutex) xSemaphoreGive(s_mutex); }

/* Before the scheduler starts, from main(). If the heap cannot supply the
 * mutex the estimator runs unlocked, as before: a glitch, not a fault. */
void scope_freq_lock_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex != NULL) scope_freq_set_lock(lock, unlock);
}
