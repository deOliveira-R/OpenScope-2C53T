/*
 * OpenScope 2C53T - shared between the files of the USB port and the shell
 *
 * usb_cdc.c (the USB serial port), shell.c (the command table and the shell
 * task) and the command files shell_system.c, shell_scope.c, shell_cal.c,
 * shell_screen.c. Nothing else includes this; the public interface is
 * usb_debug.h, and shell_internal.h is what the feature shell files use.
 */
#ifndef SHELL_PARTS_H
#define SHELL_PARTS_H

#include "usb_debug.h"
#include "battery.h"
#include "at32f403a_407.h"
#include "usbd_core.h"
#include "usbd_int.h"
#include "cdc_class.h"
#include "cdc_desc.h"
/* USB_MSC (Makefile, default 1): enumerate as CDC + the read-only gallery
 * mass-storage LUN (usb_composite.c, usb_msc.c). 0 = the CDC-only device. */
#ifndef USB_MSC
#define USB_MSC 0
#endif
#if USB_MSC
#include "usb_composite.h"
#include "usb_msc.h"
#include "../util/usb_mode_sm.h"
#endif
#include "../util/raw_block.h"
#include "dfu_boot.h"
#include "flash_fs.h"
#include "fw_loader.h"
#include "cal_backup.h"
#include "can_scope.h"
#include "usercal.h"
#include "rtt.h"
#include "continuity_buzzer.h"

/* DEBUG_SHELL_RTT_ONLY=1 (build with -DDEBUG_SHELL_RTT_ONLY=1) leaves the
 * CDC half of the shell out: no CDC input, output to RTT only. For a unit
 * whose USB stack reports CONFIGURED but does not move data, so that the
 * shell can be reached over SWD without the CDC send path in the way. */
#ifndef DEBUG_SHELL_RTT_ONLY
#define DEBUG_SHELL_RTT_ONLY 0
#endif
#include "scope_trigger.h"
#include "signal_gen.h"
#include "dac_output.h"
#include "scope_vpos.h"
#include "scope_selfcal_run.h"
#include "../util/calpage.h"
#include "fpga.h"
#include "lcd.h"
#include "meter_auto.h"
#include "meter_autoselect.h"
#include "meter_data.h"
#include "meter_stock.h"
#include "ui.h"
#include "model.h"              /* meter state: the link policy (meter.link) */
#include "meter_link_policy.h"
#include "../ui/scope_state.h"
#include "../util/settings_store.h"
#include "../ui/siggen_edit.h"
#include "../ui/scope_cal.h"
#include "../ui/scope_freq.h"
#include "../ui/scope_measure.h"
#include "../ui/meas_select.h"
#include "../ui/measure_strip.h"   /* measure_strip_shown */
#include "../ui/scope_timebase.h"
#include "../ui/scope_roll.h"
#include "../ui/scope_geom.h"
#include "../ui/scope_mask.h"
#include "../dsp/trig_policy.h"
#include "../ui/key_model.h"
#include "../tasks/input_handler.h"
#include "../ui/settings_screen.h"   /* `ui dump`: the Settings page and rows */
#include "../util/shell_persist.h"
#include "../util/lcd_health.h"
#include "../ui/gallery_ui.h"
#include "shared_mem.h"
#include "../util/gallery_store.h"
#include "../ui/logo_ui.h"
#include "watchdog.h"
#include "../util/cal_backup_core.h"

#include "fpga_bitstream_core.h"
#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include "model_shell.h"   /* get / set / list / describe rows */
#include "ui_shell.h"      /* ui press / long / seq / screen rows */
#include "../util/features.h"   /* version, features: what this build contains */
#include "stream.h"        /* binary stream over this channel (src/stream/): no-ops when off */
#include "shell_internal.h" /* the usb_debug_sh_* accessors, for the feature shell files */
#include "model_exec.h"      /* `fpga roll code`: the re-apply goes through the executor */
#include "shell_bench.h"     /* bench shell rows (FEATURE_BENCHSHELL): empty when off */
#include "../re/shell_re.h"   /* reverse-engineering toolkit rows (FEATURE_RETOOLS): empty when off */
#include "shell_meterdebug.h" /* meter diagnostics rows (FEATURE_METERDEBUG): empty when off */

