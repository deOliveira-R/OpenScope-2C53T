/*
 * gallery_ui — device side of the screenshot gallery (parity C16).
 *
 * Storage and naming: src/util/gallery_store.h. Key logic: gallery_nav.h.
 * This file owns the screen while the gallery is open (through
 * ui_modal_active, the same claim the power-off countdown uses) and does the
 * capture for short SAVE and the shell's `screenshot save`.
 */
#ifndef GALLERY_UI_H
#define GALLERY_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "gallery_store.h"

/* Capture the screen into the next free slot. Freezes rendering while the
 * LCD shadow is written to flash (~0.5 s), so the shot is one whole frame and
 * contains no "Saving..." text of its own. The caller shows the result. */
gallery_status_t gallery_ui_capture(uint32_t *seq_out);

/* Short text for a capture result, e.g. "Saved 3.bmp" / "Gallery full". */
void gallery_ui_result_text(gallery_status_t st, uint32_t seq, char *out, uint32_t out_size);

bool gallery_ui_is_open(void);
void gallery_ui_open(QueueHandle_t dq);

/* Every key while the gallery is open (button id, BTN_LONG_FLAG allowed).
 * Returns false only for POWER: the gallery has closed itself and the caller
 * must handle the key as usual. */
bool gallery_ui_key(uint8_t button, QueueHandle_t dq);

#endif /* GALLERY_UI_H */
