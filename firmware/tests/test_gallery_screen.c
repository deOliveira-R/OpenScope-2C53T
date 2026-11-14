/*
 * Host tests for the screenshot gallery as a screen of the router
 * (src/ui/gallery_screen.c; ARCH-1 B5): the real screen, the real router
 * (screen.c), the real key logic (gallery_nav.c) and the real store
 * (gallery_store.c) over the region layer on a NOR model.
 *
 * What is checked: the layer's flags; navigation in the grid and the full
 * view; arm / disarm / delete against the store; the messages; closing (its
 * keys, another task, a store that is not there); the GRAB order (the
 * gallery before the self-cal lock and the globals, POWER never, nothing
 * below); the epoch (idle: still; every change moves it; a save made
 * elsewhere moves it); and the paint plan the display task executes: a move
 * is two cells, a prompt is the title, two keys before a frame are one
 * paint of the state they left.
 *
 * Build: make test-gallery-screen.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gallery_screen.h"
#include "gallery_nav.h"
#include "gallery_store.h"
#include "flash_regions.h"
#include "theme.h"

static int run, passed;
#define CHECK(name, cond) do { run++; if (cond) { passed++; printf("  %-72s[PASS]\n", name); } \
                               else { printf("  %-72s[FAIL]\n", name); } } while (0)

/* ── NOR model (as tests/test_gallery.c) ───────────────────────────── */
#define MODEL_SIZE FLASH_REGION_CHIP_SIZE
static uint8_t *mem;
static bool fail_programs;
static int m_read(void *ctx, uint32_t a, void *b, uint32_t n) { (void)ctx; memcpy(b, mem + a, n); return 0; }
static int m_erase(void *ctx, uint32_t a) { (void)ctx; memset(mem + a, 0xFF, FLASH_REGION_SECTOR_SIZE); return 0; }
static int m_program(void *ctx, uint32_t a, const void *d, uint32_t n)
{
    (void)ctx;
    if (fail_programs) return -1;
    const uint8_t *s = d;
    for (uint32_t i = 0; i < n; i++) mem[a + i] &= s[i];
    return 0;
}
static const flash_region_backend_t backend = { m_read, m_erase, m_program, NULL };

static uint8_t pixels[GALLERY_PIXEL_BYTES];
static const uint16_t pal[16] = { 0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0xFFE0, 0x07FF, 0xF81F,
                                  0x4208, 0x8410, 0xFD20, 0xC618, 0x2945, 0x18E3, 0x39E7, 0x0841 };

/* ── the paint's stand-in: the plan the display task would execute ─── */
static gallery_view_t g_drawn; static bool g_drawn_valid;
static int g_paints; static uint8_t g_plan; static char g_plans[64];
void gallery_ui_paint(scr_paint_t how)
{
    const gallery_view_t v = gallery_screen_view();
    g_plan = gallery_paint_plan(g_drawn_valid ? &g_drawn : NULL, &v, how);
    g_paints++;
    char t[8];
    snprintf(t, sizeof t, "%u;", g_plan);
    strncat(g_plans, t, sizeof g_plans - strlen(g_plans) - 1);
    g_drawn = v; g_drawn_valid = v.open;
}
/* The display task's gate (main.c layer_paint_update). */
static uint32_t g_gate; static bool g_gate_primed;
static int frames(int n)
{
    const int p0 = g_paints;
    for (int i = 0; i < n; i++) {
        const uint32_t e = gallery_screen.epoch();
        if (g_gate_primed && e == g_gate) continue;
        gallery_screen.draw(SCR_PAINT_UPDATE, 0);
        g_gate = e; g_gate_primed = true;
    }
    return g_paints - p0;
}
/* One frame after what was just done: the plan it painted (0xFF: no paint). */
static uint8_t frame_plan(void) { return frames(1) ? g_plan : 0xFFu; }

/* ── the router around it ──────────────────────────────────────────── */
static int g_base_keys, g_locked_asked, g_globals_asked, g_after;
static scr_res_t g_after_res; static bool g_lock_on;
static scr_res_t base_key(scr_key_t k) { (void)k; g_base_keys++; return SCR_HANDLED; }
static const screen_t S_SCOPE = { .name = "scope", .id = SCR_SCOPE, .mode = MODE_OSCILLOSCOPE, .instrument = 0,
                                  .key = base_key };
