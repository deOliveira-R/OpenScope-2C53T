/*
 * OpenScope 2C53T - the property table (X-macro; see model.h)
 *
 * One PROP(...) per line, so scripts/model_config.py (M5) can parse it. No
 * include guard: model.h and model.c include it several times with their own
 * PROP definition.
 *
 * Rules (tests/test_model.c checks them):
 *   - ids are (group + 1) << 8 | n, unique, increasing in table order, and
 *     never reused once released;
 *   - the name is "<group prefix>.<field>" and sym is the name in capitals
 *     with '.' -> '_';
 *   - def is the BUILT-IN default: for PF_PERSIST it is config_init_defaults()
 *     (what a first boot runs on), for the rest scope_state_init() and the
 *     main.c / module initialisers;
 *   - enum values follow the firmware enums; no label parses as a number;
 *   - every settable property is exactly one of PF_PERSIST, PF_USERCAL,
 *     PF_SESSION, or PF_ALIAS.
 *
 * Columns:
 *   sym, id, name, type, unit, decimals, group, instruments, arg,
 *   min, max, step, def, flags, labels, label hook, get, set, store, owner
 */

/* ── ch1 / ch2 ─────────────────────────────────────────────────────────── */
PROP(CH1_ENABLED,  0x0101, "ch1.enabled",    PT_BOOL, UNIT_NONE, 0, PG_CH1, INST_SCOPE, 0,    0,      1,  1,    1, PF_HW|PF_SESSION|PF_WRAP,             NULL,                  NULL,          ch_enabled_get,  ch_enabled_set,  ch_enabled_store,  "fpga_apply_channel_enables")
PROP(CH1_VDIV,     0x0102, "ch1.vdiv",       PT_ENUM, UNIT_NONE, 0, PG_CH1, INST_SCOPE, 0,    0,      9,  1,    3, PF_HW|PF_PERSIST,                     NULL,                  vdiv_label,    ch_vdiv_get,     ch_vdiv_set,     ch_vdiv_store,     "fpga_apply_vdiv + scope_vpos")
PROP(CH1_COUPLING, 0x0103, "ch1.coupling",   PT_ENUM, UNIT_NONE, 0, PG_CH1, INST_SCOPE, 0,    0,      1,  1,    0, PF_HW|PF_PERSIST|PF_WRAP,             coupling_labels,       NULL,          ch_coupling_get, ch_coupling_set, ch_coupling_store, "fpga_apply_coupling (relay readback)")
PROP(CH1_PROBE,    0x0104, "ch1.probe",      PT_ENUM, UNIT_NONE, 0, PG_CH1, INST_SCOPE, 0,    0,      1,  1,    0, PF_PERSIST|PF_WRAP,                   probe_labels,          NULL,          ch_probe_get,    ch_probe_set,    ch_probe_store,    "scope_state")
PROP(CH1_BWLIMIT,  0x0105, "ch1.bwlimit",    PT_BOOL, UNIT_NONE, 0, PG_CH1, INST_SCOPE, 0,    0,      1,  1,    0, PF_PERSIST|PF_WRAP,                   NULL,                  NULL,          ch_bw_get,       ch_bw_set,       ch_bw_store,       "scope_state (no hardware yet)")
PROP(CH1_POS,      0x0106, "ch1.pos",        PT_INT,  UNIT_NONE, 0, PG_CH1, INST_SCOPE, 0, -100,    100,  5,    0, PF_HW|PF_SESSION,                     NULL,                  NULL,          ch_pos_get,      ch_pos_set,      ch_pos_store,      "scope_vpos_set")
PROP(CH1_MEAS,     0x0107, "ch1.meas",       PT_MASK, UNIT_NONE, 0, PG_CH1, INST_SCOPE, 0,    0,  0x0FFF, 1, 0x0507, PF_PERSIST,                       NULL,                  meas_label,    ch_meas_get,     ch_meas_set,     ch_meas_store,     "meas_select")
PROP(CH2_ENABLED,  0x0201, "ch2.enabled",    PT_BOOL, UNIT_NONE, 0, PG_CH2, INST_SCOPE, 1,    0,      1,  1,    1, PF_HW|PF_SESSION|PF_WRAP,             NULL,                  NULL,          ch_enabled_get,  ch_enabled_set,  ch_enabled_store,  "fpga_apply_channel_enables")
PROP(CH2_VDIV,     0x0202, "ch2.vdiv",       PT_ENUM, UNIT_NONE, 0, PG_CH2, INST_SCOPE, 1,    0,      9,  1,    5, PF_HW|PF_PERSIST,                     NULL,                  vdiv_label,    ch_vdiv_get,     ch_vdiv_set,     ch_vdiv_store,     "fpga_apply_vdiv + scope_vpos")
PROP(CH2_COUPLING, 0x0203, "ch2.coupling",   PT_ENUM, UNIT_NONE, 0, PG_CH2, INST_SCOPE, 1,    0,      1,  1,    0, PF_HW|PF_PERSIST|PF_WRAP,             coupling_labels,       NULL,          ch_coupling_get, ch_coupling_set, ch_coupling_store, "fpga_apply_coupling (relay readback)")
PROP(CH2_PROBE,    0x0204, "ch2.probe",      PT_ENUM, UNIT_NONE, 0, PG_CH2, INST_SCOPE, 1,    0,      1,  1,    0, PF_PERSIST|PF_WRAP,                   probe_labels,          NULL,          ch_probe_get,    ch_probe_set,    ch_probe_store,    "scope_state")
PROP(CH2_BWLIMIT,  0x0205, "ch2.bwlimit",    PT_BOOL, UNIT_NONE, 0, PG_CH2, INST_SCOPE, 1,    0,      1,  1,    0, PF_PERSIST|PF_WRAP,                   NULL,                  NULL,          ch_bw_get,       ch_bw_set,       ch_bw_store,       "scope_state (no hardware yet)")
PROP(CH2_POS,      0x0206, "ch2.pos",        PT_INT,  UNIT_NONE, 0, PG_CH2, INST_SCOPE, 1, -100,    100,  5,    0, PF_HW|PF_SESSION,                     NULL,                  NULL,          ch_pos_get,      ch_pos_set,      ch_pos_store,      "scope_vpos_set")
PROP(CH2_MEAS,     0x0207, "ch2.meas",       PT_MASK, UNIT_NONE, 0, PG_CH2, INST_SCOPE, 1,    0,  0x0FFF, 1, 0x0507, PF_PERSIST,                       NULL,                  meas_label,    ch_meas_get,     ch_meas_set,     ch_meas_store,     "meas_select")

