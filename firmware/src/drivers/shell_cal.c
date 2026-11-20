/*
 * OpenScope 2C53T - shell commands: calibration
 *
 * The calibration page, its backup, and the user calibration.
 */

#include "shell_parts.h"

void cmd_cal_status(void)
{
    cal_backup_report_t r;
    if (cal_backup_status(&r) != CAL_BK_OK) {
        usb_send_str("cal status: read failed\r\n");
        return;
    }
    usb_debug_printf("live  @0x%08lX  %-14s crc=0x%08lX\r\n",
                     (unsigned long)CAL_BACKUP_SRC_ADDR,
                     cal_page_class_str(r.live_class),
                     (unsigned long)r.live_crc);
    if (!r.backup_present) {
        usb_send_str("backup: region unreadable\r\n");
    } else if (r.backup_status != CAL_REC_OK) {
        usb_debug_printf("backup: %s (no valid record)\r\n",
                         cal_rec_status_str(r.backup_status));
    } else {
        usb_debug_printf("backup: valid v%u  payload_crc=0x%08lX  src=0x%08lX\r\n",
                         (unsigned)r.backup_version,
                         (unsigned long)r.backup_payload_crc,
                         (unsigned long)r.backup_src_addr);
        usb_debug_printf("match : %s\r\n", r.match ? "yes (live == backup)"
                                                   : "NO (live differs from backup)");
    }
}

void cmd_cal_backup(void)
{
    cal_bk_status_t st = cal_backup_store();
    usb_debug_printf("cal backup: %s\r\n", cal_bk_status_str(st));
    if (st == CAL_BK_ERR_LIVE_BLANK) {
        usb_send_str("  (MCU page is blank/zeroed — nothing to preserve, existing backup kept)\r\n");
    }
}

void cmd_cal_restore(const char *args)
{
    char abuf[40];
    char *saveptr = NULL;
    bool force = false;
    const char *confirm = NULL;

    if (strlen(args) >= sizeof(abuf)) {
        usb_send_str("Usage: cal restore [force] CONFIRM\r\n"); return;
    }
    strcpy(abuf, args);
    char *t1 = strtok_r(abuf, " \t", &saveptr);
    char *t2 = strtok_r(NULL, " \t", &saveptr);
    if (t1 != NULL && strcmp(t1, "force") == 0) {
        force = true;
        confirm = t2;
    } else {
        confirm = t1;
    }
    if (confirm == NULL || strcmp(confirm, "CONFIRM") != 0) {
        usb_send_str("Refused: append CONFIRM. This WRITES MCU flash 0x08006000.\r\n"
                     "  cal restore CONFIRM        (refuses over a programmed page)\r\n"
                     "  cal restore force CONFIRM  (overwrites a programmed page)\r\n");
        return;
    }
    cal_bk_status_t st = cal_backup_restore(force);
    usb_debug_printf("cal restore%s: %s\r\n", force ? " force" : "", cal_bk_status_str(st));
    if (st == CAL_BK_ERR_LIVE_PRECIOUS) {
        usb_send_str("  live page holds data — use 'cal restore force CONFIRM' only if you are sure\r\n");
    }
}

/*
 * Stock calibration page in use (parity C4; src/util/calpage.h, scope_vpos.h).
 *   cal page [on|off|reload]     decoded table, bank in force, substitutions
 *   cal page calc <1|2> <bank> <range> <pos>   one code, ours vs stock's
 * The vertical position is the property chN.pos; `cal page` prints the
 * offset code it gave and the reference read back, per channel.
 * The page is only READ here; `cal restore` is still the one writer.
 */
static void fmt_g(char *out, size_t n, float g)
{
    if (g <= 0.0f) { snprintf(out, n, "-- (compiled table)"); return; }
    const unsigned long x = (unsigned long)(g * 10000.0f + 0.5f);
    snprintf(out, n, "%lu.%04lu", x / 10000ul, x % 10000ul);
}

