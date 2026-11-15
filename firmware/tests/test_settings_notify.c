/*
 * Host test: a settings save is triggered by a changed persisted property,
 * not by "a key was pressed" (ARCH-1 B8).
 *
 * Until B8 the input task captured the whole settings image after every key
 * (settings_store_note_change) to find out whether anything had changed.
 * Now the model says so: a commit of a persisted property notes the change
 * (model.c may_note -> model_hw_ops_t.note_change -> settings_store_
 * note_change), and after a key the input task only gives the store its
 * turn (settings_store_service: the settle window, unchanged).
 *
 * Here: the real settings store and record codec (settings_store.c,
 * config.c, model_persist_v5.c) on a NOR flash model, the real property
 * model, and every real screen on the real router (fw_screens_env.h). The
 * model's note is the store's, the router's after-key hook is the store's
 * service, and nothing else looks at the settings.
 *
 *   1. Every key, short and long, on every screen and page: if the key
 *      changed the record image, the change is written once the settle
 *      window has passed and the record read back from flash is the live
 *      image, byte for byte; if it changed nothing, nothing is written.
 *   2. The write policy is the one it was: random key sequences with random
 *      pauses, run under the old policy (capture after every key) and the
 *      new one, write the same number of records and leave the same bytes
 *      in flash; a held arrow still costs one record.
 *   3. The boundaries still capture everything: a flush (mode change, the
 *      shell) and the power-off flush.
 *
 * Build: make test-settings-notify.
 */
#define SCREEN_TEST_ENV_REAL_FOCUS
#include "screen_test_env.h"
#include "fw_screens_env.h"

#include <stdlib.h>
#include "config.h"
#include "settings_store.h"
#include "flash_regions.h"
#include "startup_policy.h"

/* ── a NOR flash (erase sets 0xFF, a program only clears bits) ─────── */
static uint8_t *nor;
static int nor_read(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
    (void)ctx;
    if (addr >= FLASH_REGION_CHIP_SIZE || len > FLASH_REGION_CHIP_SIZE - addr) exit(3);
    memcpy(buf, nor + addr, len);
    return 0;
}
static int nor_erase(void *ctx, uint32_t addr)
{
    (void)ctx;
    if (addr % FLASH_REGION_SECTOR_SIZE || addr >= FLASH_REGION_CHIP_SIZE) exit(3);
    memset(nor + addr, 0xFF, FLASH_REGION_SECTOR_SIZE);
    return 0;
}
static int nor_program(void *ctx, uint32_t addr, const void *data, uint32_t len)
{
    (void)ctx;
    if (addr >= FLASH_REGION_CHIP_SIZE || len > FLASH_REGION_CHIP_SIZE - addr) exit(3);
    for (uint32_t i = 0; i < len; i++) nor[addr + i] &= ((const uint8_t *)data)[i];
    return 0;
}
static const flash_region_backend_t nor_backend = {
    .read = nor_read, .erase_sector = nor_erase, .program = nor_program, .ctx = NULL,
};
/* The device binding, replaced for the host build (settings_store_init). */
flash_region_status_t flash_regions_bind_w25q(void) { return flash_regions_init(&nor_backend); }

/* ── the input task's two hooks ────────────────────────────────────── */
static uint32_t now_ms;
static bool     old_policy;          /* the pre-B8 after-key: capture, then service */
static unsigned model_notes;
static void store_note(void)    { model_notes++; settings_store_note_change(now_ms); }
static void store_service(void)
{
    if (old_policy) settings_store_note_change(now_ms);
    (void)settings_store_service(now_ms);
}

static uint32_t writes(void) { return settings_store_get_status()->writes; }

/* A blank chip and a store started on it. */
static void power_up_blank(void)
{
    if (!nor) nor = malloc(FLASH_REGION_CHIP_SIZE);
    memset(nor, 0xFF, FLASH_REGION_CHIP_SIZE);
    now_ms = 1000;
    fw_note_hook = store_note;
    fw_service_hook = store_service;
    env_fresh(MODE_OSCILLOSCOPE);
    settings_store_init();
}

/* State `st` (and overlay) with the record in flash equal to the live
 * state: whatever getting there changed is written now. */