/* ── tb ────────────────────────────────────────────────────────────────── */
PROP(TB_CODE,      0x0301, "tb.code",        PT_ENUM, UNIT_NONE, 0, PG_TB,   INST_SCOPE, 0,   0,     20,  1,   10, PF_HW|PF_PERSIST,                     NULL,                  tb_label,      tb_code_get,     tb_code_set,     tb_code_store,     "fpga_apply_timebase")
PROP(TB_ROLL,      0x0302, "tb.roll",        PT_BOOL, UNIT_NONE, 0, PG_TB,   INST_SCOPE, 0,   0,      1,  1,    0, PF_HW|PF_SESSION|PF_WRAP,             NULL,                  NULL,          tb_roll_get,     tb_roll_set,     tb_roll_store,     "fpga_apply_roll (revert on refusal)")
PROP(TB_ROLLSTEP,  0x0303, "tb.rollstep",    PT_INT,  UNIT_NONE, 0, PG_TB,   INST_SCOPE, 0,   0,      8,  1,    0, PF_HW|PF_SESSION,                     NULL,                  NULL,          tb_rollstep_get, tb_rollstep_set, tb_rollstep_store, "fpga_apply_roll (revert on refusal)")

/* ── trig ──────────────────────────────────────────────────────────────── */
PROP(TRIG_MODE,    0x0401, "trig.mode",      PT_ENUM, UNIT_NONE, 0, PG_TRIG, INST_SCOPE, 0,   0,      2,  1,    0, PF_HW|PF_PERSIST|PF_WRAP,             trigger_mode_labels,   NULL,          trig_mode_get,   trig_mode_set,   trig_mode_store,   "fpga_acq_single_rearm (Single)")
PROP(TRIG_EDGE,    0x0402, "trig.edge",      PT_ENUM, UNIT_NONE, 0, PG_TRIG, INST_SCOPE, 0,   0,      1,  1,    0, PF_PERSIST|PF_WRAP,                   trigger_edge_labels,   NULL,          trig_edge_get,   trig_edge_set,   trig_edge_store,   "scope_state (MCU edge filter)")
PROP(TRIG_SOURCE,  0x0403, "trig.source",    PT_ENUM, UNIT_NONE, 0, PG_TRIG, INST_SCOPE, 0,   0,      1,  1,    0, PF_HW|PF_PERSIST|PF_WRAP,             trigger_source_labels, NULL,          trig_source_get, trig_source_set, trig_source_store, "fpga_apply_trigger_source (reg 0x06)")
PROP(TRIG_LEVEL,   0x0404, "trig.level",     PT_INT,  UNIT_PX,   0, PG_TRIG, INST_SCOPE, 0, -100,   100,  5,    0, PF_HW|PF_PERSIST,                     NULL,                  NULL,          trig_level_get,  trig_level_set,  trig_level_store,  "fpga_apply_trigger_level (reg 0x08)")
PROP(TRIG_X,       0x0405, "trig.x",         PT_INT,  UNIT_PX,   0, PG_TRIG, INST_SCOPE, 0,   0,    300, 16,  150, PF_SESSION,                           NULL,                  NULL,          trig_x_get,      trig_x_set,      trig_x_store,      "scope_state")
PROP(TRIG_SOFT,    0x0406, "trig.soft",      PT_BOOL, UNIT_NONE, 0, PG_TRIG, INST_SCOPE, 0,   0,      1,  1,    1, PF_SESSION|PF_WRAP,                   NULL,                  NULL,          trig_soft_get,   trig_soft_set,   trig_soft_store,   "scope_state")

