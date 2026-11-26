/*
 * The hot code and data, first in .text, so they are in the zero-wait flash
 * (the first 128 KB with EOPB0=0xFE; _flash_zw_end in ld/at32f403a_app.ld).
 * Past that line every fetch waits: measured 2026-11-25, canvas_runs.o linked
 * at 0x08047e18 made a live Y-T frame 37 ms instead of 23.6 ms.
 *
 * Three groups, hottest first, each ending at its mark; at32f403a_app.ld
 * ASSERTs every mark, so a group that outgrows the area fails the link and
 * says which. What is in each group is what the call graph reaches from that
 * class's roots (scripts/check_zero_wait.py, `make check-zw`, in the gate);
 * a hot function or table that is not listed here and lands past the line
 * fails that check, which names it with its object and class. An object goes
 * in whole when at least 80 % of its code is reached, else by function (a
 * name with a compiler clone also takes NAME.*); library members go in whole.
 * Each group's code is followed by the read-only data it reads.
 *
 * Left out on purpose: the CMSIS FFT tables in arm_common_tables.o (40 KB,
 * they do not fit; the transform finds them at run time through its
 * instance), meter and fuse mode (about 30 KB, at meter rates, reached
 * through the screen table the walk does not follow) and the merged string
 * pool (the hot formats are a few hundred bytes of 78 KB). The check's
 * DATA_LEFT_OUT gives the reason for the property table, the popup title
 * font and the generator's wave tables, and its STOP for the cold paths hot
 * code can reach (fatal errors, creation, USB enumeration, calibration, mask
 * teaching, CAN decoding).
 *
 * Shared with ld/at32f403a_guest.ld, which takes the order without the
 * ASSERTs.
 */
_ezw_start = .;

/* Per pixel: every pixel or column of a frame (about 66,000 a frame), the
 * column painters (yt_paint_column, yt_paint_base, fft_paint_column, xy_paint,
 * waterfall_blit, cursor_layer_paint, canvas_draw_columns) and what they
 * call: the canvas, the Y-T elements, the LCD column writes, fonts' pixels,
 * persistence, math, markers. */
    */fpga_acq.o(.text.fpga_acq_record_cfg_ptr .text.fpga_data_ready)
    */scope_spectrum.o(.text .text*)
    */scope_yt.o(.text .text*)
    */scope_cursor_draw.o(.text .text*)
    */scope_yt_frame.o(.text .text*)
    */scope_view.o(.text .text*)
    */scope_xy.o(.text .text*)
    */mask_pf.o(.text .text*)
    */scope_mask.o(.text.scope_mask_bound_at .text.scope_mask_column)
    */lcd.o(.text.lcd_bus_delay .text.lcd_note_drawer .text.lcd_set_window .text.lcd_shadow_palette_index
            .text.lcd_shadow_write_pixel .text.lcd_watch_mark .text.lcd_watch_rows .text.lcd_watch_take
            .text.lcd_write_column .text.lcd_write_pixels)
    */math_channel.o(.text .text*)
    */font.o(.text .text*)
    */canvas.o(.text .text*)
    */canvas_runs.o(.text .text*)
    */plot_elements.o(.text .text*)
    */scope_cal.o(.text.scope_cal_mv_per_count .text.scope_cal_probe_factor .text.scope_cal_volts_per_count)
    */persistence.o(.text .text*)
    */paused_view.o(.text .text*)
    */waterfall_blit.o(.text .text*)
    */plot_markers.o(.text .text*)
    */shared_mem.o(.text .text*)
    */scope_state.o(.text .text*)
    */scope_vpos.o(.text.scope_vpos_active)
    */lcd.o(.rodata.lcd_shadow_palette)
    */plot_markers.o(.rodata.k_glyph)
    */scope_xy.o(.rodata.xy_nosig)
    */scope_yt.o(.rodata.label.* .rodata.sine_percent.* .rodata.yt_base .rodata.yt_overlays)
_ezw_pixel = .;

