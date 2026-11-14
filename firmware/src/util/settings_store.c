/*
 * settings_store.c — capture / apply live settings, and decide when to write.
 *
 * See settings_store.h for the write policy and for the known gap.
 *
 * This is the first automated writer of the external W25Q in this firmware's
 * history. It reaches flash only through config.c, which reaches it only
 * through the region layer's append log, which refuses anything outside
 * FLASH_REGION_SETTINGS. There is deliberately no flash_fs_raw_* call and no
 * flash_regions_*_abs() call anywhere in this path.
 */

#include "settings_store.h"

#include "flash_regions.h"
#include "model_persist_v5.h"

#include <string.h>

static device_config_t         g_live;    /* current settings image           */
static device_config_t         g_saved;   /* last image known to be in flash  */
static config_autosave_t       g_auto;
static settings_store_status_t g_status;

static void stamp(device_config_t *cfg)
{
    cfg->magic    = CONFIG_MAGIC;
    cfg->version  = CONFIG_VERSION;
    cfg->checksum = config_compute_checksum(cfg);
}

/* ═══════════════════════════════════════════════════════════════════
 * Capture / apply
 *
 * Both are loops over the v5 codec table (src/model/model_persist_v5.c):
 * one row per record field, naming the property it holds, its encoding and
 * what a load does with a value out of range. Capture and apply cannot drift
 * apart, because there is one list. Fields with no live owner (fft_*,
 * language, display_persist_mode, the padding) are carried through
 * untouched, as before.
 *
 * Deliberately NOT restored, as before: ss->running (a scope always boots
 * running), the channel enables (always on after a load, parity C2: a CH2-off
 * saved here would come back as the 250 MS/s single-channel mode with nothing
 * on screen saying why; the record says true), the cursor, the channel
 * positions, and persistence (turning it on claims the shared memory pool
 * and evicts FFT, which a boot should not do behind the user's back). The
 * generator output comes back ON only when the user opted in (gen.bootout)
 * and it was on at the last save.
 * ═══════════════════════════════════════════════════════════════════ */

void settings_store_capture(device_config_t *cfg)
{
    if (!cfg) return;
    model_persist_v5_capture(cfg);
    stamp(cfg);
}

void settings_store_apply(const device_config_t *cfg)
{
    if (!cfg) return;
    /* Each value goes through its property's load hook: the backing state,
     * no hardware (the owners' inits push it later in main()), clamped by
     * the row's out-of-range policy so nothing indexes a table with a value
     * a record should not hold. */
    model_persist_v5_apply(cfg);
}

/* ═══════════════════════════════════════════════════════════════════
 * Dirty tracking
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum {
    CAPTURE_MATCHES_FLASH = 0,  /* nothing to write                         */
    CAPTURE_STILL_PENDING,      /* differs from flash, but nothing new       */
    CAPTURE_NEW_CHANGE,         /* differs from the previous capture too     */
} capture_result_t;

/* Capture into g_live and classify it against BOTH the previous capture and
 * the last saved image.
 *
 * The distinction is what makes the settle window terminate. If every press
 * restarted the window, a pending change would never be written while the user
 * kept pressing unrelated buttons. Only a NEW change restarts it.
 *
 * The comparison is over the whole record image, checksum included, which is
 * exactly the byte string the append log compares for its no-op elision — so
 * "unchanged here" and "elided there" can never disagree. */
static capture_result_t capture(void)
{
    device_config_t next = g_live;      /* keep the fields we do not own */
    settings_store_capture(&next);

    bool differs_from_last_look = (memcmp(&next, &g_live, sizeof next) != 0);
    bool differs_from_flash     = (memcmp(&next, &g_saved, sizeof next) != 0);
    g_live = next;

    if (!differs_from_flash)      return CAPTURE_MATCHES_FLASH;
    if (differs_from_last_look)   return CAPTURE_NEW_CHANGE;
    return CAPTURE_STILL_PENDING;
}

/* Set when a write failed, cleared when something new changes. Without it, a
 * device whose settings region cannot be written (region not blank, flash
 * failing, table wrong) would re-attempt the save on EVERY button press, and
 * each attempt costs a full scan of the log in short SPI2 reads. One refusal
 * per change is a diagnostic; one refusal per keypress is a fault of its own. */