/* ── view ──────────────────────────────────────────────────────────────── */
PROP(VIEW_MODE,    0x0501, "view.mode",      PT_ENUM, UNIT_NONE, 0, PG_VIEW, INST_SCOPE, 0,   0,      5,  1,    0, PF_HW|PF_SESSION,                     lbl_view_mode,         NULL,          view_mode_get,   view_mode_set,   view_mode_store,   "scope_view_select + fpga_apply_roll")
PROP(VIEW_PERSIST, 0x0502, "view.persist",   PT_ENUM, UNIT_NONE, 0, PG_VIEW, INST_SCOPE, 0,   0,      4,  1,    0, PF_HW|PF_SESSION,                     lbl_view_persist,      NULL,          view_persist_get,view_persist_set,view_persist_store,"scope_apply_persist_on/off (shared pool)")
PROP(VIEW_MATH,    0x0503, "view.math",      PT_ENUM, UNIT_NONE, 0, PG_VIEW, INST_SCOPE, 0,   0,      8,  1,    0, PF_PERSIST,                           lbl_view_math,         NULL,          view_math_get,   view_math_set,   view_math_store,   "math_enabled + math_op")
PROP(VIEW_CURSOR,  0x0504, "view.cursor",    PT_ENUM, UNIT_NONE, 0, PG_VIEW, INST_SCOPE, 0,   0,      3,  1,    0, PF_SESSION|PF_WRAP,                   lbl_view_cursor,       NULL,          view_cursor_get, view_cursor_set, view_cursor_store, "scope_state cursor")
PROP(VIEW_TRUESCALE,0x0505,"view.truescale", PT_BOOL, UNIT_NONE, 0, PG_VIEW, INST_SCOPE, 0,   0,      1,  1,    1, PF_SESSION|PF_WRAP,                   NULL,                  NULL,          view_ts_get,     view_ts_set,     view_ts_store,     "scope_state")
PROP(VIEW_MEASHIDE,0x0506, "view.meashide",  PT_BOOL, UNIT_NONE, 0, PG_VIEW, INST_SCOPE, 0,   0,      1,  1,    0, PF_SESSION|PF_WRAP,                   NULL,                  NULL,          view_mh_get,     view_mh_set,     view_mh_store,     "scope_state prm_hidden")
PROP(VIEW_RUN,     0x0507, "view.run",       PT_BOOL, UNIT_NONE, 0, PG_VIEW, INST_SCOPE, 0,   0,      1,  1,    1, PF_SESSION|PF_WRAP,                   NULL,                  NULL,          view_run_get,    view_run_set,    view_run_store,    "scope_state running")

