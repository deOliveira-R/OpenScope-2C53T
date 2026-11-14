/*
 * OpenScope 2C53T - Configuration Save/Load
 *
 * See config.h for the interface and for why the storage backend is the
 * region layer's append log rather than a raw sector rewrite.
 *
 * HISTORY, worth knowing before changing anything here: until 2026-08-13 this
 * file wrote to a static RAM buffer and had ZERO call sites, while the repo
 * advertised "config save/load with checksum". Nothing had ever survived a
 * reboot. The fix is not just "point it at flash" — it is that every failure
 * mode now has a defined, tested behaviour, and that the write goes through
 * flash_regions.h so a bug here cannot erase the factory calibration.
 */

#include "config.h"
#include "flash_regions.h"
#include "model_persist_v5.h"

#include <stddef.h>
#include <string.h>

/* A config record must fit the append log's per-record limit. If someone grows
 * the struct past 1 KB this fails at compile time rather than at the first
 * save on a user's device. */
_Static_assert(sizeof(device_config_t) <= FLASH_REGION_RECORD_MAX,
               "device_config_t no longer fits an append record");

/* The v4 -> v5 upgrade relies on v5 being v4 with fields appended before the
 * checksum: v4's checksum sat where v5's new block starts. */
_Static_assert(offsetof(device_config_t, meas_ch1_mask) == CONFIG_V4_SIZE - 4u,
               "v5 must extend v4 at its checksum offset (config_upgrade_v4)");

/* The settings live in their own region. This is the ONLY place the storage
 * location is named, and it is named by id — not by address — so the region
 * table decides what is writable. Do not add a flash_regions_*_abs() call to
 * this file; the whole safety argument rests on that. */
#define CONFIG_REGION  FLASH_REGION_SETTINGS

static config_persist_stats_t g_stats;

/* ========================================================================
 * Default configuration values
 * ======================================================================== */

void config_init_defaults(device_config_t *cfg)
{
    if (!cfg) return;

    /* Memset first: this is what makes the struct's padding deterministic,
     * which is what makes the append log's no-op elision work. */
    memset(cfg, 0, sizeof(*cfg));

    cfg->magic   = CONFIG_MAGIC;
    cfg->version = CONFIG_VERSION;

    /* Every field from the property table's built-in defaults through the
     * v5 codec table (src/model/model_persist_v5.c): the scope 2 V/div CH1,
     * 200 mV/div CH2, 50 us/div, Auto/rising/CH1 at 0; stock's factory
     * generator (sine, 1 kHz, 3.0 V, 50 %) with the output off at boot;
     * brightness 100, volume 1, auto-off off, sys_prefs_mark set (C15);
     * startup Meter; the five-measurement set on both channels (v5); FFT
     * Hanning, no averaging. Fields with no property (display_persist_mode)
     * stay zero. tests/test_model_persist.c pins the bytes against the
     * literal list this replaced. */
    model_persist_v5_defaults(cfg);

    /* Compute and store checksum */
    cfg->checksum = config_compute_checksum(cfg);
}

/* ========================================================================
 * Checksum: simple byte-sum of all fields before the checksum field
 * ======================================================================== */

uint32_t config_compute_checksum(const device_config_t *cfg)
{
    if (!cfg) return 0;

    const uint8_t *bytes = (const uint8_t *)cfg;
    /* Sum all bytes up to (but not including) the checksum field */
    uint32_t len = offsetof(device_config_t, checksum);
    uint32_t sum = 0;

    for (uint32_t i = 0; i < len; i++) {
        sum += bytes[i];
    }

    return sum;
}

/* ========================================================================
 * Validation
 * ======================================================================== */

bool config_validate(const device_config_t *cfg)
{
    if (!cfg) return false;
    if (cfg->magic != CONFIG_MAGIC) return false;
    if (cfg->version != CONFIG_VERSION) return false;
    if (cfg->checksum != config_compute_checksum(cfg)) return false;

    return true;
}

/* ========================================================================
 * Serialization (struct <-> byte buffer)
 *
 * For v1, the struct is memcpy'd directly. This works because:
 * - Target is always ARM Cortex-M4 (little-endian, consistent alignment)
 * - The same compiler and platform are used for save and load
 *
 * A record written by a different build of the struct is caught by the
 * version field and by the length check in config_load().
 * ======================================================================== */

uint32_t config_serialize(const device_config_t *cfg, uint8_t *buf, uint32_t buf_size)
{
    if (!cfg || !buf) return 0;
    if (buf_size < sizeof(device_config_t)) return 0;

    memcpy(buf, cfg, sizeof(device_config_t));
    return (uint32_t)sizeof(device_config_t);
}

bool config_deserialize(device_config_t *cfg, const uint8_t *buf, uint32_t buf_size)
{
    if (!cfg || !buf) return false;
    if (buf_size < sizeof(device_config_t)) return false;

    memcpy(cfg, buf, sizeof(device_config_t));

    /* Validate the deserialized config */
    return config_validate(cfg);
}

