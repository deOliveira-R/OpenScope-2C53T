/*
 * OpenScope 2C53T - oscilloscope screen: waterfall view
 *
 * Spectra over time as rows of colour.
 */

#include "scope_ui_parts.h"

#if FEATURE_FFT
/* Waterfall
 *
 * The 20 KB history buffer (WATERFALL_ROWS x WATERFALL_COLS bytes) lives in
 * the shared pool as a sub-tenant of the FFT region (see
 * SHMEM_FFT_WATERFALL_OFFSET) rather than in .bss. It cannot be a pool owner
 * of its own: draw_waterfall_screen() calls fft_process(), so it only ever
 * runs while SHMEM_OWNER_FFT holds the pool, and claiming the pool
 * separately would evict the FFT it depends on.
 *
 * Only waterfall_row_idx and the pool generation counter are static here.
 * The one assembled screen line is in ui_draw_scratch.
 */
#define WATERFALL_ROWS  SHMEM_FFT_WATERFALL_ROWS

#define WATERFALL_COLS  SHMEM_FFT_WATERFALL_COLS

static uint8_t waterfall_row_idx = 0;

static uint32_t waterfall_pool_generation = 0;

/* The blit in draw_waterfall_screen() sets one window across the full plot
 * width per screen line and streams into it, so a history row must be exactly
 * one screen line wide. */
_Static_assert(WATERFALL_COLS == LCD_WIDTH,
               "waterfall row blit assumes one history column per screen pixel");

/*
 * One assembled screen line, 640 bytes (WATERFALL_COLS RGB565 pixels). Colours
 * are converted once per history row and pushed row_height times, instead of
 * being recomputed for every screen line of the row. It is kept outside the
 * 20 KB history buffer: that lives in the shared pool (see shared_mem.h)
 * because it is large, whereas one line is small enough that the pool's
 * ownership rules would cost more than they save.
 */
#define waterfall_line (ui_draw_scratch.waterfall)   /* shared, see ui_draw_scratch */

/*
 * Intensity -> colour, with a BLACK floor.
 *
 * intensity 0 = at/above the display reference, 255 = at/below the floor
 * (reference - db_range). The ramp runs black -> blue -> cyan -> yellow ->
 * red in four equal 64-step segments, the conventional spectrogram ordering,
 * and puts "nothing here" (the noise floor) at black.
 */
static uint16_t intensity_to_color(uint8_t intensity)
{
    uint16_t level = (uint16_t)(255u - intensity);   /* 0 = floor, 255 = hot */
    uint8_t  t = (uint8_t)((level & 63u) * 4u);      /* position in segment  */

    if (level < 64)  return RGB565(0, 0, t);                        /* black->blue  */
    if (level < 128) return RGB565(0, t, 255);                      /* blue ->cyan  */
    if (level < 192) return RGB565(t, 255, (uint8_t)(255 - t));     /* cyan ->yellow*/
    return RGB565(255, (uint8_t)(255 - t), 0);                      /* yellow->red  */
}

/* What the last waterfall frame did, for the shell screen diagnostic
 * (scope_ui_waterfall_status). */
static uint8_t g_wf_status;   /* WF_ST_* */

enum { WF_ST_NEVER = 0, WF_ST_DRAWN, WF_ST_NO_FFT, WF_ST_WAITING, WF_ST_NOT_OWNER };

uint8_t scope_ui_waterfall_status(void) { return g_wf_status; }

uint8_t scope_ui_waterfall_row(void)    { return waterfall_row_idx; }

/* The waterfall view: one new spectrum row per call, scrolled through a
 * history of WATERFALL_ROWS rows, newest at the top.
 * Steps: get the input (fft_prepare_input) and transform it; resolve the
 * history buffer in the pool and reset it when the pool owner or the source
 * changed; store the new row as intensities (0 = at the reference, 255 = at
 * the floor); blit the whole plot line by line. Draws a message instead when
 * there is no input. Sets g_wf_status. */
