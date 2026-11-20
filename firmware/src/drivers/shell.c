/*
 * OpenScope 2C53T - debug shell: table and task
 *
 * The command table, the line editor and dispatcher, the shell task, and
 * the helpers every command file uses.
 */

#include "shell_parts.h"

/* ═══════════════════════════════════════════════════════════════════
 * Hex Parsing Helpers
 * ═══════════════════════════════════════════════════════════════════ */

/* Parse a hex string like "0x40021000" or "40021000" into uint32_t.
 * Returns 0 on success, -1 on error. */
int parse_hex32(const char *s, uint32_t *out)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    char *end;
    *out = strtoul(s, &end, 16);
    return (*end == '\0' || *end == ' ' || *end == '\r' || *end == '\n') ? 0 : -1;
}

/* For the feature shell files (shell_internal.h); parse_hex32() unchanged. */
int usb_debug_sh_parse_hex32(const char *s, uint32_t *out)
{
    return parse_hex32(s, out);
}

/* Parse a decimal or hex string into uint32_t */
int parse_int(const char *s, uint32_t *out)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return parse_hex32(s, out);
    char *end;
    *out = strtoul(s, &end, 10);
    return (*end == '\0' || *end == ' ' || *end == '\r' || *end == '\n') ? 0 : -1;
}

/* For the feature shell files (shell_internal.h); parse_int() unchanged. */
int usb_debug_sh_parse_int(const char *s, uint32_t *out)
{
    return parse_int(s, out);
}

/* ═══════════════════════════════════════════════════════════════════
 * Command Handlers
 * ═══════════════════════════════════════════════════════════════════ */

/* cmd_help is defined next to the command table at the bottom of the file —
 * it renders the per-row help strings, so it must live after the table. */
static void cmd_help(void);

static void cmd_version(void)
{
    /* The compiler rides on the Build line: whether the RAM installer called
     * flash-resident memset (issue #42) depended on the toolchain, and every
     * bench script already logs the line that starts with "Build:". */
    usb_debug_printf(
        "OpenScope 2C53T\r\n"
        "Build: " __DATE__ " " __TIME__ ", gcc " __VERSION__ "\r\n"
        "FPGA built-in design: " FPGA_BITSTREAM_NAME " (OpenScope's own capture core)\r\n"
        "MCU: AT32F403A @ %uMHz\r\n"
        "SRAM: 224KB (EOPB0=0xFE)\r\n",
        system_core_clock / 1000000
    );
#ifdef MODEL_PROFILE_NAME
    /* Built with a defaults profile (make DEFAULTS=..., model.c): the
     * built-in defaults of this image are not all the table's. */
    usb_debug_printf("Defaults profile: " MODEL_PROFILE_NAME " (%d built-in defaults changed)\r\n",
                     MODEL_PROFILE_OVERRIDES);
#endif
    /* What this build contains (firmware/features.mk); `features` lists the
     * ones that are off too. */
    usb_send_str("Features:");
    for (uint8_t i = 0; i < features_count(); i++) {
        const feature_info_t *f = features_at(i);
        if (f->on) usb_debug_printf(" %s", f->name);
    }
    usb_debug_printf(" (mask 0x%08lx)\r\n", (unsigned long)features_mask());
}

/* features: every feature the manifest knows, one per line:
 *   <name> on|off bit <n>  <description>
 * so a script can tell "off in this build" from "never heard of it". */
static void cmd_features(void)
{
    for (uint8_t i = 0; i < features_count(); i++) {
        const feature_info_t *f = features_at(i);
        usb_debug_printf("%-10s %-3s bit %-2u %s\r\n", f->name, f->on ? "on" : "off", (unsigned)f->bit, f->desc);
    }
    usb_debug_printf("mask 0x%08lx\r\n", (unsigned long)features_mask());
}

uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

/* For the feature shell files (shell_internal.h); crc32_update() unchanged. */
uint32_t usb_debug_sh_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    return crc32_update(crc, data, len);
}

uint32_t shell_now_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }

/* ── Stock-parity meter shell (M1/M4/M5) ────────────────────────────────
 *
 * (the function is the property meter.func, degC/degF is meter.degf)
 * `meter state`           everything decoded from the last frame, the switch
 *                         counters, the beep source and the guard
 * `meter fast [on|off]`   one-word switching (default on); off = old PC11 path
 * `meter keepalive [..]`  the 4 Hz unobeyed poll (default on)
 * `meter beep ...`        the SoC-edge buzzer (default off)
 */
const char *onoff(bool b) { return b ? "on" : "off"; }

/* For the feature shell files (shell_internal.h); onoff() unchanged. */
const char *usb_debug_sh_onoff(bool b)
{
    return onoff(b);
}

bool shell_onoff_arg(const char *args, bool *out)
{
    while (*args == ' ') args++;
    if (strncmp(args, "on", 2) == 0)  { *out = true;  return true; }
    if (strncmp(args, "off", 3) == 0) { *out = false; return true; }
    return false;
}

