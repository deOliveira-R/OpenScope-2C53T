/*
 * DAC owner (src/drivers/dac.c) write-log test, built with -DHOST_REGS:
 * the DAC is a plain struct, and every register store, clock enable, pin
 * setup and interrupt mask is logged in order.
 *
 *  1. Boot: the store sequence through the owner equals the one the
 *     drivers made before the owner existed (written out below from the
 *     previous scope_trigger.c / dac_output.c), value for value.
 *  2. A CH1 offset write while the generator runs changes only channel 1's
 *     data register and fires only channel 1's trigger: no DAC_CTRL store,
 *     no re-init, channel 2's half and data untouched. (This is why the
 *     scope_vpos generator guard is gone.)
 *  3. Starting and stopping the generator never changes channel 1's enable,
 *     trigger bits or data, whatever channel 1's half holds.
 *  Every DAC_CTRL store happens inside a mask section.
 *
 * Links dac.c and scope_trigger.c (its TMR13 half is target-only). The
 * generator side is driven through the owner calls dac_output.c makes, in
 * its order (dac_output_init, dac_output_apply, dac_output_stop).
 */
#include <stdio.h>
#include <string.h>

#include "dac.h"
#include "scope_trigger.h"
#include "siggen_wave.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

dac_host_regs_t dac_host_regs;

typedef struct { int what; uint32_t v; } ev_t;
static ev_t ev[256];
static int n_ev;

void dac_host_log(int what, uint32_t value)
{
    if (n_ev < (int)(sizeof ev / sizeof ev[0])) ev[n_ev++] = (ev_t){ what, value };
    /* The software-trigger bits clear themselves one APB clock later. */
    if (what == DAC_HOST_SWTRG) dac_host_regs.swtrg = 0;
}

static const char *ev_name(int w)
{
    static const char *n[] = { "CTRL", "SWTRG", "D1DTH12R", "D2DTH12R", "CLK_DAC",
                               "CLK_GPIOA", "PA4_ANALOG", "MASK", "UNMASK" };
    return (w >= 0 && w < (int)(sizeof n / sizeof n[0])) ? n[w] : "?";
}

static void log_reset(void) { n_ev = 0; }

/* The log without the mask events (the stores and setup steps only). */
static int stores(ev_t *out, int max)
{
    int k = 0;
    for (int i = 0; i < n_ev && k < max; i++)
        if (ev[i].what != DAC_HOST_MASK && ev[i].what != DAC_HOST_UNMASK) out[k++] = ev[i];
    return k;
}

/* Every CTRL store inside MASK..UNMASK; the sections are balanced. */
static void check_ctrl_masked(const char *what)
{
    int depth = 0, bad = 0;
    for (int i = 0; i < n_ev; i++) {
        if (ev[i].what == DAC_HOST_MASK) depth++;
        else if (ev[i].what == DAC_HOST_UNMASK) depth--;
        else if (ev[i].what == DAC_HOST_CTRL && depth != 1) bad++;
    }
    CHECK(bad == 0 && depth == 0, "%s: %d CTRL stores outside a mask section, depth %d", what, bad, depth);
}

static int count(int what)
{
    int k = 0;
    for (int i = 0; i < n_ev; i++) k += (ev[i].what == what);
    return k;
}

/* What dac_output.c asks of the owner. */
static void gen_init(void)    /* dac_output_init(), DAC part */
{
    dac_ch2_config(SIGGEN_DAC_TSEL_TMR7, false);
    dac_ch2_data_clear();
    dac_ch2_config(SIGGEN_DAC_TSEL_TMR7, true);
}
static void gen_apply(void)   /* dac_output_apply(), DAC part (self-heal) */
{
    if ((dac_ctrl_get() & ~SIGGEN_DAC_CH1_MASK) !=
        (siggen_dac_ctrl_ch2(0, SIGGEN_DAC_TSEL_TMR7, true) & ~SIGGEN_DAC_CH1_MASK))
        dac_ch2_config(SIGGEN_DAC_TSEL_TMR7, true);
}
static void gen_stop(void)    /* dac_output_stop(), DAC part */
{
    dac_ch2_output_zero();
}

/* 1. Boot, in main()'s order: dac_output_init(); siggen_init(NULL) with the
 * output off -> dac_output_stop(); fpga_init() -> scope_trigger_dac_raw(2048),
 * the first CH1 write, which initialises channel 1. */