/* ── gen ───────────────────────────────────────────────────────────────── */
PROP(GEN_WAVE,     0x0601, "gen.wave",       PT_ENUM, UNIT_NONE, 0, PG_GEN,  INST_GEN,   0,   0,     12,  1,    0, PF_HW|PF_PERSIST|PF_WRAP,             lbl_gen_wave,          NULL,          gen_wave_get,    gen_wave_set,    gen_wave_store,    "siggen_set_waveform")
PROP(GEN_FREQ,     0x0602, "gen.freq",       PT_INT,  UNIT_HZ,   0, PG_GEN,  INST_GEN,   0,   0,  50000,  1, 1000, PF_HW|PF_PERSIST,                     NULL,                  NULL,          gen_freq_get,    gen_freq_set,    gen_freq_store,    "siggen_set_frequency (TMR7)")
PROP(GEN_AMP,      0x0603, "gen.amp",        PT_INT,  UNIT_V,    1, PG_GEN,  INST_GEN,   0,   0,     30,  1,   30, PF_HW|PF_PERSIST,                     NULL,                  NULL,          gen_amp_get,     gen_amp_set,     gen_amp_store,     "siggen_set_amplitude_dv")
PROP(GEN_DUTY,     0x0604, "gen.duty",       PT_INT,  UNIT_PCT,  0, PG_GEN,  INST_GEN,   0,   0,    100,  5,   50, PF_HW|PF_PERSIST,                     NULL,                  NULL,          gen_duty_get,    gen_duty_set,    gen_duty_store,    "siggen_set_duty")
PROP(GEN_OUT,      0x0605, "gen.out",        PT_BOOL, UNIT_NONE, 0, PG_GEN,  INST_GEN,   0,   0,      1,  1,    0, PF_HW|PF_PERSIST|PF_WRAP,             NULL,                  NULL,          gen_out_get,     gen_out_set,     gen_out_store,     "siggen_enable (DAC2/PA5); saved only with gen.bootout")
PROP(GEN_BOOTOUT,  0x0606, "gen.bootout",    PT_BOOL, UNIT_NONE, 0, PG_GEN,  INST_GEN,   0,   0,      1,  1,    0, PF_HW|PF_PERSIST|PF_WRAP,             NULL,                  NULL,          gen_bootout_get, gen_bootout_set, gen_bootout_store, "siggen_set_start_on")

/* ── meter ─────────────────────────────────────────────────────────────── */
PROP(METER_FUNC,   0x0701, "meter.func",     PT_ENUM, UNIT_NONE, 0, PG_METER,INST_METER, 0,   0,     11,  1,    0, PF_HW|PF_PERSIST|PF_WRAP|PF_POSTED,   lbl_meter_func,        NULL,          meter_func_get,  meter_func_set,  meter_func_store,  "meter_function_set + fpga_request_meter_mode")
PROP(METER_LAYOUT, 0x0702, "meter.layout",   PT_ENUM, UNIT_NONE, 0, PG_METER,INST_METER, 0,   0,      4,  1,    0, PF_PERSIST|PF_WRAP,                   lbl_meter_layout,      NULL,          meter_layout_get,meter_layout_set,meter_layout_store,"meter_layout")
PROP(METER_FUSETYPE,0x0703,"meter.fusetype", PT_ENUM, UNIT_NONE, 0, PG_METER,INST_METER, 0,   0,      4,  1,    0, PF_SESSION|PF_WRAP,                   lbl_fuse_type,         NULL,meter_fusetype_get,meter_fusetype_set,meter_fusetype_store,"fuse_type (fuse_table.h)")
PROP(METER_FUSERATING,0x0704,"meter.fuserating",PT_INT,UNIT_NONE,0, PG_METER,INST_METER, 0,   0,     12,  1,    6, PF_SESSION,                           NULL,                  meter_fuserating_label,meter_fuserating_get,meter_fuserating_set,meter_fuserating_store,"fuse_rating_idx (index into the type's table)")
PROP(METER_FUSETHRESH,0x0705,"meter.fusethresh",PT_INT,UNIT_MV,  1, PG_METER,INST_METER, 0,   1,     50,  1,    1, PF_SESSION,                           NULL,                  NULL,          meter_fusethr_get,meter_fusethr_set,meter_fusethr_store,"fuse_scan_threshold_mv (0.1 mV steps)")
PROP(METER_LINK,   0x0706, "meter.link",     PT_ENUM, UNIT_NONE, 0, PG_METER,INST_METER, 0,   0,      1,  1,    0, PF_PERSIST|PF_WRAP,                   lbl_meter_link,        NULL,          meter_link_get,  meter_link_set,  meter_link_store,  "without a USB host: screen = the meter runs only while its screen is shown; always = it keeps running. With a USB host it always runs.")
PROP(METER_LIVE,   0x0707, "meter.live",     PT_BOOL, UNIT_NONE, 0, PG_METER,INST_METER, 0,   0,      1,  1,    0, PF_RO,                                NULL,                  NULL,          meter_live_get,  NULL,            NULL,              "the meter link is on now (PC11, USART2, poll task): see meter.link")

