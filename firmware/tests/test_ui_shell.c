/*
 * Host test for the UI route's token parser (src/model/ui_shell.c, ARCH-1 M2):
 *
 *   - key names, any case; POWER found (so callers can refuse it)
 *   - KEY, KEY*n (1..50), long:KEY, wait:<ms> (0..5000)
 *   - bad tokens: unknown key, KEY*0, KEY*51, KEY*, KEY*x, wait:, wait:-1,
 *     wait:5001, long:UP (no long event), long:KEY*2
 *   - POWER refused as a key, as KEY*n and as long:POWER
 *   - the whole line is checked first: the bad token's index and text, and
 *     the event count of a good line
 *   - the long set is KEY_LONG_MASK's
 *
 * Build/run:  make -C firmware test-ui-shell
 */
#include "ui_shell.h"
#include "ui.h"
#include "key_model.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static ui_parse_t one(const char *s, ui_tok_t *t)
{
    char tok[24];
    const char *p = s;
    return ui_seq_next(&p, t, tok, sizeof tok);
}

static void test_names(void)
{
    CHECK(ui_key_find("MENU", 4) == BTN_MENU);
    CHECK(ui_key_find("menu", 4) == BTN_MENU);
    CHECK(ui_key_find("Ch2", 3) == BTN_CH2);
    CHECK(ui_key_find("OK", 2) == BTN_OK);
    CHECK(ui_key_find("TRIGGER", 7) == BTN_TRIGGER);
    CHECK(ui_key_find("POWER", 5) == BTN_POWER);
    CHECK(ui_key_find("MEN", 3) == -1);
    CHECK(ui_key_find("MENUX", 5) == -1);
    CHECK(ui_key_find("", 0) == -1);
    CHECK(strcmp(ui_key_name(BTN_SELECT), "SELECT") == 0);
    CHECK(strcmp(ui_key_name(BTN_NONE), "?") == 0);
    /* every key from BTN_CH1 to BTN_OK has a name that finds it back */
    for (int b = BTN_CH1; b <= BTN_OK; b++) {
        const char *n = ui_key_name((uint8_t)b);
        CHECK(strcmp(n, "?") != 0 && ui_key_find(n, strlen(n)) == b);
    }
    /* the long set is KEY_LONG_MASK's: the arrows have none, MENU/OK/SAVE do */
    CHECK(ui_key_has_long(BTN_MENU) && ui_key_has_long(BTN_OK) && ui_key_has_long(BTN_SAVE));
    CHECK(ui_key_has_long(BTN_SELECT) && ui_key_has_long(BTN_CH1) && ui_key_has_long(BTN_AUTO));
    CHECK(!ui_key_has_long(BTN_UP) && !ui_key_has_long(BTN_DOWN) &&
          !ui_key_has_long(BTN_LEFT) && !ui_key_has_long(BTN_RIGHT));
    CHECK(!ui_key_has_long(BTN_POWER));
}

static void test_tokens(void)
{
    ui_tok_t t;
    CHECK(one("UP", &t) == UI_PARSE_OK && t.kind == UI_TOK_PRESS && t.key == BTN_UP && t.count == 1);
    CHECK(one("  up  ", &t) == UI_PARSE_OK && t.key == BTN_UP);
    CHECK(one("UP*5", &t) == UI_PARSE_OK && t.kind == UI_TOK_PRESS && t.key == BTN_UP && t.count == 5);
    CHECK(one("down*50", &t) == UI_PARSE_OK && t.count == 50);
    CHECK(one("long:MENU", &t) == UI_PARSE_OK && t.kind == UI_TOK_LONG && t.key == BTN_MENU &&
          t.count == 1);
    CHECK(one("LONG:select", &t) == UI_PARSE_OK && t.kind == UI_TOK_LONG && t.key == BTN_SELECT);
    CHECK(one("wait:250", &t) == UI_PARSE_OK && t.kind == UI_TOK_WAIT && t.ms == 250 && t.count == 0);
    CHECK(one("wait:0", &t) == UI_PARSE_OK && t.ms == 0);
    CHECK(one("WAIT:5000", &t) == UI_PARSE_OK && t.ms == 5000);
    CHECK(one("", &t) == UI_PARSE_END);
    CHECK(one("   ", &t) == UI_PARSE_END);

    /* bad tokens */
    CHECK(one("UPP", &t) == UI_PARSE_BAD_KEY);
    CHECK(one("BTN_UP", &t) == UI_PARSE_BAD_KEY);
    CHECK(one("UP*0", &t) == UI_PARSE_BAD_FORM);
    CHECK(one("UP*51", &t) == UI_PARSE_BAD_FORM);
    CHECK(one("UP*", &t) == UI_PARSE_BAD_FORM);
    CHECK(one("UP*x", &t) == UI_PARSE_BAD_FORM);
    CHECK(one("UP*-1", &t) == UI_PARSE_BAD_FORM);
    CHECK(one("UP*999999", &t) == UI_PARSE_BAD_FORM);
    CHECK(one("UP*2*2", &t) == UI_PARSE_BAD_FORM);
    CHECK(one("*3", &t) == UI_PARSE_BAD_KEY);
    CHECK(one("wait:", &t) == UI_PARSE_BAD_FORM);
    CHECK(one("wait:-1", &t) == UI_PARSE_BAD_FORM);
    CHECK(one("wait:5001", &t) == UI_PARSE_BAD_FORM);
    CHECK(one("wait:1s", &t) == UI_PARSE_BAD_FORM);
    CHECK(one("long:", &t) == UI_PARSE_BAD_KEY);
    CHECK(one("long:UP", &t) == UI_PARSE_NO_LONG);
    CHECK(one("long:MENU*2", &t) == UI_PARSE_BAD_FORM);
    CHECK(one("long:FOO", &t) == UI_PARSE_BAD_KEY);

    /* POWER refused in every form */
    CHECK(one("POWER", &t) == UI_PARSE_POWER);
    CHECK(one("power*2", &t) == UI_PARSE_POWER);
    CHECK(one("long:POWER", &t) == UI_PARSE_POWER);
}

