/*
 * OpenScope 2C53T - binary stream sender (stream.h, docs/stream_protocol.md)
 *
 * WHO SENDS. The shell task (usb_dbg, prio 2, 768-word stack), from its own
 * loop, between commands. It is the only writer of the CDC IN endpoint:
 * usb_send_bytes() has no lock and ignores a refused packet, so a second
 * writer task would splice bytes into shell replies. On the shell task a
 * frame can never land inside a command's output, and no lock is needed.
 * What it can starve: only the shell's own RX polling while one frame is in
 * flight (a 2-channel record is ~34 packets: ~34 ms paced by the tick, a few
 * ms with `fast`). Acquisition (prio 3) and the input task (4) preempt it;
 * the display (1) runs during every tick wait. With `fast`, the per-packet
 * wait spins up to ~0.3 ms before yielding, which holds off the display and
 * the prio-2 meter tasks for the length of one frame.
 *
 * NO BUFFERS. Records go out straight from fpga's published buffers through
 * one 64-byte packet on the stack (stream_frame.c). A record is taken once:
 * its generation (fpga_acq_frame_generation, the display's tear counter) is
 * read with the metadata, the record is marked consumed whether or not it is
 * sent, and the generation is read again into the frame's trailer after the
 * samples. A commit during the ~34 ms send shows on the host as gen_end !=
 * gen (torn; counted here too). Nothing is retried, nothing waits on the
 * acquisition, and no lock is taken.
 *
 * DROP, DON'T WAIT. A frame starts only if the IN endpoint is free within
 * STREAM_START_WAIT_MS; otherwise it is dropped and counted. Mid-frame, a
 * packet that cannot leave within STREAM_PKT_WAIT_MS abandons the frame (the
 * host resynchronises on the next marker) and counts an abort. A host that
 * stops reading for STREAM_IDLE_OFF_MS turns the stream off, so a terminal
 * opened later sees the shell, not binary.
 *
 * RAM: the state below (~170 B .bss); the writer (~84 B) and a meter
 * snapshot (~130 B) are on the shell task's stack, only inside a send.
 */
#if FEATURE_STREAM

#include "stream.h"
#include "stream_frame.h"

#include "fpga.h"
#include "fw_loader.h"
#include "meter_data.h"
#include "model.h"
#include "scope_cal.h"
#include "scope_measure.h"
#include "scope_roll.h"
#include "scope_state.h"
#include "scope_timebase.h"
#include "scope_vpos.h"
#include "usb_debug.h"

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

void usb_delay_us(uint32_t us);              /* usb_debug.c (usb_conf.h) */

#define STREAM_START_WAIT_MS   2u     /* IN endpoint must be free this soon, else drop  */
#define STREAM_PKT_WAIT_MS     50u    /* one packet stalled this long: abandon the frame */
#define STREAM_IDLE_OFF_MS     5000u  /* nothing left for this long: stream off          */
#define STREAM_STATS_MS        1000u
#define STREAM_PROPS_CHECK_MS  50u
#define STREAM_SPIN_POLLS      100u   /* `fast`: ~100 x 2 us polls before yielding    */

#define CAP_ALL (SF_CAP_SCOPE | SF_CAP_METER | SF_CAP_PROPS)

static struct {
    bool        on, fast, hello_pending, props_pending, have_gen, have_meter;
    uint8_t     want;                       /* SF_CAP_* */
    uint16_t    seq, desc_next;
    uint32_t    last_gen, last_meter, props_hash, table_crc;
    uint32_t    props_ms, stats_ms, ok_ms, gen_end;
    const char *off_reason;
    sf_stats_t  st;
} s;

static uint32_t now_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }

/* ── the sink: one packet into the CDC, never waiting long ─────────────── */
static bool wait_ready(uint32_t budget_ms)
{
    if (usb_debug_stream_tx_ready(false)) return true;
    if (s.fast) {
        for (uint32_t i = 0; i < STREAM_SPIN_POLLS; i++) {
            usb_delay_us(2u);
            if (usb_debug_stream_tx_ready(false)) return true;
        }
    }
    for (uint32_t t = 0; t < budget_ms; t++) {
        vTaskDelay(1);
        if (usb_debug_stream_tx_ready(t >= 1u)) return true;   /* heal after 2 ms, like the shell */
    }
    return false;
}

static bool sink(const uint8_t *data, uint16_t len, void *ctx)
{
    (void)ctx;
    return wait_ready(STREAM_PKT_WAIT_MS) && usb_debug_stream_write(data, len);
}

/* Before a frame: may it start? Counts the drop when not. */
static bool frame_gate(uint8_t type)
{
    if (wait_ready(STREAM_START_WAIT_MS)) return true;
    s.st.dropped[type]++;
    return false;
}

/* After a frame: count it. The seq was spent either way. */
static bool frame_done(uint8_t type, const sf_writer_t *w, bool ok)
{
    s.seq++;
    s.st.bytes += w->sent;
    if (ok) {
        s.st.sent[type]++;
        s.ok_ms = now_ms();
    } else {
        s.st.aborts++;
        s.st.dropped[type]++;
    }
    return ok;
}

/* ── property table identity and snapshot ─────────────────────────────── */
static uint32_t table_crc(void)
{
    uint32_t c = 0xFFFFFFFFu;
    for (uint16_t i = 0; i < prop_count(); i++) {
        const prop_desc_t *p = prop_at(i);
        c = sf_table_crc_add(c, p->id, p->type, p->unit, p->decimals, p->group);
    }
    return ~c;
}

/* Values and epochs. Epochs alone are not enough: the keys still store most
 * settings directly (only the shell's `set` and sys.mode go through the
 * executor), so a V/div key press moves no epoch. The hash catches every
 * writer, the way redraw_gate.h argues for hashes over dirty flags. */
static uint32_t props_hash(void)
{
    uint32_t c = 0xFFFFFFFFu;
    for (uint16_t i = 0; i < prop_count(); i++) {
        const prop_desc_t *p = prop_at(i);
        const int32_t v = prop_get(p);
        c = sf_crc32_update(c, (const uint8_t *)&v, sizeof v);
    }
    for (uint8_t g = 0; g < PG_COUNT; g++) {
        const uint32_t e = prop_epoch[g];
        c = sf_crc32_update(c, (const uint8_t *)&e, sizeof e);
    }
    return c;
}

static sf_prop_t prop_i(uint16_t i, void *ctx)
{
    (void)ctx;
    const prop_desc_t *p = prop_at(i);
    const sf_prop_t r = { p->id, prop_get(p) };
    return r;
}

/* ── frames ──────────────────────────────────────────────────────────── */
static bool send_hello(void)
{
    if (!frame_gate(SF_T_HELLO)) return false;
    const sf_hello_t h = {
        .caps = CAP_ALL, .on = s.want, .max_payload = SF_MAX_PAYLOAD,
        .rec_samples = FPGA_ADC_BUF_SIZE, .n_props = prop_count(), .table_crc = s.table_crc,
        .fw = "OpenScope 2C53T " __DATE__ " " __TIME__,
    };
    sf_writer_t w;
    const bool ok = frame_done(SF_T_HELLO, &w, sf_send_hello(&w, sink, NULL, s.seq, &h));
    if (ok) s.hello_pending = false;
    return ok;
}

static bool send_desc(void)
{
    if (!frame_gate(SF_T_PROPDESC)) return false;
    const prop_desc_t *p = prop_at(s.desc_next);
    const sf_propdesc_t d = {
        .id = p->id, .type = p->type, .unit = p->unit, .decimals = p->decimals, .group = p->group,
        .flags = p->flags, .min = p->min, .max = p->max, .def = prop_default(p),
        .name = p->name, .unit_sym = prop_unit_symbol(p->unit),
    };
    sf_writer_t w;
    const bool ok = frame_done(SF_T_PROPDESC, &w, sf_send_propdesc(&w, sink, NULL, s.seq, &d));
    if (ok) s.desc_next++;
    return ok;
}

