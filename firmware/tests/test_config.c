/*
 * OpenScope 2C53T - Tests for the Config module
 *
 * Build and run: make test-config
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "config.h"

/* ========================================================================
 * Test helpers
 * ======================================================================== */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { printf("  TEST: %-50s ", name); } while (0)

#define PASS() \
    do { printf("[PASS]\n"); tests_passed++; } while (0)

#define FAIL(msg) \
    do { printf("[FAIL] %s\n", msg); tests_failed++; } while (0)

#define ASSERT_TRUE(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while (0)

#define ASSERT_FALSE(cond, msg) \
    do { if (cond) { FAIL(msg); return; } } while (0)

#define ASSERT_EQ(a, b, msg) \
    do { if ((a) != (b)) { FAIL(msg); return; } } while (0)

/* ========================================================================
 * Config tests
 * ======================================================================== */

static void test_config_defaults(void)
{
    TEST("config_init_defaults sets magic and version");
    device_config_t cfg;
    config_init_defaults(&cfg);
    ASSERT_EQ(cfg.magic, CONFIG_MAGIC, "magic mismatch");
    ASSERT_EQ(cfg.version, CONFIG_VERSION, "version mismatch");
    ASSERT_TRUE(cfg.scope_ch1_enabled, "ch1 should be enabled");
    ASSERT_TRUE(cfg.scope_ch2_enabled, "ch2 should be enabled");
    ASSERT_EQ(cfg.scope_ch1_vdiv, 3, "ch1 vdiv default");
    ASSERT_EQ(cfg.display_brightness, 100, "brightness default");
    ASSERT_EQ(cfg.language, 0, "language default");
    ASSERT_EQ(cfg.startup_mode, CONFIG_MODE_RESTORE, "startup mode default (Restore)");
    ASSERT_EQ(cfg.last_screen, 0, "no last screen by default (Restore boots to Home)");
    PASS();
}

static void test_config_checksum_changes(void)
{
    TEST("checksum changes when data changes");
    device_config_t cfg;
    config_init_defaults(&cfg);
    uint32_t original = config_compute_checksum(&cfg);

    cfg.scope_ch1_vdiv = 7;
    uint32_t modified = config_compute_checksum(&cfg);

    ASSERT_TRUE(original != modified, "checksums should differ");
    PASS();
}

static void test_config_validate_valid(void)
{
    TEST("validate returns true for valid config");
    device_config_t cfg;
    config_init_defaults(&cfg);
    ASSERT_TRUE(config_validate(&cfg), "should validate");
    PASS();
}

static void test_config_validate_bad_magic(void)
{
    TEST("validate returns false for bad magic");
    device_config_t cfg;
    config_init_defaults(&cfg);
    cfg.magic = 0xDEADBEEF;
    /* Recompute checksum so only magic is wrong */
    cfg.checksum = config_compute_checksum(&cfg);
    ASSERT_FALSE(config_validate(&cfg), "should fail on bad magic");
    PASS();
}

static void test_config_validate_bad_checksum(void)
{
    TEST("validate returns false for corrupted checksum");
    device_config_t cfg;
    config_init_defaults(&cfg);
    cfg.checksum = cfg.checksum + 1; /* Corrupt it */
    ASSERT_FALSE(config_validate(&cfg), "should fail on bad checksum");
    PASS();
}

static void test_config_serialize_roundtrip(void)
{
    TEST("serialize/deserialize round-trip preserves data");
    device_config_t original, loaded;
    config_init_defaults(&original);
    original.siggen_frequency = 5000.0f;
    original.scope_trigger_level = -42;
    original.language = 1;
    original.startup_mode = CONFIG_MODE_SIGNAL_GEN;
    original.checksum = config_compute_checksum(&original);

    uint8_t buf[256];
    uint32_t written = config_serialize(&original, buf, sizeof(buf));
    ASSERT_TRUE(written > 0, "serialize should succeed");
    ASSERT_EQ(written, (uint32_t)sizeof(device_config_t), "size mismatch");

    bool ok = config_deserialize(&loaded, buf, written);
    ASSERT_TRUE(ok, "deserialize should succeed");
    ASSERT_EQ(loaded.magic, CONFIG_MAGIC, "magic");
    ASSERT_EQ(loaded.scope_trigger_level, -42, "trigger level");
    ASSERT_EQ(loaded.language, 1, "language");
    ASSERT_EQ(loaded.startup_mode, CONFIG_MODE_SIGNAL_GEN, "startup mode");
    ASSERT_TRUE(loaded.siggen_frequency == 5000.0f, "frequency");
    PASS();
}

static void test_config_deserialize_bad_magic(void)
{
    TEST("deserialize rejects bad magic");
    device_config_t cfg;
    config_init_defaults(&cfg);

    uint8_t buf[256];
    config_serialize(&cfg, buf, sizeof(buf));

    /* Corrupt magic */
    buf[0] = 0xFF;

    device_config_t loaded;
    ASSERT_FALSE(config_deserialize(&loaded, buf, sizeof(buf)),
                 "should reject bad magic");
    PASS();
}

static void test_config_deserialize_bad_checksum(void)
{
    TEST("deserialize rejects bad checksum");
    device_config_t cfg;
    config_init_defaults(&cfg);

    uint8_t buf[256];
    uint32_t written = config_serialize(&cfg, buf, sizeof(buf));

    /* Corrupt a data byte (not the checksum) */
    buf[8] ^= 0xFF;

    device_config_t loaded;
    ASSERT_FALSE(config_deserialize(&loaded, buf, written),
                 "should reject bad checksum");
    PASS();
}

static void test_config_serialize_buffer_too_small(void)
{
    TEST("serialize fails with too-small buffer");
    device_config_t cfg;
    config_init_defaults(&cfg);

    uint8_t buf[4];
    uint32_t written = config_serialize(&cfg, buf, sizeof(buf));
    ASSERT_EQ(written, 0u, "should return 0");
    PASS();
}

static void test_config_save_without_storage_refuses(void)
{
    /* 2026-08-13: config_save() used to write to a static RAM buffer, and this
     * test asserted the round-trip through it — which is why "config save/load
     * with checksum" was believed to work for months while nothing survived a
     * reboot. The backend is now the W25Q region layer, and with no backend
     * bound a save must REFUSE rather than claim success.
     *
     * The real round-trip is tested against a NOR flash model in
     * tests/test_config_persist.c, which is where persistence belongs. */
    TEST("save refuses when no storage is bound");
    device_config_t original, loaded;
    config_init_defaults(&original);
    original.auto_shutdown_mins = 15;
    original.checksum = config_compute_checksum(&original);

    ASSERT_FALSE(config_save(&original), "save must not claim success with no backend");
    ASSERT_FALSE(config_load(&loaded), "load must not invent a record");
    PASS();
}

int main(void)
{
    printf("\n=== Config Tests ===\n");
    test_config_defaults();
    test_config_checksum_changes();
    test_config_validate_valid();
    test_config_validate_bad_magic();
    test_config_validate_bad_checksum();
    test_config_serialize_roundtrip();
    test_config_deserialize_bad_magic();
    test_config_deserialize_bad_checksum();
    test_config_serialize_buffer_too_small();
    test_config_save_without_storage_refuses();

    printf("\n=== Results: %d passed, %d failed ===\n\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
