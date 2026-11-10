#ifndef DECODE_CAN_H
#define DECODE_CAN_H

#include "decoder.h"

/*
 * Classic CAN (2.0A/B) decoder for a sampled bus.
 *
 * Input is one signal per sample: ideally the differential CAN_H - CAN_L, but a
 * single wire works too. A sample at or above `threshold` is "high"; which of
 * high/low is the recessive (idle) level is set by `polarity`:
 *   CAN_POL_AUTO          the level that holds the longest run in the record
 *                         (the bus idles recessive, and only idle lasts > 10
 *                         bits), so swapped leads or an inverting front end
 *                         decode the same;
 *   CAN_POL_RECESSIVE_HIGH / _LOW to force it.
 *
 * Per frame it reports the identifier, DLC and data, and checks what a bus
 * analyser needs: CRC-15, the ACK slot (ACK / NAK), stuff and form errors, and
 * frames cut off by the end of the record (truncated, with whatever was read).
 * Bit timing re-synchronises on every edge, as a CAN controller does, so a node
 * whose clock is a few percent off still decodes over a full frame. Works from
 * 2 samples per bit upward.
 */

#define CAN_POL_AUTO            0
#define CAN_POL_RECESSIVE_HIGH  1
#define CAN_POL_RECESSIVE_LOW   2

typedef struct {
    uint32_t bit_rate;       /* 125000, 250000, 500000, 1000000 */
    int16_t  threshold;      /* level between dominant and recessive */
    uint8_t  polarity;       /* CAN_POL_*; 0 = auto */
} can_config_t;

void decode_can(const int16_t *samples, uint32_t num_samples,
                float sample_rate, const can_config_t *cfg,
                decode_result_t *result);

/* CRC-15/CAN over `nbits` bits (values 0/1, first bit first); for tests. */
uint16_t can_crc15(const uint8_t *bits, uint32_t nbits);

#endif /* DECODE_CAN_H */