static bool send_props(void)
{
    if (!frame_gate(SF_T_PROPS)) return false;
    uint32_t ep[PG_COUNT];
    for (uint8_t g = 0; g < PG_COUNT; g++) ep[g] = prop_epoch[g];
    const uint32_t h = props_hash();
    sf_writer_t w;
    const bool ok = frame_done(SF_T_PROPS, &w,
                               sf_send_props(&w, sink, NULL, s.seq, s.table_crc, PG_COUNT, ep,
                                             prop_count(), prop_i, NULL));
    if (ok) {
        s.props_pending = false;
        s.props_hash = h;
    }
    return ok;
}

static uint32_t gen_end_cb(void)
{
    s.gen_end = fpga_acq_frame_generation();
    return s.gen_end;
}

static void rec_channel(sf_rec_ch_t *c, const scope_state_t *ss, uint8_t ch)
{
    const channel_state_t *cs = ch ? &ss->ch2 : &ss->ch1;
    const float pf = scope_cal_probe_factor((uint8_t)(ch + 1u));
    c->ch              = ch;
    c->vdiv_idx        = cs->vdiv_idx;
    c->zero_code       = scope_measure_zero_for(scope_vpos_active(), cs->position, (int)FPGA_ADC_OFFSET);
    c->coupling        = (uint8_t)cs->coupling;
    c->position        = cs->position;
    c->probe_x         = (uint16_t)(pf + 0.5f);
    c->volts_per_count = scope_cal_volts_per_count((uint8_t)(ch + 1u), cs->vdiv_idx);
    c->volts_per_div   = scope_cal_volts_per_div((uint8_t)(ch + 1u), cs->vdiv_idx);
    c->codes           = ch ? fpga_get_ch2_buf() : fpga_get_ch1_buf();
}

static bool poll_record(void)
{
    const uint32_t g0 = fpga_acq_frame_generation();
    if (g0 == 0u || (g0 & 1u) != 0u) return false;        /* none committed yet / committing */
    if (s.have_gen && g0 == s.last_gen) return false;     /* already taken */
    if (!fpga_data_ready() || fpga_get_ch1_buf() == NULL) return false;

    const scope_state_t *ss = scope_state_get();
    sf_record_t r;
    memset(&r, 0, sizeof r);
    r.gen  = g0;
    r.t_ms = now_ms();
    r.fs_hz = fpga_record_sample_rate();
    r.s_per_div = ss->roll ? scope_roll_seconds_per_div(ss->roll_step)
                           : scope_timebase_seconds_per_div(ss->timebase_idx);
    r.n = FPGA_ADC_BUF_SIZE;
    const bool ordered = fpga_acq_record_time_ordered();
    r.trig_idx = ordered ? (uint16_t)(FPGA_ADC_BUF_SIZE / 2u) : (uint16_t)SF_TRIG_NONE;
    r.flags = (uint8_t)((fpga_acq_record_interleaved() ? SF_REC_INTERLEAVED : 0u) |
                        (ordered ? SF_REC_TIME_ORDERED : 0u) |
                        ((ss->roll || fpga_roll_active()) ? SF_REC_ROLL : 0u) |
                        (fpga_acq_record_coherent() ? SF_REC_COHERENT : 0u) |
                        (ss->running ? SF_REC_RUNNING : 0u) |
                        (ss->true_scale ? SF_REC_TRUE_SCALE : 0u));
    r.kind = fpga_acq_record_kind();
    r.tb_code = ss->timebase_idx;
    if (ss->ch1.enabled) rec_channel(&r.ch[r.nch++], ss, 0u);
    if (ss->ch2.enabled && !fpga_acq_record_interleaved()) rec_channel(&r.ch[r.nch++], ss, 1u);
    if (r.nch == 0u) rec_channel(&r.ch[r.nch++], ss, 0u);  /* the model refuses both-off */
    if (fpga_acq_frame_generation() != g0) return false;  /* a commit hit the metadata: next pass */

    if (s.have_gen) {
        const uint32_t steps = (g0 - s.last_gen) / 2u;
        if (steps > 1u) s.st.rec_missed += steps - 1u;
    }
    s.have_gen = true;
    s.last_gen = g0;                                       /* consumed, sent or not */

    if (!frame_gate(SF_T_RECORD)) return false;
    sf_writer_t w;
    const bool ok = frame_done(SF_T_RECORD, &w, sf_send_record(&w, sink, NULL, s.seq, &r, gen_end_cb));
    if (ok && s.gen_end != g0) s.st.rec_torn++;
    return ok;
}