/* For the feature shell files (shell_internal.h); shell_onoff_arg() unchanged. */
bool usb_debug_sh_onoff_arg(const char *args, bool *out)
{
    return shell_onoff_arg(args, out);
}

/* One scratch buffer shared by the shell's bus-snapshot commands (`spi3 read`
 * uses the first half; `spi3 frame` uses both halves for a coherent CH1+CH2
 * grab). They all run on the single shell task and never nest, so sharing is
 * safe — and it keeps guest RAM in budget: two per-command 1 KB buffers
 * overflowed the region by 368 bytes (2ff992c, caught once #31 unblocked the
 * guest link). One 2 KB buffer costs +1 KB over the old lone snapshot. */
uint8_t shell_bus_scratch[2 * FPGA_ADC_BUF_SIZE] __attribute__((aligned(4)));

/* For the feature shell files (shell_internal.h); shell_bus_scratch unchanged. */
uint8_t *usb_debug_sh_scratch(void)
{
    return shell_bus_scratch;
}

/* The 1 s timer hook of the pin log (`pinlog`, src/re/re_board.c): a no-op
 * in an image without the reverse-engineering toolkit. */
void usb_debug_pinlog_tick(void)
{
    retools_pinlog_tick();
}

static void cmd_uptime(void)
{
    extern volatile uint32_t uptime_seconds;
    uint32_t s = uptime_seconds;
    usb_debug_printf("Uptime: %lu:%02lu:%02lu\r\n", s / 3600, (s % 3600) / 60, s % 60);
}

/* Claim the SPI3 bus for a raw shell command (audit 2026-08-20, P0.1).
 * The acquisition task runs at priority 3, ABOVE this shell task at 2, so
 * without a park it preempts any spi3_raw_xfer sequence mid-CS-window and
 * both transfers interleave into garbage — a live source of bad experimental
 * data before 2026-08-20. Used at the DISPATCH level around every command
 * that drives the bus raw; pair with fpga_acq_resume() after the command
 * returns (dispatch-level pairing survives the commands' early returns). */
static bool spi3_shell_claim(void)
{
    if (!fpga_acq_pause()) {
        usb_send_str("ERR: SPI3 busy — acquisition task would not park\r\n");
        return false;
    }
    return true;
}

/* ── `tasks`: every FreeRTOS task's stack and the heap ──────────────────
 * One line per task: name, priority, state, stack size and the least stack
 * that was ever left, both in words (4 bytes), and that margin as a share
 * of the stack. The size is read from the task's control block
 * (configRECORD_STACK_HIGH_ADDRESS: the top is aligned down to 8 bytes, so
 * it is the created depth rounded to an even number of words). The key
 * (input) task is the one named "key" (main.c: every key, every screen and
 * every property setter runs on it); its line is marked and repeated at the
 * end. uxTaskGetSystemState suspends the scheduler while it walks the task
 * lists and scans each stack for its untouched fill (not timed on the unit
 * yet: a few thousand byte compares). Lines are in creation order. */
static void cmd_tasks(void)
{
    static const char *const state[] = { "run", "ready", "block", "susp", "del", "?" };
    TaskStatus_t *t = (TaskStatus_t *)(void *)shell_bus_scratch;      /* 4-aligned, shell task only */
    const UBaseType_t max = (UBaseType_t)(sizeof(shell_bus_scratch) / sizeof(TaskStatus_t));
    const UBaseType_t n = uxTaskGetSystemState(t, max, NULL);
    long key_size = -1, key_free = -1;

    if (n == 0u) {
        usb_debug_printf("tasks: more than %u tasks (%u)\r\n", (unsigned)max,
                         (unsigned)uxTaskGetNumberOfTasks());
        return;
    }
    usb_debug_printf("tasks: %u (stack and min-free in words of 4 bytes; min-free = the least"
                     " ever left)\r\n", (unsigned)n);
    usb_send_str("  name             prio state  stack min-free  free%\r\n");
    UBaseType_t last = 0;
    for (UBaseType_t k = 0; k < n; k++) {                             /* in creation order */
        UBaseType_t i = n;
        for (UBaseType_t j = 0; j < n; j++)
            if (t[j].xTaskNumber > last && (i == n || t[j].xTaskNumber < t[i].xTaskNumber)) i = j;
        if (i == n) break;
        last = t[i].xTaskNumber;
        const unsigned size = (unsigned)(((uintptr_t)t[i].pxEndOfStack -
                                          (uintptr_t)t[i].pxStackBase) / sizeof(StackType_t) + 2u) & ~1u;
        const unsigned hwm = (unsigned)t[i].usStackHighWaterMark;
        const bool key = strcmp(t[i].pcTaskName, "key") == 0;
        if (key) { key_size = (long)size; key_free = (long)hwm; }
        usb_debug_printf("  %-16s %4u %-5s %6u %8u %5u%%%s\r\n", t[i].pcTaskName,
                         (unsigned)t[i].uxCurrentPriority,
                         state[t[i].eCurrentState <= eDeleted ? t[i].eCurrentState : 5],
                         size, hwm, size ? hwm * 100u / size : 0u,
                         key ? "  <- key (input) task" : "");
    }
    usb_debug_printf("heap: free %lu B, min ever %lu B, of %lu B\r\n",
                     (unsigned long)xPortGetFreeHeapSize(),
                     (unsigned long)xPortGetMinimumEverFreeHeapSize(),
                     (unsigned long)configTOTAL_HEAP_SIZE);
    if (key_size >= 0)
        usb_debug_printf("key (input) task: stack %ld words, min-free %ld words (%ld B)\r\n",
                         key_size, key_free, key_free * 4L);
    else
        usb_send_str("key (input) task: not found (no task named \"key\")\r\n");
}

