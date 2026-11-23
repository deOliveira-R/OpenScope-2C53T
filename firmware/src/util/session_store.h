/*
 * session_store.h - where the session snapshot lives, and when it is read
 * and written (the format and the membership are model/model_session.h).
 *
 * WHERE. The external W25Q128JV, an append log of its own (flash_regions.h)
 * with two banks of one 4 KB sector each: FLASH_REGION_SESSION at 0xF20000 and
 * FLASH_REGION_SESSION_B at 0xF6F000, both taken from "modules". Not the
 * settings log: config_try_load() takes the newest record of that log and
 * rejects it by length, so a snapshot there would default the settings (and
 * an older firmware would too). The 60-byte v5 settings record, its region
 * and its version are exactly as they were. Nothing here can reach any other
 * region (the region layer bounds every write and erase to these two), and
 * the MCU's own flash (the application, the calibration page at 0x08006000,
 * the bootloader) is a different device this file has no call into. A
 * firmware update over HID programs MCU flash only, so the snapshot
 * survives it; the property ids make it readable by the new table.
 *
 * One sector per bank on purpose: the only erase this file can cause is of the
 * bank not holding the newest snapshot, when the other takes no more records,
 * and that is one sector erase (the W25Q's erase unit, FLASH_REGION_SECTOR_SIZE
 * = 4096). A snapshot is about 210 bytes, so a bank takes ~18 saves.
 *
 * WHEN IT IS WRITTEN. Only by session_snapshot_save() (the orderly power-off,
 * and the shell's `session save`) and by session_snapshot_clear() ("Reset to
 * defaults", `session clear`), and the latter writes nothing when there is
 * no snapshot to invalidate. Never on a change, never at boot.
 *
 * WHEN IT IS READ. Once, by session_snapshot_boot_load(), from the settings
 * load before the scheduler (settings_store_load_and_apply), and only when
 * the settings record just loaded says Startup on Boot = Restore. It keeps
 * values and moves no live state: the input task sets them through the
 * setters (model.h, model_boot_apply).
 *
 * WHAT RESTORE DOES WITHOUT A USABLE SNAPSHOT: exactly what it did before
 * there was one (the settings record, its last screen, its view). The cases
 * (session_load_t) are: no record (a first boot after the update that
 * brought this file, an erased region), a tombstone (Reset to defaults,
 * `session clear`), a record that is not a snapshot or fails its checksum,
 * a format version this build does not read, no storage, a read error, and
 * STALE.
 *
 * STALE. A snapshot is saved WITH a settings record: its header holds the
 * CRC-32 of the settings image an immediate flush would write
 * (settings_store_peek_image). At boot it is used only when the settings
 * image just loaded has that CRC. Otherwise the settings record moved after
 * the snapshot was taken (a change that was autosaved and then a shutdown
 * that saves nothing: the battery-critical one, a crash, a reset; a record
 * written by another firmware; a settings write that failed at power-off),
 * the snapshot is older than the settings and applying it would put old
 * values over newer ones: it is left alone and reported. The order of the
 * two saves at power-off does not matter, as long as the last screen has
 * been noted (model_last_screen_note_power_off) before this save.
 *
 * Startup on Boot other than Restore: nothing is read and nothing changes;
 * a power-off still writes the snapshot, so that the one in flash is always
 * the last orderly power-off's, whatever the choice was that day.
 *
 * A power cut during a save leaves a torn record the log steps over: the
 * snapshot before it is then the newest, and it is STALE unless the
 * settings are still the ones it was saved with. A power cut during the
 * tombstone of a clear leaves the old snapshot; Reset writes the settings
 * record after it, which makes that snapshot STALE unless it was saved on
 * the defaults.
 */
#ifndef SESSION_STORE_H
#define SESSION_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

typedef enum {
    SESSION_LOAD_NOT_RUN = 0,   /* no boot load yet                                  */
    SESSION_LOAD_OK,            /* a snapshot was read and its values kept           */
    SESSION_LOAD_NOT_RESTORE,   /* Startup on Boot is not Restore: not read          */
    SESSION_LOAD_NONE,          /* the log holds no record                           */
    SESSION_LOAD_CLEARED,       /* the newest record is a tombstone                  */
    SESSION_LOAD_BAD,           /* not a snapshot, or it fails its checksum          */
    SESSION_LOAD_VERSION,       /* a format this build does not read                 */
    SESSION_LOAD_STALE,         /* the settings record changed after it was saved    */
    SESSION_LOAD_NO_STORAGE,    /* region layer not bound (emulator), or bad table   */
    SESSION_LOAD_IO,            /* flash read error                                  */
} session_load_t;