static bool poll_meter(void)
{
    if (meter_reading.update_count == s.last_meter && s.have_meter) return false;
    meter_reading_t m;
    if (!meter_data_snapshot(&m) || !m.valid) return false;
    if (s.have_meter && m.update_count == s.last_meter) return false;
    s.have_meter = true;
    s.last_meter = m.update_count;
    if (!frame_gate(SF_T_METER)) return false;
    const sf_meter_t f = {
        .update_count = m.update_count, .t_ms = now_ms(), .value = m.value, .bcd = m.bcd_value,
        .func = m.submode, .auto_fn = m.auto_fn, .result_class = (uint8_t)m.result_class,
        .status_code = m.status_code, .decimal_pos = m.decimal_pos, .range_index = m.range_index,
        .flags = (uint8_t)((m.valid ? SF_MTR_VALID : 0u) | (m.negative ? SF_MTR_NEGATIVE : 0u) |
                           (m.is_ac ? SF_MTR_AC : 0u) | (m.is_auto_range ? SF_MTR_AUTORANGE : 0u) |
                           (m.is_hold ? SF_MTR_HOLD : 0u) | (m.continuity_beep ? SF_MTR_BEEP : 0u)),
        .unit = m.unit_suffix, .display = m.display_str, .range_label = m.range_label,
    };
    sf_writer_t w;
    return frame_done(SF_T_METER, &w, sf_send_meter(&w, sink, NULL, s.seq, &f));
}

static bool send_stats(void)
{
    s.stats_ms = now_ms();
    if (!frame_gate(SF_T_STATS)) return false;
    s.st.t_ms = s.stats_ms;
    sf_writer_t w;
    return frame_done(SF_T_STATS, &w, sf_send_stats(&w, sink, NULL, s.seq, &s.st));
}

/* ── the loop hook ───────────────────────────────────────────────────── */
static void stream_stop(const char *why)
{
    s.on = false;
    s.off_reason = why;
}

bool stream_poll(void)
{
    if (!s.on) return false;
    if (!usb_debug_connected()) { stream_stop("usb disconnected"); return false; }
    if (fw_loader_active()) return false;                 /* the loader owns the channel */
    const uint32_t now = now_ms();
    if ((uint32_t)(now - s.ok_ms) > STREAM_IDLE_OFF_MS) { stream_stop("host not reading"); return false; }

    if (s.hello_pending) return send_hello();
    if (s.want & SF_CAP_PROPS) {
        if (s.desc_next < prop_count()) return send_desc();
        if (!s.props_pending && (uint32_t)(now - s.props_ms) >= STREAM_PROPS_CHECK_MS) {
            s.props_ms = now;
            if (props_hash() != s.props_hash) s.props_pending = true;
        }
        if (s.props_pending) return send_props();
    }
    if ((s.want & SF_CAP_SCOPE) && poll_record()) return true;
    if ((s.want & SF_CAP_METER) && poll_meter()) return true;
    if ((uint32_t)(now - s.stats_ms) >= STREAM_STATS_MS) return send_stats();
    return false;
}

