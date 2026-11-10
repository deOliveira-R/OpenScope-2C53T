#ifndef CAN_BUS_H
#define CAN_BUS_H

/*
 * CAN bus analysis on a scope record: the glue between the two channels and
 * decode_can(). Pure (no hardware), so it runs in the host tests.
 *
 *   CH1 = CAN_H, CH2 = CAN_L (either way round: polarity is found from the
 *   idle level, so swapped leads and the 2C53T's inverting front end decode the
 *   same). With ch2 == NULL, CH1 alone is decoded (single-wire probing).
 *
 * The bit width is measured from the record itself: the shortest run length
 * that repeats, refined over every in-frame run. So the decode does not depend
 * on knowing the sample rate; it only needs the bus to have traffic. When the
 * sample rate IS known (fs > 0), the bus bit rate is reported as fs / spb.
 */

#include <stdint.h>
#include "decoder.h"
#include "decode_can.h"

typedef struct {
    uint8_t  signal;        /* 1: two levels found and bits resolved          */
    float    spb;           /* measured samples per bit                       */
    float    bit_rate;      /* fs / spb when fs is known, else 0              */
    int16_t  level_rec;     /* recessive level (diff counts)                  */
    int16_t  level_dom;     /* dominant level (diff counts)                   */
    int16_t  threshold;
    int32_t  rotation;      /* samples rotated to time order, -1 = none       */
} can_bus_report_t;

/* Fill `diff` (n samples) from the channels, analyse, decode into `res`.
 *
 * `rotated`: the record is a ring buffer not yet in time order (the 2C53T's
 * strobed records are, unless OpenScope found their seam: see
 * fpga_acq_record_time_ordered()). A CAN record's seam sits in idle, where the
 * generic value-step seam finder sees nothing. Here the start of frame that
 * ends the longest idle stretch (the trigger) is rotated to n/2, where the
 * 2C53T puts its trigger, which restores time order: frames come out whole and
 * in order, and a frame cut by the end of the acquisition is reported truncated
 * rather than as a missing ACK.
 *
 * Returns the number of frames decoded (0 when there is no CAN signal). */
uint32_t can_bus_analyse(const volatile uint8_t *ch1, const volatile uint8_t *ch2,
                         uint32_t n, float fs, uint8_t rotated, int16_t *diff,
                         can_bus_report_t *rep, decode_result_t *res);

/* Rotate `s` left by k in place (exposed for tests). */
void can_bus_rotate(int16_t *s, uint32_t n, uint32_t k);

/* Samples per bit from a thresholded record; 0 if it cannot tell. */
float can_bus_measure_spb(const int16_t *s, uint32_t n, int16_t thr);

/* Running statistics over many records. */
#define CAN_BUS_MAX_IDS 12
typedef struct {
    uint32_t id;
    uint8_t  ext;
    uint32_t seen;
    uint32_t ack, nak, crc, err;
    uint8_t  dlc;
    uint8_t  data[8];       /* last payload */
} can_bus_id_t;

typedef struct {
    uint32_t records;       /* records analysed                                */
    uint32_t idle;          /* records without a CAN signal                    */
    uint32_t frames;        /* complete frames                                 */
    uint32_t ack, nak, crc, err, trunc;
    uint8_t  nids;
    uint32_t ids_dropped;   /* frames whose ID did not fit the table           */
    can_bus_id_t ids[CAN_BUS_MAX_IDS];
} can_bus_stats_t;

void can_bus_stats_reset(can_bus_stats_t *st);
void can_bus_stats_add(can_bus_stats_t *st, const can_bus_report_t *rep,
                       const decode_result_t *res);

/* One-line summary for the screen, e.g. "CAN 500k 1B0 4B0 181 ok" or
 * "CAN 1B0 NAK 12/40". Returns 1 when the bus looks healthy. */
int can_bus_summary(const can_bus_stats_t *st, const can_bus_report_t *last,
                    char *out, uint32_t len);

#endif /* CAN_BUS_H */
