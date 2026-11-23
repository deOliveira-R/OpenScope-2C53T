/*
 * session_store.c - the session snapshot's flash side (session_store.h).
 *
 * Reaches flash only through the region layer's append log, by region id:
 * there is no flash_fs_raw_* and no flash_regions_*_abs() call here, so
 * nothing outside FLASH_REGION_SESSION can be written or erased from this
 * file (the same argument as config.c's).
 */
#include "session_store.h"

#include "flash_regions.h"
#include "model.h"
#include "model_session.h"
#include "settings_store.h"
#include "ui.h"                  /* STARTUP_RESTORE */

#include <string.h>

#define SESSION_REGION  FLASH_REGION_SESSION

_Static_assert(MODEL_SESSION_MAX_BYTES <= FLASH_REGION_RECORD_MAX,
               "a snapshot must fit one append record");

/* Built and read here: the input task (save, clear) and the boot load before
 * the scheduler are the only users, never at the same time. */
static uint8_t          g_buf[MODEL_SESSION_MAX_BYTES];
static session_status_t g_st;

const session_status_t *session_snapshot_status(void) { return &g_st; }

const char *session_load_text(uint8_t load)
{
    switch ((session_load_t)load) {
    case SESSION_LOAD_NOT_RUN:     return "not loaded";
    case SESSION_LOAD_OK:          return "valid";
    case SESSION_LOAD_NOT_RESTORE: return "not read (sys.startup is not restore)";
    case SESSION_LOAD_NONE:        return "none (no record)";
    case SESSION_LOAD_CLEARED:     return "none (cleared)";
    case SESSION_LOAD_BAD:         return "not usable (not a snapshot, or bad checksum)";
    case SESSION_LOAD_VERSION:     return "not usable (another format version)";
    case SESSION_LOAD_STALE:       return "not used (stale: the settings record changed after it)";
    case SESSION_LOAD_NO_STORAGE:  return "no storage bound";
    case SESSION_LOAD_IO:          return "flash read error";
    }
    return "unknown";
}

const char *session_save_text(uint8_t save)
{
    switch ((session_save_t)save) {
    case SESSION_SAVE_NONE:       return "none this session";
    case SESSION_SAVE_OK:         return "saved";
    case SESSION_SAVE_DISABLED:   return "NOT saved (flash writes are disabled in this build)";
    case SESSION_SAVE_NO_STORAGE: return "NOT saved (no storage bound)";
    case SESSION_SAVE_TOO_BIG:    return "NOT saved (snapshot larger than its buffer)";
    case SESSION_SAVE_FAILED:     return "NOT saved (flash error)";
    }
    return "unknown";
}

static uint32_t stamp_of(const device_config_t *cfg)
{
    return model_session_crc32(cfg, (uint32_t)sizeof *cfg);
}

/* The newest record into g_buf. OK: *len and *info are its; anything else
 * says why there is none to use. */
static session_load_t read_newest(uint16_t *len, model_session_info_t *info)
{
    uint32_t n = 0;
    memset(info, 0, sizeof *info);
    *len = 0;
    if (!flash_regions_ready()) return SESSION_LOAD_NO_STORAGE;
    const flash_region_status_t st = flash_region_read_latest(SESSION_REGION, g_buf, sizeof g_buf, &n);
    g_st.last_flash_status = (int32_t)st;
    if (st == FLASH_REGION_ERR_NOT_FOUND) return SESSION_LOAD_NONE;
    if (st == FLASH_REGION_ERR_BOUNDS)    return SESSION_LOAD_BAD;      /* longer than a snapshot */
    if (st == FLASH_REGION_ERR_BAD_ARG || st == FLASH_REGION_ERR_READ_ONLY) return SESSION_LOAD_NO_STORAGE;
    if (st != FLASH_REGION_OK)            return SESSION_LOAD_IO;
    *len = (uint16_t)n;
    /* Parsed only: the caller checks the stamp before anything is kept. */
    switch (model_session_parse(g_buf, *len, info)) {
    case MODEL_SESSION_OK:       return SESSION_LOAD_OK;
    case MODEL_SESSION_CLEARED:  return SESSION_LOAD_CLEARED;
    case MODEL_SESSION_VERSION_: return SESSION_LOAD_VERSION;
    default:                     return SESSION_LOAD_BAD;
    }
}

