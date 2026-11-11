/*
 * usb_mode_sm.c — see usb_mode_sm.h.
 */
#include "usb_mode_sm.h"

#include <string.h>

void usb_mode_sm_init(usb_mode_sm_t *m)
{
    memset(m, 0, sizeof *m);
    m->active = USB_DEV_CDC;
    m->want = USB_DEV_CDC;
    m->state = USB_ST_STEADY;
    m->last = USB_RES_NONE;
}

void usb_mode_sm_request(usb_mode_sm_t *m, bool share)
{
    m->want = share ? USB_DEV_COMPOSITE : USB_DEV_CDC;
    if (share) m->fallback_pending = false;
}

usb_mode_action_t usb_mode_sm_step(usb_mode_sm_t *m, uint32_t now, bool configured)
{
    const uint32_t dt = now - m->t0;

    switch (m->state) {
    case USB_ST_STEADY:
        if (m->want != m->active) {
            m->state = USB_ST_DRAIN;
            m->t0 = now;
        }
        return USB_ACT_NONE;

    case USB_ST_DRAIN:
        if (dt < USB_MODE_DRAIN_MS) return USB_ACT_NONE;
        m->state = USB_ST_DETACHED;
        m->t0 = now;
        return USB_ACT_DETACH;

    case USB_ST_DETACHED:
        if (dt < USB_MODE_DETACH_MS) return USB_ACT_NONE;
        m->active = m->want;            /* the latest request wins */
        m->state = USB_ST_ATTACHING;
        m->t0 = now;
        m->switches++;
        return USB_ACT_ATTACH;

    case USB_ST_ATTACHING:
        if (configured) {
            m->state = USB_ST_STEADY;
            /* the CDC re-attach after a fallback keeps reporting the fallback */
            if (!(m->fallback_pending && m->active == USB_DEV_CDC)) m->last = USB_RES_OK;
            m->attach_ms = dt;
            return USB_ACT_NONE;
        }
        if (dt < USB_MODE_ATTACH_TIMEOUT_MS) return USB_ACT_NONE;
        m->attach_ms = dt;
        if (m->active == USB_DEV_COMPOSITE) {
            /* The safety net: the host never configured the composite
             * device. Come back as the CDC device that is known to work,
             * and do not try again until asked. */
            m->fallbacks++;
            m->last = USB_RES_FALLBACK;
            m->fallback_pending = true;
            m->want = USB_DEV_CDC;
            m->state = USB_ST_DETACHED;
            m->t0 = now;
            return USB_ACT_DETACH;
        }
        /* CDC with nobody configuring it: no host. Nothing better to be. */
        m->state = USB_ST_STEADY;
        if (!m->fallback_pending) m->last = USB_RES_NO_HOST;
        return USB_ACT_NONE;
    }
    return USB_ACT_NONE;
}

bool usb_mode_sm_sharing(const usb_mode_sm_t *m) { return m->want == USB_DEV_COMPOSITE; }

const char *usb_mode_state_str(usb_mode_state_t s)
{
    switch (s) {
    case USB_ST_STEADY:    return "steady";
    case USB_ST_DRAIN:     return "draining";
    case USB_ST_DETACHED:  return "detached";
    case USB_ST_ATTACHING: return "attaching";
    }
    return "?";
}

const char *usb_mode_result_str(usb_mode_result_t r)
{
    switch (r) {
    case USB_RES_NONE:     return "none";
    case USB_RES_OK:       return "ok";
    case USB_RES_FALLBACK: return "FALLBACK (host did not configure CDC+MSC within 3 s; back to CDC-only)";
    case USB_RES_NO_HOST:  return "no host configured the device";
    }
    return "?";
}