static bool h_locked(button_id_t b, bool l) { (void)b; (void)l; g_locked_asked++; return g_lock_on; }
static bool h_globals(button_id_t b, bool l)
{
    (void)l;
    g_globals_asked++;
    if (b != BTN_POWER) return false;
    /* the firmware's POWER global: the gallery is closed first (legacy_screens.c) */
    if (gallery_ui_is_open()) { gallery_ui_close(); (void)screen_close(SCR_GALLERY); }
    return true;
}
static void h_after(scr_res_t r) { g_after++; g_after_res = r; }
static const screen_hooks_t HOOKS = { .locked = h_locked, .globals = h_globals, .after_key = h_after };

static scr_res_t key(uint8_t raw) { return (scr_res_t)screen_route_key((button_id_t)raw); }
static gallery_view_t V(void) { return gallery_screen_view(); }
static uint8_t count(void) { gallery_index_t i; return gallery_scan(&i) == GALLERY_OK ? i.count : 0xFF; }
static uint32_t seq_at(uint8_t cell) { gallery_index_t i; (void)gallery_scan(&i); return i.seq[cell]; }
static const char *described(void)
{
    static char d[24];
    gallery_screen.describe(d, sizeof d);
    return d;
}

/* A scope with `shots` screenshots stored and the gallery closed. */
static void boot(unsigned shots)
{
    if (!mem) mem = malloc(MODEL_SIZE);
    memset(mem, 0xFF, MODEL_SIZE);
    fail_programs = false;
    if (flash_regions_init(&backend) != FLASH_REGION_OK) { printf("FATAL init\n"); exit(2); }
    for (unsigned i = 0; i < shots; i++) {
        memset(pixels, (int)(0x11u * (i + 1u)), sizeof pixels);
        if (gallery_save(pixels, pal, NULL, NULL) != GALLERY_OK) { printf("FATAL save\n"); exit(2); }
    }
    theme_init(THEME_DARK_BLUE);
    gallery_ui_close();
    screen_reset();
    screen_register(&S_SCOPE); screen_register(&gallery_screen);
    screen_set_hooks(&HOOKS);
    screen_init(SCR_SCOPE);
    g_base_keys = g_locked_asked = g_globals_asked = g_after = 0; g_lock_on = false;
    g_paints = 0; g_plans[0] = '\0'; g_drawn_valid = false; g_gate_primed = false;
}
/* Long SAVE and the stack mirror: open the gallery, push its layer, paint it. */
static void open_gallery(void)
{
    gallery_screen_open();
    (void)screen_push(SCR_GALLERY);
    (void)frames(1);
    g_plans[0] = '\0';
}

/* ── tests ─────────────────────────────────────────────────────────── */

static void test_not_ready(void)
{
    printf("a store that is not there (before the flash is bound)\n");
    theme_init(THEME_DARK_BLUE);
    screen_reset();
    screen_register(&S_SCOPE); screen_register(&gallery_screen);
    screen_set_hooks(&HOOKS);
    screen_init(SCR_SCOPE);
    gallery_screen_open();
    (void)screen_push(SCR_GALLERY);
    CHECK("it opens all the same (the paint says \"flash not ready\")", gallery_ui_is_open());
    CHECK("any key leaves: the layer is popped, a full repaint asked",
          key(BTN_LEFT) == SCR_DRAW_FULL && !gallery_ui_is_open() && screen_top() == SCR_SCOPE);
}

static void test_screen_entry(void)
{
    printf("the screen\n");
    CHECK("name gallery, id SCR_GALLERY, no mode, no instrument",
          strcmp(gallery_screen.name, "gallery") == 0 && gallery_screen.id == SCR_GALLERY &&
          gallery_screen.mode == MODE_COUNT && gallery_screen.instrument == SCR_INSTRUMENT_NONE);
    CHECK("OPAQUE | GRAB | NO_BARS: the whole LCD and every key but POWER",
          gallery_screen.flags == (SCR_F_OPAQUE | SCR_F_GRAB | SCR_F_NO_BARS));
    CHECK("it has key, close, draw, epoch and describe; it shows no property group",
          gallery_screen.key && gallery_screen.close && gallery_screen.draw && gallery_screen.epoch &&
          gallery_screen.describe && gallery_screen.groups == 0);
    boot(3);
    open_gallery();
    CHECK("while it is up: no bars, no popups, nothing of the base shown",
          !screen_bars_wanted() && !screen_popups_allowed() && screen_groups() == 0);
}

