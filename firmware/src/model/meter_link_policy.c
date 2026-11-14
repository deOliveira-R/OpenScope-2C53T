/*
 * OpenScope 2C53T - meter link policy (see meter_link_policy.h)
 */
#include "meter_link_policy.h"

bool meter_link_want_on(bool screen_is_meter, bool usb_host_configured, meter_link_policy_t policy)
{
    return screen_is_meter || usb_host_configured || policy == METER_LINK_ALWAYS;
}

bool meter_link_debounce(meter_link_debounce_t *d, bool raw, uint32_t now_ms, uint32_t hold_ms)
{
    if (!d->primed || raw != d->raw) {
        d->primed   = true;
        d->raw      = raw;
        d->since_ms = now_ms;
    }
    if (d->raw != d->stable && (uint32_t)(now_ms - d->since_ms) >= hold_ms)
        d->stable = d->raw;
    return d->stable;
}