typedef struct {
    const char *name;                    /* full command word(s) */
    void (*fn_args)(const char *args);   /* exactly one of these two is set */
    void (*fn_void)(void);
    uint8_t flags;
    const char *help;                    /* preformatted line(s); NULL = alias row */
} shell_cmd_t;

#define SC_EXACT    0x01u

#define SC_NEEDARGS 0x02u

#define SC_SPI3     0x04u

#define CMD_A(name, fn, flags, help)  { name, fn, NULL, flags, help }

#define CMD_V(name, fn, flags, help)  { name, NULL, fn, flags, help }

static const shell_cmd_t shell_cmds[] = {
    CMD_V("help", cmd_help, SC_EXACT,
          "help                            Show this help\r\n"),
    CMD_V("?", cmd_help, SC_EXACT,
          NULL /* alias */),
    CMD_V("version", cmd_version, SC_EXACT,
          "version                         Firmware info\r\n"),
    CMD_V("features", cmd_features, SC_EXACT,
          "features                        Features of this build: name, on/off, HELLO bit\r\n"),
    CMD_V("status", cmd_status, SC_EXACT,
          "status                          FPGA & system status, the timebase in force\r\n"),
    CMD_A("fwload", cmd_fwload, SC_NEEDARGS,
          "fwload <size> <crc32hex> [a|b]  Stage an image into W25Q cache slot a/b\r\n"
          "  then stream exactly <size> raw bytes (scripts/cdc_flash.py does both)\r\n"),
    CMD_V("fwstat", cmd_fwstat, SC_EXACT,
          "fwstat                          Staging state / byte count / verdict\r\n"),
    CMD_V("fwapply", cmd_fwapply, SC_EXACT,
          "fwapply                         Install the staged image over the app slot\r\n"),
    CMD_A("fwswap", cmd_fwswap, SC_NEEDARGS,
          "fwswap a|b                      Install a cached image (no transfer needed)\r\n"),
    CMD_A("siggen", cmd_siggen, 0,
          "siggen [dump]                   Signal generator state: gen.*, TMR7/DAC/DMA2 ch4; dump = the cycle\r\n"),
    CMD_V("flash jedec", cmd_flash_jedec, SC_EXACT,
          "flash jedec                     Read external W25Q128 JEDEC ID\r\n"),
    CMD_A("flash read", cmd_flash_read, SC_NEEDARGS,
          "flash read <addr> <len>         Read external flash bytes (max 256)\r\n"),
    CMD_A("flash dump", cmd_flash_dump, SC_NEEDARGS,
          "flash dump <addr> <len>         Stream external flash bytes (max 4096)\r\n"),
    CMD_V("can decode", cmd_can_decode, SC_EXACT,
          "can decode                      Decode CAN in the record (CH1=CAN_H, CH2=CAN_L)\r\n"),
    CMD_A("can stats", cmd_can_stats, 0,
          "can stats [n]                   CAN over n records: IDs, ACK/NAK, CRC\r\n"),
    CMD_A("can screen", cmd_can_screen, 0,
          "can screen [on|off]             CAN readout on the scope screen\r\n"),
    CMD_V("cal status", cmd_cal_status, SC_EXACT,
          "cal status                      Factory-cal page + W25Q backup state\r\n"),
    CMD_V("cal backup", cmd_cal_backup, SC_EXACT,
          "cal backup                      Copy MCU factory-cal page to W25Q (safe)\r\n"),
    CMD_A("cal restore", cmd_cal_restore, SC_NEEDARGS,
          "cal restore [force] CONFIRM     Write W25Q backup back to MCU flash 0x08006000\r\n"),
    CMD_A("cal page", cmd_cal_page, 0,
          "cal page [on|off|reload]        Stock cal page in use: table, bank, substitutions, DAC codes\r\n"
          "cal page calc <1|2> <b> <r> <p> One offset code from the page: stock's vs ours\r\n"),
    CMD_A("cal base", cmd_cal_base, 0,
          "cal base [clear]                Baseline-correction deltas held in RAM (long AUTO)\r\n"),
    CMD_A("cal user", cmd_cal_user, 0,
          "cal user [show|save|clear]      Save/clear OpenScope's own corrections (baseline + interleave) in W25Q\r\n"),
    CMD_V("fpga scope autoset", cmd_fpga_scope_autoset, SC_EXACT,
          "fpga scope autoset              Autoset, as short AUTO (V/div, position, timebase, trigger)\r\n"),
    CMD_V("fft auto", cmd_fft_auto, SC_EXACT,
          "fft auto                        Fit the spectrum's span, as AUTO in a spectrum view\r\n"),
    CMD_A("fpga scope basecal", cmd_fpga_scope_basecal, 0,
          "fpga scope basecal [banks]      Baseline correction, as long AUTO; input open (default banks 3)\r\n"),
    CMD_V("fpga scope center50", cmd_fpga_scope_center50, SC_EXACT,
          "fpga scope center50             50% centre, as long OK (positions, trigger column/level)\r\n"),
    CMD_A("fpga scope selfcal", cmd_fpga_scope_selfcal, 0,
          "fpga scope selfcal [cancel]     Autoset/baseline progress and result\r\n"),
    CMD_A("screen dump", cmd_screen_dump, 0,
          "screen dump [shadow] [x y w h]  Dump text indexed4 LCD shadow\r\n"),
    CMD_A("screen dumpbin", cmd_screen_dumpbin, 0,
          "screen dumpbin [x y w h]        Binary indexed4 LCD shadow dump\r\n"),
    CMD_A("screenshot", cmd_screenshot, 0,
          "screenshot [list]               On-device gallery: <n>.bmp in W25Q scratch\r\n"
          "screenshot save|get|delete|check <n>  Save the screen / raw BMP (SHOTBIN) / delete\r\n"),
    CMD_A("logo", cmd_logo, 0,
          "logo [info]                     Boot logo status (CRC-checked)\r\n"
          "logo put <len> <crc>|blk <off> <n> <crc>|end  Upload 320x240 RGB565 (scripts/logo_put.py)\r\n"
          "logo pos|data <off> <hex>       Upload position / legacy 48-byte hex chunk\r\n"
          "logo show [ms]|clear            Preview now / remove (built-in splash)\r\n"),

    CMD_A("usb", cmd_usb, 0,
          "usb [status]                    USB device mode, switch result, CDC/MSC counters\r\n"
          "usb share [on|off]              Re-enumerate as CDC + read-only gallery drive / CDC only\r\n"
          "usb trace                       Last attach: resets/SETUPs, then the SCSI commands\r\n"),
    CMD_A("screen shadow", cmd_screen_shadow, 0,
          "screen shadow page [y]          Clear full-screen shadow capture\r\n"),
    CMD_A("fpga scope range", cmd_fpga_scope_range, SC_NEEDARGS,
          "fpga scope range <0-9> <1|2|both>  Coarse frontend range; channel is MANDATORY\r\n"),
    CMD_A("fpga scope measure", cmd_fpga_scope_measure, 0,
          "fpga scope measure [reps]       Badge values raw (uV/permille/mHz) for bench validation\r\n"),
    CMD_A("fpga scope cursor", cmd_fpga_scope_cursor, 0,
          "fpga scope cursor               X1 X2 X1-X2 Y1 Y2 Y1-Y2 1/dX as the screen computes them\r\n"),
    CMD_V("settings", cmd_settings, SC_EXACT,
          "settings                        Persistence status: bound, load result, writes, failures\r\n"),
    CMD_A("reset settings", cmd_reset_settings, 0,
          "reset settings CONFIRM          Built-in defaults over the user settings, applied and saved\r\n"
          "  (never calibration, gallery, logo); list changed shows what differs now\r\n"),
    CMD_A("session", cmd_session, 0,
          "session [status]|list|save|clear  Session snapshot (Startup = Restore): what the boot found\r\n"
          "  and applied; save = what a power-off writes; clear = none; list = members and exclusions\r\n"),
    /* The property model's raw route (src/model/model_shell.c): the same
     * setters as the keys, applied by the input task. */
    CMD_A("get", cmd_model_get, SC_NEEDARGS,
          "get <prop|glob> [...] | changed Property values: get ch1.vdiv, get ch1.*, get *; changed =\r\n"
          "  only what is not at its built-in default\r\n"),
    CMD_A("set", cmd_model_set, SC_NEEDARGS,
          "set <prop> <value> | set p=v .. Set through the input task (label, number, step +=N/-=N)\r\n"),
    CMD_A("list", cmd_model_list, 0,
          "list [prefix|glob]              Properties: name, type, range, value, flags\r\n"
          "list changed [prefix|glob]      Values that differ from the built-in defaults, with the default\r\n"),
    CMD_A("describe", cmd_model_describe, SC_NEEDARGS,
          "describe <prop>                 Range/labels, built-in default, unit, saved where, reset, owner\r\n"),
    STREAM_SHELL_ROWS       /* `stream`: from src/stream/stream.h, empty when the feature is off */
    /* The UI route (src/model/ui_shell.c): exact key replay into the key
     * queue, each press waited for until the input task has handled it. */
    CMD_A("ui press", cmd_ui_press, SC_NEEDARGS,
          "ui press <KEY> [n]              Press a key n times (not POWER), wait until handled\r\n"),
    CMD_A("ui long", cmd_ui_long, SC_NEEDARGS,
          "ui long <KEY>                   The long-press event of a key that has one\r\n"),
    CMD_A("ui seq", cmd_ui_seq, SC_NEEDARGS,
          "ui seq <tok> ...                tok = KEY | KEY*n | long:KEY | wait:ms (all parsed first)\r\n"),
    CMD_V("ui screen", cmd_ui_screen, SC_EXACT,
          "ui screen                       Where the UI is: one key=value line for scripts\r\n"),
    CMD_A("mask", cmd_mask, 0,
          "mask [status]                   Waveform pass/fail: state, counts, last verdict\r\n"
          "mask teach [n] [ch1|ch2|both]   Teach from the next n good records (default 8)\r\n"
          "mask clear|reset|run            Drop mask | zero counts | release a fail hold\r\n"
          "mask dump                       Bounds per column (bench scripts)\r\n"),
    CMD_A("fpga cfgsettle", cmd_fpga_cfgsettle, 0,
          "fpga cfgsettle [ms|off|default] Records only after the capture config settled; drops so far\r\n"),
    CMD_A("fpga interleave", cmd_fpga_interleave, 0,
          "fpga interleave [cal [piecewise]|tone]  Single-channel merge status (il.*); cal = B from a DC sweep (shorted CH1); tone = skew + B gain from a 5-50 MHz tone\r\n"
          "fpga interleave skew measure    B timing skew from a 5-50 MHz tone\r\n"),
    CMD_A("fpga core", cmd_fpga_core, 0,
          "fpga core [fix on|off]          The capture core: identity, status, register check, AUTO's forced records\r\n"),
    CMD_A("fpga unrotate", cmd_fpga_unrotate, 0,
          "fpga unrotate [on|off]          Time-order strobed records at the seam; trigger at 512 (default on)\r\n"),
    CMD_A("fpga scope trigger", cmd_fpga_scope_trigger, 0,
          "fpga scope trigger              Trigger: trig.* in force, reg 0x06/0x08, status, record kind, where the window landed\r\n"),
    CMD_A("fpga roll", cmd_fpga_roll, 0,
          "fpga roll [code <hex>|code stock]  Roll (tb.roll, tb.rollstep) status: fifo, op03, newest point; code = entry byte\r\n"),
    CMD_A("fpga scope center", cmd_fpga_scope_center, 0,
          "fpga scope center [ch2] [0-9]   Auto-center offset ref per range (CH1/DAC1 default); all if omitted\r\n"),
    CMD_V("meter state", cmd_meter_state, SC_EXACT,
          "meter state                     Decoded frame, range, status text, link, beep, cal guard\r\n"),
    CMD_A("meter relays", cmd_meter_relays, 0,
          "meter relays [on|off]           Meter entry moves the scope's range relays (default off)\r\n"),
    CMD_A("persist", cmd_persist, 0,
          "persist [on|off]                Shell commands save settings (off: bench sweeps, RAM only)\r\n"),
    CMD_A("batt", cmd_batt, 0,
          "batt                            Battery gauge: raw/resting mV, percent, external power\r\n"),
    CMD_A("meter auto", cmd_meter_auto_async, 0,
          "meter auto [start|status|cancel] Async DMM function auto-select\r\n"),
    CMD_V("ui dump", cmd_ui_dump, SC_EXACT,
          "ui dump                         UI state: mode, redraws, meter page, measurement strip, graticule, key timing\r\n"),
    CMD_V("reboot bootloader", cmd_reboot_bootloader, SC_EXACT,
          "reboot bootloader               Reboot into USB HID updater\r\n"),
    CMD_V("uptime", cmd_uptime, SC_EXACT,
          "uptime                          Show uptime\r\n"),
    CMD_V("tasks", cmd_tasks, SC_EXACT,
          "tasks                           FreeRTOS tasks: priority, state, stack size and least free (words); heap\r\n"),
    BENCHSHELL_SHELL_ROWS   /* the bench shell: src/drivers/shell_bench.h, empty when the feature is off */
    RETOOLS_SHELL_ROWS      /* the reverse-engineering toolkit: src/re/shell_re.h, empty when off */
    METERDEBUG_SHELL_ROWS   /* the meter diagnostics: src/drivers/shell_meterdebug.h, empty when off */
    { NULL, NULL, NULL, 0, NULL }   /* terminator */
};

