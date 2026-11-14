/*
 * OpenScope 2C53T - the executor (model_exec.h): request queue, drain on the
 * input task, seq-tagged result slots, withdrawal on timeout.
 *
 * No FreeRTOS here: everything outside this file is model_exec_port_t.
 */
#include "model_exec.h"

#include <string.h>

static const model_exec_port_t *g_port;
static volatile bool g_ready;

/* Caller side. One waiting caller at a time (the shell task runs one command
 * at a time); posts use seq 0 and touch none of this. */
static uint32_t g_next_seq = 1u;
/* Every request with seq <= this (modular) was given up by its caller. */
static volatile uint32_t g_withdrawn_upto;
/* Executor side: the request being applied (written before it looks at the
 * withdrawal; model_exec.h WITHDRAWAL). */
static volatile uint32_t g_started_seq;

/* Results: slot seq % N, tagged with seq (written last). */
static volatile model_result_t g_slot[MODEL_EXEC_QUEUE_LEN];

/* The drain's working copies: static, so the input task's stack carries no
 * request (72 B) or note. Executor only. */
static model_req_t    g_cur;
static model_result_t g_res;
static prop_note_t    g_note;

static model_exec_stats_t g_stats;
volatile uint32_t model_foreign_sets;

/* ═══ helpers ════════════════════════════════════════════════════════ */

static void note_set(prop_note_t *n, const char *t)
{
    size_t i = 0;
    for (; t && t[i] && i + 1 < sizeof n->text; i++) n->text[i] = t[i];
    n->text[i] = '\0';
}

static bool withdrawn(uint32_t seq)
{
    return (int32_t)(seq - g_withdrawn_upto) <= 0;
}

static uint32_t epoch_sum(void)
{
    uint32_t s = 0;
    for (unsigned g = 0; g < PG_COUNT; g++) s += prop_epoch[g];
    return s;
}

static const char *unwired_id(uint16_t id)
{
    return (g_port && g_port->unwired) ? g_port->unwired(id) : NULL;
}

const char *prop_unwired_reason(const prop_desc_t *p)
{
    return p ? unwired_id(p->id) : NULL;
}

/* Validation both sides share: the model's all-or-nothing check, then the
 * not-wired list. PS_OK or the status, with *failed and the note set. */
static prop_status_t check_pairs(const prop_pair_t *pairs, uint8_t n, uint8_t *failed,
                                 prop_note_t *note)
{
    prop_status_t s = prop_validate_pairs(pairs, n, failed);
    if (s != PS_OK) {
        if (n > MODEL_MAX_PAIRS)             note_set(note, "more than 8 pairs");
        else if (*failed >= n)               note_set(note, "more than 4 HW pairs"); /* split the set */
        else                                 note_set(note, "read only/out of range");
        return s;
    }
    for (uint8_t i = 0; i < n; i++) {
        const char *why = unwired_id(pairs[i].id);
        if (why) { *failed = i; note_set(note, why); return PS_REFUSED; }
    }
    *failed = n;
    return PS_OK;
}

static void slot_publish(uint32_t seq, const model_result_t *r)
{
    volatile model_result_t *s = &g_slot[seq % MODEL_EXEC_QUEUE_LEN];
    s->seq = 0u;                              /* invalid while it is being written */
    s->status = r->status;
    s->failed = r->failed;
    s->applied = r->applied;
    s->in_progress = false;
    for (size_t i = 0; i < sizeof r->note.text; i++) s->note.text[i] = r->note.text[i];
    s->seq = seq;                             /* the tag last */
}

static bool slot_take(uint32_t seq, model_result_t *out)
{
    volatile model_result_t *s = &g_slot[seq % MODEL_EXEC_QUEUE_LEN];
    if (s->seq != seq) return false;
    out->status = s->status;
    out->failed = s->failed;
    out->applied = s->applied;
    out->in_progress = false;
    for (size_t i = 0; i < sizeof out->note.text; i++) out->note.text[i] = s->note.text[i];
    out->note.text[sizeof out->note.text - 1] = '\0';
    if (s->seq != seq) return false;          /* rewritten while copying */
    out->seq = seq;
    return true;
}

