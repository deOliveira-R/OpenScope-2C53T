/*
 * OpenScope 2C53T - the shell's raw route over the property model (ARCH-1 M1)
 *
 *   get <prop|glob> [...]     one "name=value[ unit][  # label]" line each
 *   set <prop> <value>        through the input task (model_exec.h)
 *   set <p>=<v> <p>=<v> ...   <= 8 pairs (<= 4 PF_HW), validated together
 *   list [prefix|glob]        name, type, range, value, flags
 *   list changed [glob]       only what differs from the built-in defaults
 *   get changed               the same set, as get lines (a profile of the differences)
 *   describe <prop>           type, range or labels, default, unit, saved
 *                             where, owner, whether the shell can set it
 *
 * The cores write through an emit callback (one line, no line ending), so
 * tests/test_model_exec.c runs them on the host. The cmd_* wrappers are the
 * usb_debug.c table's handlers.
 */
#ifndef MODEL_SHELL_H
#define MODEL_SHELL_H

#include "model.h"

typedef void (*model_emit_fn)(const char *line);

void model_shell_get(const char *args, model_emit_fn emit);
void model_shell_set(const char *args, model_emit_fn emit);
void model_shell_list(const char *args, model_emit_fn emit);
void model_shell_describe(const char *args, model_emit_fn emit);

/* The answer of `reset settings CONFIRM`: the status line, what is still
 * not at its default, what a reset keeps and why, what it never touches.
 * status / changed / left: model_reset_report_t; saved =
 * settings_flush_outcome_text() of the write. */
void model_shell_reset_report(uint8_t status, unsigned changed, unsigned left, const char *saved,
                              model_emit_fn emit);

/* `session status` and `session list` (session_shell.c): the snapshot the
 * boot found, the boot apply's result and refused list, the log; and the
 * members with the exclusion list and its reasons. */
void model_shell_session_status(model_emit_fn emit);
void model_shell_session_members(model_emit_fn emit);
/* usb_debug.c's row: session [status] | list | save | clear. */
void cmd_session(const char *args);

/* One get line for p at value v (what `get` prints). */
void model_shell_get_line(const prop_desc_t *p, int32_t v, char *out, size_t n);

/* usb_debug.c's rows (firmware only). */
void cmd_model_get(const char *args);
void cmd_model_set(const char *args);
void cmd_model_list(const char *args);
void cmd_model_describe(const char *args);
/* One set for another shell handler (a bench command that used to store the
 * variable itself): through the input task, as `set`. false: not set, and
 * the line "<STATUS> <name> (<why>)" has been printed. */
bool model_shell_request(uint16_t id, int32_t value);

#endif /* MODEL_SHELL_H */