static void cmd_help(void)
{
    usb_send_str("\r\n=== OpenScope 2C53T Debug Shell ===\r\n");
    for (const shell_cmd_t *c = shell_cmds; c->name != NULL; c++) {
        if (c->help != NULL)
            usb_send_str(c->help);
    }
    usb_send_str("\r\n");
}

static void dispatch_command(char *line)
{
    /* Strip trailing \r\n */
    int len = strlen(line);
    while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n'))
        line[--len] = '\0';

    if (len == 0) return;

    /* Longest whole-word-prefix match over the table (see table comment). */
    const shell_cmd_t *best = NULL;
    size_t best_len = 0;
    for (const shell_cmd_t *c = shell_cmds; c->name != NULL; c++) {
        size_t n = strlen(c->name);
        if (strncmp(line, c->name, n) != 0)
            continue;
        char next = line[n];
        if (next != '\0' && next != ' ' && next != '\t')
            continue;                     /* word boundary required */
        if ((c->flags & SC_EXACT) && next != '\0')
            continue;                     /* takes no arguments */
        if (n > best_len) {
            best = c;
            best_len = n;
        }
    }

    if (best == NULL) {
        usb_debug_printf("Unknown command: '%s'  (type 'help')\r\n", line);
        return;
    }

    const char *args = line + best_len;
    while (*args == ' ' || *args == '\t') args++;

    if ((best->flags & SC_NEEDARGS) && *args == '\0') {
        usb_send_str("Missing arguments. Usage:\r\n");
        if (best->help != NULL)
            usb_send_str(best->help);
        return;
    }

    if (best->flags & SC_SPI3) {
        if (!spi3_shell_claim())
            return;
        if (best->fn_args) best->fn_args(args); else best->fn_void();
        fpga_acq_resume();
        if (shell_persist_after_command())
            input_request_settings_flush();
        return;
    }

    if (best->fn_args) best->fn_args(args); else best->fn_void();
    /* Any command may have changed a persisted setting (timebase, vdiv,
     * coupling, probe, trigger, siggen, meter mode, prefs, ...) and none of
     * them has a key press behind it, so settings_after_button() never sees
     * it. One request per command, served by the input task within 100 ms: a
     * no-op (no flash access) when nothing differs from the saved image.
     * `persist off` (bench sweeps) skips the request; see shell_persist.h. */
    if (shell_persist_after_command())
        input_request_settings_flush();
}