/* Per record and ISR: every record of the acquisition task (fpga_acq_task,
 * its SPI read, the interleave merge, the trigger search, the roll FIFO, the
 * mask test) and every interrupt (FPGA data ready, key scan, SysTick, PendSV
 * and the scheduler, the meter UART, USB transfers). */
    */fpga_acq.o(.text.EXINT0_IRQHandler .text.fpga_acq_auto_wait_get .text.fpga_acq_cfg_snapshot
                 .text.fpga_acq_post_edge_get .text.fpga_acq_rate_idx_get .text.fpga_acq_read_channel
                 .text.fpga_acq_record_core_ordered .text.fpga_acq_task .text.fpga_acq_trig_code_get
                 .text.fpga_interleave_active .text.fpga_scope_write_reg .text.spi3_set_br .text.spi3_xfer)
    */tasks.o(.text.prvAddCurrentTaskToDelayedList .text.prvResetNextTaskUnblockTime .text.uxTaskGetNumberOfTasks
              .text.vTaskDelay .text.vTaskGenericNotifyGiveFromISR .text.vTaskSuspendAll .text.vTaskSwitchContext
              .text.xTaskGetSchedulerState .text.xTaskGetTickCount .text.xTaskGetTickCountFromISR
              .text.xTaskIncrementTick .text.xTaskPriorityDisinherit .text.xTaskRemoveFromEventList
              .text.xTaskResumeAll)
    */scope_mask.o(.text.scope_mask_hold_active .text.scope_mask_on_commit .text.scope_mask_service)
    */meter_link.o(.text.USART2_IRQHandler)
    */scope_roll.o(.text.scope_roll_fifo_get .text.scope_roll_fifo_push .text.scope_roll_period_ms
                   .text.scope_roll_record_code .text.scope_roll_trig_decide .text.scope_roll_trig_hit
                   .text.scope_roll_trig_step)
    */button_scan.o(.text.TMR3_GLOBAL_IRQHandler .text.pin_input_pullup .text.pin_output_pp)
    */interleave.o(.text.interleave_correct_b .text.interleave_map_b .text.interleave_merge)
    */usbd_int.o(.text .text*)
    */key_model.o(.text.key_scan_tick)
    */scope_timebase.o(.text .text*)
    */usbd_core.o(.text.usbd_core_in_handler .text.usbd_core_out_handler .text.usbd_ctrl_recv_status
                  .text.usbd_ctrl_send_status .text.usbd_enter_suspend .text.usbd_ept_recv .text.usbd_ept_send)
    */trig_edge.o(.text .text*)
    */port.o(.text.PendSV_Handler .text.SVC_Handler .text.SysTick_Handler .text.vPortEnterCritical
             .text.vPortExitCritical .text.vPortValidateInterruptPriority)
    */acq_cfg.o(.text .text*)
    */queue.o(.text.prvCopyDataToQueue .text.xQueueGenericSendFromISR)
    */usb_cdc.o(.text.USBFS_L_CAN1_RX0_IRQHandler .text.usb_trace_add)
    */at32f403a_407_gpio.o(.text .text*)
    */at32f403a_407_usb.o(.text.usb_enter_suspend .text.usb_exit_suspend .text.usb_flag_clear .text.usb_read_packet
                          .text.usb_set_address .text.usb_write_packet)
    */trig_policy.o(.text .text*)
    */list.o(.text .text*)
    */at32f403a_407_exint.o(.text .text*)
    *libc_nano.a:libc_a-memcpy-stub.o(.text .text*)
    */continuity_buzzer.o(.text.EXINT3_IRQHandler)
    *libc_nano.a:libc_a-memset.o(.text .text*)
    */scope_frontend.o(.text.scope_frontend_channel_mask)
    */button_scan.o(.rodata.bit_to_button)
    */scope_roll.o(.rodata.roll_arr)
_ezw_record = .;