/* ── sys ───────────────────────────────────────────────────────────────── */
PROP(SYS_INSTRUMENT,0x0801,"sys.instrument", PT_ENUM, UNIT_NONE, 0, PG_SYS,  0,          0,   0,      3,  1,    1, PF_HW|PF_SESSION|PF_POSTED,           lbl_sys_instrument,    NULL,          sys_instrument_get,sys_instrument_set,sys_instrument_store,"enter_mode / mode posture (none = home or settings is up)")
PROP(SYS_MODE,     0x0802, "sys.mode",       PT_ENUM, UNIT_NONE, 0, PG_SYS,  0,          0,   0,      4,  1,    1, PF_HW|PF_ALIAS|PF_POSTED,             lbl_sys_mode,          NULL,          sys_mode_get,    sys_mode_set,    sys_mode_store,    "enter_mode / mode posture (alias of sys.instrument + home/settings)")
PROP(SYS_STARTUP,  0x0803, "sys.startup",    PT_ENUM, UNIT_NONE, 0, PG_SYS,  0,          0,   0,      3,  1,    1, PF_PERSIST|PF_WRAP,                   lbl_sys_startup,       NULL,          sys_startup_get, sys_startup_set, sys_startup_store, "startup_mode")
PROP(SYS_BRIGHTNESS,0x0804,"sys.brightness", PT_INT,  UNIT_PCT,  0, PG_SYS,  0,          0,  25,    100,  5,  100, PF_HW|PF_PERSIST,                     NULL,                  NULL,          sys_bright_get,  sys_bright_set,  sys_bright_store,  "sys_prefs + sys_pwm (TMR10 PB8)")
PROP(SYS_VOLUME,   0x0805, "sys.volume",     PT_INT,  UNIT_NONE, 0, PG_SYS,  0,          0,   0,     10,  1,    1, PF_PERSIST,                           NULL,                  NULL,          sys_volume_get,  sys_volume_set,  sys_volume_store,  "sys_prefs (beep duty)")
PROP(SYS_AUTOOFF,  0x0806, "sys.autooff",    PT_ENUM, UNIT_NONE, 0, PG_SYS,  0,          0,   0,      3,  1,    0, PF_HW|PF_PERSIST|PF_WRAP,             lbl_sys_autooff,       NULL,          sys_autooff_get, sys_autooff_set, sys_autooff_store, "sys_prefs + input task idle timer")
PROP(SYS_THEME,    0x0807, "sys.theme",      PT_ENUM, UNIT_NONE, 0, PG_SYS,  0,          0,   0,      3,  1,    0, PF_PERSIST|PF_WRAP,                   lbl_sys_theme,         NULL,          sys_theme_get,   sys_theme_set,   sys_theme_store,   "theme_set")
PROP(SYS_LANG,     0x0808, "sys.lang",       PT_ENUM, UNIT_NONE, 0, PG_SYS,  0,          0,   0,      0,  1,    0, PF_PERSIST|PF_STORED_ONLY,            lbl_sys_lang,          NULL,          sys_lang_get,    sys_lang_set,    sys_lang_store,    "stored only (nothing applies it)")

/* ── fft (stored only: the live FFT config is fixed in scope_view_select) ─ */
PROP(FFT_WINDOW,   0x0901, "fft.window",     PT_ENUM, UNIT_NONE, 0, PG_FFT,  INST_SCOPE, 0,   0,      4,  1,    1, PF_PERSIST|PF_STORED_ONLY|PF_WRAP,    lbl_fft_window,        NULL,          fft_window_get,  fft_window_set,  fft_window_store,  "stored only (nothing applies it)")
PROP(FFT_AVG,      0x0902, "fft.avg",        PT_ENUM, UNIT_NONE, 0, PG_FFT,  INST_SCOPE, 0,   0,      5,  1,    0, PF_PERSIST|PF_STORED_ONLY,            lbl_fft_avg,           NULL,          fft_avg_get,     fft_avg_set,     fft_avg_store,     "stored only (nothing applies it)")
PROP(FFT_MAXHOLD,  0x0903, "fft.maxhold",    PT_BOOL, UNIT_NONE, 0, PG_FFT,  INST_SCOPE, 0,   0,      1,  1,    0, PF_PERSIST|PF_STORED_ONLY|PF_WRAP,    NULL,                  NULL,          fft_maxhold_get, fft_maxhold_set, fft_maxhold_store, "stored only (nothing applies it)")