typedef enum {
    SESSION_SAVE_NONE = 0,      /* no save this session                              */
    SESSION_SAVE_OK,            /* one record appended                               */
    SESSION_SAVE_DISABLED,      /* SETTINGS_PERSIST_WRITES == 0: nothing attempted   */
    SESSION_SAVE_NO_STORAGE,
    SESSION_SAVE_TOO_BIG,       /* the table outgrew MODEL_SESSION_MAX_BYTES         */
    SESSION_SAVE_FAILED,        /* flash error (last_flash_status)                   */
} session_save_t;

typedef struct {
    /* the boot load */
    uint8_t  load;              /* session_load_t                                    */
    uint16_t load_seq;          /* of the record read (OK, CLEARED, STALE)           */
    uint16_t load_count;        /* entries in it                                     */
    uint16_t load_bytes;        /* its length                                        */
    uint8_t  kept, unknown, excluded, out_of_range;   /* model_session_info_t        */
    uint32_t load_stamp;        /* the snapshot's settings stamp                     */
    uint32_t settings_stamp;    /* the settings image the boot loaded                */
    /* this session's writes */
    uint8_t  save;              /* session_save_t of the last session_snapshot_save  */
    uint16_t save_seq, save_count, save_bytes;
    uint16_t saves;             /* records appended by session_snapshot_save         */
    uint16_t clears;            /* tombstones appended by session_snapshot_clear     */
    uint16_t compactions;       /* the log moved to its other bank                    */
    int32_t  last_flash_status; /* flash_region_status_t of the last append / erase  */
} session_status_t;

/* Boot: called by settings_store_load_and_apply() with the settings image it
 * loaded (the record's, or the defaults'), after that image was applied.
 * Reads flash, keeps values, moves no live state. */
void session_snapshot_boot_load(const device_config_t *loaded);

/* Write the snapshot of the live state: ONE record appended to the session
 * log. For the orderly power-off, after the last screen has been noted.
 *
 *   Context:   the input task (the power-off path and `session save` both
 *              run there). Other tasks may be running: the values are read
 *              through the getters, each a single aligned word or byte, and
 *              SPI2 is taken per primitive under the flash_fs mutex like
 *              every other W25Q access. Not reentrant (one static buffer).
 *   Memory:    no allocation; MODEL_SESSION_MAX_BYTES static.
 *   Stack:     0.65 KB of frames below the caller on the deepest chain
 *              (-fstack-usage, -Os: this function 104, flash_region_log_save
 *              120, flash_region_append 184, the record's 24, its
 *              program-and-verify 96, the flash_fs program wrapper and its
 *              read-back 120), plus the FreeRTOS mutex call: budget 0.8 KB.
 *              A move to the other bank does not go through the append's
 *              frame, so it is shallower. The settings flush that runs from the same place
 *              goes as deep.
 *   Time:      NOT MEASURED ON A DEVICE YET (`session save` prints the
 *              milliseconds of this call). Estimated: with no move to the other bank it
 *              is two scans of the log (<= 4 KB each, in 64-byte SPI2
 *              reads; settings_store.h puts a 64 KB scan at ~100 ms, so
 *              ~7 ms each at most), one header and one payload program (2
 *              page programs, 3 when the payload crosses a page) and their
 *              read-backs: ~15 ms typical, < 40 ms with every page program
 *              at its maximum. When the log moves to its other bank (about
 *              one save in 18) one 4 KB sector erase, its blank check and
 *              one more program are added: < 0.5 s. The flash figures are the
 *              Winbond W25Q128JV datasheet's (rev. I, AC characteristics):
 *              page program tPP 0.4 ms typical, 3 ms maximum; sector erase
 *              tSE 45 ms typical, 400 ms maximum. A unit with another part
 *              (bench unit #2: a Zbit ZB25VQ128) is not covered by them.
 *              Fault bounds, never reached on a working bus: each flash
 *              primitive waits at most 5 s for the flash_fs mutex
 *              (FS_MUTEX_TIMEOUT_MS) when another task holds it, and a part
 *              that never leaves BUSY is given up after flash_fs.c's
 *              20,000,000-poll guard; either ends the save as FAILED.
 *
 * Returns true when a record was written. */
bool session_snapshot_save(void);

/* Invalidate the snapshot ("Reset to defaults", `session clear`): the next
 * Restore boot falls back to the settings record alone. Appends a 16-byte
 * tombstone when the newest record is a snapshot of this format; writes
 * NOTHING when there is none (an empty log, a tombstone, something this
 * build does not read). When the log takes no more records the region is
 * erased instead (an empty log is "no snapshot"). Input task. Returns false
 * only when a snapshot is still there (a flash error, writes disabled). */
bool session_snapshot_clear(void);

const session_status_t *session_snapshot_status(void);
const char *session_load_text(uint8_t load);
const char *session_save_text(uint8_t save);

/* Log occupancy for `session status` (a scan of the 4 KB log: not for a hot
 * path). false: no storage. */
bool session_snapshot_log_info(uint32_t *bytes_used, uint32_t *bytes_total, uint32_t *records);

#endif /* SESSION_STORE_H */