/* ========================================================================
 * Save / Load — the W25Q settings append log
 * ======================================================================== */

const char *config_load_result_name(config_load_result_t r)
{
    switch (r) {
    case CONFIG_LOAD_OK:         return "loaded";
    case CONFIG_LOAD_NO_STORAGE: return "no storage bound";
    case CONFIG_LOAD_EMPTY:      return "no saved record";
    case CONFIG_LOAD_BAD_SIZE:   return "record size mismatch";
    case CONFIG_LOAD_INVALID:    return "record failed validation";
    case CONFIG_LOAD_IO:         return "flash read error";
    }
    return "unknown";
}

const config_persist_stats_t *config_persist_stats(void)
{
    /* Stamped on every read so a caller can never be handed a stats block that
     * does not say which kind of build produced it. saves_ok == 0 means two
     * completely different things depending on this flag, and a diagnostic
     * that cannot tell them apart is the failure mode this project keeps
     * paying for. */
    g_stats.writes_enabled = (SETTINGS_PERSIST_WRITES != 0);
    return &g_stats;
}

void config_persist_stats_reset(void)
{
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.writes_enabled = (SETTINGS_PERSIST_WRITES != 0);
}

bool config_save(const device_config_t *cfg)
{
    if (!cfg) {
        g_stats.saves_failed++;
        return false;
    }
#if !SETTINGS_PERSIST_WRITES
    /* Writes disabled at build time (the default — see config.h). Bail before
     * touching the region layer at all, so a disabled build issues no erase,
     * no program, and no CS assert on SPI2. Counted separately from failures:
     * this is a choice, not a fault. */
    g_stats.saves_disabled++;
    return false;
#else
    if (!flash_regions_ready()) {
        /* No backend bound (host build with no model, emulator, or a region
         * table that failed its self-check). Refuse loudly instead of
         * returning true and losing the data. */
        g_stats.saves_failed++;
        g_stats.last_save_status = (int32_t)FLASH_REGION_ERR_NOT_INIT;
        return false;
    }

    /* Stamp identity and checksum from THIS build rather than trusting the
     * caller's copy: a record can then never claim a version it is not. */
    device_config_t rec = *cfg;
    rec.magic    = CONFIG_MAGIC;
    rec.version  = CONFIG_VERSION;
    rec.checksum = config_compute_checksum(&rec);

    uint8_t buf[sizeof(device_config_t)];
    if (config_serialize(&rec, buf, sizeof(buf)) != sizeof(buf)) {
        g_stats.saves_failed++;
        return false;
    }

    flash_region_status_t st = flash_region_append(CONFIG_REGION, buf, sizeof(buf));

    if (st == FLASH_REGION_ERR_FULL || st == FLASH_REGION_ERR_LOG_DAMAGED) {
        /* The log filled, or it ends at a record the scanner cannot read (a
         * header torn by a power cut, a half-erased sector), after which
         * nothing can ever be appended (issue #57). This is the ONE place this
         * file erases anything, it is an explicit whole-region reset of our
         * own append region (the layer bounds it), and it is the documented
         * way out of a log that takes no more records. A power cut between the
         * reset and the re-append costs the settings — the device comes up on
         * defaults, or on an older record if the erase was cut part-way — which
         * is why compaction is driven by "takes no more" and not by a
         * heuristic. buf is the newest setting, so nothing after the damage
         * that the save would keep is lost. */
        flash_region_status_t rst = flash_region_reset(CONFIG_REGION);
        if (rst != FLASH_REGION_OK) {
            g_stats.saves_failed++;
            g_stats.last_save_status = (int32_t)rst;
            return false;
        }
        g_stats.compactions++;
        st = flash_region_append(CONFIG_REGION, buf, sizeof(buf));
    }

    g_stats.last_save_status = (int32_t)st;
    if (st != FLASH_REGION_OK) {
        /* Anything else is NOT retried with an erase: an I/O or verify
         * failure says nothing about whether the region is ours to erase.
         * Surface it instead (config_persist_stats()->last_save_status). */
        g_stats.saves_failed++;
        return false;
    }

    g_stats.saves_ok++;
    return true;
#endif /* SETTINGS_PERSIST_WRITES */
}

/* Forward: the load worker, defined below with the rest of the load path. */
static config_load_result_t config_try_load(device_config_t *cfg);

bool config_load(device_config_t *cfg)
{
    if (!cfg) return false;

    if (config_try_load(cfg) != CONFIG_LOAD_OK) {
        /* *cfg is untouched — deliberately NOT filled with defaults here. A
         * caller that wants defaults asks for them by name via
         * config_load_or_defaults(); a caller that wants "did my saved config
         * come back" gets a clean no. */
        g_stats.loads_defaulted++;
        return false;
    }
    g_stats.loads_ok++;
    return true;
}