/* ═══ setup ══════════════════════════════════════════════════════════ */

void model_exec_init(const model_exec_port_t *port)
{
    g_port = port;
    g_ready = (port && port->send && port->receive);
}

bool model_exec_ready(void) { return g_ready; }

void model_exec_stats(model_exec_stats_t *out)
{
    if (out) *out = g_stats;
}

void model_exec_check_context(void)
{
    if (g_port && g_port->on_executor && !g_port->on_executor()) model_foreign_sets++;
}

#ifdef MODEL_EXEC_TEST
void model_exec_test_reset(uint32_t first_seq)
{
    g_port = NULL;
    g_ready = false;
    if (first_seq == 0u) first_seq = 1u;
    g_next_seq = first_seq;
    g_withdrawn_upto = first_seq - 1u;
    g_started_seq = 0u;
    for (unsigned i = 0; i < MODEL_EXEC_QUEUE_LEN; i++) g_slot[i].seq = 0u;
    memset(&g_stats, 0, sizeof g_stats);
    model_foreign_sets = 0u;
}
#endif

/* ═══ caller side ════════════════════════════════════════════════════ */

uint32_t prop_request_timeout(const prop_pair_t *pairs, uint8_t n)
{
    for (uint8_t i = 0; pairs && i < n; i++) {
        const prop_desc_t *p = prop_by_id(pairs[i].id);
        if (p && (p->flags & PF_HW)) return MODEL_EXEC_HW_TIMEOUT_MS;
    }
    return MODEL_EXEC_TIMEOUT_MS;
}

prop_status_t prop_request(const prop_pair_t *pairs, uint8_t n, uint8_t origin,
                           uint32_t timeout_ms, model_result_t *out)
{
    model_result_t dummy;
    if (!out) out = &dummy;
    memset(out, 0, sizeof *out);
    out->failed = n;

    if (!g_ready) {
        note_set(&out->note, "executor not up yet");
        return (prop_status_t)(out->status = PS_BUSY);
    }
    if (g_port->on_executor && g_port->on_executor()) {
        /* Waiting here would wait for ourselves. */
        note_set(&out->note, "called on the executor");
        return (prop_status_t)(out->status = PS_BUSY);
    }
    prop_status_t s = check_pairs(pairs, n, &out->failed, &out->note);
    if (s != PS_OK) return (prop_status_t)(out->status = (uint8_t)s);

    model_req_t req;
    memset(&req, 0, sizeof req);
    uint32_t seq = g_next_seq++;
    if (seq == 0u) seq = g_next_seq++;        /* 0 means "posted" */
    req.seq = seq;
    req.n = n;
    req.origin = origin;
    for (uint8_t i = 0; i < n; i++) req.pairs[i] = pairs[i];

    if (!g_port->send(&req)) {
        g_stats.full++;
        note_set(&out->note, "request queue full");
        return (prop_status_t)(out->status = PS_BUSY);
    }
    g_stats.requests++;
    if (g_port->wake) g_port->wake();

    for (uint32_t waited = 0;; waited += MODEL_EXEC_POLL_MS) {
        if (slot_take(seq, out)) return (prop_status_t)out->status;
        if (waited >= timeout_ms) break;
        if (g_port->sleep_ms) g_port->sleep_ms(MODEL_EXEC_POLL_MS);
    }

    /* Give it up: withdrawal first, then look at the start (model_exec.h). */
    g_withdrawn_upto = seq;
    if (slot_take(seq, out)) return (prop_status_t)out->status;   /* answered just now */
    g_stats.timeouts++;
    out->seq = seq;
    out->in_progress = (g_started_seq == seq);
    out->failed = n;
    out->applied = 0;
    note_set(&out->note, out->in_progress ? "in progress" : "withdrawn");
    return (prop_status_t)(out->status = PS_TIMEOUT);
}

