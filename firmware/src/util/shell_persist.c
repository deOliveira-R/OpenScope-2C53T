#include "shell_persist.h"

static bool     s_on = true;
static uint32_t s_skipped;

void shell_persist_reset(void)
{
    s_on = true;
    s_skipped = 0;
}

void shell_persist_set(bool on)
{
    if (on && !s_on)
        s_skipped = 0;
    if (!on && s_on)
        s_skipped = 0;
    s_on = on;
}

bool shell_persist_enabled(void)
{
    return s_on;
}

bool shell_persist_after_command(void)
{
    if (s_on)
        return true;
    if (s_skipped != UINT32_MAX)
        s_skipped++;
    return false;
}

uint32_t shell_persist_skipped(void)
{
    return s_skipped;
}
