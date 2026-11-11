/*
 * test_usb.c — host tests for the USB device layer (make test-usb).
 *
 *  1. Descriptors: the composite device + configuration bytes the firmware
 *     sends (src/util/usb_comp_desc.c, the same object file's tables) pass a
 *     strict structural checker (usb_desc_check): lengths and types, the
 *     wTotalLength sum, bNumInterfaces, IAD placement and range, CDC
 *     functional descriptors and their interface numbers, unique endpoints,
 *     full-speed packet sizes, MSC 08/06/50 with bulk IN/OUT, and the packet
 *     memory layout of src/usb_conf.h (no overlap with the BTABLE or each
 *     other, within the AT32F403A's 512 bytes, every endpoint covered).
 *     The library's CDC-only descriptor (cdc_desc.c, transcribed) passes too
 *     and the composite's CDC part is byte-identical to it. Twenty mutants of
 *     the composite descriptor and the PMA layout must each be rejected.
 *  2. The endpoint-register CTR fix in src/usb_conf.h: on a modelled rc_w0
 *     register, clearing one completion flag while the other completes
 *     mid-read-modify-write keeps the other (the library's macro loses it).
 *  3. usb_mode_sm: boot CDC, switch, configured, 3 s fallback, no retry
 *     loop, share off, no-host, tick wrap.
 *  4. raw_block: the `logo blk` receiver.
 */
#include "usb_comp_desc.h"
#include "usb_mode_sm.h"
#include "raw_block.h"
#include "cal_backup_core.h"
#include "usb_conf.h"            /* via tests/stubs_usb: PMA addresses + the CTR macros */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

usb_stub_regs_t usb_stub_regs;
void usb_delay_ms(uint32_t ms) { (void)ms; }
void usb_delay_us(uint32_t us) { (void)us; }

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ── 1. descriptors ─────────────────────────────────────────────────── */

static const usb_pma_buf_t k_pma[] = {
    { 0x80, EPT0_TX_ADDR, 64 }, { 0x00, EPT0_RX_ADDR, 64 },
    { 0x81, EPT1_TX_ADDR, 64 }, { 0x01, EPT1_RX_ADDR, 64 },
    { 0x82, EPT2_TX_ADDR, 64 },
    { 0x83, EPT3_TX_ADDR, 64 }, { 0x03, EPT3_RX_ADDR, 64 },
};
static const usb_pma_layout_t k_layout = { 512, 64, k_pma, sizeof k_pma / sizeof k_pma[0] };

/* The AT32 library's CDC-only descriptors (cdc_desc.c g_usbd_descriptor /
 * g_usbd_configuration), the device unit #2's macOS accepts. */
static const uint8_t k_cdc_dev[18] = {
    0x12, 0x01, 0x00, 0x02, 0x02, 0x00, 0x00, 0x40, 0x3C, 0x2E, 0x40, 0x57, 0x00, 0x02, 1, 2, 3, 1,
};
static const uint8_t k_cdc_cfg[67] = {
    0x09, 0x02, 0x43, 0x00, 0x02, 0x01, 0x00, 0xC0, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x01, 0x02, 0x02, 0x01, 0x00,
    0x05, 0x24, 0x00, 0x10, 0x01,
    0x05, 0x24, 0x01, 0x00, 0x01,
    0x04, 0x24, 0x02, 0x02,
    0x05, 0x24, 0x06, 0x00, 0x01,
    0x07, 0x05, 0x82, 0x03, 0x08, 0x00, 0xFF,
    0x09, 0x04, 0x01, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00,
    0x07, 0x05, 0x81, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, 0x01, 0x02, 0x40, 0x00, 0x00,
};

static void dump(const char *name, const uint8_t *p, size_t n)
{
    printf("%s (%u bytes):", name, (unsigned)n);
    for (size_t i = 0; i < n; i++) printf("%s%02X", (i % 16) ? " " : "\n   ", p[i]);
    printf("\n");
}

typedef struct { size_t off; uint8_t val; const char *what; } mutant_t;