/* A v4 record (CONFIG_V4_SIZE bytes): its magic, version 4 and its own
 * byte-sum checksum over the first CONFIG_V4_SIZE - 4 bytes must all hold.
 * Then those bytes are v5's leading fields; the v5 block takes its defaults
 * and the result is restamped as v5. False (out untouched) otherwise. */
static bool config_upgrade_v4(device_config_t *out, const uint8_t *buf, uint32_t len)
{
    const uint32_t body = CONFIG_V4_SIZE - 4u;
    uint32_t magic, sum = 0, stored;
    uint16_t version;
    if (len != CONFIG_V4_SIZE) return false;
    memcpy(&magic, buf, sizeof magic);
    memcpy(&version, buf + offsetof(device_config_t, version), sizeof version);
    memcpy(&stored, buf + body, sizeof stored);
    for (uint32_t i = 0; i < body; i++) sum += buf[i];
    if (magic != CONFIG_MAGIC || version != 4u || stored != sum) return false;

    device_config_t c;
    config_init_defaults(&c);                 /* v5 defaults, zeroed padding */
    memcpy(&c, buf, body);
    c.version  = CONFIG_VERSION;
    c.checksum = config_compute_checksum(&c);
    *out = c;
    return true;
}

/* Reads the newest record. Writes *cfg ONLY when it returns CONFIG_LOAD_OK,
 * so every failure path leaves the caller's struct as it found it. */
static config_load_result_t config_try_load(device_config_t *cfg)
{
    uint8_t buf[sizeof(device_config_t)];
    uint32_t len = 0;

    if (!flash_regions_ready()) {
        return CONFIG_LOAD_NO_STORAGE;
    }

    flash_region_status_t st =
        flash_region_read_latest(CONFIG_REGION, buf, sizeof(buf), &len);
    g_stats.last_load_status = (int32_t)st;

    if (st == FLASH_REGION_ERR_NOT_FOUND) {
        return CONFIG_LOAD_EMPTY;            /* first boot: blank log */
    }
    if (st == FLASH_REGION_ERR_BOUNDS) {
        return CONFIG_LOAD_BAD_SIZE;         /* record bigger than a config */
    }
    if (st == FLASH_REGION_ERR_BAD_ARG || st == FLASH_REGION_ERR_READ_ONLY) {
        /* The region this build points at is not an append log (or is not
         * writable at all). That is a region-table problem, not a flash
         * problem — report it as "no storage" so it cannot be mistaken for a
         * transient read failure. */
        return CONFIG_LOAD_NO_STORAGE;
    }
    if (st != FLASH_REGION_OK) {
        return CONFIG_LOAD_IO;
    }
    if (len == CONFIG_V4_SIZE) {
        /* The previous layout: upgraded, so an update keeps the settings. */
        device_config_t up;
        if (!config_upgrade_v4(&up, buf, len)) return CONFIG_LOAD_INVALID;
        *cfg = up;
        return CONFIG_LOAD_OK;
    }
    if (len != sizeof(device_config_t)) {
        /* Right region, wrong shape — e.g. a record written by a build whose
         * struct was a different size. Never memcpy a short record into a
         * config: the tail would be whatever was on the stack. */
        return CONFIG_LOAD_BAD_SIZE;
    }

    /* Deserialize into scratch so a rejected record never lands in the
     * caller's struct: config_deserialize() overwrites before it validates,
     * and that behaviour must stay contained here. */
    device_config_t scratch;
    if (!config_deserialize(&scratch, buf, len)) {
        return CONFIG_LOAD_INVALID;          /* magic / version / checksum */
    }

    *cfg = scratch;
    return CONFIG_LOAD_OK;
}

config_load_result_t config_load_or_defaults(device_config_t *cfg)
{
    if (!cfg) return CONFIG_LOAD_INVALID;

    config_load_result_t result = config_try_load(cfg);
    if (result == CONFIG_LOAD_OK) {
        g_stats.loads_ok++;
        return result;
    }

    config_init_defaults(cfg);
    g_stats.loads_defaulted++;
    return result;
}

/* ========================================================================
 * Autosave policy (pure; no I/O)
 * ======================================================================== */

void config_autosave_init(config_autosave_t *a, uint32_t settle_ms)
{
    if (!a) return;
    a->pending   = false;
    a->marked_ms = 0;
    a->settle_ms = settle_ms;
}

void config_autosave_mark(config_autosave_t *a, uint32_t now_ms)
{
    if (!a) return;
    /* Re-marking pushes the deadline out: a burst of adjustments collapses to
     * one write once the user stops, instead of one write per keypress. */
    a->pending   = true;
    a->marked_ms = now_ms;
}

bool config_autosave_due(const config_autosave_t *a, uint32_t now_ms)
{
    if (!a || !a->pending) return false;
    /* Unsigned subtraction, so a tick-counter wrap yields the true elapsed
     * time rather than a ~49-day stall. */
    return (uint32_t)(now_ms - a->marked_ms) >= a->settle_ms;
}

void config_autosave_done(config_autosave_t *a)
{
    if (!a) return;
    a->pending = false;
}