static void clean_state(fw_state_t st, fw_over_t ov)
{
    fw_boot(st, ov, false);
    (void)settings_store_flush(now_ms);
    model_notes = 0;
}

/* The newest record in flash holds the live state: the live capture laid
 * over the loaded record (the bytes no property owns are carried) changes
 * nothing. */
static bool flash_is_live(void)
{
    device_config_t loaded, live;
    memset(&loaded, 0, sizeof loaded);
    if (!config_load(&loaded)) return false;
    live = loaded;
    settings_store_capture(&live);
    return memcmp(&live, &loaded, sizeof loaded) == 0;
}

/* ── 1. every key on every screen ──────────────────────────────────── */
static void test_every_key(void)
{
    printf("every key on every screen: a changed record is written, an unchanged one is not\n");
    power_up_blank();
    int cases = 0, changed = 0, bad = 0, silent = 0;
    char why[200] = "";
    for (int st = 0; st < ST_COUNT; st++)
    for (int ov = 0; ov < OV_COUNT; ov++)
    for (int b = BTN_CH1; b <= BTN_OK; b++)             /* POWER: the power-off flush, below */
    for (int lng = 0; lng < 2; lng++)
    for (int rep = 0; rep < 3; rep++) {                 /* the key once, twice, three times */
        const uint8_t raw = (uint8_t)(b | (lng ? BTN_LONG_FLAG : 0));
        clean_state((fw_state_t)st, (fw_over_t)ov);
        device_config_t before, after;
        memset(&before, 0, sizeof before); memset(&after, 0, sizeof after);
        settings_store_capture(&before);
        const uint32_t w0 = writes();
        for (int i = 0; i <= rep; i++) { now_ms += 150; fw_key(raw); }
        settings_store_capture(&after);
        const bool moved = memcmp(&before, &after, sizeof after) != 0;
        const bool wrote_early = writes() != w0;
        now_ms += SETTINGS_STORE_SETTLE_MS + 1;
        (void)settings_store_service(now_ms);           /* the next key's turn, the window over */
        const bool wrote = writes() != w0;
        cases++;
        const char *err = NULL;
        if (wrote_early) err = "written inside the settle window";
        else if (moved) {
            changed++;
            if (model_notes == 0) { err = "the record changed and the model did not say so"; silent++; }
            else if (!wrote) err = "the change was not written";
            else if (writes() != w0 + 1) err = "more than one record";
            else if (!flash_is_live()) err = "the record in flash is not the live image";
        } else if (wrote) err = "a key that changed nothing wrote a record";
        if (err) {
            if (bad < 12) printf("    %s, overlay %d, key %u x%d: %s\n", st_name[st], ov, raw, rep + 1, err);
            if (!bad) snprintf(why, sizeof why, "%s", err);
            bad++;
        }
    }
    printf("    %d cases, %d changed the record, %d of them with no note from the model\n", cases, changed, silent);
    CHECK("every persisted change reaches the record; a key that changes nothing writes nothing", bad == 0);
    CHECK("... and the keys did change the record (72 of them do)", changed >= 60);
}

/* The same, deep into the screens: random walks of every key from each
 * state (into every pane of the Oscilloscope Settings page, the cursors,
 * the meter's views, the generator's editor ...), each key judged alone:
 * the window is let pass after every key. */