/* ═══════════════════════════════════════════════════════════════════
 * FreeRTOS Task — USB Debug Shell
 * ═══════════════════════════════════════════════════════════════════ */

#define CMD_BUF_SIZE 128

/* Line accumulator shared by both transports. `echo` is on for USB (a raw
 * serial terminal shows nothing otherwise) and off for RTT, where the host
 * telnet client is line-buffered and echoes locally — echoing there would
 * double every character. */
static char cmd_buf[CMD_BUF_SIZE];

static int  cmd_pos = 0;

/* Returns how many bytes were consumed. Less than len only when a command
 * armed a raw block: the rest of the packet is that block's data.
 *
 * Echo is batched: one packet per received chunk instead of one IN transfer
 * per character. Per-character echo meant ~115 one-byte IN transfers for
 * every 115-byte `logo data` line, interleaved with the host's OUT packets
 * on the same endpoint: slow, and exactly the IN/OUT overlap that triggered
 * the lost-completion race (usb_conf.h). */
static uint16_t shell_feed(const uint8_t *bytes, uint16_t len, bool echo)
{
    uint8_t eb[64];
    uint16_t ne = 0;
#define ECHO_PUT(ch) do { if (ne == sizeof eb) { usb_send_bytes(eb, ne); ne = 0; } eb[ne++] = (uint8_t)(ch); } while (0)
#define ECHO_FLUSH() do { if (ne) { usb_send_bytes(eb, ne); ne = 0; } } while (0)

    for (uint16_t i = 0; i < len; i++) {
        char c = (char)bytes[i];

        if (c == '\r' || c == '\n') {
            if (echo) { ECHO_PUT('\r'); ECHO_PUT('\n'); }
            ECHO_FLUSH();
            cmd_buf[cmd_pos] = '\0';
            if (cmd_pos > 0) {
                dispatch_command(cmd_buf);
            }
            cmd_pos = 0;
            if (raw_block_active(&s_raw)) {
                /* the line ending's second half is not block data */
                if (c == '\r' && i + 1u < len && bytes[i + 1u] == '\n') i++;
                return (uint16_t)(i + 1u);
            }
            usb_send_str("> ");
        } else if (c == '\b' || c == 0x7F) {
            if (cmd_pos > 0) {
                cmd_pos--;
                if (echo) { ECHO_PUT('\b'); ECHO_PUT(' '); ECHO_PUT('\b'); }
            }
        } else if (c >= ' ' && cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
            if (echo) ECHO_PUT(c);
        }
    }
    ECHO_FLUSH();
#undef ECHO_PUT
#undef ECHO_FLUSH
    return len;
}