static void test_open(void)
{
    printf("open\n");
    boot(5);
    const uint8_t opens = V().opens;
    gallery_screen_open();
    CHECK("open: the grid, the newest shot selected, nothing armed",
          gallery_ui_is_open() && V().open && V().sel == 0 && !V().full && !V().armed &&
          V().msg == GALLERY_MSG_NONE && V().opens == (uint8_t)(opens + 1u));
    CHECK("describe: gal=grid/0", strcmp(described(), "gal=grid/0") == 0);
    gallery_screen_open();
    CHECK("open while open: nothing (the selection is kept)", V().opens == (uint8_t)(opens + 1u));
    (void)screen_push(SCR_GALLERY);
    CHECK("the first frame paints the whole grid, once", frame_plan() == GALLERY_PAINT_ALL && frames(100) == 0);
}

static void test_grid_nav(void)
{
    printf("the grid: 5 shots (cells 0..4; row 0 = 0 1 2, row 1 = 3 4)\n");
    boot(5);
    open_gallery();
    CHECK("RIGHT: the next shot; the paint is the two cells only",
          key(BTN_RIGHT) == SCR_DRAW && V().sel == 1 && frame_plan() == GALLERY_PAINT_CELLS);
    CHECK("DOWN: a row down", key(BTN_DOWN) == SCR_DRAW && V().sel == 4 && frame_plan() == GALLERY_PAINT_CELLS);
    CHECK("DOWN on the last row: nothing, no paint",
          key(BTN_DOWN) == SCR_HANDLED && V().sel == 4 && frames(3) == 0);
    CHECK("RIGHT on the last shot wraps to the first", key(BTN_RIGHT) == SCR_DRAW && V().sel == 0);
    CHECK("LEFT on the first wraps to the last", key(BTN_LEFT) == SCR_DRAW && V().sel == 4);
    CHECK("UP: a row up", key(BTN_UP) == SCR_DRAW && V().sel == 1);
    CHECK("UP on the first row: nothing", key(BTN_UP) == SCR_HANDLED && V().sel == 1);
    (void)frames(1); g_plans[0] = '\0';
    (void)key(BTN_RIGHT); (void)key(BTN_LEFT); (void)key(BTN_DOWN);    /* 1 -> 2 -> 1 -> 4 */
    CHECK("three keys before a frame: one paint, of the cell left and the cell reached",
          V().sel == 4 && frames(5) == 1 && g_plan == GALLERY_PAINT_CELLS && g_drawn.sel == 4);
    (void)key(BTN_RIGHT); (void)key(BTN_LEFT);
    CHECK("there and back before a frame: no paint", V().sel == 4 && frames(5) == 0);
    CHECK("the other keys (MOVE, CH1, AUTO, long OK ...) do nothing and are not passed on",
          key(BTN_MOVE) == SCR_HANDLED && key(BTN_CH1) == SCR_HANDLED && key(BTN_AUTO) == SCR_HANDLED &&
          key(BTN_OK | BTN_LONG_FLAG) == SCR_HANDLED && key(BTN_MENU | BTN_LONG_FLAG) == SCR_DRAW_FULL);
}