static void print_vpos_channel(uint8_t ch)
{
    uint8_t bank = 0, range = 0;
    uint16_t code = 0;
    /* The record median is post FPGA_ADC_OFFSET (raw - 28, clamped at 0), so
     * raw ~= median + 28; a quiet input at pos p should sit at raw ~128 + p. */
    const uint32_t med = scope_center_buf_median(ch);
    const uint16_t hw = (ch == 2u) ? scope_trigger_ch2_last() : scope_trigger_dac_last();
    if (scope_vpos_last(ch, &bank, &range, &code))
        usb_debug_printf("CH%u: range %u pos %d -> %s %u (bank %u)  ref now %u  "
                         "rec median %lu (raw ~%lu)\r\n",
                         (unsigned)ch, (unsigned)range, scope_vpos_get(ch),
                         ch == 2u ? "TMR13_C1DT" : "DAC1", (unsigned)code, (unsigned)bank,
                         (unsigned)hw, (unsigned long)med, (unsigned long)(med + 28u));
    else
        usb_debug_printf("CH%u: pos %d, not written from the page  ref now %u  "
                         "rec median %lu (raw ~%lu)\r\n",
                         (unsigned)ch, scope_vpos_get(ch), (unsigned)hw,
                         (unsigned long)med, (unsigned long)(med + 28u));
}

static void cmd_cal_page_calc(const char *args)
{
    char *end;
    long v[4];
    for (int i = 0; i < 4; i++) {
        while (*args == ' ') args++;
        v[i] = strtol(args, &end, 10);
        if (end == args) {
            usb_send_str("Usage: cal page calc <1|2> <bank 0-2> <range 0-9> <pos -100..100>\r\n");
            return;
        }
        args = end;
    }
    if ((v[0] != 1 && v[0] != 2) || v[1] < 0 || v[1] > 2 || v[2] < 0 || v[2] > 9) {
        usb_send_str("Usage: cal page calc <1|2> <bank 0-2> <range 0-9> <pos -100..100>\r\n");
        return;
    }
    calpage_t p;
    calpage_parse((const uint8_t *)CAL_BACKUP_SRC_ADDR, CALPAGE_LEN, &p);
    const uint8_t c = (uint8_t)(v[0] - 1), b = (uint8_t)v[1], r = (uint8_t)v[2];
    const int pos = calpage_clamp_pos((int)v[3]);
    /* What stock writes for the entry AS STORED: CH1 low 12 bits (bfi at
     * 0x08008A44), CH2 the full value into TMR13 C1DT. */
    const uint32_t st = calpage_stock_formula(p.raw[c][0][b][r], p.raw[c][1][b][r], pos);
    uint16_t ours = 0;
    const bool ok = calpage_dac_code(&p, c, b, r, pos, &ours);
    usb_debug_printf("CH%ld b%ld r%ld pos %d: stock %lu%s  ours ", v[0], v[1], v[2], pos,
                     (unsigned long)(c == 0u ? (st & 0x0FFFu) : st),
                     c == 0u ? " (12-bit cut)" : " (unmasked)");
    if (ok)
        usb_debug_printf("%u (pair from bank %d)\r\n", (unsigned)ours, (int)p.src_bank[c][b][r]);
    else
        usb_debug_printf("none (%s)\r\n", calpage_usable(&p) ? "no clean bank" : calpage_status_str(p.status));
}