static void test_descriptors(bool verbose)
{
    char err[160];
    const uint8_t *dev = usb_comp_device_desc, *cfg = usb_comp_config_desc;

    if (verbose) { dump("composite device", dev, 18); dump("composite configuration", cfg, USB_COMP_CONFIG_LEN); }

    CHECK(USB_COMP_CONFIG_LEN == 98u, "config length %u", (unsigned)USB_COMP_CONFIG_LEN);
    bool ok = usb_desc_check(dev, 18, cfg, USB_COMP_CONFIG_LEN, &k_layout, err, sizeof err);
    CHECK(ok, "composite descriptors rejected: %s", err);
    ok = usb_desc_check(k_cdc_dev, 18, k_cdc_cfg, sizeof k_cdc_cfg, &k_layout, err, sizeof err);
    CHECK(ok, "library CDC descriptors rejected: %s", err);

    /* The CDC function inside the composite is the CDC build's, byte for byte:
     * composite[9+8 .. 9+8+58) == cdc[9 .. 67). */
    CHECK(memcmp(cfg + 17, k_cdc_cfg + 9, 58) == 0, "composite CDC part differs from the library CDC descriptor");
    CHECK(dev[8] == k_cdc_dev[8] && dev[9] == k_cdc_dev[9] && dev[10] == k_cdc_dev[10] &&
          dev[11] == k_cdc_dev[11], "VID:PID differ from the CDC build");
    CHECK(dev[4] == 0xEF && dev[5] == 0x02 && dev[6] == 0x01, "device class not EF/02/01");
    CHECK(dev[12] == 0x01 && dev[13] == 0x02, "bcdDevice not 2.01");
    CHECK(cfg[9 + 8 + 9 + 5 + 5 + 4 + 5 + 7 + 9 + 7 + 7 + 5] == 0x08 &&
          cfg[9 + 8 + 9 + 5 + 5 + 4 + 5 + 7 + 9 + 7 + 7 + 6] == 0x06 &&
          cfg[9 + 8 + 9 + 5 + 5 + 4 + 5 + 7 + 9 + 7 + 7 + 7] == 0x50, "MSC triple not 08/06/50");

    /* mutants: each single-byte change must be caught */
    /* Offsets: config 0, IAD 9, if0 17, header 26, call mgmt 31, ACM 36,
     * union 40, EP 0x82 45, if1 52, EP 0x81 61, EP 0x01 68, if2 75,
     * EP 0x83 84, EP 0x03 91. */
    static const mutant_t mut[] = {
        { 2, 97, "wTotalLength one short" },
        { 2, 99, "wTotalLength one long" },
        { 4, 2, "bNumInterfaces 2" },
        { 4, 4, "bNumInterfaces 4" },
        { 5, 0, "bConfigurationValue 0" },
        { 7, 0x40, "bmAttributes without bit 7" },
        { 9, 9, "IAD bLength 9" },
        { 11, 1, "IAD bFirstInterface 1" },
        { 12, 3, "IAD bInterfaceCount 3" },
        { 13, 0x0A, "IAD function class != comm interface class" },
        { 21, 2, "comm interface bNumEndpoints 2" },
        { 28, 0x01, "first functional descriptor not the header" },
        { 35, 2, "call management data interface 2" },
        { 43, 1, "union control interface 1" },
        { 44, 2, "union subordinate 2 (the MSC interface)" },
        { 47, 0x81, "notify endpoint = bulk IN address" },
        { 49, 65, "notify wMaxPacketSize 65" },
        { 51, 0, "interrupt bInterval 0" },
        { 65, 48, "CDC bulk IN wMaxPacketSize 48" },
        { 80, 0x0A, "MSC interface class 0A" },
        { 81, 0x05, "MSC subclass 05" },
        { 82, 0x00, "MSC protocol 00 (CBI)" },
        { 87, 0x03, "MSC IN endpoint interrupt" },
        { 93, 0x83, "MSC OUT = MSC IN address" },
        { 84, 6, "endpoint bLength 6" },
        { 76, 0x05, "interface descriptor typed as endpoint" },
        { 78, 1, "MSC alternate setting 1" },
    };
    for (size_t i = 0; i < sizeof mut / sizeof mut[0]; i++) {
        uint8_t m[USB_COMP_CONFIG_LEN];
        memcpy(m, cfg, sizeof m);
        if (m[mut[i].off] == mut[i].val) { CHECK(0, "mutant '%s' changes nothing", mut[i].what); continue; }
        m[mut[i].off] = mut[i].val;
        bool caught = !usb_desc_check(dev, 18, m, sizeof m, &k_layout, err, sizeof err);
        CHECK(caught, "mutant '%s' passed the checker", mut[i].what);
        if (caught && verbose) printf("  mutant %-45s -> %s\n", mut[i].what, err);
    }
    /* truncated transfer (what a host sees if the device clamps wrongly) */
    CHECK(!usb_desc_check(dev, 18, cfg, 64, &k_layout, err, sizeof err), "64-byte truncation passed");
    /* device descriptor mutants */
    {
        uint8_t d[18];
        memcpy(d, dev, 18); d[17] = 2;
        CHECK(!usb_desc_check(d, 18, cfg, USB_COMP_CONFIG_LEN, &k_layout, err, sizeof err), "2 configurations passed");
        memcpy(d, dev, 18); d[6] = 0x00;
        CHECK(!usb_desc_check(d, 18, cfg, USB_COMP_CONFIG_LEN, &k_layout, err, sizeof err), "EF/02/00 passed");
        memcpy(d, dev, 18); d[7] = 63;
        CHECK(!usb_desc_check(d, 18, cfg, USB_COMP_CONFIG_LEN, &k_layout, err, sizeof err), "EP0 size 63 passed");
    }
    /* a composite without the IAD (class EF) */
    {
        uint8_t m[USB_COMP_CONFIG_LEN - 8];
        memcpy(m, cfg, 9);
        memcpy(m + 9, cfg + 17, sizeof m - 9);
        m[2] = (uint8_t)sizeof m;
        CHECK(!usb_desc_check(dev, 18, m, sizeof m, &k_layout, err, sizeof err), "EF device without IAD passed");
    }
    /* PMA layout mutants */
    {
        usb_pma_buf_t p[sizeof k_pma / sizeof k_pma[0]];
        usb_pma_layout_t L = { 512, 64, p, sizeof p / sizeof p[0] };
        memcpy(p, k_pma, sizeof p);
        p[6].offset = 0x1E0;
        CHECK(!usb_desc_check(dev, 18, cfg, USB_COMP_CONFIG_LEN, &L, err, sizeof err), "PMA past 512 passed");
        memcpy(p, k_pma, sizeof p);
        p[5].offset = 0x140;
        CHECK(!usb_desc_check(dev, 18, cfg, USB_COMP_CONFIG_LEN, &L, err, sizeof err), "PMA overlap passed");
        memcpy(p, k_pma, sizeof p);
        p[0].offset = 0x20;
        CHECK(!usb_desc_check(dev, 18, cfg, USB_COMP_CONFIG_LEN, &L, err, sizeof err), "PMA over BTABLE passed");
        L.nbufs = 5;    /* no EPT3 buffers: the MSC endpoints have nowhere to go */
        memcpy(p, k_pma, sizeof p);
        CHECK(!usb_desc_check(dev, 18, cfg, USB_COMP_CONFIG_LEN, &L, err, sizeof err), "MSC without PMA passed");
    }
    /* usb_conf.h: the layout fills the 512 bytes exactly and EPT2's RX slot is unused */
    CHECK(EPT2_RX_ADDR == 0 && EPT3_RX_ADDR + 64 == 512, "usb_conf.h layout changed: re-check");
}