static void test_line(void)
{
    unsigned ntok, ev, bad;
    char tok[24];
    /* the plan's example: 9 events in 5 tokens */
    CHECK(ui_seq_check("MENU DOWN DOWN RIGHT UP*5", &ntok, &ev, &bad, tok, sizeof tok) == UI_PARSE_OK);
    CHECK(ntok == 5 && ev == 9 && bad == 0);
    CHECK(ui_seq_check("long:SELECT wait:500 OK", &ntok, &ev, &bad, tok, sizeof tok) == UI_PARSE_OK);
    CHECK(ntok == 3 && ev == 2);
    CHECK(ui_seq_check("wait:100", &ntok, &ev, &bad, tok, sizeof tok) == UI_PARSE_OK && ntok == 1 && ev == 0);
    CHECK(ui_seq_check("\tUP\t\tDOWN\r\n", &ntok, &ev, &bad, tok, sizeof tok) == UI_PARSE_OK && ntok == 2);
    CHECK(ui_seq_check("", &ntok, &ev, &bad, tok, sizeof tok) == UI_PARSE_END && ntok == 0);
    CHECK(ui_seq_check(NULL, &ntok, &ev, &bad, tok, sizeof tok) == UI_PARSE_END);
    /* the first bad token's index (1-based) and text; nothing counted */
    CHECK(ui_seq_check("MENU DOWN FOO UP", &ntok, &ev, &bad, tok, sizeof tok) == UI_PARSE_BAD_KEY);
    CHECK(bad == 3 && strcmp(tok, "FOO") == 0 && ntok == 0 && ev == 0);
    CHECK(ui_seq_check("UP UP POWER", &ntok, &ev, &bad, tok, sizeof tok) == UI_PARSE_POWER);
    CHECK(bad == 3 && strcmp(tok, "POWER") == 0);
    CHECK(ui_seq_check("long:UP", &ntok, &ev, &bad, tok, sizeof tok) == UI_PARSE_NO_LONG && bad == 1);
    CHECK(ui_seq_check("UP*60 POWER", &ntok, &ev, &bad, tok, sizeof tok) == UI_PARSE_BAD_FORM && bad == 1);
    /* a long token's text is truncated to the buffer, still NUL-terminated */
    char small[5];
    CHECK(ui_seq_check("ABCDEFGHIJ", &ntok, &ev, &bad, small, sizeof small) == UI_PARSE_BAD_KEY);
    CHECK(strcmp(small, "ABCD") == 0);
    /* a full 128-character line of presses (the shell's line buffer) */
    char line[128];
    line[0] = '\0';
    while (strlen(line) + 3 < sizeof line - 8) strcat(line, "UP ");
    CHECK(ui_seq_check(line, &ntok, &ev, &bad, tok, sizeof tok) == UI_PARSE_OK && ev == ntok && ntok > 30);
    /* every result has a message */
    for (int r = UI_PARSE_OK; r <= UI_PARSE_NO_LONG; r++) CHECK(strcmp(ui_parse_text((ui_parse_t)r), "?") != 0);
}

int main(void)
{
    test_names();
    test_tokens();
    test_line();
    printf("test_ui_shell: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
