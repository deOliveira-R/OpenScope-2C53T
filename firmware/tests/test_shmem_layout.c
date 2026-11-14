/*
 * Shared pool layout (src/dsp/shared_mem.h) — RAM diet, 2026-11-11.
 *
 * The pool is now sized to exactly the FFT owner's footprint and the waterfall
 * sub-tenant starts where the FFT layout ends. Built twice by `make
 * test-shmem-layout`: once as the host (radix-2) layout and once with
 * -DUSE_CMSIS_DSP, which is what every firmware image compiles, so the
 * numbers the target actually links are pinned here, not only the host ones.
 */
#include <stdio.h>
#include <string.h>
#include "shared_mem.h"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)

int main(void)
{
#ifdef USE_CMSIS_DSP
    const char *name = "cmsis";
    CHECK(SHMEM_NEED_FFT == 81920, "CMSIS FFT layout is 80 KB");
    CHECK(shared_mem_size() == 102400, "CMSIS pool is 100 KB (was 116 KB)");
#else
    const char *name = "radix2";
    CHECK(SHMEM_NEED_FFT == 98304, "radix-2 FFT layout is 96 KB");
    CHECK(shared_mem_size() == 118784, "radix-2 pool unchanged at 116 KB");
#endif
    /* The waterfall sits directly above the FFT layout, and ends at the top
     * of the pool: no slack, no overlap. */
    CHECK(SHMEM_FFT_WATERFALL_OFFSET == SHMEM_NEED_FFT, "waterfall starts at FFT layout end");
    CHECK(SHMEM_FFT_WATERFALL_OFFSET + SHMEM_FFT_WATERFALL_SIZE == shared_mem_size(),
          "waterfall ends at the top of the pool");
    CHECK(SHMEM_NEED_PERSISTENCE <= shared_mem_size(), "persistence fits");
    CHECK(SHMEM_NEED_SCREENSHOT > shared_mem_size(), "legacy screenshot still fails closed");

    /* Whole pool is addressable and zeroed on an owner change. */
    uint8_t *p = shared_mem_acquire(SHMEM_OWNER_FFT);
    memset(p, 0xA5, shared_mem_size());
    CHECK(shared_mem_get(SHMEM_OWNER_FFT) == p, "FFT owns the pool");
    p = shared_mem_acquire(SHMEM_OWNER_PERSISTENCE);
    CHECK(p[0] == 0 && p[shared_mem_size() - 1u] == 0, "owner change zeroes the whole pool");
    CHECK(shared_mem_get(SHMEM_OWNER_FFT) == NULL, "FFT evicted");
    shared_mem_release();

    /* The waterfall gets its history while the FFT owns the pool, at the
     * firmware's real size: the whole 64 x 320 history is writable inside the
     * pool and does not touch the FFT layout below it. */
    {
        uint8_t *q = shared_mem_acquire(SHMEM_OWNER_FFT);
        uint8_t *wf = shared_mem_get(SHMEM_OWNER_FFT);
        CHECK(wf != NULL, "waterfall: the FFT-owned pool is available");
        CHECK(SHMEM_FFT_WATERFALL_OFFSET + SHMEM_FFT_WATERFALL_SIZE <= shared_mem_size(),
              "waterfall history fits the pool");
        memset(q, 0x11, SHMEM_FFT_WATERFALL_OFFSET);
        if (wf) memset(wf + SHMEM_FFT_WATERFALL_OFFSET, 0xFF, SHMEM_FFT_WATERFALL_SIZE);
        CHECK(q[SHMEM_FFT_WATERFALL_OFFSET - 1u] == 0x11, "history leaves the FFT layout alone");
        printf("  %s: pool %lu B, FFT layout 0..%lu, waterfall %u x %u at %lu..%lu\n", name,
               (unsigned long)shared_mem_size(), (unsigned long)SHMEM_NEED_FFT,
               (unsigned)SHMEM_FFT_WATERFALL_ROWS, (unsigned)SHMEM_FFT_WATERFALL_COLS,
               (unsigned long)SHMEM_FFT_WATERFALL_OFFSET,
               (unsigned long)(SHMEM_FFT_WATERFALL_OFFSET + SHMEM_FFT_WATERFALL_SIZE));
        /* X-Y / persistence take the pool: the waterfall must see it is gone
         * (its "pool not FFT's" status), not draw from another owner's data. */
        (void)shared_mem_acquire(SHMEM_OWNER_PERSISTENCE);
        CHECK(shared_mem_get(SHMEM_OWNER_FFT) == NULL, "after X-Y/persistence, no waterfall pool");
        shared_mem_release();
    }

    if (fails) { printf("test_shmem_layout(%s): %d check(s) FAILED\n", name, fails); return 1; }
    printf("test_shmem_layout(%s): all checks passed (pool %lu B)\n", name,
           (unsigned long)shared_mem_size());
    return 0;
}