/* ── 2. the CTR race fix ───────────────────────────────────────────── */

/* rc_w0 model of the TXTC/RXTC bits: a write of 0 clears, 1 keeps; the
 * hardware may set a flag between the macro's read and its write. */
static uint32_t hw_after_write(uint32_t hw_now, uint32_t written)
{
    const uint32_t tc = USB_TXTC | USB_RXTC;
    return (written & ~tc) | (hw_now & written & tc);
}

static void test_ctr_race(void)
{
    /* Clearing RXTC while TXTC is set by the hardware mid-RMW. */
    usb_stub_regs.ept[1] = USB_RXTC | 0x0001u;          /* read: only RX pending */
    USB_CLEAR_RXTC(1);
    uint32_t w = usb_stub_regs.ept[1];
    uint32_t hw = hw_after_write(USB_RXTC | USB_TXTC | 0x0001u, w);
    CHECK((hw & USB_TXTC) != 0, "IN completion lost by USB_CLEAR_RXTC (written %04X)", (unsigned)w);
    CHECK((hw & USB_RXTC) == 0, "USB_CLEAR_RXTC did not clear RXTC");
    CHECK((w & 0x000Fu) == 1u, "endpoint address not preserved");
    CHECK((w & (USB_TXSTS | USB_RXSTS | USB_TXDTS | USB_RXDTS)) == 0, "toggle bits written as 1");

    /* Clearing TXTC while RXTC is set by the hardware mid-RMW. */
    usb_stub_regs.ept[1] = USB_TXTC | 0x0001u;
    USB_CLEAR_TXTC(1);
    w = usb_stub_regs.ept[1];
    hw = hw_after_write(USB_RXTC | USB_TXTC | 0x0001u, w);
    CHECK((hw & USB_RXTC) != 0, "OUT completion lost by USB_CLEAR_TXTC (written %04X)", (unsigned)w);
    CHECK((hw & USB_TXTC) == 0, "USB_CLEAR_TXTC did not clear TXTC");

    /* The library's original definition loses it (proves the model bites). */
    usb_stub_regs.ept[1] = USB_RXTC | 0x0001u;
    usb_stub_regs.ept[1] &= 0x7FFF & USB_EPT_BIT_MASK;
    hw = hw_after_write(USB_RXTC | USB_TXTC | 0x0001u, usb_stub_regs.ept[1]);
    CHECK((hw & USB_TXTC) == 0, "model: the library macro should lose TXTC");
}

