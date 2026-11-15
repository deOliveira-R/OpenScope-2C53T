/*
 * settings_store.h — the glue between live UI state and persisted settings.
 *
 * config.c owns the record format and the flash access; this file owns the
 * question "which live variables are settings, and when do we write them".
 * Keeping them apart is what lets config.c be host-tested against a NOR model
 * with no UI linked in.
 *
 * WHEN DOES IT WRITE (the interesting design question)
 * ---------------------------------------------------
 * Not on every keypress: adjusting the timebase with ten presses would be ten
 * records, and a record per keypress is how an append log turns into a
 * compaction (i.e. an erase) loop.
 *
 * Instead:
 *   - every button press captures live state and compares it against the last
 *     saved image. Unchanged -> nothing pending, no flash access at all;
 *   - a change starts (or restarts) a settle window, currently 2 s. The write
 *     happens on the first press after the window expires, so a burst of
 *     adjustments collapses into one record;
 *   - deliberate boundaries flush immediately, ignoring the window: switching
 *     device mode (MENU), and the power-off countdown just before PC9 drops.
 *
 * Everything is driven from the input task, on purpose. Every settings change
 * on this device originates from a button press, so the input task is where
 * the work belongs; it also keeps SPI2 flash traffic out of the FreeRTOS timer
 * service task, which is the highest-priority task in the system and also
 * feeds the watchdog. A nearly-full 64 KB append log takes a few thousand
 * short SPI reads to scan (~100 ms at the /16 prescaler), and that is not
 * something to do from the timer task.
 *
 * KNOWN GAP, stated rather than hidden: a change followed by no further button
 * press and no orderly power-off (battery pulled, auto power-off) is lost. The
 * settings are only ever as durable as the last press or the last flush point.
 */

#ifndef SETTINGS_STORE_H
#define SETTINGS_STORE_H

#include <stdint.h>
#include <stdbool.h>

#include "config.h"
#include "session_store.h"   /* session_snapshot_save(): the other save of an orderly power-off,
                              * declared with this one so that path needs one header */

/* How long a change must be quiet before it is written. */
#define SETTINGS_STORE_SETTLE_MS  2000u

typedef struct {
    bool                 storage_bound;   /* region layer accepted the backend */
    config_load_result_t load_result;     /* how boot-time load ended          */
    uint32_t             writes;          /* successful config_save() calls    */
    uint32_t             write_failures;
    uint32_t             changes_seen;    /* captures that differed            */
} settings_store_status_t;

/* Bind the region layer to the W25Q, load the saved settings (or defaults) and
 * apply them to live UI state. Call once from main(), after theme_init() and
 * scope_state_init() and after flash_fs_init() (the region backend uses the
 * flash_fs raw primitives and their mutex).
 *
 * Never fails in a way the caller must handle: with no storage, or with an
 * unreadable/corrupt/foreign record, the device comes up on defaults. */
void settings_store_init(void);

/* Load the newest record (or defaults) and apply it. Split out of _init() so
 * host tests can drive it against a flash model. */
config_load_result_t settings_store_load_and_apply(void);

/* Capture live state; if it differs from the last saved image, start/restart
 * the settle window. Cheap: one struct compare, no flash access. Call at the
 * end of every button handled. */
void settings_store_note_change(uint32_t now_ms);

/* Write if a change has settled. Returns true if a record was written. */
bool settings_store_service(uint32_t now_ms);

/* Capture and write immediately if anything is pending, ignoring the settle
 * window. Use at deliberate boundaries (mode change, power off). Returns true
 * if a record was written. */
bool settings_store_flush(uint32_t now_ms);

/* The flush of an orderly power-off, and the one place the record's
 * last-screen byte moves (Startup on Boot = Restore): `screen` is the
 * STARTUP_SCREEN_* of the screen shown now (startup_policy.h). Every other
 * capture carries the byte the record already holds, so a screen change
 * alone writes nothing and a setting changed on another screen does not
 * move it. Writes one record when the screen or anything else differs from
 * flash. */
bool settings_store_flush_power_off(uint32_t now_ms, uint8_t screen);

/* What a flush actually did. settings_store_flush()'s bool cannot tell "nothing
 * to write" (fine: already saved) from "write refused" (not saved), and the
 * shell prints a promise to the operator, so it needs the difference. */
typedef enum {
    SETTINGS_FLUSH_SAVED = 0,   /* a record was written                          */
    SETTINGS_FLUSH_CLEAN,       /* live state already equals the saved image     */
    SETTINGS_FLUSH_BLOCKED,     /* changed, but a failed write blocks retries    */
    SETTINGS_FLUSH_FAILED,      /* changed, write attempted and failed           */
} settings_flush_outcome_t;

/* Same as settings_store_flush() but reports the outcome. */
settings_flush_outcome_t settings_store_flush_ex(uint32_t now_ms);

/* One short word for the shell: "saved", "already saved", "NOT saved (...)". */
const char *settings_flush_outcome_text(settings_flush_outcome_t o);

/* "Reset to defaults": the BUILT-IN DEFAULTS (the property table's, const)
 * copied over the USER SETTINGS (the saved record), in three steps, on the
 * input task (it runs setters and writes the record):
 *   1. model_reset_to_defaults(between): every property a reset covers back
 *      to its default through its setter, so the hardware follows. BUSY
 *      while self-cal runs: nothing is touched, nothing is written.
 *   2. the record image starts again from config_init_defaults() (so the
 *      bytes no property owns are the defaults' too) with the last-screen
 *      byte cleared: Startup on Boot is "restore" again and there is no
 *      last screen, so the next boot shows Home until an orderly power-off
 *      names a screen.
 *   3. the live state is captured over it and written at once.
 * When every property reached its default (left == 0) the record
 * written is byte for byte the one config_init_defaults() builds; a property
 * an owner refused keeps its live value in the record, as in RAM.
 *
 * This path can reach the settings region only (config_save -> the region
 * layer's append log): never the factory calibration page or its W25Q
 * backup, the user calibration record, the gallery, the logo or the
 * firmware cache. tests/test_settings_reset.c holds that against the flash
 * model. */
typedef struct {
    uint8_t  status;        /* prop_status_t (model.h): PS_OK; PS_BUSY: nothing happened;
                             * PS_NOT_SET: `left` properties are not at their default   */
    uint8_t  changed;       /* properties whose value moved                            */
    uint8_t  left;          /* covered by a reset and still not at their default       */
    uint16_t first_left;    /* the id of the first of them (0: none)                   */
    settings_flush_outcome_t saved;   /* what the write did (not with PS_BUSY)         */
} settings_reset_result_t;
settings_reset_result_t settings_store_reset_to_defaults(uint32_t now_ms, void (*between)(void));

/* Apply a config to live UI state, clamping every index to its live range. A
 * record can pass its CRC and its checksum and still hold, say, vdiv index 200
 * — from an older layout, a truncated struct, or a bit flip that the byte-sum
 * checksum happens to miss. Nothing here indexes a table with an unclamped
 * value. */
void settings_store_apply(const device_config_t *cfg);

/* Capture live UI state into *cfg (fields not owned by this layer are left
 * as-is), then stamp magic/version/checksum. */
void settings_store_capture(device_config_t *cfg);

/* The record image a flush would write NOW: the live state captured over
 * the image this store holds, stamped. Changes nothing here (not the held
 * image, not the settle window). The session snapshot stamps itself with
 * its CRC (session_store.h, STALE). Input task. */
void settings_store_peek_image(device_config_t *out);

const settings_store_status_t *settings_store_get_status(void);

#endif /* SETTINGS_STORE_H */