/* ── shell ───────────────────────────────────────────────────────────── */
static void print_status(void)
{
    const sf_stats_t *t = &s.st;
    usb_debug_printf("stream: on=%u scope=%u meter=%u props=%u fast=%u seq=%u gen=%lu bytes=%lu "
                     "aborts=%lu%s%s\r\n",
                     s.on, !!(s.want & SF_CAP_SCOPE), !!(s.want & SF_CAP_METER),
                     !!(s.want & SF_CAP_PROPS), s.fast, s.seq,
                     (unsigned long)fpga_acq_frame_generation(), (unsigned long)t->bytes,
                     (unsigned long)t->aborts,
                     s.off_reason ? " off_reason=" : "", s.off_reason ? s.off_reason : "");
    usb_debug_printf("stream: sent/dropped record=%lu/%lu missed=%lu torn=%lu meter=%lu/%lu "
                     "props=%lu/%lu desc=%lu/%lu hello=%lu/%lu stats=%lu/%lu\r\n",
                     (unsigned long)t->sent[SF_T_RECORD], (unsigned long)t->dropped[SF_T_RECORD],
                     (unsigned long)t->rec_missed, (unsigned long)t->rec_torn,
                     (unsigned long)t->sent[SF_T_METER], (unsigned long)t->dropped[SF_T_METER],
                     (unsigned long)t->sent[SF_T_PROPS], (unsigned long)t->dropped[SF_T_PROPS],
                     (unsigned long)t->sent[SF_T_PROPDESC], (unsigned long)t->dropped[SF_T_PROPDESC],
                     (unsigned long)t->sent[SF_T_HELLO], (unsigned long)t->dropped[SF_T_HELLO],
                     (unsigned long)t->sent[SF_T_STATS], (unsigned long)t->dropped[SF_T_STATS]);
}

void cmd_stream(const char *args)
{
    while (*args == ' ') args++;
    if (strncmp(args, "on", 2) == 0 && (args[2] == '\0' || args[2] == ' ')) {
        uint8_t want = 0;
        bool fast = false;
        const char *p = args + 2;
        while (*p) {
            while (*p == ' ') p++;
            const char *e = p;
            while (*e && *e != ' ') e++;
            const size_t n = (size_t)(e - p);
            if (n == 0u) break;
            if (n == 5u && strncmp(p, "scope", 5) == 0) want |= SF_CAP_SCOPE;
            else if (n == 5u && strncmp(p, "meter", 5) == 0) want |= SF_CAP_METER;
            else if (n == 5u && strncmp(p, "props", 5) == 0) want |= SF_CAP_PROPS;
            else if (n == 4u && strncmp(p, "fast", 4) == 0) fast = true;
            else if (n == 3u && strncmp(p, "all", 3) == 0) want |= CAP_ALL;
            else {
                usb_debug_printf("stream: unknown '%.*s' (scope meter props all fast)\r\n", (int)n, p);
                return;
            }
            p = e;
        }
        if (want == 0u) want = CAP_ALL;
        const uint16_t seq = s.seq;
        memset(&s, 0, sizeof s);
        s.seq = seq;
        s.want = want;
        s.fast = fast;
        s.table_crc = table_crc();
        s.hello_pending = true;
        s.props_pending = true;
        s.ok_ms = s.props_ms = s.stats_ms = now_ms();
        s.on = true;
        usb_debug_printf("stream: on%s%s%s%s (frames follow; docs/stream_protocol.md)\r\n",
                         (want & SF_CAP_SCOPE) ? " scope" : "", (want & SF_CAP_METER) ? " meter" : "",
                         (want & SF_CAP_PROPS) ? " props" : "", fast ? " fast" : "");
        return;
    }
    if (strcmp(args, "off") == 0) {
        if (s.on) (void)send_stats();                     /* the last counters, before the reply */
        stream_stop("off");
        print_status();
        return;
    }
    if (*args == '\0' || strcmp(args, "status") == 0) {
        print_status();
        return;
    }
    usb_debug_printf("usage: stream on [scope] [meter] [props] [fast] | off | status\r\n");
}

#endif /* FEATURE_STREAM */