bool prop_post(uint16_t id, int32_t value, uint8_t origin)
{
    if (!g_ready) return false;
    model_req_t req;
    memset(&req, 0, sizeof req);
    req.seq = 0u;
    req.n = 1u;
    req.origin = origin;
    req.pairs[0].id = id;
    req.pairs[0].value = value;
    if (!g_port->send(&req)) { g_stats.full++; return false; }
    g_stats.posts++;
    if (g_port->wake) g_port->wake();
    return true;
}

/* ═══ executor side ══════════════════════════════════════════════════ */

/* The answer of a request whose pairs were all applied and some only stored
 * (model.h DEFERRED). The shell prints the note once, after the last pair,
 * so one pair keeps the op's reason ("stored: scope FE off") and a multi-set
 * names the stored pairs by position, 1-based ("stored, FE off: 1,3"; at
 * most MODEL_MAX_HW_PAIRS of them, since only a PF_HW pair can defer: 23
 * characters at most, the note's size). */
static void deferred_note(prop_note_t *note, uint8_t n, uint8_t mask, const char *why)
{
    if (n == 1u) { note_set(note, why); return; }
    char t[sizeof note->text];
    size_t l = 0;
    const char *head = "stored, FE off: ";
    while (*head && l + 1u < sizeof t) t[l++] = *head++;
    for (uint8_t i = 0; i < n && i < 8u; i++) {
        if (!(mask & (1u << i))) continue;
        if (t[l - 1u] != ' ' && l + 1u < sizeof t) t[l++] = ',';
        if (l + 1u < sizeof t) t[l++] = (char)('1' + i);
    }
    t[l] = '\0';
    note_set(note, t);
}

static void run(const model_req_t *r, model_result_t *res)
{
    memset(res, 0, sizeof *res);
    res->failed = r->n;
    prop_status_t s = check_pairs(r->pairs, r->n, &res->failed, &res->note);
    if (s != PS_OK) { res->status = (uint8_t)s; return; }

    const uint32_t before = epoch_sum();
    prop_status_t worst = PS_OK;
    uint8_t i, deferred = 0;                              /* bit i: pair i only stored */
    const char *why = NULL;
    for (i = 0; i < r->n; i++) {
        if (i && g_port->checkin) g_port->checkin();      /* one apply per check-in gap */
        s = prop_set_local(prop_by_id(r->pairs[i].id), r->pairs[i].value, r->origin, &g_note);
        if (g_note.text[0]) res->note = g_note;
        if (s != PS_OK && s != PS_CLAMPED) break;
        if (s == PS_CLAMPED) worst = PS_CLAMPED;
        if (prop_last_deferred()) { deferred |= (uint8_t)(1u << i); why = prop_last_deferred(); }
    }
    if (i < r->n) {
        res->status = (uint8_t)s;
        res->failed = i;
        res->applied = i;
        if (!g_note.text[0]) res->note.text[0] = '\0';   /* the failing pair's note only */
    } else {
        res->status = (uint8_t)worst;
        res->failed = r->n;
        res->applied = r->n;
        if (deferred) deferred_note(&res->note, r->n, deferred, why);   /* the fact that matters most */
    }
    if (r->origin != ORIGIN_KEY && epoch_sum() != before && g_port->changed)
        g_port->changed();                                 /* one redraw per request */
}

unsigned model_exec_drain(void)
{
    if (!g_ready) return 0;
    if (g_port->on_executor && !g_port->on_executor()) model_foreign_sets++;
    if (g_port->service) g_port->service();
    unsigned taken = 0;
    /* Bounded: the queue holds MODEL_EXEC_QUEUE_LEN, and a caller waits for
     * each answer, so twice that is more than one pass can be owed. */
    while (taken < 2u * MODEL_EXEC_QUEUE_LEN && g_port->receive(&g_cur)) {
        taken++;
        if (g_cur.seq) {
            g_started_seq = g_cur.seq;                     /* start, then look */
            if (withdrawn(g_cur.seq)) { g_stats.withdrawn++; continue; }
        }
        run(&g_cur, &g_res);
        g_stats.applied++;
        if (g_cur.seq) slot_publish(g_cur.seq, &g_res);
        if (g_port->checkin) g_port->checkin();
    }
    return taken;
}