static bool g_write_blocked;

static bool write_now(void)
{
    if (config_save(&g_live)) {
        g_write_blocked = false;
        /* g_live was stamped by settings_store_capture() with the same magic /
         * version / checksum config_save() stamps on its own copy, so g_live
         * is byte-identical to the record that just landed in flash. */
        g_saved = g_live;
        config_autosave_done(&g_auto);
        g_status.writes++;
        return true;
    }
    g_write_blocked = true;
    g_status.write_failures++;
    return false;
}

void settings_store_note_change(uint32_t now_ms)
{
    switch (capture()) {
    case CAPTURE_MATCHES_FLASH:
        /* Nothing to write — including the "changed and changed back" case,
         * where clearing here is what stops a cancelled edit costing a
         * record. */
        config_autosave_done(&g_auto);
        break;

    case CAPTURE_NEW_CHANGE:
        g_status.changes_seen++;
        g_write_blocked = false;        /* new input is worth another attempt */
        config_autosave_mark(&g_auto, now_ms);
        break;

    case CAPTURE_STILL_PENDING:
        /* Leave the running window alone. Re-arm only if something cleared
         * the flag without writing, so a pending change can never be
         * stranded. */
        if (!g_auto.pending) {
            config_autosave_mark(&g_auto, now_ms);
        }
        break;
    }
}

bool settings_store_service(uint32_t now_ms)
{
    if (g_write_blocked || !config_autosave_due(&g_auto, now_ms)) {
        return false;
    }
    return write_now();
}

settings_flush_outcome_t settings_store_flush_ex(uint32_t now_ms)
{
    settings_store_note_change(now_ms);
    if (g_write_blocked) {
        return SETTINGS_FLUSH_BLOCKED;
    }
    if (!g_auto.pending) {
        return SETTINGS_FLUSH_CLEAN;
    }
    return write_now() ? SETTINGS_FLUSH_SAVED : SETTINGS_FLUSH_FAILED;
}

bool settings_store_flush(uint32_t now_ms)
{
    return settings_store_flush_ex(now_ms) == SETTINGS_FLUSH_SAVED;
}

const char *settings_flush_outcome_text(settings_flush_outcome_t o)
{
    switch (o) {
    case SETTINGS_FLUSH_SAVED:   return "saved";
    case SETTINGS_FLUSH_CLEAN:   return "already saved";
    case SETTINGS_FLUSH_BLOCKED: return "NOT saved (settings storage blocked after a failed write)";
    case SETTINGS_FLUSH_FAILED:  return "NOT saved (write failed)";
    default:                     return "NOT saved (unknown)";
    }
}

const settings_store_status_t *settings_store_get_status(void) { return &g_status; }

/* ═══════════════════════════════════════════════════════════════════
 * Boot
 * ═══════════════════════════════════════════════════════════════════ */

config_load_result_t settings_store_load_and_apply(void)
{
    config_autosave_init(&g_auto, SETTINGS_STORE_SETTLE_MS);

    g_write_blocked = false;

    config_load_result_t r = config_load_or_defaults(&g_live);
    g_status.load_result = r;

    settings_store_apply(&g_live);

    /* Baseline for change detection. On a defaulted boot this is the DEFAULT
     * image, not a flash image — so the first real change writes a record and
     * an untouched device never writes at all. Both config_load_or_defaults()
     * outcomes leave a stamped, checksum-consistent struct. */
    g_saved = g_live;

    return r;
}

void settings_store_init(void)
{
#ifdef EMULATOR_BUILD
    /* Renode does not model the W25Q on SPI2. Binding here would make every
     * record scan spin the bounded SPI timeout loops in flash_fs.c — slow, and
     * it would "persist" into a peripheral that is not there. The emulator
     * therefore runs on defaults, with storage_bound false, and says so. */
    g_status.storage_bound = false;
#else
    g_status.storage_bound = (flash_regions_bind_w25q() == FLASH_REGION_OK);
#endif
    (void)settings_store_load_and_apply();
}