/* Which FPGA configuration path completed an upload this boot. `status`
 * reports configuration fields only for the path that wrote them; a field
 * no path wrote holds its zero-init value, which looks like a reading.
 * Decided from two fields of `fpga`, in order (shell_system.c):
 *   h2_upload_done  1 when an upload sent every byte: set by the bit-bang
 *                   upload (fpga_boot.c) and by the hardware-SPI3 upload
 *                   (re/re_fpga_driver.c). Clear: no upload completed (for
 *                   example the part already ran a design and nothing was
 *                   sent), so no configuration result is reported.
 *   diag_spi_ctrl1  0xBBBB is a marker the bit-bang upload stores, not a
 *                   register value. The hardware-SPI3 path leaves the real
 *                   SPI3 CTRL1 there.
 * `fpga reinit` (reverse-engineering toolkit) re-runs the hardware-SPI3
 * path and rewrites both fields, so the label follows it. */
typedef enum {
    CFG_PATH_NONE = 0,   /* no upload completed this boot */
    CFG_PATH_BITBANG,    /* fpga_bitbang_config_sequence() (fpga_boot.c) */
    CFG_PATH_HWSPI,      /* fpga_spi3_config_sequence() (re/re_fpga_driver.c) */
} cfg_path_t;

/* usb_cdc.c */
extern usbd_core_type usb_core_dev;
#if USB_MSC
extern usb_mode_sm_t s_usb_mode;
#endif
void cdc_rx_heal(cdc_struct_type *pcdc);
void usb_send_bytes(const uint8_t *data, uint16_t len);
#if USB_MSC
void usb_mode_poll(void);
#endif
void usb_send_str(const char *str);
void cmd_usb(const char *args);

/* shell.c */
int parse_hex32(const char *s, uint32_t *out);
int parse_int(const char *s, uint32_t *out);
uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len);
uint32_t shell_now_ms(void);
const char *onoff(bool b);
bool shell_onoff_arg(const char *args, bool *out);
extern uint8_t shell_bus_scratch[2 * FPGA_ADC_BUF_SIZE];

/* shell_system.c */
void cmd_status(void);
void cmd_flash_jedec(void);
void cmd_flash_read(const char *args);
void cmd_flash_dump(const char *args);
void cmd_can_decode(void);
void cmd_can_stats(const char *args);
void cmd_can_screen(const char *args);
void cmd_meter_relays(const char *args);
void cmd_meter_state(void);
void cmd_reset_settings(const char *args);
void cmd_settings(void);
void cmd_reboot_bootloader(void);
const char *meter_layout_name(uint8_t layout);
void cmd_persist(const char *args);
void cmd_batt(const char *args);
void cmd_meter_auto_async(const char *args);
void cmd_siggen(const char *args);
void fwl_print_status(void);
void cmd_fwload(const char *args);
void cmd_fwstat(void);
void cmd_fwswap(const char *args);
void cmd_fwapply(void);

/* shell_scope.c */
void cmd_fpga_cfgsettle(const char *args);
void cmd_fpga_unrotate(const char *args);
void cmd_fpga_core(const char *args);
void cmd_fpga_interleave(const char *args);
void cmd_fpga_scope_trigger(const char *args);
void cmd_fpga_scope_range(const char *args);
uint32_t scope_center_buf_median(uint8_t ch);
void timebase_print_status(bool after_set);
void cmd_fpga_roll(const char *args);
void cmd_fpga_scope_measure(const char *args);
void prm_print_status(void);
void cmd_fpga_scope_cursor(const char *args);
void cmd_fpga_scope_center(const char *args);
void cmd_fpga_scope_autoset(void);
void cmd_fft_auto(void);
void cmd_fpga_scope_basecal(const char *args);
void cmd_fpga_scope_center50(void);
void cmd_fpga_scope_selfcal(const char *args);
void cmd_mask(const char *args);

/* shell_cal.c */
void cmd_cal_status(void);
void cmd_cal_backup(void);
void cmd_cal_restore(const char *args);
void cmd_cal_page(const char *args);
void cmd_cal_base(const char *args);
void cmd_cal_user(const char *args);

/* shell_screen.c */
void cmd_screen_dump(const char *args);
void cmd_screen_dumpbin(const char *args);
void cmd_screenshot(const char *args);
extern logo_upload_t s_logo_up;
extern raw_block_t s_raw;
void logo_blk_finish(void);
void cmd_logo(const char *args);
void cmd_screen_shadow(const char *args);
void cmd_ui_dump(void);

#endif /* SHELL_PARTS_H */