static void test_full_view(void)
{
    printf("the full-screen view\n");
    boot(4);
    open_gallery();
    (void)key(BTN_RIGHT); (void)frames(1);
    CHECK("OK: the selected shot full screen (a whole paint)",
          key(BTN_OK) == SCR_DRAW && V().full && V().sel == 1 && frame_plan() == GALLERY_PAINT_ALL &&
          strcmp(described(), "gal=full/1") == 0);
    CHECK("RIGHT: the next shot (a whole paint)", key(BTN_RIGHT) == SCR_DRAW && V().sel == 2 &&
          frame_plan() == GALLERY_PAINT_ALL);
    CHECK("LEFT twice: back and back", key(BTN_LEFT) == SCR_DRAW && key(BTN_LEFT) == SCR_DRAW && V().sel == 0);
    CHECK("UP / DOWN do nothing there", key(BTN_UP) == SCR_HANDLED && key(BTN_DOWN) == SCR_HANDLED && V().sel == 0);
    (void)frames(1);
    CHECK("OK: back to the grid, the selection kept", key(BTN_OK) == SCR_DRAW && !V().full && V().sel == 0 &&
          frame_plan() == GALLERY_PAINT_ALL);
    (void)key(BTN_OK);
    CHECK("MENU in the full view: back to the grid, the gallery stays",
          key(BTN_MENU) == SCR_DRAW && !V().full && gallery_ui_is_open() && screen_top() == SCR_GALLERY);
    boot(0);
    open_gallery();
    CHECK("an empty gallery: OK, SAVE and the arrows do nothing",
          key(BTN_OK) == SCR_HANDLED && key(BTN_SAVE) == SCR_HANDLED && key(BTN_RIGHT) == SCR_HANDLED &&
          !V().full && !V().armed);
}

static void test_delete(void)
{
    printf("delete: SAVE arms, SAVE again deletes, any other key disarms\n");
    boot(5);
    open_gallery();
    (void)key(BTN_RIGHT); (void)frames(1);
    const uint32_t victim = seq_at(1), next = seq_at(2);
    CHECK("SAVE arms: the prompt in the title bar, nothing deleted",
          key(BTN_SAVE) == SCR_DRAW && V().armed && count() == 5 && frame_plan() == GALLERY_PAINT_PROMPT &&
          strcmp(described(), "gal=grid/1/del") == 0);
    CHECK("an arrow only disarms: the selection stays on the shot the prompt named, the title comes back",
          key(BTN_RIGHT) == SCR_DRAW && !V().armed && V().sel == 1 && count() == 5 &&
          frame_plan() == GALLERY_PAINT_TITLE);
    (void)key(BTN_SAVE);
    const uint32_t c0 = V().content;
    CHECK("SAVE, SAVE: that shot is gone from the store",
          key(BTN_SAVE) == SCR_DRAW && count() == 4 && seq_at(1) == next && seq_at(0) != victim &&
          gallery_find(victim, &(uint32_t){ 0 }) != GALLERY_OK);
    CHECK("the selection is on the shot that took its place; a whole repaint (the thumbnails moved)",
          V().sel == 1 && !V().armed && V().content != c0 && frame_plan() == GALLERY_PAINT_ALL);
    (void)key(BTN_LEFT); (void)key(BTN_LEFT);                    /* wraps to the last */
    (void)key(BTN_SAVE); (void)key(BTN_SAVE);
    CHECK("deleting the last shot: the selection clamps to the new last", count() == 3 && V().sel == 2);

    boot(2);
    open_gallery();
    (void)key(BTN_OK); (void)frames(1);
    CHECK("full view: SAVE arms (a strip over the shot)", key(BTN_SAVE) == SCR_DRAW && V().armed &&
          frame_plan() == GALLERY_PAINT_PROMPT);
    CHECK("... OK disarms and stays in the full view: the shot again",
          key(BTN_OK) == SCR_DRAW && !V().armed && V().full && frame_plan() == GALLERY_PAINT_SHOT);
    (void)key(BTN_SAVE);
    CHECK("... SAVE, SAVE deletes and shows the next shot full screen",
          key(BTN_SAVE) == SCR_DRAW && count() == 1 && V().full && V().sel == 0 &&
          frame_plan() == GALLERY_PAINT_ALL);
    (void)key(BTN_SAVE); (void)key(BTN_SAVE);
    CHECK("deleting the only shot: back to the (empty) grid", count() == 0 && !V().full && V().sel == 0);

    boot(3);
    open_gallery();
    (void)key(BTN_SAVE);
    CHECK("MENU while armed closes the gallery, nothing deleted",
          key(BTN_MENU) == SCR_DRAW_FULL && !gallery_ui_is_open() && count() == 3);

    boot(3);
    open_gallery();
    (void)key(BTN_SAVE);
    fail_programs = true;
    CHECK("a delete the flash refuses: the shot stays, \"Delete failed\" in the title",
          key(BTN_SAVE) == SCR_DRAW && count() == 3 && V().msg == GALLERY_MSG_DELETE_FAILED &&
          frame_plan() == (GALLERY_PAINT_ALL | GALLERY_PAINT_TITLE));
    fail_programs = false;
    CHECK("a move keeps the message (the title is not repainted)",
          key(BTN_RIGHT) == SCR_DRAW && V().msg == GALLERY_MSG_DELETE_FAILED &&
          frame_plan() == GALLERY_PAINT_CELLS);
    CHECK("the next key that repaints the title drops it",
          key(BTN_SAVE) == SCR_DRAW && V().msg == GALLERY_MSG_NONE);
    boot(3);
    open_gallery();
    (void)key(BTN_OK); (void)key(BTN_SAVE);
    fail_programs = true;
    CHECK("... in the full view: no message (as before)",
          key(BTN_SAVE) == SCR_DRAW && V().msg == GALLERY_MSG_NONE && V().full);
    fail_programs = false;
}