void cmd_cal_page(const char *args)
{
    while (args && *args == ' ') args++;
    if (args && strncmp(args, "calc", 4) == 0) { cmd_cal_page_calc(args + 4); return; }
    if (args && *args) {
        if (strcmp(args, "on") == 0) {
            if (!scope_vpos_enable(true)) usb_send_str("refused: page not usable\r\n");
        } else if (strcmp(args, "off") == 0) {
            (void)scope_vpos_enable(false);
        } else if (strcmp(args, "reload") == 0) {
            scope_vpos_init();
        } else {
            usb_send_str("Usage: cal page [on|off|reload] | cal page calc <1|2> <bank> <range> <pos>\r\n");
            return;
        }
    }

    calpage_t p;   /* ~440 B on this task's 3 KB stack */
    calpage_parse((const uint8_t *)CAL_BACKUP_SRC_ADDR, CALPAGE_LEN, &p);
    usb_debug_printf("cal page @0x%08lX: %s, %s  sig 0x%02X  sentinel 0x%04X\r\n",
                     (unsigned long)CAL_BACKUP_SRC_ADDR, calpage_status_str(p.status),
                     scope_vpos_active() ? "IN USE" : "not in use",
                     (unsigned)p.signature, (unsigned)p.sentinel);
    char g1[24], g2[24];
    fmt_g(g1, sizeof(g1), scope_cal_stock_gain(1u));
    fmt_g(g2, sizeof(g2), scope_cal_stock_gain(2u));
    usb_debug_printf("gain g=(m-28)/150: CH1 m=%u g=%s  CH2 m=%u g=%s\r\n",
                     (unsigned)p.m[0], g1, (unsigned)p.m[1], g2);
    usb_debug_printf("page header (stock UI state, not used): pos %d/%d mask %u tb 0x%02X\r\n",
                     (int)p.hdr_pos[0], (int)p.hdr_pos[1], (unsigned)p.hdr_mask, (unsigned)p.hdr_tb);
    const uint8_t tb = fpga_acq_rate_idx_get(), mask = fpga.channel_mask;
    usb_debug_printf("in force: tb 0x%02X mask %u -> bank %u (tb>=5: 0; tb==4 or mask 3: 1; else 2)\r\n",
                     (unsigned)tb, (unsigned)mask, (unsigned)calpage_bank_select(tb, mask));
    print_vpos_channel(1u);
    print_vpos_channel(2u);
    if (p.status == CALPAGE_ST_SIGNATURE || p.status == CALPAGE_ST_SHORT ||
        p.status == CALPAGE_ST_NONE)
        return;

    usb_debug_printf("clean pairs %u/60, substituted %u, unusable %u\r\n",
                     (unsigned)p.n_clean_pairs, (unsigned)p.n_substituted, (unsigned)p.n_unusable);
    for (uint8_t c = 0; c < CALPAGE_CHANNELS; c++)
        for (uint8_t b = 0; b < CALPAGE_BANKS; b++)
            for (uint8_t r = 0; r < CALPAGE_RANGES; r++) {
                const int8_t sb = p.src_bank[c][b][r];
                if (sb == (int8_t)b) continue;
                const uint8_t f = p.flags[c][0][b][r] | p.flags[c][1][b][r];
                usb_debug_printf("  CH%u b%u r%u: base %u upper %u [%s%s%s] -> ",
                                 (unsigned)(c + 1u), (unsigned)b, (unsigned)r,
                                 (unsigned)p.raw[c][0][b][r], (unsigned)p.raw[c][1][b][r],
                                 (f & CALPAGE_F_RANGE) ? " >4095/blank" : "",
                                 (f & CALPAGE_F_SPAN)  ? " span" : "",
                                 (f & CALPAGE_F_XBANK) ? " cross-bank" : "");
                if (sb < 0) usb_send_str("NO clean bank: page not used for this range\r\n");
                else        usb_debug_printf("bank %d used\r\n", (int)sb);
            }
    usb_send_str("table ('!' >4095/blank, '?' span or cross-bank outlier):\r\n");
    for (uint8_t c = 0; c < CALPAGE_CHANNELS; c++)
        for (uint8_t e = 0; e < CALPAGE_ENDPOINTS; e++)
            for (uint8_t b = 0; b < CALPAGE_BANKS; b++) {
                char line[96];
                int n = snprintf(line, sizeof(line), "CH%u %-5s b%u:", (unsigned)(c + 1u),
                                 e ? "upper" : "base", (unsigned)b);
                for (uint8_t r = 0; r < CALPAGE_RANGES && n > 0 && n < (int)sizeof(line); r++) {
                    const uint8_t f = p.flags[c][e][b][r];
                    n += snprintf(line + n, sizeof(line) - (size_t)n, "%6u%c",
                                  (unsigned)p.raw[c][e][b][r],
                                  (f & CALPAGE_F_RANGE) ? '!' : (f ? '?' : ' '));
                }
                usb_debug_printf("%s\r\n", line);
            }
}