static void test_boot_sequence(void)
{
    /* Before the owner (77c08dd), from reset (all DAC registers 0):
     * dac_output_init: dac_ctrl_ch2_write(TMR7, off); D2DTH12R = 0;
     *                  dac_ctrl_ch2_write(TMR7, on)
     * dac_output_stop: D2DTH12R &= ~0xFFF; ch2_write(SW, on); SWTRG = 2;
     *                  ch2_write(TMR7, on)
     * scope_trigger_dac_raw(2048) -> scope_trigger_dac_init (not inited):
     *   clocks DAC, GPIOA; PA4 analog; CTRL = (CTRL & 0xFFFF0000) | 0x3C;
     *   CTRL |= D1EN; then DHR12R1 = (DHR12R1 & ~0xFFF) | 2048; SWTRG |= 1
     *   (reads 0: the bits self-clear). */
    static const ev_t ref[] = {
        { DAC_HOST_CTRL, 0x10140000u }, { DAC_HOST_D2DTH12R, 0 }, { DAC_HOST_CTRL, 0x10150000u },
        { DAC_HOST_D2DTH12R, 0 }, { DAC_HOST_CTRL, 0x103D0000u }, { DAC_HOST_SWTRG, 2 },
        { DAC_HOST_CTRL, 0x10150000u },
        { DAC_HOST_CLK_DAC, 1 }, { DAC_HOST_CLK_GPIOA, 1 }, { DAC_HOST_PA4_ANALOG, 0 },
        { DAC_HOST_CTRL, 0x1015003Cu }, { DAC_HOST_CTRL, 0x1015003Du },
        { DAC_HOST_D1DTH12R, 0x800u }, { DAC_HOST_SWTRG, 1 },
    };
    const int nref = (int)(sizeof ref / sizeof ref[0]);

    memset(&dac_host_regs, 0, sizeof dac_host_regs);
    log_reset();
    gen_init();
    gen_stop();
    CHECK(!dac_ch1_ready(), "channel 1 initialised before its first use");
    scope_trigger_dac_raw(2048);

    ev_t got[64];
    const int n = stores(got, 64);
    CHECK(n == nref, "boot: %d stores, expected %d", n, nref);
    for (int i = 0; i < n && i < nref; i++)
        CHECK(got[i].what == ref[i].what && got[i].v == ref[i].v,
              "boot store %d: %s 0x%08X, expected %s 0x%08X", i, ev_name(got[i].what),
              (unsigned)got[i].v, ev_name(ref[i].what), (unsigned)ref[i].v);
    check_ctrl_masked("boot");
    CHECK(dac_host_regs.ctrl == 0x1015003Du, "boot CTRL 0x%08X", (unsigned)dac_host_regs.ctrl);
    CHECK(dac_ch1_ready() && scope_trigger_dac_last() == 2048u, "channel 1 state after boot");

    /* scope_vpos_init's write: data and trigger only. */
    log_reset();
    scope_trigger_dac_raw(1900);
    CHECK(n_ev == 4 && count(DAC_HOST_D1DTH12R) == 1 && count(DAC_HOST_SWTRG) == 1 &&
          count(DAC_HOST_CTRL) == 0, "second CH1 write: %d events", n_ev);

    /* Later init calls (the shell's `trig`, the X-Y bring-up) do nothing. */
    log_reset();
    scope_trigger_dac_init();
    dac_ch1_init();
    CHECK(n_ev == 0, "init after boot logged %d events", n_ev);
}

