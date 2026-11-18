/*
 * OpenScope 2C53T - what the feature shell files need from the shell core
 *
 * shell_bench.c and shell_meterdebug.c hold command handlers that were moved
 * out of usb_debug.c unchanged. The helpers they call are static there and
 * stay so; next to each one usb_debug.c has a thin non-static accessor
 * (usb_debug_sh_*), declared here. Unused in an image without those
 * features, the linker drops them.
 *
 * A feature file defines SHELL_FEATURE_FILE before including this header and
 * then also gets the helpers under the names the handlers were written
 * with, so the moved code reads as it did. usb_debug.c includes it without
 * the define.
 */
#ifndef SHELL_INTERNAL_H
#define SHELL_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

void usb_debug_sh_send_str(const char *str);
int usb_debug_sh_parse_hex32(const char *s, uint32_t *out);
int usb_debug_sh_parse_int(const char *s, uint32_t *out);
void usb_debug_sh_print_last_tx_frame(void);
void usb_debug_sh_print_recent_tx_frames(void);
uint32_t usb_debug_sh_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len);
const char *usb_debug_sh_onoff(bool b);
bool usb_debug_sh_onoff_arg(const char *args, bool *out);
const char *usb_debug_sh_meter_layout_name(uint8_t layout);
uint8_t *usb_debug_sh_scratch(void);

#ifdef SHELL_FEATURE_FILE
static inline void usb_send_str(const char *str) { usb_debug_sh_send_str(str); }
static inline int parse_hex32(const char *s, uint32_t *out) { return usb_debug_sh_parse_hex32(s, out); }
static inline int parse_int(const char *s, uint32_t *out) { return usb_debug_sh_parse_int(s, out); }
static inline void usb_print_last_tx_frame(void) { usb_debug_sh_print_last_tx_frame(); }
static inline void usb_print_recent_tx_frames(void) { usb_debug_sh_print_recent_tx_frames(); }
static inline uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len) { return usb_debug_sh_crc32_update(crc, data, len); }
static inline const char *onoff(bool b) { return usb_debug_sh_onoff(b); }
static inline bool shell_onoff_arg(const char *args, bool *out) { return usb_debug_sh_onoff_arg(args, out); }
static inline const char *meter_layout_name(uint8_t layout) { return usb_debug_sh_meter_layout_name(layout); }
/* The shell's one scratch buffer, 2 * FPGA_ADC_BUF_SIZE bytes, 4-aligned (shell task only). */
#define shell_bus_scratch (usb_debug_sh_scratch())
#endif /* SHELL_FEATURE_FILE */

#endif /* SHELL_INTERNAL_H */