/* `cal base [clear]`: the baseline correction's RAM deltas on the page pair. */
void cmd_cal_base(const char *args)
{
    while (*args == ' ') args++;
    if (strcmp(args, "clear") == 0) {
        if (scope_selfcal_busy()) { usb_send_str("busy\r\n"); return; }
        scope_vpos_corr_clear();
        usb_send_str("corrections cleared (page values in force)\r\n");
        return;
    }
    if (*args) { usb_send_str("usage: cal base [clear]\r\n"); return; }
    usb_debug_printf("baseline corrections in RAM: %u (`cal user save` keeps them across power-off)\r\n",
                     scope_vpos_corr_count());
    for (uint8_t ch = 1; ch <= 2u; ch++)
        for (uint8_t b = 0; b < CALPAGE_BANKS; b++)
            for (uint8_t r = 0; r < CALPAGE_RANGES; r++) {
                int16_t db, du;
                uint16_t pb, pu;
                if (!scope_vpos_corr_get(ch, b, r, &db, &du)) continue;
                const bool pair = scope_vpos_page_pair(ch, b, r, &pb, &pu);
                usb_debug_printf("  CH%u b%u r%u: base %+d upper %+d", (unsigned)ch, (unsigned)b,
                                 (unsigned)r, (int)db, (int)du);
                if (pair)
                    usb_debug_printf("  -> %u/%u (page %u/%u)", (unsigned)(pb + db),
                                     (unsigned)(pu + du), (unsigned)pb, (unsigned)pu);
                usb_send_str("\r\n");
            }
}

/* `cal user [show|save|clear]` — OpenScope's own corrections in the W25Q
 * (usercal.h, FLASH_REGION_USER_CAL 0xF00000): the baseline deltas (long
 * AUTO / `fpga scope basecal`) and the interleave B correction (`fpga
 * interleave cal`). Owner policy: nothing is saved unless asked here (or by
 * a UI confirm); the stock page is never written. `clear` appends an empty
 * record (no erase) and drops the RAM corrections too, so the live state and
 * the next boot agree; `cal base clear` is RAM only. */
void cmd_cal_user(const char *args)
{
    while (*args == ' ') args++;
    const usercal_report_t *r = usercal_report();
    if (strcmp(args, "save") == 0 || strcmp(args, "clear") == 0) {
        if (scope_selfcal_busy()) { usb_send_str("busy\r\n"); return; }
        const bool clr = (args[0] == 'c');
        const flash_region_status_t st = clr ? usercal_clear_saved() : usercal_save();
        if (st != FLASH_REGION_OK) {
            usb_debug_printf("cal user %s: FAILED, %s; nothing changed\r\n", args, flash_region_strerror(st));
            return;
        }
        if (clr) usb_send_str("cal user clear: empty record saved; RAM corrections dropped (page values in force)\r\n");
        else usb_debug_printf("cal user save: saved %u baseline deltas\r\n", scope_vpos_corr_count());
        /* fall through to the status below */
    } else if (*args && strcmp(args, "show") != 0) {
        usb_send_str("usage: cal user [show|save|clear]\r\n");
        return;
    }
    usercal_t u;   /* ~290 B on this task's 3 KB stack */
    const usercal_status_t st = usercal_store_load(&u);
    const uint32_t live = usercal_live_table_crc();
    uint32_t used = 0, nrec = 0;
    (void)flash_region_log_info(USERCAL_REGION, &used, NULL, &nrec);
    usb_debug_printf("user cal @0x%06lX: %s; log %lu B, %lu records\r\n",
                     (unsigned long)flash_region_get(USERCAL_REGION)->start, usercal_status_str(st),
                     (unsigned long)used, (unsigned long)nrec);
    if (st == USERCAL_ST_OK) {
        usb_debug_printf("  saved: %u baseline deltas, table %08lX (%s live %08lX); interleave %s",
                         usercal_count(&u), (unsigned long)u.table_crc,
                         u.table_crc == live ? "matches" : "STALE vs", (unsigned long)live,
                         u.il_present ? "gain " : "none");
        if (u.il_present)
            usb_debug_printf("%ld/1000000 offset %ld/10000", (long)u.il_gain_e6, (long)u.il_off_e4);
        usb_debug_printf(", %u knots, skew %ld ppm\r\n", (unsigned)u.il_n_knots, (long)u.il_skew_ppm);
    }
    usb_debug_printf("  boot: %s", usercal_status_str(r->load_st));
    if (r->loaded)
        usb_debug_printf(", %u applied, %u not applied%s, interleave %s",
                         (unsigned)r->applied, (unsigned)r->refused,
                         r->table_match ? "" : " (page table changed since the save)",
                         r->il_applied ? "applied" : "none");
    usb_debug_printf("\r\n  live: %u baseline deltas in RAM\r\n", scope_vpos_corr_count());
}
