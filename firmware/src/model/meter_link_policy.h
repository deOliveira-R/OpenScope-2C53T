/*
 * OpenScope 2C53T - meter link policy (pure; host test: make test-meter-link-policy)
 *
 * The meter link is the isolated meter SoC's supply/enable (PC11), USART2 and
 * the poll task that keeps it talking. It is independent of the scope front
 * end, so the meter can run while any screen is shown.
 *
 * The owner's rule: with a USB host the meter always runs (a host can take
 * all the data). Without one it follows the meter screen by default; the
 * property meter.link (Settings > "Meter on Battery") can make it keep
 * running.
 *
 * No hardware and no RTOS here: the firmware feeds the three inputs and acts
 * on the answer (main.c, the display task's mode-change block).
 */
#ifndef METER_LINK_POLICY_H
#define METER_LINK_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* meter.link: the policy WITHOUT a USB host. Values are the record's. */
typedef enum {
    METER_LINK_SCREEN = 0,      /* the meter runs only while its screen is shown */
    METER_LINK_ALWAYS = 1,      /* it keeps running                              */
    METER_LINK_POLICY_COUNT
} meter_link_policy_t;

/* The one decision: should the link be on? */
bool meter_link_want_on(bool screen_is_meter, bool usb_host_configured, meter_link_policy_t policy);

/* Debounce of "the host has configured the USB device". Every rise of PC11
 * restarts the meter SoC (~1.4 s), so a re-enumeration (configured -> not ->
 * configured within a second) must not reach the pin: a new raw level counts
 * only after it has held for hold_ms. Starts at "no host". Time is a
 * free-running millisecond counter (wrap-safe). */
typedef struct {
    bool     stable;        /* the debounced level                       */
    bool     raw;           /* the last raw level seen                   */
    bool     primed;        /* since_ms is valid                         */
    uint32_t since_ms;      /* when raw last changed                     */
} meter_link_debounce_t;

#define METER_LINK_USB_HOLD_MS  1000u

bool meter_link_debounce(meter_link_debounce_t *d, bool raw, uint32_t now_ms, uint32_t hold_ms);

#endif /* METER_LINK_POLICY_H */
