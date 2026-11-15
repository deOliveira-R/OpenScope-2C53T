/*
 * OpenScope 2C53T - the shell's `session` command (model_shell.h)
 *
 *   session status   the snapshot the boot found and what became of it, the
 *                    boot apply's result with the properties it could not
 *                    put in force, the log, this session's writes
 *   session save     the settings flush and the snapshot save an orderly
 *                    power-off makes, without the power-off (the last
 *                    screen is not noted: the record keeps the one it has)
 *   session clear    invalidate the snapshot (the next Restore boot is the
 *                    settings record alone)
 *
 * save and clear run on the input task (input_call_sync): it is the one
 * writer of the settings record and of the snapshot, and the shell task's
 * stack is not the place for a flash log scan. The status core writes
 * through an emit callback, so tests/test_session.c runs it on the host.
 */
#include "model_shell.h"
#include "model_session.h"
#include "session_store.h"

#include <stdio.h>
#include <string.h>

#define SLINE 128u

void model_shell_session_status(model_emit_fn emit)
{
    char line[SLINE];
    const session_status_t *st = session_snapshot_status();
    const model_boot_report_t *br = model_boot_report();

    snprintf(line, sizeof line, "session: %s", session_load_text(st->load));
    emit(line);
    if (st->load == SESSION_LOAD_OK || st->load == SESSION_LOAD_STALE || st->load == SESSION_LOAD_CLEARED) {
        /* No clock on this device: the age of a snapshot is its sequence number. */
        snprintf(line, sizeof line, "  snapshot: seq %u, %u values, %u bytes, format %u",
                 (unsigned)st->load_seq, (unsigned)st->load_count, (unsigned)st->load_bytes,
                 (unsigned)MODEL_SESSION_VERSION);
        emit(line);
    }
    if (st->load == SESSION_LOAD_STALE) {
        snprintf(line, sizeof line, "  settings stamp: snapshot %08lX, record loaded %08lX",
                 (unsigned long)st->load_stamp, (unsigned long)st->settings_stamp);
        emit(line);
    }
    if (st->load == SESSION_LOAD_OK) {
        snprintf(line, sizeof line, "  loaded: %u kept; skipped %u unknown id, %u not restored by this build, "
                 "%u out of range", (unsigned)st->kept, (unsigned)st->unknown, (unsigned)st->excluded,
                 (unsigned)st->out_of_range);
        emit(line);
    }
    /* The boot apply covers the settings record's view too, snapshot or not. */
    snprintf(line, sizeof line, "  boot apply: %u kept, %u set, %u in force, %u refused",
             (unsigned)br->kept, (unsigned)br->set, (unsigned)br->applied, (unsigned)br->refused);
    emit(line);
    if (br->refused) {
        snprintf(line, sizeof line, "  refused:");
        for (uint16_t i = 0; i < prop_count(); i++) {
            const prop_desc_t *p = prop_at(i);
            if (model_boot_state(p) != BOOT_KEEP_REFUSED) continue;
            if (strlen(line) + strlen(p->name) + 2u > SLINE - 1u) { emit(line); snprintf(line, sizeof line, "   "); }
            strncat(line, " ", sizeof line - strlen(line) - 1u);
            strncat(line, p->name, sizeof line - strlen(line) - 1u);
        }
        emit(line);
    }
    uint32_t used = 0, total = 0, recs = 0;
    if (session_snapshot_log_info(&used, &total, &recs))
        snprintf(line, sizeof line, "  log: %lu records, %lu of %lu bytes used", (unsigned long)recs,
                 (unsigned long)used, (unsigned long)total);
    else
        snprintf(line, sizeof line, "  log: no storage");
    emit(line);
    if (st->save == SESSION_SAVE_OK)
        snprintf(line, sizeof line, "  last save: seq %u, %u values, %u bytes", (unsigned)st->save_seq,
                 (unsigned)st->save_count, (unsigned)st->save_bytes);
    else
        snprintf(line, sizeof line, "  last save: %s", session_save_text(st->save));
    emit(line);
    snprintf(line, sizeof line, "  this session: %u saves, %u clears, %u log erases, last flash status %ld",
             (unsigned)st->saves, (unsigned)st->clears, (unsigned)st->compactions,
             (long)st->last_flash_status);
    emit(line);
    snprintf(line, sizeof line, "  members: %u of %u properties (session list)",
             (unsigned)model_session_member_count(), (unsigned)prop_count());
    emit(line);
}