/* ── 3. mode state machine ─────────────────────────────────────────── */

typedef struct { uint32_t now; bool configured; int detaches, attaches; usb_dev_kind_t last_attach; } sim_t;

static void run(usb_mode_sm_t *m, sim_t *s, uint32_t until, uint32_t configure_at)
{
    for (; s->now != until; s->now += 10u) {
        if (configure_at != 0u && s->now == configure_at) s->configured = true;
        usb_mode_action_t a = usb_mode_sm_step(m, s->now, s->configured);
        if (a == USB_ACT_DETACH) { s->detaches++; s->configured = false; }
        if (a == USB_ACT_ATTACH) { s->attaches++; s->last_attach = m->active; s->configured = false; }
    }
}

static void test_mode_sm(uint32_t base)
{
    usb_mode_sm_t m;
    sim_t s = { base, true, 0, 0, USB_DEV_CDC };
    usb_mode_sm_init(&m);
    CHECK(m.active == USB_DEV_CDC && !usb_mode_sm_sharing(&m), "boot is not CDC");
    run(&m, &s, base + 5000u, 0);
    CHECK(s.detaches == 0 && s.attaches == 0, "idle CDC switched by itself");

    /* share on -> composite, configured 400 ms after attach */
    usb_mode_sm_request(&m, true);
    CHECK(usb_mode_sm_sharing(&m), "sharing not reported after request");
    const uint32_t t_req = s.now;
    run(&m, &s, t_req + USB_MODE_DRAIN_MS + USB_MODE_DETACH_MS + 1000u,
        t_req + USB_MODE_DRAIN_MS + USB_MODE_DETACH_MS + 400u + 20u);
    CHECK(s.detaches == 1 && s.attaches == 1 && s.last_attach == USB_DEV_COMPOSITE, "switch to composite: %d/%d",
          s.detaches, s.attaches);
    CHECK(m.state == USB_ST_STEADY && m.last == USB_RES_OK && m.active == USB_DEV_COMPOSITE, "composite not steady");

    /* share off -> CDC */
    usb_mode_sm_request(&m, false);
    run(&m, &s, s.now + 2000u, s.now + USB_MODE_DRAIN_MS + USB_MODE_DETACH_MS + 200u);
    CHECK(m.active == USB_DEV_CDC && m.state == USB_ST_STEADY && s.attaches == 2, "share off did not return to CDC");

    /* share on, host never configures: fallback after 3 s, then CDC, no retry */
    usb_mode_sm_request(&m, true);
    const uint32_t t1 = s.now;
    const uint32_t t_attach = t1 + USB_MODE_DRAIN_MS + USB_MODE_DETACH_MS;
    run(&m, &s, t_attach + USB_MODE_ATTACH_TIMEOUT_MS - 20u, 0);
    CHECK(m.state == USB_ST_ATTACHING && m.active == USB_DEV_COMPOSITE, "gave up before 3 s");
    run(&m, &s, t_attach + USB_MODE_ATTACH_TIMEOUT_MS + USB_MODE_DETACH_MS + 1000u,
        t_attach + USB_MODE_ATTACH_TIMEOUT_MS + USB_MODE_DETACH_MS + 300u);
    CHECK(m.fallbacks == 1 && m.last == USB_RES_FALLBACK, "no fallback recorded (last=%d)", (int)m.last);
    CHECK(m.active == USB_DEV_CDC && m.state == USB_ST_STEADY && s.last_attach == USB_DEV_CDC,
          "fallback did not come back as CDC");
    CHECK(!usb_mode_sm_sharing(&m), "sharing still reported after fallback");
    const int att = s.attaches;
    run(&m, &s, s.now + 20000u, 0);
    CHECK(s.attaches == att && m.last == USB_RES_FALLBACK, "retried by itself after a fallback");

    /* a new request clears the fallback memory and tries again */
    usb_mode_sm_request(&m, true);
    run(&m, &s, s.now + 1500u, s.now + USB_MODE_DRAIN_MS + USB_MODE_DETACH_MS + 100u);
    CHECK(m.active == USB_DEV_COMPOSITE && m.last == USB_RES_OK, "retry after fallback failed");

    /* CDC re-attach with no host: stays CDC, says so */
    usb_mode_sm_request(&m, false);
    run(&m, &s, s.now + 5000u, 0);
    CHECK(m.active == USB_DEV_CDC && m.state == USB_ST_STEADY && m.last == USB_RES_NO_HOST, "no-host CDC");
}

