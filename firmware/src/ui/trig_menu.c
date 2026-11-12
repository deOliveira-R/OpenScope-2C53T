/* See trig_menu.h (parity C3; the Trigger pane's model since UI direct). */
#include "trig_menu.h"

static int wrap(int v, int n) { return ((v % n) + n) % n; }

uint8_t trig_menu_change(const trig_menu_t *m, scope_state_t *ss, int dir)
{
    const int d = (dir < 0) ? -1 : 1;
    switch (m->row) {
    case TM_ROW_MODE:
        ss->trigger.mode = (trigger_mode_t)wrap((int)ss->trigger.mode + d, TRIG_COUNT);
        return TM_CHANGED_MODE;
    case TM_ROW_EDGE:
        ss->trigger.edge = (trigger_edge_t)wrap((int)ss->trigger.edge + d, TRIG_EDGE_COUNT);
        return TM_CHANGED_EDGE;
    case TM_ROW_SOURCE: {
        trigger_source_t to = (trigger_source_t)wrap((int)ss->trigger.source + d, TRIG_SRC_COUNT);
        const bool on = (to == TRIG_SRC_CH2) ? ss->ch2.enabled : ss->ch1.enabled;
        if (!on)
            return TM_REFUSED_OFF;
        ss->trigger.source = to;
        return TM_CHANGED_SOURCE;
    }
    default:
        return 0;
    }
}

const char *trig_menu_row_label(uint8_t row)
{
    switch (row) {
    case TM_ROW_MODE:   return "Mode:";
    case TM_ROW_EDGE:   return "Edge:";
    case TM_ROW_SOURCE: return "Channel:";
    default:            return "";
    }
}

const char *trig_menu_row_value(const scope_state_t *ss, uint8_t row)
{
    switch (row) {
    case TM_ROW_MODE:
        return ((unsigned)ss->trigger.mode < (unsigned)TRIG_COUNT)
                   ? trigger_mode_labels[ss->trigger.mode] : "?";
    case TM_ROW_EDGE:
        return ((unsigned)ss->trigger.edge < (unsigned)TRIG_EDGE_COUNT)
                   ? trigger_edge_labels[ss->trigger.edge] : "?";
    case TM_ROW_SOURCE:
        return ((unsigned)ss->trigger.source < (unsigned)TRIG_SRC_COUNT)
                   ? trigger_source_labels[ss->trigger.source] : "?";
    default:
        return "";
    }
}