/* 2. CH1 offset write with the generator running. */
static void test_ch1_write_beside_generator(void)
{
    gen_apply();                                     /* generator started */
    dac_host_regs.d2dth12r = 0x0ABCu;                /* the DMA's last code */
    dac_host_regs.d1dth12r = 0xFFFF0000u | 0x123u;   /* upper bits kept by stock */
    const uint32_t ctrl0 = dac_host_regs.ctrl;
    CHECK((ctrl0 & ~SIGGEN_DAC_CH1_MASK) == 0x10150000u && (ctrl0 & 0xFFFFu) == 0x3Du,
          "running CTRL 0x%08X", (unsigned)ctrl0);

    log_reset();
    scope_trigger_dac_raw(0x0F0Fu);                  /* `set ch1.pos`, scope_vpos */
    CHECK(count(DAC_HOST_CTRL) == 0, "CH1 write stored DAC_CTRL %d times", count(DAC_HOST_CTRL));
    CHECK(count(DAC_HOST_D2DTH12R) == 0, "CH1 write touched D2DTH12R");
    CHECK(count(DAC_HOST_CLK_DAC) == 0 && count(DAC_HOST_PA4_ANALOG) == 0, "CH1 write re-initialised");
    int sw = 0, d1 = 0;
    for (int i = 0; i < n_ev; i++) {
        if (ev[i].what == DAC_HOST_SWTRG) { sw++; CHECK(ev[i].v == 1u, "SWTRG 0x%X", (unsigned)ev[i].v); }
        if (ev[i].what == DAC_HOST_D1DTH12R) { d1++; CHECK(ev[i].v == 0xFFFF0F0Fu, "DHR 0x%08X", (unsigned)ev[i].v); }
    }
    CHECK(sw == 1 && d1 == 1, "CH1 write: %d triggers, %d data stores", sw, d1);
    /* data first, then the trigger, both in one mask section */
    CHECK(n_ev == 4 && ev[0].what == DAC_HOST_MASK && ev[1].what == DAC_HOST_D1DTH12R &&
          ev[2].what == DAC_HOST_SWTRG && ev[3].what == DAC_HOST_UNMASK, "CH1 write sequence");
    CHECK(dac_host_regs.ctrl == ctrl0 && dac_host_regs.d2dth12r == 0x0ABCu,
          "CTRL 0x%08X D2 0x%X after CH1 write", (unsigned)dac_host_regs.ctrl,
          (unsigned)dac_host_regs.d2dth12r);

    /* Even a channel-2 half the owner did not expect does not provoke a
     * channel-1 re-init (the old check-then-init path is gone). */
    dac_host_regs.ctrl = 0x103D003Du;
    log_reset();
    scope_trigger_dac_set(5, 50);
    CHECK(count(DAC_HOST_CTRL) == 0 && dac_host_regs.ctrl == 0x103D003Du, "dac_set touched CTRL");
    gen_apply();                                     /* self-heal: back to run */
    CHECK(dac_host_regs.ctrl == 0x1015003Du, "self-heal CTRL 0x%08X", (unsigned)dac_host_regs.ctrl);
}

/* 3. Generator start/stop against any channel-1 state. */
static void test_generator_leaves_ch1(void)
{
    static const uint16_t ch1_halves[] = { 0x003Du, 0x5A3Du, 0xFFFFu, 0x0000u, 0x0038u };
    for (unsigned k = 0; k < sizeof ch1_halves / sizeof ch1_halves[0]; k++) {
        const uint32_t half = ch1_halves[k];
        dac_host_regs.ctrl = 0x10150000u | half;
        dac_host_regs.d1dth12r = 0x00000777u + k;
        const uint32_t d1 = dac_host_regs.d1dth12r;
        log_reset();
        for (int r = 0; r < 3; r++) {               /* stop, start, stop again */
            gen_stop();
            dac_host_regs.ctrl &= ~0x00010000u;      /* force the self-heal path */
            gen_apply();
        }
        gen_init();                                  /* even a re-init of channel 2 */
        gen_stop();
        int bad = 0;
        for (int i = 0; i < n_ev; i++) {
            if (ev[i].what == DAC_HOST_CTRL && (ev[i].v & SIGGEN_DAC_CH1_MASK) != half) bad++;
            if (ev[i].what == DAC_HOST_SWTRG && (ev[i].v & 1u)) bad++;
            if (ev[i].what == DAC_HOST_D1DTH12R || ev[i].what == DAC_HOST_PA4_ANALOG) bad++;
        }
        CHECK(bad == 0, "ch1 half 0x%04X: %d stores disturbed channel 1", (unsigned)half, bad);
        CHECK(count(DAC_HOST_CTRL) > 0 && count(DAC_HOST_SWTRG) == 4, "generator stores ran (%d CTRL, %d SWTRG)",
              count(DAC_HOST_CTRL), count(DAC_HOST_SWTRG));
        CHECK((dac_host_regs.ctrl & SIGGEN_DAC_CH1_MASK) == half && dac_host_regs.d1dth12r == d1,
              "ch1 half 0x%04X: CTRL 0x%08X D1 0x%08X after", (unsigned)half,
              (unsigned)dac_host_regs.ctrl, (unsigned)dac_host_regs.d1dth12r);
        CHECK((dac_host_regs.ctrl & ~SIGGEN_DAC_CH1_MASK) == 0x10150000u, "channel 2 left in run setup");
        check_ctrl_masked("generator");
    }
}

int main(void)
{
    test_boot_sequence();
    test_ch1_write_beside_generator();
    test_generator_leaves_ch1();
    printf("test_dac: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