/* ── 4. raw_block ──────────────────────────────────────────────────── */

static void test_raw_block(void)
{
    uint8_t buf[1024], data[1100];
    for (unsigned i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 7u + 3u);
    raw_block_t b;
    memset(&b, 0, sizeof b);
    CHECK(!raw_block_begin(&b, buf, sizeof buf, 0, 0, 0, 0), "len 0 accepted");
    CHECK(!raw_block_begin(&b, buf, sizeof buf, 1025, 0, 0, 0), "len > cap accepted");
    CHECK(!raw_block_active(&b), "active after refusal");

    const uint32_t crc = cal_backup_crc32(data, 1024);
    CHECK(raw_block_begin(&b, buf, sizeof buf, 1024, crc, 256, 100), "begin");
    uint32_t used = 0;
    for (uint32_t k = 0; k < 1024u - 64u; k += 64u) used += raw_block_feed(&b, data + k, 64, 100 + k);
    CHECK(raw_block_active(&b), "done early");
    used += raw_block_feed(&b, data + 960, 100, 1200);       /* 64 block bytes + 36 shell bytes */
    CHECK(used == 1024u, "consumed %u", (unsigned)used);
    CHECK(b.state == RAW_DONE_OK && memcmp(buf, data, 1024) == 0 && b.tag == 256u, "block content/CRC");

    CHECK(raw_block_begin(&b, buf, sizeof buf, 100, crc, 0, 0), "begin 2");
    raw_block_feed(&b, data, 100, 10);
    CHECK(b.state == RAW_DONE_CRC, "bad CRC not detected");

    CHECK(raw_block_begin(&b, buf, sizeof buf, 100, 0, 0, 1000), "begin 3");
    raw_block_feed(&b, data, 50, 1500);
    raw_block_poll(&b, 1500 + RAW_BLOCK_IDLE_MS - 1u);
    CHECK(raw_block_active(&b), "timed out early");
    raw_block_poll(&b, 1500 + RAW_BLOCK_IDLE_MS);
    CHECK(b.state == RAW_TIMEOUT && b.got == 50u, "no timeout");
    CHECK(raw_block_feed(&b, data, 10, 5000) == 0u, "fed after timeout");
}

int main(int argc, char **argv)
{
    const bool verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    test_descriptors(verbose);
    test_ctr_race();
    test_mode_sm(0);
    test_mode_sm(0xFFFFF000u);      /* tick counter wraps mid-switch */
    test_raw_block();
    printf("test_usb: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