static void test_walks(void)
{
    printf("random walks through every screen: each key judged alone\n");
    power_up_blank();
    int keys = 0, changed = 0, bad = 0, silent = 0;
    uint32_t lcg = 2026u;
    for (int walk = 0; walk < 44; walk++) {
        clean_state((fw_state_t)(walk % ST_COUNT), OV_NONE);
        for (int i = 0; i < 500; i++) {
            lcg = lcg * 1664525u + 1013904223u;
            const int b = BTN_CH1 + (int)((lcg >> 16) % (BTN_OK - BTN_CH1 + 1));
            lcg = lcg * 1664525u + 1013904223u;
            const bool lng = ((lcg >> 16) % 6u) == 0u && b != BTN_UP && b != BTN_DOWN && b != BTN_LEFT && b != BTN_RIGHT;
            const uint8_t raw = (uint8_t)(b | (lng ? BTN_LONG_FLAG : 0));
            device_config_t before, after;
            memset(&before, 0, sizeof before); memset(&after, 0, sizeof after);
            settings_store_capture(&before);
            const uint32_t w0 = writes();
            const unsigned n0 = model_notes;
            const char *screen = screen_get(screen_top())->name;
            now_ms += 150;
            fw_key(raw);
            settings_store_capture(&after);
            const bool moved = memcmp(&before, &after, sizeof after) != 0;
            now_ms += SETTINGS_STORE_SETTLE_MS + 1;
            (void)settings_store_service(now_ms);
            const bool wrote = writes() != w0;
            keys++;
            const char *err = NULL;
            if (moved) {
                changed++;
                if (model_notes == n0) { err = "the record changed and the model did not say so"; silent++; }
                else if (!wrote || writes() != w0 + 1) err = "the change was not written as one record";
                else if (!flash_is_live()) err = "the record in flash is not the live image";
            } else if (wrote) err = "a key that changed nothing wrote a record";
            if (err) {
                if (bad < 12) {
                    const uint8_t *a = (const uint8_t *)&before, *c = (const uint8_t *)&after;
                    unsigned at = 0;
                    while (at < sizeof before && a[at] == c[at]) at++;
                    printf("    walk %d key #%d (%u) on %s: %s (first byte %u)\n", walk, i, raw, screen, err, at);
                }
                bad++;
                (void)settings_store_flush(now_ms);     /* start the next key clean */
            }
        }
    }
    printf("    %d keys, %d changed the record, %d of them with no note from the model\n", keys, changed, silent);
    CHECK("22000 keys: every persisted change reaches the record; a key that changes nothing writes nothing", bad == 0);
    CHECK("... and a good share of them changed the record", changed > 2000);
}

/* ── 2. the write policy is unchanged ──────────────────────────────── */
typedef struct { uint32_t writes; device_config_t flash; bool loaded; } run_t;

static run_t run_sequence(uint32_t seed, bool old, int nkeys)
{
    static const uint8_t pool[] = {
        BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_RIGHT, BTN_OK, BTN_MENU, BTN_CH1, BTN_CH2, BTN_MOVE, BTN_SELECT,
        BTN_TRIGGER, BTN_PRM, BTN_AUTO, LONG(BTN_CH1), LONG(BTN_CH2), LONG(BTN_PRM), LONG(BTN_OK), LONG(BTN_AUTO),
        LONG(BTN_MENU), LONG(BTN_MOVE), LONG(BTN_SELECT), LONG(BTN_TRIGGER), BTN_UP, BTN_DOWN, BTN_RIGHT, BTN_OK };
    power_up_blank();
    old_policy = old;
    fw_boot(ST_SCOPE, OV_NONE, false);
    const uint32_t w0 = writes();
    uint32_t lcg = seed;
    for (int i = 0; i < nkeys; i++) {
        lcg = lcg * 1664525u + 1013904223u;
        const uint8_t k = pool[(lcg >> 16) % sizeof pool];
        lcg = lcg * 1664525u + 1013904223u;
        /* mostly quick presses, now and then a pause longer than the window */
        const uint32_t gap = ((lcg >> 20) % 10u == 0u) ? 2500u + (lcg >> 8) % 3000u : 80u + (lcg >> 8) % 700u;
        now_ms += gap;
        fw_key(k);
    }
    now_ms += SETTINGS_STORE_SETTLE_MS + 1;
    (void)settings_store_service(now_ms);
    run_t r;
    memset(&r, 0, sizeof r);
    r.writes = writes() - w0;
    r.loaded = config_load(&r.flash);
    old_policy = false;
    return r;
}