void model_shell_session_members(model_emit_fn emit)
{
    char line[SLINE];
    snprintf(line, sizeof line, "in the snapshot (%u):", (unsigned)model_session_member_count());
    for (uint16_t i = 0; i < prop_count(); i++) {
        const prop_desc_t *p = prop_at(i);
        if (model_session_excluded(p)) continue;
        if (strlen(line) + strlen(p->name) + 2u > 100u) { emit(line); snprintf(line, sizeof line, "   "); }
        strncat(line, " ", sizeof line - strlen(line) - 1u);
        strncat(line, p->name, sizeof line - strlen(line) - 1u);
    }
    emit(line);
    emit("left out:");
    for (uint8_t i = 0; i < model_session_exclusion_count; i++) {
        const prop_desc_t *p = prop_by_id(model_session_exclusions[i].id);
        snprintf(line, sizeof line, "  %-16s %s", p ? p->name : "?", model_session_exclusions[i].why);
        emit(line);
    }
    emit("  (read-only properties are never saved)");
}

/* ═══ usb_debug.c row ════════════════════════════════════════════════ */

#ifndef SESSION_SHELL_HOST
#include "FreeRTOS.h"
#include "task.h"
#include "usb_debug.h"
#include "input_handler.h"
#include "settings_store.h"

static void usb_emit(const char *line) { (void)usb_debug_printf("%s\r\n", line); }

/* On the input task; ms is the call itself, as the power-off will pay it. */
typedef struct { bool clear, ok; uint32_t ms; } session_call_t;
static void session_call_run(void *vc)
{
    session_call_t *c = (session_call_t *)vc;
    const TickType_t t0 = xTaskGetTickCount();
    c->ok = c->clear ? session_snapshot_clear() : session_snapshot_save();
    c->ms = (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
}

void cmd_session(const char *args)
{
    while (*args == ' ') args++;
    if (*args == '\0' || strcmp(args, "status") == 0) { model_shell_session_status(usb_emit); return; }
    if (strcmp(args, "list") == 0) { model_shell_session_members(usb_emit); return; }
    const bool save = strcmp(args, "save") == 0, clear = strcmp(args, "clear") == 0;
    if (!save && !clear) {
        (void)usb_debug_printf("usage: session [status] | list | save | clear\r\n");
        return;
    }
    if (save) {
        /* As the power-off does: the settings record, then the snapshot. */
        settings_flush_outcome_t o;
        if (input_settings_flush_sync(1000u, &o))
            (void)usb_debug_printf("settings: %s\r\n", settings_flush_outcome_text(o));
        else
            (void)usb_debug_printf("settings: flush request timed out (the input task saves when it is free)\r\n");
    }
    session_call_t c = { clear, false, 0u };
    /* A full log makes the save erase its sector first (<= 0.4 s). */
    if (!input_call_sync(session_call_run, &c, 3000u)) {
        (void)usb_debug_printf("TIMEOUT: input task busy (gallery/countdown/selfcal?); nothing written\r\n");
        return;
    }
    const session_status_t *st = session_snapshot_status();
    if (save) {
        if (c.ok)
            (void)usb_debug_printf("session: saved seq %u, %u values, %u bytes, in %lu ms%s\r\n",
                                   (unsigned)st->save_seq, (unsigned)st->save_count, (unsigned)st->save_bytes,
                                   (unsigned long)c.ms, st->compactions ? " (the log was erased first)" : "");
        else
            (void)usb_debug_printf("session: %s (flash status %ld)\r\n", session_save_text(st->save),
                                   (long)st->last_flash_status);
    } else {
        (void)usb_debug_printf(c.ok ? "session: no snapshot (the next Restore boot uses the settings record alone)\r\n"
                                    : "session: NOT cleared (flash status %ld)\r\n", (long)st->last_flash_status);
    }
}
#endif