void session_snapshot_boot_load(const device_config_t *loaded)
{
    model_session_info_t info;
    uint16_t len = 0;

    /* A boot: everything starts again (RAM is zero then anyway; a host
     * test's "power cycle" is this call). */
    memset(&g_st, 0, sizeof g_st);
    g_st.settings_stamp = loaded ? stamp_of(loaded) : 0u;

    /* Startup on Boot, as the settings record just loaded has it. Anything
     * but Restore: the snapshot is not even read. */
    if (prop_get(P(SYS_STARTUP)) != (int32_t)STARTUP_RESTORE) {
        g_st.load = SESSION_LOAD_NOT_RESTORE;
        return;
    }
    session_load_t r = read_newest(&len, &info);
    g_st.load_seq   = info.seq;
    g_st.load_count = info.count;
    g_st.load_bytes = len;
    g_st.load_stamp = info.stamp;
    if (r == SESSION_LOAD_OK) {
        if (!loaded || info.stamp != g_st.settings_stamp) {
            r = SESSION_LOAD_STALE;
        } else if (model_session_keep(g_buf, len, &info) != MODEL_SESSION_OK) {
            r = SESSION_LOAD_BAD;               /* cannot happen: it parsed a line above */
        } else {
            g_st.kept = info.kept;
            g_st.unknown = info.unknown;
            g_st.excluded = info.excluded;
            g_st.out_of_range = info.out_of_range;
        }
    }
    g_st.load = (uint8_t)r;
}

#if SETTINGS_PERSIST_WRITES
/* One record. When the bank takes no more, the log's other bank is erased (one
 * sector) and the record goes there: the last snapshot stays readable meanwhile. */
static flash_region_status_t append_record(const uint8_t *buf, uint16_t len)
{
    bool moved = false;
    const flash_region_status_t st = flash_region_log_save(SESSION_REGION, buf, len, &moved);
    if (moved) g_st.compactions++;
    g_st.last_flash_status = (int32_t)st;
    return st;
}
#endif

bool session_snapshot_save(void)
{
#if !SETTINGS_PERSIST_WRITES
    g_st.save = SESSION_SAVE_DISABLED;
    return false;
#else
    model_session_info_t prev;
    uint16_t plen = 0;
    if (!flash_regions_ready()) { g_st.save = SESSION_SAVE_NO_STORAGE; return false; }

    /* The sequence number follows the newest record this build can read
     * (a snapshot or a tombstone); 1 after anything else. */
    const session_load_t have = read_newest(&plen, &prev);
    if (have == SESSION_LOAD_IO) { g_st.save = SESSION_SAVE_FAILED; return false; }
    const uint16_t seq = (have == SESSION_LOAD_OK || have == SESSION_LOAD_CLEARED)
                         ? (uint16_t)(prev.seq + 1u) : 1u;

    device_config_t img;
    settings_store_peek_image(&img);            /* what a settings flush writes now */
    const uint16_t len = model_session_encode(g_buf, sizeof g_buf, seq, stamp_of(&img));
    if (len == 0u) { g_st.save = SESSION_SAVE_TOO_BIG; return false; }

    if (append_record(g_buf, len) != FLASH_REGION_OK) { g_st.save = SESSION_SAVE_FAILED; return false; }
    g_st.save = SESSION_SAVE_OK;
    g_st.save_seq = seq;
    g_st.save_count = (uint16_t)(g_buf[6] | ((uint16_t)g_buf[7] << 8));
    g_st.save_bytes = len;
    g_st.saves++;
    return true;
#endif
}

bool session_snapshot_clear(void)
{
    model_session_info_t prev;
    uint16_t plen = 0;
    const session_load_t have = read_newest(&plen, &prev);
    /* Nothing this build would ever load: nothing to invalidate, no write. */
    if (have != SESSION_LOAD_OK) return have != SESSION_LOAD_IO;
#if !SETTINGS_PERSIST_WRITES
    return false;
#else
    const uint16_t len = model_session_encode_cleared(g_buf, sizeof g_buf, (uint16_t)(prev.seq + 1u));
    const flash_region_status_t st = append_record(g_buf, len);
    if (st == FLASH_REGION_OK) g_st.clears++;
    return st == FLASH_REGION_OK;
#endif
}

bool session_snapshot_log_info(uint32_t *bytes_used, uint32_t *bytes_total, uint32_t *records)
{
    uint32_t used = 0, free_ = 0, recs = 0;
    if (!flash_regions_ready() ||
        flash_region_log_info(SESSION_REGION, &used, &free_, &recs) != FLASH_REGION_OK) return false;
    if (bytes_used)  *bytes_used = used;
    if (bytes_total) *bytes_total = used + free_;
    if (records)     *records = recs;
    return true;
}