/* All shell input goes through here: a raw block in progress takes its
 * bytes first, the line editor gets the rest. */
static void shell_rx(const uint8_t *buf, uint16_t n, bool echo)
{
    while (n > 0u) {
        if (raw_block_active(&s_raw)) {
            const uint32_t used = raw_block_feed(&s_raw, buf, n, shell_now_ms());
            buf += used;
            n = (uint16_t)(n - used);
            if (!raw_block_active(&s_raw)) logo_blk_finish();
            continue;
        }
        const uint16_t used = shell_feed(buf, n, echo);
        buf += used;
        n = (uint16_t)(n - used);
    }
}

static const char shell_banner[] =
    "\r\n\r\n"
    "+----------------------------------+\r\n"
    "|  OpenScope 2C53T Debug Shell     |\r\n"
    "|  Type 'help' for commands        |\r\n"
    "+----------------------------------+\r\n"
    "\r\n> ";

/* Instrumentation for the 2026-08-11 "shell task never polls RTT" hunt.
 * Non-static so the addresses come out of the ELF with nm and can be read over
 * SWD on a running target — no console needed, which is the whole problem.
 *   dbg_shell_entered == 0  -> the task never ran; look at task creation
 *   dbg_shell_loops   == 0  -> it started but never completed an iteration
 *   dbg_shell_loops growing -> the loop is fine and rtt_read() is the fault */
