/*
 * usercal.h — device glue for OpenScope's persisted calibration corrections
 * (record format and storage: usercal_rec.h). Captures the live baseline
 * deltas (scope_vpos) and the interleave B correction (fpga), saves them on
 * request, and re-applies the saved record at boot.
 *
 * Saves happen only when asked (`cal user save`, or a future UI confirm
 * calling usercal_save()); the long-AUTO run and `fpga interleave cal` still
 * change RAM only.
 */
#ifndef USERCAL_H
#define USERCAL_H

#include <stdint.h>
#include <stdbool.h>

#include "usercal_rec.h"

typedef struct {
    usercal_status_t      load_st;      /* last load (boot), USERCAL_ST_EMPTY before */
    bool                  loaded;       /* a record was decoded                       */
    bool                  table_match;  /* its fingerprint matched the live page      */
    bool                  il_applied;
    uint8_t               applied;      /* deltas accepted by scope_vpos_corr_set     */
    uint8_t               refused;      /* deltas it refused, or skipped (stale page) */
    flash_region_status_t last_save;    /* layer status of the last save/clear        */
    bool                  saved;        /* a save or clear ran this session           */
} usercal_report_t;

/* Fill *u from live state (the page fingerprint, every delta, the interleave
 * correction when it is not identity). */
void usercal_capture(usercal_t *u);

/* Apply a record to live state: deltas only when the fingerprint matches the
 * live page (and the page is in use), interleave always. Re-applies the
 * offset DACs. Updates the report's applied/refused/table_match/il_applied. */
void usercal_apply(const usercal_t *u);

/* Boot: load the newest record and apply it. Call after settings_store_init()
 * (binds the W25Q) and scope_vpos_init() (the page must be parsed). */
void usercal_boot_load(void);

/* Explicit save of the live corrections. */
flash_region_status_t usercal_save(void);

/* Explicit clear: append an empty record (no erase) and drop the RAM
 * corrections, so live state and the next boot agree. */
flash_region_status_t usercal_clear_saved(void);

/* Fingerprint of the live page's endpoint table (0 in the emulator). */
uint32_t usercal_live_table_crc(void);

const usercal_report_t *usercal_report(void);

#endif /* USERCAL_H */
