/*
 * OpenScope 2C53T - weak stand-ins for R1-K's key_model getters (UI round 1)
 *
 * status_bar.c reads what the arrows adjust and the active channel through
 * key_focus_get() / key_active_channel_get(), which R1-K exports from
 * key_model. Until that branch is merged these weak versions return today's
 * globals (input_handler.c), so this branch builds and behaves alone; the
 * strong definitions win at link time once they exist. Delete this file (and
 * its C_SOURCES line) after the merge.
 */
#include "ui.h"
#include "key_model.h"

extern volatile key_focus_t scope_key_focus;     /* input_handler.c */

__attribute__((weak)) uint8_t key_focus_get(void)
{
    return (uint8_t)scope_key_focus;
}

__attribute__((weak)) uint8_t key_active_channel_get(void)
{
    return (uint8_t)(active_channel ? 1u : 0u);
}