/* ── il (single-channel 250 MS/s interleave, fpga.c) ───────────────────── */
PROP(IL_ENABLED,   0x0A01, "il.enabled",     PT_BOOL, UNIT_NONE,      0, PG_IL, INST_SCOPE, 0,      0,       1,    1,       1, PF_HW|PF_SESSION|PF_WRAP, NULL,            NULL,          il_enabled_get,  il_enabled_set,  il_enabled_store,  "fpga_interleave_set")
PROP(IL_GAIN,      0x0A02, "il.gain",        PT_INT,  UNIT_GAIN_E6,   6, PG_IL, INST_SCOPE, 0, 500000, 2000000, 1000, 1000000, PF_HW|PF_USERCAL,         NULL,            NULL,          il_gain_get,     il_gain_set,     il_gain_store,     "fpga_interleave_corr_set (B gain)")
PROP(IL_OFFSET,    0x0A03, "il.offset",      PT_INT,  UNIT_REC_CODE,  2, PG_IL, INST_SCOPE, 0, -10000,   10000,   10,       0, PF_HW|PF_USERCAL,         NULL,            NULL,          il_offset_get,   il_offset_set,   il_offset_store,   "fpga_interleave_corr_set (B offset)")
PROP(IL_SKEW,      0x0A04, "il.skew",        PT_INT,  UNIT_PPM_PERIOD,0, PG_IL, INST_SCOPE, 0,-150000,  150000,  100,       0, PF_HW|PF_USERCAL,         NULL,            NULL,          il_skew_get,     il_skew_set,     il_skew_store,     "fpga_interleave_skew_set")

/* ── mask (pass/fail mask test; applied by the acquisition task) ───────── */
PROP(MASK_TOLV,    0x0B01, "mask.tolv",      PT_INT,  UNIT_NONE, 0, PG_MASK, INST_SCOPE, 0,   0,     64,  2,    8, PF_HW|PF_SESSION|PF_POSTED,           NULL,                  NULL,          mask_tolv_get,   mask_tolv_set,   mask_tolv_store,   "scope_mask_post(REQ_TOL) (record counts)")
PROP(MASK_TOLH,    0x0B02, "mask.tolh",      PT_INT,  UNIT_NONE, 0, PG_MASK, INST_SCOPE, 0,   0,     16,  1,    2, PF_HW|PF_SESSION|PF_POSTED,           NULL,                  NULL,          mask_tolh_get,   mask_tolh_set,   mask_tolh_store,   "scope_mask_post(REQ_TOL) (samples)")
PROP(MASK_STOPFAIL,0x0B03, "mask.stopfail",  PT_BOOL, UNIT_NONE, 0, PG_MASK, INST_SCOPE, 0,   0,      1,  1,    1, PF_HW|PF_SESSION|PF_POSTED|PF_WRAP,   NULL,                  NULL,          mask_stopfail_get,mask_stopfail_set,mask_stopfail_store,"scope_mask_post(REQ_STOPFAIL)")

/* ── ui (read only: reported, only the keys change them) ───────────────── */
PROP(UI_FOCUS,     0x0C01, "ui.focus",       PT_ENUM, UNIT_NONE, 0, PG_UI,   0,          0,   0,      4,  1,    0, PF_RO,                                NULL,                  ui_focus_label,ui_focus_get,    NULL,            NULL,              "key_model focus (MOVE)")
PROP(UI_ACH,       0x0C02, "ui.ach",         PT_ENUM, UNIT_NONE, 0, PG_UI,   0,          0,   0,      1,  1,    0, PF_RO,                                trigger_source_labels, NULL,          ui_ach_get,      NULL,            NULL,              "active_channel (CH1/CH2 key)")
PROP(UI_SCREEN,    0x0C03, "ui.screen",      PT_ENUM, UNIT_NONE, 0, PG_UI,   0,          0,   0,      4,  1,    0, PF_RO,                                lbl_sys_mode,          NULL,          ui_screen_get,   NULL,            NULL,              "current_mode (no screen stack before B1)")
