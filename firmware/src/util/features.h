/*
 * OpenScope 2C53T - the features of this build (core, always present)
 *
 * The list comes from firmware/features.mk (the manifest): the Makefile hands
 * features.c one row per feature KNOWN to the manifest, on or off, so a host
 * can tell "off in this build" from "never heard of it". Shown by the shell
 * (`version`, `features`) and sent in the stream HELLO (the mask).
 *
 * A feature's bit number is fixed for ever (NAME_bit in features.mk).
 */
#ifndef FEATURES_H
#define FEATURES_H

#include <stdint.h>

typedef struct {
    const char *name;   /* lower case, as in WITH= / WITHOUT= */
    const char *desc;
    uint8_t     bit;    /* in features_mask() */
    uint8_t     on;     /* 1 = built in */
} feature_info_t;

uint8_t               features_count(void);
const feature_info_t *features_at(uint8_t i);     /* NULL past the end */
uint32_t              features_mask(void);        /* bit set = feature on */

#endif /* FEATURES_H */