/* Per frame: every display frame (20 a second) and the measurement strip's
 * refresh: the view renderers and their table, the status bar, the strip and
 * the measurements, the FFT with CMSIS, libm (cosf, log10f, sqrtf, floorf),
 * nano-printf and the soft-double helpers. */
    */fpga_acq.o(.text.fpga_acq_frame_generation .text.fpga_acq_record_has_ch .text.fpga_acq_record_interleaved
                 .text.fpga_acq_record_rate_idx .text.fpga_acq_record_time_ordered .text.fpga_get_ch1_buf
                 .text.fpga_get_ch2_buf .text.fpga_record_sample_rate .text.fpga_roll_fifo .text.fpga_trigger_status)
    */status_bar.o(.text .text*)
    */scope_measure.o(.text .text*)
    */scope_furniture.o(.text .text*)
    */main.o(.text.info_bar_epoch .text.layer_paint .text.native_base .text.native_paint .text.redraw_all
             .text.render_fft .text.render_split .text.render_waterfall .text.render_xy .text.render_yt
             .text.scope_render .text.scope_time_rows .text.scope_view_fft_epoch .text.scope_view_tick
             .text.view_shows)
    */arm_cfft_f32.o(.text .text*)
    */fft.o(.text.fft_auto_configure .text.fft_auto_take .text.fft_engine_ok .text.fft_get_config
            .text.fft_get_sample_buf .text.fft_is_initialized .text.fft_process .text.fft_reset_history
            .text.fft_reset_max_hold .text.fft_set_sample_rate)
    */scope_mask.o(.text.scope_mask_epoch .text.scope_mask_extent .text.scope_mask_state .text.scope_mask_summary)
    */scope_screen.o(.text.key_active_channel_get .text.key_focus_get .text.scope_screen_changed
                     .text.scope_screen_cursor_epoch .text.scope_screen_epochs .text.scope_screen_shown
                     .text.scope_view_sel .text.view_desc_of .text.view_half_has_job .text.view_key_domain)
    */lcd.o(.text.lcd_blit_rect .text.lcd_fill_rect .text.lcd_rect_begin .text.lcd_rect_row
            .text.lcd_set_pixel .text.lcd_write_data)
    */scope_cursor.o(.text.converts .text.converts.* .text.scope_cursor_cal_conf .text.scope_cursor_dt
                     .text.scope_cursor_dv .text.scope_cursor_format .text.scope_cursor_inv_dt
                     .text.scope_cursor_on_plot .text.scope_cursor_px .text.scope_cursor_tb_conf)
    */arm_cfft_radix8_f32.o(.text .text*)
    */scope_freq.o(.text .text*)
    */fft_live.o(.text .text*)
    *libgcc.a:_arm_muldivdf3.o(.text .text*)
    */scope_split.o(.text .text*)
    *libc_nano.a:libc_a-nano-vfprintf_i.o(.text .text*)
    *libgcc.a:_udivmoddi4.o(.text .text*)
    */scope_roll.o(.text.scope_roll_age_at_column .text.scope_roll_fifo_columns .text.scope_roll_fit_codes
                   .text.scope_roll_label)
    *libgcc.a:_arm_addsubdf3.o(.text .text*)
    */scope_waterfall.o(.text .text*)
    *libc_nano.a:libc_a-nano-svfprintf.o(.text .text*)
    */scope_cal.o(.text.format_range_label .text.scope_cal_codes_per_div .text.scope_cal_get_tier
                  .text.scope_cal_range_label .text.scope_cal_true_scale_ok .text.scope_cal_volts_per_div)
    */key_model.o(.text.key_focus_sanitize .text.view_desc_of)
    *libm.a:libm_a-ef_rem_pio2.o(.text .text*)
    */icons.o(.text .text*)
    */scope_geom.o(.text .text*)
    */measure_strip.o(.text .text*)
    *libm.a:libm_a-ef_log.o(.text .text*)
    */arm_rfft_fast_f32.o(.text .text*)
    */scope_popup.o(.text.draw_popup .text.draw_popup_box .text.popup_hole_in .text.popup_live
                    .text.scope_popup_active .text.scope_popup_overlay_tick .text.scope_popup_view_tick)
    */softkey_draw.o(.text .text*)
    */sinc_interp.o(.text .text*)
    *libgcc.a:_arm_addsubsf3.o(.text .text*)
    *libm.a:libm_a-kf_cos.o(.text .text*)
    */scope_ui.o(.text .text*)
    */fft_source.o(.text .text*)
    */siggen_wave.o(.text.siggen_wave_build .text.siggen_wave_uses_duty)
    */scope_roll_meas.o(.text .text*)
    */screen.o(.text.copy_stack .text.lock .text.screen_base .text.screen_get .text.screen_is_open .text.screen_page
               .text.unlock)
    *libm.a:libm_a-ef_log10.o(.text .text*)
    *libc_nano.a:libc_a-memchr.o(.text .text*)
    *libgcc.a:_aeabi_ldivmod.o(.text .text*)
    *libgcc.a:_arm_truncdfsf2.o(.text .text*)
    *libgcc.a:_arm_cmpdf2.o(.text .text*)
    *libm.a:libm_a-sf_floor.o(.text .text*)
    */meter_screen.o(.text.meter_screen_bar .text.meter_softkey_bar .text.meter_softkeys_epoch)
    *libm.a:libm_a-kf_sin.o(.text .text*)
    *libm.a:libm_a-sf_sin.o(.text .text*)
    */meas_select.o(.text .text*)
    *libm.a:libm_a-sf_cos.o(.text .text*)
    */arm_bitreversal2.o(.text .text*)
    *libc_nano.a:libc_a-snprintf.o(.text .text*)
    *libm.a:libm_a-wf_log10.o(.text .text*)
    */softkey.o(.text .text*)
    *libm.a:libm_a-sf_lround.o(.text .text*)
    *libm.a:libm_a-wf_sqrt.o(.text .text*)
    */arm_cmplx_mag_f32.o(.text .text*)
    *libc_nano.a:libc_a-memmove.o(.text .text*)
    *libgcc.a:_aeabi_uldivmod.o(.text .text*)
    *libc_nano.a:libc_a-strncat.o(.text .text*)
    *libc_nano.a:libc_a-strncpy.o(.text .text*)
    */theme.o(.text .text*)
    */battery.o(.text.battery_is_charging .text.battery_is_critical .text.battery_percent)
    */props_scope.o(.text.model_view_pair)
    */arm_scale_f32.o(.text .text*)
    *libc_nano.a:libc_a-strcpy.o(.text .text*)
    *libc_nano.a:libc_a-strlen.o(.text .text*)
    */model.o(.text.prop_get)
    *libm.a:libm_a-sf_fabs.o(.text .text*)
    */meter_ui.o(.text.meter_softkeys_draw)
    */can_scope.o(.text.can_scope_screen_on)
    *libc_nano.a:libc_a-errno.o(.text .text*)
    *libm.a:libm_a-sf_nan.o(.text .text*)
    */signal_gen.o(.text.siggen_get_config)
    *libm.a:libm_a-ef_sqrt.o(.text .text*)
    *libgcc.a:_dvmd_tls.o(.text .text*)
    */font_medium.o(.rodata.font_medium .rodata.font_medium_advances .rodata.font_medium_charmap
                    .rodata.font_medium_data .rodata.font_medium_offsets .rodata.font_medium_widths)
    */font_small.o(.rodata.font_small .rodata.font_small_advances .rodata.font_small_charmap .rodata.font_small_data
                   .rodata.font_small_offsets .rodata.font_small_widths)
    */icons.o(.rodata.arrow_down .rodata.arrow_left .rodata.arrow_right .rodata.arrow_up .rodata.bolt .rodata.dot
              .rodata.edge_fall .rodata.edge_rise .rodata.icon_table .rodata.trig_disc .rodata.trig_t)
    */key_model.o(.rodata.view_table)
    *libm.a:libm_a-ef_rem_pio2.o(.rodata.npio2_hw .rodata.two_over_pi)
    */main.o(.rodata.k_renderers .rodata.view_table)
    */math_channel.o(.rodata.mant.* .rodata.names.* .rodata.suffix.*)
    */meas_select.o(.rodata.fft_meas_labels)
    */measure_strip.o(.rodata.mstrip_show_priority)
    */meter_screen.o(.rodata.meter_bar_cont .rodata.meter_bar_fuse .rodata.meter_bar_fuse_scan
                     .rodata.meter_bar_fuse_types .rodata.meter_bar_limits .rodata.meter_bar_main
                     .rodata.meter_bar_temp)
    */persistence.o(.rodata.CSWTCH.*)
    */scope_cal.o(.rodata.mv_per_count .rodata.stock_nominal_mv .rodata.tier_by_range)
    */scope_furniture.o(.rodata.CSWTCH.* .rodata.view_table)
    */scope_measure.o(.rodata.prm_labels)
    */scope_screen.o(.rodata.view_table)
    */scope_spectrum.o(.rodata.wn.*)
    */scope_split.o(.rodata.k_half_draw .rodata.view_table)
    */scope_timebase.o(.rodata.core_divider .rodata.merged.* .rodata.per_adc.*)
    */scope_xy.o(.rodata.xy_title)
    */siggen_wave.o(.rodata.CSWTCH.*)
    */sinc_interp.o(.rodata.sinc_q15)
    */status_bar.o(.rodata.CSWTCH.* .rodata.trig_mode_short .rodata.words.*)
    */theme.o(.rodata.themes)
_ezw_frame = .;