void draw_waterfall_screen(void)
{
    const fft_config_t *cfg = fft_get_config();

    int16_t *sbuf = fft_get_sample_buf();
    if (!sbuf) {                    /* FFT not initialized */
        g_wf_status = WF_ST_NO_FFT;
        return;
    }
    uint16_t nsamp = 0;
    float fs = 0.0f;
    fft_source_t sel;
    sel.note[0] = '\0';
    fft_src_t src = fft_prepare_input(sbuf, &nsamp, &fs, &sel);
    if (src == FFT_SRC_NONE || nsamp == 0) {
        /* Composed, not blanked and redrawn: "CH2 is off" is a steady state. */
        g_wf_status = WF_ST_WAITING;
        fft_note_compose(SCOPE_TOP, SCOPE_H, sel.note[0] ? sel.note : "WFALL: waiting for capture");
        return;
    }
    bool live = (src == FFT_SRC_LIVE);
    fft_process(sbuf, nsamp, &fft_result);

    /* Resolve the history buffer out of the FFT's pool tenancy. Returns NULL
     * if FFT does not own the pool, in which case there is nothing valid to
     * draw into: return rather than write over another owner's data. This is
     * a stricter check than the fft_get_sample_buf() guard above, which only
     * proves the FFT was initialised at some point. */
    uint8_t *wf_pool = shared_mem_get(SHMEM_OWNER_FFT);
    if (!wf_pool) {
        g_wf_status = WF_ST_NOT_OWNER;
        return;
    }
    g_wf_status = WF_ST_DRAWN;
    uint8_t (*waterfall_buf)[WATERFALL_COLS] =
        (uint8_t (*)[WATERFALL_COLS])(wf_pool + SHMEM_FFT_WATERFALL_OFFSET);

    /* shared_mem_acquire() zeroes the pool whenever the owner changes, so a
     * round trip through screenshot/persistence wipes the history. Detect that
     * and restart from row 0 instead of scrolling through a zeroed tail.
     *
     * Refill with 0xFF rather than leaving the pool's zeros: intensity 0 maps
     * to the top of the ramp (red = at the display reference), so zeroed rows
     * would render as maximum signal. 0xFF is the other end of the ramp
     * (black = reference - db_range = the noise floor), which is what "no data
     * yet" looks like. */
    uint32_t pool_generation = shared_mem_transition_count();
    if (pool_generation != waterfall_pool_generation || waterfall_restart) {
        /* waterfall_restart: the source changed (fft_history_track), so the
         * rows on screen are another channel's. */
        waterfall_restart = false;
        waterfall_pool_generation = pool_generation;
        waterfall_row_idx = 0;
        memset(waterfall_buf, 0xFF, SHMEM_FFT_WATERFALL_SIZE);
    }

    const float *draw_data = (fft_result.avg_db != NULL)
                             ? fft_result.avg_db : fft_result.level_db;

    uint16_t zoom_start = cfg->zoom_start_bin;
    uint16_t zoom_end = cfg->zoom_end_bin;
    uint16_t zoom_span = zoom_end - zoom_start;
    if (zoom_span == 0) zoom_span = 1;

    float ref_db = fft_display_ref(cfg, draw_data, zoom_start, zoom_end);

    uint16_t x;
    for (x = 0; x < WATERFALL_COLS; x++) {
        uint16_t bin = zoom_start + (uint16_t)((uint32_t)x * zoom_span / WATERFALL_COLS);
        if (bin >= FFT_BINS) bin = FFT_BINS - 1;
        float db = draw_data[bin];
        float normalized = (ref_db - db) / cfg->db_range;
        if (normalized < 0.0f) normalized = 0.0f;
        if (normalized > 1.0f) normalized = 1.0f;
        waterfall_buf[waterfall_row_idx][x] = (uint8_t)(normalized * 255.0f);
    }

    uint8_t newest_row = waterfall_row_idx;
    waterfall_row_idx = (uint8_t)((waterfall_row_idx + 1) % WATERFALL_ROWS);

    /* ── Blit ───────────────────────────────────────────────────────
     *
     * The plot is not cleared first. Every screen line of the plot is written
     * once: a history row is converted once into waterfall_line and streamed
     * row_height times, one window and one lcd_write_pixels() call per screen
     * line. The residual lines under the block (SCOPE_H is not a multiple of
     * WATERFALL_ROWS: 14 lines at 206/64) are written black, or with the axis
     * labels.
     *
     * One window for the WHOLE block would need far fewer set_windows, but the
     * window is global driver state and vInputTask runs at priority 4 against
     * the display task's 1: its power-off countdown overlay (the POWER loop in
     * tasks/input_handler.c) calls lcd_fill_rect() and so re-points the window
     * from under a stream in progress. A window per line bounds that to one
     * corrupted line for one frame.
     *
     * Why not scroll-and-append, which would be O(1 row)? The history scrolls
     * by exactly one row per frame, so 63 of 64 rows are unchanged in content,
     * but they all MOVE, and with no framebuffer on this side of the bus there
     * is nothing to shift. The ST7789's own vertical scroll works on panel
     * rows, which MADCTL = 0xA0 (MV = 1) maps to screen COLUMNS, i.e. it would
     * scroll the waterfall sideways. An O(1)-per-frame waterfall needs either
     * a RAM framebuffer (the shared pool is already used by the FFT) or a
     * different history layout that lets the newest row wrap in place instead
     * of the image scrolling. That changes what is drawn. */
    uint16_t row_height = SCOPE_H / WATERFALL_ROWS;
    if (row_height < 1) row_height = 1;

    uint16_t rows_drawn = WATERFALL_ROWS;
    if ((uint32_t)rows_drawn * row_height > SCOPE_H)
        rows_drawn = (uint16_t)(SCOPE_H / row_height);
    const uint16_t block_h = (uint16_t)(rows_drawn * row_height);

    /* The header and the axis labels are composed into the lines they
     * cross (font_text_px) instead of drawn over the streamed rows, so
     * nothing is painted twice and nothing blinks; the residual strip under
     * the block (SCOPE_H - block_h rows) carries the axis labels. */
    ctext_set_t T;
    T.n = 0;
    char wf_hdr[16];
    if (live) snprintf(wf_hdr, sizeof wf_hdr, "WFALL LIVE %s", sel.label);
    else      snprintf(wf_hdr, sizeof wf_hdr, "WFALL DEMO");
    ctext_add(&T, 4, SCOPE_TOP + 2, wf_hdr,
              live ? COLOR_WHITE : COLOR_ORANGE, live ? COLOR_WHITE : COLOR_ORANGE, false);
    {
        char l[12];
        const int ly = SCOPE_TOP + SCOPE_H - 12;
        fft_live_axis_label(fs, FFT_SIZE, zoom_start, l, sizeof l);
        ctext_add(&T, 2, ly, l, COLOR_WHITE, COLOR_BLACK, true);
        fft_live_axis_label(fs, FFT_SIZE, zoom_end, l, sizeof l);
        ctext_add(&T, LCD_WIDTH - 2 - (int)font_string_width(l, &font_small), ly, l,
                  COLOR_WHITE, COLOR_BLACK, true);
    }

    for (uint16_t sy = 0; sy < SCOPE_H; sy++) {
        const uint16_t y = (uint16_t)(SCOPE_TOP + sy);
        const bool in_block = sy < block_h;
        const uint16_t r = (uint16_t)(sy / row_height);
        /* A history row is converted once and reused for its row_height
         * lines unless a label crosses this line. */
        const bool row_first = in_block && (sy % row_height) == 0u;
        bool text_line = false;
        for (uint8_t k = 0; k < T.n; k++)
            if ((int)y >= T.t[k].y && (int)y < T.t[k].y + (int)font_small.height) text_line = true;
        if (row_first || text_line || !in_block) {
            const uint8_t *src = in_block
                ? waterfall_buf[(newest_row + WATERFALL_ROWS - r) % WATERFALL_ROWS] : NULL;
            for (x = 0; x < WATERFALL_COLS; x++) {
                uint16_t c = src ? intensity_to_color(src[x]) : COLOR_BLACK;
                if (text_line) (void)ctext_px(&T, x, y, &c);
                waterfall_line[x] = c;
            }
        }
        lcd_set_window(0, y, WATERFALL_COLS, 1);
        lcd_write_pixels(waterfall_line, WATERFALL_COLS);
        if (text_line && in_block && (sy + 1u) % row_height != 0u) {
            /* the next line of this row has no text: rebuild it plain */
            const uint8_t *src = waterfall_buf[(newest_row + WATERFALL_ROWS - r) % WATERFALL_ROWS];
            for (x = 0; x < WATERFALL_COLS; x++)
                waterfall_line[x] = intensity_to_color(src[x]);
        }
    }
}

#endif
