/* Host test for src/util/shell_persist.c (the shell's `persist` switch). */
#include <stdio.h>
#include "shell_persist.h"

static int fails, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("  FAIL: %s\n", msg); } } while (0)

int main(void)
{
    shell_persist_reset();
    CHECK(shell_persist_enabled(), "on at boot");
    CHECK(shell_persist_after_command(), "on: every command requests a flush");
    CHECK(shell_persist_skipped() == 0, "on: nothing counted as skipped");

    shell_persist_set(false);
    CHECK(!shell_persist_enabled(), "off");
    for (int i = 0; i < 5; i++)
        CHECK(!shell_persist_after_command(), "off: no flush requested");
    CHECK(shell_persist_skipped() == 5, "off: 5 skipped requests counted");

    shell_persist_set(false);
    CHECK(shell_persist_skipped() == 5, "off again keeps the count");

    shell_persist_set(true);
    CHECK(shell_persist_enabled() && shell_persist_skipped() == 0, "on clears the count");
    CHECK(shell_persist_after_command(), "on again: flush requested");

    shell_persist_set(false);
    shell_persist_after_command();
    shell_persist_reset();
    CHECK(shell_persist_enabled() && shell_persist_skipped() == 0, "reset = boot state");

    printf("test_shell_persist: %d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
