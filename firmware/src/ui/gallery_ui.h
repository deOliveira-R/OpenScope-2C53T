/*
 * gallery_ui — device side of the screenshot gallery (parity C16).
 *
 * Storage and naming: src/util/gallery_store.h. Key logic: gallery_nav.h.
 * The gallery itself is a screen of the router (gallery_screen.h: open,
 * close, is-open, its keys); this file is its paint and the capture for
 * short SAVE and the shell's `screenshot save`. All LCD work is the display
 * task's.
 */
#ifndef GALLERY_UI_H
#define GALLERY_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "gallery_store.h"

#include "gallery_screen.h"   /* gallery_ui_is_open, gallery_ui_close, gallery_ui_paint */

/* Before the first capture: the display queue DCMD_CAPTURE is posted to. */
void gallery_ui_bind(QueueHandle_t display_queue);

/* Capture the screen into the next free slot, from any task but the display
 * task: the display task saves the LCD shadow between two frames (~0.5 s
 * with the screen still), so the shot is one whole frame and contains no
 * "Saving..." text of its own. Waits for the result; the caller shows it. */
gallery_status_t gallery_ui_capture(uint32_t *seq_out);
/* Display task, on DCMD_CAPTURE. */
void gallery_ui_capture_run(void);

/* Short text for a capture result, e.g. "Saved 3.bmp" / "Gallery full". */
void gallery_ui_result_text(gallery_status_t st, uint32_t seq, char *out, uint32_t out_size);

#endif /* GALLERY_UI_H */