static void test_write_policy(void)
{
    printf("the write policy: as it was\n");
    int ok = 1; uint32_t total = 0;
    {
        /* the harness itself: the same sequence twice under one policy is the same run */
        const run_t a = run_sequence(4242u, false, 600), b = run_sequence(4242u, false, 600);
        CHECK("(two runs of one sequence under one policy are identical: the comparison below means something)",
              a.writes == b.writes && a.loaded && b.loaded && memcmp(&a.flash, &b.flash, sizeof a.flash) == 0);
    }
    for (uint32_t seed = 1; seed <= 12; seed++) {
        const run_t o = run_sequence(seed * 7919u, true, 600);
        const uint32_t ow = o.writes;
        const run_t n = run_sequence(seed * 7919u, false, 600);
        if (o.loaded != n.loaded || ow != n.writes || memcmp(&o.flash, &n.flash, sizeof o.flash) != 0) {
            ok = 0;
            printf("    seed %u: capture-per-key wrote %u records, notify %u; same bytes %d\n", (unsigned)seed,
                   (unsigned)ow, (unsigned)n.writes, memcmp(&o.flash, &n.flash, sizeof o.flash) == 0);
            const uint8_t *a = (const uint8_t *)&o.flash, *b = (const uint8_t *)&n.flash;
            for (unsigned i = 0; i < sizeof o.flash; i++)
                if (a[i] != b[i]) printf("      byte %u: %02X vs %02X\n", i, a[i], b[i]);
        }
        total += n.writes;
    }
    printf("    12 x 600 keys: %u records\n", (unsigned)total);
    CHECK("12 random sequences of 600 keys: the same number of records and the same bytes as capture-per-key", ok);
    CHECK("... and they did write (the sequences change settings)", total > 50);

    /* a held arrow: ten values, one record */
    power_up_blank();
    clean_state(ST_GEN, OV_NONE);
    const uint32_t w0 = writes();
    for (int i = 0; i < 10; i++) { now_ms += 150; fw_key(BTN_DOWN); }       /* gen.amp, ten steps */
    CHECK("ten steps 150 ms apart: nothing written yet", writes() == w0 && model_notes == 10);
    now_ms += 1900; fw_key(BTN_AUTO);                                       /* a key that changes nothing, inside the window */
    CHECK("a key inside the window that changes nothing: still nothing", writes() == w0);
    now_ms += 200; fw_key(BTN_AUTO);
    CHECK("the next key after the window: one record for the ten values", writes() == w0 + 1 && flash_is_live());
    now_ms += 5000; fw_key(BTN_AUTO); fw_key(BTN_CH1);
    CHECK("more keys that change nothing: no more records", writes() == w0 + 1);
    /* changed and changed back inside the window: no record */
    now_ms += 100; fw_key(BTN_UP);
    now_ms += 100; fw_key(BTN_DOWN);
    now_ms += 3000; fw_key(BTN_AUTO);
    CHECK("a value changed and changed back inside the window costs no record", writes() == w0 + 1);
}

/* ── 3. the boundaries ─────────────────────────────────────────────── */
static void test_boundaries(void)
{
    printf("the boundaries still capture everything\n");
    power_up_blank();
    clean_state(ST_GEN, OV_NONE);
    const uint32_t w0 = writes();
    now_ms += 100; fw_key(BTN_DOWN);
    CHECK("a change inside the window is not in flash yet", writes() == w0 && !flash_is_live());
    CHECK("a flush (a mode change, the shell after a command) writes it at once",
          settings_store_flush(now_ms) && writes() == w0 + 1 && flash_is_live());
    now_ms += 100; fw_key(BTN_DOWN);
    CHECK("the power-off flush writes a pending change and the screen shown",
          settings_store_flush_power_off(now_ms, startup_screen_of_mode(current_mode)) && flash_is_live());
    const uint32_t w1 = writes();
    CHECK("a second power-off flush with nothing changed writes nothing",
          !settings_store_flush_power_off(now_ms, startup_screen_of_mode(current_mode)) && writes() == w1);
    /* a change made with no key and no note (a variable stored behind the
     * model) is still caught by the next boundary */
    scope_state_get()->trigger.level = (int16_t)(scope_state_get()->trigger.level + 3);
    now_ms += 5000; fw_key(BTN_AUTO);
    CHECK("a store behind the model is not noticed by a key any more ...", writes() == w1);
    CHECK("... the next flush still records it", settings_store_flush(now_ms) && flash_is_live());
}

int main(void)
{
    printf("settings: saved on a changed property\n");
    test_every_key();
    test_walks();
    test_write_policy();
    test_boundaries();
    printf("\n%d/%d passed\n", passed, run);
    return passed == run ? 0 : 1;
}