volatile uint32_t dbg_shell_entered;

volatile uint32_t dbg_shell_loops;

volatile uint32_t dbg_shell_rtt_bytes;

static void vUsbDebugTask(void *pvParameters)
{
    (void)pvParameters;

    dbg_shell_entered = 0xA5A5A5A5u;

    uint8_t rx_buf[USBD_CDC_OUT_MAXPACKET_SIZE];
    uint8_t rtt_buf[64];
    bool usb_banner_sent = false;
    bool rtt_banner_sent = false;
    uint32_t usb_settle = 0;
    /* When a transfer dies mid-stream the host still has bytes in flight, and
     * without this they land in shell_feed() and are parsed as command lines —
     * a binary typed at the prompt. cdc_flash.py stops at the first chunk
     * carrying ERROR, so the tail is short, but any other host makes it long.
     * Swallow exactly what the aborted image still owed, and give up on the
     * wait after the same silence the loader itself uses, so a host that never
     * sends the rest cannot leave the shell deaf to the operator. */
    uint32_t fwl_discard = 0;
    uint32_t fwl_discard_idle = 0;

    /* The firmware loader borrows this task's bus scratch (the arena `spi3
     * read` / `spi3 frame` already share, 545a9d8) instead of owning a 512 B
     * buffer of its own — the RAM ceiling is that close. Safe for the same
     * reason theirs is: every fw_loader entry point runs on this task, and no
     * other command can interleave with a transfer (RX is routed to the
     * loader) or an install (interrupts are off). */
    fw_loader_attach_scratch(shell_bus_scratch, sizeof(shell_bus_scratch));
    fw_loader_breadcrumb_capture();   /* last install's record, read once and cleared */

    for (;;) {
        bool did_work = false;
        dbg_shell_loops++;

#if defined(FAULT_SELFTEST) && FAULT_SELFTEST
        /* Deliberate, self-inflicted fault ~10 s after boot, from a task (so the
         * frame lands on the PSP exactly like the fault under investigation).
         *
         * Two questions at once:
         *  1. does the fault handler actually record anything? If this writes a
         *     valid g_fault, the handler and vector wiring are sound and the
         *     ~55 s fault is special in some way (most likely an unusable stack
         *     frame). If it records nothing, the instrument itself is broken.
         *  2. does the device freeze at ~10 s WITH NO DEBUGGER ATTACHED? The UI
         *     stopping on its own is proof the fault is real and not something
         *     the OpenOCD attach provokes — which is the confound that makes the
         *     whole "~55 s HardFault" reading suspect.
         *
         * udf #0 is the permanently-undefined encoding: guaranteed UNDEFINSTR
         * -> UsageFault, no memory access involved, nothing ambiguous. */
        if (dbg_shell_loops == 1000u) {
            __asm volatile ("udf #0");
        }
#endif

        /* ---- RTT (SWD) transport ---- */
        if (!rtt_banner_sent && rtt_host_attached()) {
            usb_send_str(shell_banner);
            rtt_banner_sent = true;
        }

        size_t rtt_len = rtt_read(rtt_buf, sizeof(rtt_buf));
        dbg_shell_rtt_bytes += (uint32_t)rtt_len;
        if (rtt_len > 0) {
            /* Attaching mid-session: the host may never have moved read_pos
             * before typing, so print the banner on first input too. */
            if (!rtt_banner_sent) {
                usb_send_str(shell_banner);
                rtt_banner_sent = true;
            }
            shell_rx(rtt_buf, (uint16_t)rtt_len, false);
            did_work = true;
        }

        /* ---- USB CDC transport ---- */
#if !DEBUG_SHELL_RTT_ONLY
        if (usb_debug_connected()) {
            if (!usb_banner_sent) {
                /* Let the host finish enumerating before the first write. */
                if (usb_settle < 50) {
                    usb_settle++;
                } else {
                    usb_send_str(shell_banner);
                    usb_banner_sent = true;
                }
            } else {
                /* recover an OUT completion the library lost (see cdc_rx_heal) */
                cdc_rx_heal((cdc_struct_type *)usb_core_dev.class_handler->pdata);
                uint16_t rx_len = usb_vcp_get_rxdata(&usb_core_dev, rx_buf);
                if (rx_len > 0) {
                    if (fw_loader_active()) {
                        /* Raw image bytes for fw_loader — no echo, no line
                         * editor. Progress every 64 KB so a long transfer
                         * is visibly alive without flooding the port. */
                        uint32_t before = fw_loader_bytes();
                        fw_loader_feed(rx_buf, rx_len);
                        if ((fw_loader_bytes() >> 16) != (before >> 16)) {
                            char pbuf[24];
                            snprintf(pbuf, sizeof(pbuf), "..%luK\r\n",
                                     (unsigned long)(fw_loader_bytes() >> 10));
                            usb_send_str(pbuf);
                        }
                        if (!fw_loader_active()) {
                            fwl_print_status();  /* staged or failed — say so */
                            if (fw_loader_state() == FW_LOADER_ERROR) {
                                /* The host has sent `before + rx_len` bytes
                                 * so far — the tail of THIS packet is already
                                 * off the wire even when the loader refused
                                 * it, so `expected - bytes()` would over-
                                 * count and the drain would eat up to a
                                 * packet of the operator's next keystrokes.
                                 * Count what is still in flight instead. */
                                uint32_t sent = before + rx_len;
                                fwl_discard =
                                    sent < fw_loader_expected()
                                        ? fw_loader_expected() - sent
                                        : 0;
                                fwl_discard_idle = 0;
                            }
                        }
                    } else if (fwl_discard > 0) {
                        uint32_t drop = rx_len < fwl_discard ? rx_len
                                                             : fwl_discard;
                        fwl_discard -= drop;
                        fwl_discard_idle = 0;
                        if (fwl_discard == 0) {
                            usb_send_str("fwload: aborted image drained; "
                                         "shell is listening again\r\n");
                        }
                        if (rx_len > drop) {
                            shell_rx(rx_buf + drop,
                                       (uint16_t)(rx_len - drop), true);
                        }
                    } else {
                        shell_rx(rx_buf, rx_len, true);
                    }
                    did_work = true;
                }
            }
        } else {
            usb_banner_sent = false;
            usb_settle = 0;
        }
#else
        (void)rx_buf; (void)usb_banner_sent; (void)usb_settle;
#endif

        /* Age the fw_loader RX-silence timeout, and report if it fires —
         * the poll is what turns a wedged transfer back into a live shell. */
        {
            bool was_loading = fw_loader_active();
            fw_loader_poll();
            if (was_loading && !fw_loader_active()) {
                fwl_print_status();
                /* A timeout kills the transfer too, and a host that merely
                 * stalled (>3 s of silence mid-image) may well resume: arm
                 * the same drain the feed-path error arms, or the rest of
                 * the image lands in shell_feed() as command lines. No
                 * packet is in flight here, so expected - received is the
                 * exact figure. A host that is truly gone costs one more
                 * 3 s drain expiry, which reports itself. */
                if (fw_loader_state() == FW_LOADER_ERROR) {
                    fwl_discard = fw_loader_expected() - fw_loader_bytes();
                    fwl_discard_idle = 0;
                }
            }
        }

        /* The drain waits on the same 3 s of silence the loader does: a host
         * that aborted and walked away must not cost the operator a shell. */
        if (fwl_discard > 0 && !did_work) {
            if (++fwl_discard_idle >= FW_LOADER_TIMEOUT_POLLS) {
                fwl_discard = 0;
                fwl_discard_idle = 0;
                usb_send_str("fwload: stopped waiting for the aborted image; "
                             "shell is listening again\r\n");
            }
        }

        /* A raw `logo blk` block whose host went quiet: abandon it. */
        raw_block_poll(&s_raw, shell_now_ms());
        if (s_raw.state == RAW_TIMEOUT) logo_blk_finish();

#if USB_MSC
        /* Which device we are: switch on request, fall back to CDC-only when
         * the host does not configure the composite device (usb_mode_sm.h). */
        usb_mode_poll();

        /* USB Sharing: serve READ sectors (W25Q reads need this task's
         * context; the USB interrupt only latches them) and pick up gallery
         * changes. Only while the composite device is up: as the CDC device
         * there is no drive to keep current. */
        if (s_usb_mode.active == USB_DEV_COMPOSITE && usb_msc_poll(&usb_core_dev)) did_work = true;
#endif

        /* Binary stream frames, between commands (src/stream/stream.h; a
         * constant false without the stream). */
        if (fwl_discard == 0u && !raw_block_active(&s_raw) && stream_poll()) did_work = true;

        if (!did_work) {
            /* 10 ms idle cadence, 1 ms while an upload is in flight (each
             * `logo blk` round trip would otherwise wait out a poll period
             * twice). */
            const TickType_t idle = (raw_block_active(&s_raw) || s_logo_up.active)
                                        ? 1 : pdMS_TO_TICKS(10);
#if USB_MSC
            /* The MSC interrupt can cut it short (task notification) so a
             * READ does not wait a poll period per sector. Nothing else
             * notifies this task. */
            (void)ulTaskNotifyTake(pdTRUE, idle);
#else
            vTaskDelay(idle);
#endif
        }
    }
}

void usb_debug_create_task(void)
{
#ifndef EMULATOR_BUILD
    TaskHandle_t h = NULL;
    xTaskCreate(vUsbDebugTask, "usb_dbg", 768, NULL, 2, &h);
#if USB_MSC
    usb_msc_set_notify_task(h);
#else
    (void)h;
#endif
#endif
}