static void test_close(void)
{
    printf("close\n");
    boot(3);
    open_gallery();
    g_after = 0;
    CHECK("MENU on the grid closes: the layer popped, a full repaint asked",
          key(BTN_MENU) == SCR_DRAW_FULL && !gallery_ui_is_open() && screen_top() == SCR_SCOPE &&
          screen_snapshot().n == 1 && screen_last_handler() == SCR_GALLERY && g_after == 1 &&
          g_after_res == SCR_DRAW_FULL);
    CHECK("then the base gets the keys", key(BTN_UP) == SCR_HANDLED && g_base_keys == 1);
    open_gallery();
    CHECK("long SAVE closes too", key(BTN_SAVE | BTN_LONG_FLAG) == SCR_DRAW_FULL && !gallery_ui_is_open());
    open_gallery();
    (void)key(BTN_RIGHT);
    CHECK("reopened: the newest shot again, a whole paint",
          (key(BTN_MENU), gallery_screen_open(), (void)screen_push(SCR_GALLERY), V().sel == 0) &&
          frame_plan() == GALLERY_PAINT_ALL);

    boot(3);
    open_gallery();
    g_base_keys = 0;
    gallery_ui_close();                                          /* the shell's `mode`, from its task */
    CHECK("closed by another task: a flag; the layer is still on the stack until the input task looks",
          !gallery_ui_is_open() && screen_top() == SCR_GALLERY);
    CHECK("the next key pops the layer and goes to the screen under it",
          key(BTN_UP) == SCR_HANDLED && screen_top() == SCR_SCOPE && g_base_keys == 1 &&
          screen_last_handler() == SCR_SCOPE);
    open_gallery();
    screen_switch(SCR_SCOPE);                                    /* a mode switch pops every layer */
    CHECK("a mode switch under it closes it", !gallery_ui_is_open() && screen_top() == SCR_SCOPE);
}

static void test_grab_order(void)
{
    printf("GRAB order: the gallery first, POWER never, nothing below\n");
    boot(4);
    open_gallery();
    g_lock_on = true;
    CHECK("an arrow: the gallery takes it before the self-cal lock and the globals are asked",
          key(BTN_RIGHT) == SCR_DRAW && V().sel == 1 && g_locked_asked == 0 && g_globals_asked == 0 &&
          g_base_keys == 0 && screen_last_handler() == SCR_GALLERY);
    CHECK("SAVE is the gallery's there (arm delete), not the screenshot global",
          key(BTN_SAVE) == SCR_DRAW && V().armed && g_globals_asked == 0);
    (void)key(BTN_OK);
    g_lock_on = false;
    const gallery_view_t v0 = V();
    CHECK("POWER is never the gallery's: the globals get it, and close the gallery first",
          key(BTN_POWER) == SCR_HANDLED && g_globals_asked == 1 && screen_last_handler() == SCR_COUNT &&
          !gallery_ui_is_open() && screen_top() == SCR_SCOPE && V().sel == v0.sel && g_base_keys == 0);
}

