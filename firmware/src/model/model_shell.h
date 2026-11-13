/*
 * OpenScope 2C53T - the shell's raw route over the property model (ARCH-1 M1)
 *
 *   get <prop|glob> [...]     one "name=value[ unit][  # label]" line each
 *   set <prop> <value>        through the input task (model_exec.h)
 *   set <p>=<v> <p>=<v> ...   <= 8 pairs (<= 4 PF_HW), validated together
 *   list [prefix|glob]        name, type, range, value, flags
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

/* One get line for p at value v (what `get` prints). */
void model_shell_get_line(const prop_desc_t *p, int32_t v, char *out, size_t n);

/* usb_debug.c's rows (firmware only). */
void cmd_model_get(const char *args);
void cmd_model_set(const char *args);
void cmd_model_list(const char *args);
void cmd_model_describe(const char *args);

#endif /* MODEL_SHELL_H */