static void test_epoch(void)
{
    printf("the epoch: still when idle, moved by every change\n");
    boot(4);
    open_gallery();
    const uint32_t e0 = gallery_screen.epoch();
    int still = 1;
    for (int i = 0; i < 100; i++) still &= gallery_screen.epoch() == e0;
    CHECK("idle: the same, and 200 frames paint nothing", still && frames(200) == 0);
    (void)key(BTN_RIGHT);
    const uint32_t e1 = gallery_screen.epoch();
    (void)key(BTN_OK);
    const uint32_t e2 = gallery_screen.epoch();
    (void)key(BTN_SAVE);
    const uint32_t e3 = gallery_screen.epoch();
    (void)key(BTN_OK);
    const uint32_t e4 = gallery_screen.epoch();
    CHECK("a move, the full view, arming and disarming each move it",
          e1 != e0 && e2 != e1 && e3 != e2 && e4 != e3 && e4 == e2);
    (void)frames(1);
    memset(pixels, 0x77, sizeof pixels);
    CHECK("a screenshot saved while it is open (the shell's `screenshot save`): a whole repaint",
          gallery_save(pixels, pal, NULL, NULL) == GALLERY_OK && frame_plan() == GALLERY_PAINT_ALL &&
          frames(20) == 0);
    theme_init(THEME_CLASSIC_GREEN);
    CHECK("a theme change moves it", gallery_screen.epoch() != g_gate);
    theme_init(THEME_DARK_BLUE);
}

static void test_plan(void)
{
    printf("the paint plan\n");
    gallery_view_t d = { .open = true, .opens = 3, .sel = 2, .full = false, .armed = false, .msg = 0, .content = 9 };
    gallery_view_t v = d;
    CHECK("nothing changed: nothing", gallery_paint_plan(&d, &v, SCR_PAINT_UPDATE) == 0);
    CHECK("FULL / RESTORE: everything, whatever was drawn",
          gallery_paint_plan(&d, &v, SCR_PAINT_FULL) == GALLERY_PAINT_ALL &&
          gallery_paint_plan(&d, &v, SCR_PAINT_RESTORE) == GALLERY_PAINT_ALL);
    CHECK("nothing of it on the LCD yet: everything", gallery_paint_plan(NULL, &v, SCR_PAINT_UPDATE) == GALLERY_PAINT_ALL);
    v.opens = 4;
    CHECK("a new visit: everything", gallery_paint_plan(&d, &v, SCR_PAINT_UPDATE) == GALLERY_PAINT_ALL);
    v = d; v.content = 10;
    CHECK("other shots: everything", gallery_paint_plan(&d, &v, SCR_PAINT_UPDATE) == GALLERY_PAINT_ALL);
    v = d; v.sel = 5;
    CHECK("grid, the selection moved: the two cells", gallery_paint_plan(&d, &v, SCR_PAINT_UPDATE) == GALLERY_PAINT_CELLS);
    v = d; v.armed = true;
    CHECK("armed: the prompt", gallery_paint_plan(&d, &v, SCR_PAINT_UPDATE) == GALLERY_PAINT_PROMPT);
    d.armed = true; v = d; v.armed = false;
    CHECK("grid, disarmed: the title", gallery_paint_plan(&d, &v, SCR_PAINT_UPDATE) == GALLERY_PAINT_TITLE);
    d.full = true; v = d; v.armed = false;
    CHECK("full view, disarmed: the shot", gallery_paint_plan(&d, &v, SCR_PAINT_UPDATE) == GALLERY_PAINT_SHOT);
    d.armed = false; v = d; v.sel = 0;
    CHECK("full view, another shot: everything", gallery_paint_plan(&d, &v, SCR_PAINT_UPDATE) == GALLERY_PAINT_ALL);
    v = d; v.full = false;
    CHECK("full view -> grid: everything", gallery_paint_plan(&d, &v, SCR_PAINT_UPDATE) == GALLERY_PAINT_ALL);
    v.armed = true;
    CHECK("a whole paint of an armed view carries the prompt",
          gallery_paint_plan(&d, &v, SCR_PAINT_UPDATE) == (GALLERY_PAINT_ALL | GALLERY_PAINT_PROMPT));
    v = d; v.open = false;
    CHECK("closed: nothing to paint", gallery_paint_plan(&d, &v, SCR_PAINT_FULL) == 0);
}

int main(void)
{
    test_not_ready();
    test_screen_entry();
    test_open();
    test_grid_nav();
    test_full_view();
    test_delete();
    test_close();
    test_grab_order();
    test_epoch();
    test_plan();
    printf("\n%d/%d passed\n", passed, run);
    free(mem);
    return passed == run ? 0 : 1;
}
