/*
 * OpenScope 2C53T - the features of this build. See features.h.
 *
 * FEATURE_XLIST is passed by the Makefile to this file only:
 *   X(name, bit, on, "description") X(...) ...
 * one X per feature in firmware/features.mk, in manifest order.
 */
#include "features.h"

#include <stddef.h>

#ifndef FEATURE_XLIST
#error "FEATURE_XLIST is not defined: build through the Makefile (features.mk)"
#endif

#define X(name, bit, on, desc) { #name, desc, (uint8_t)(bit), (uint8_t)(on) },
static const feature_info_t feature_table[] = { FEATURE_XLIST };
#undef X

#define X(name, bit, on, desc) | ((uint32_t)(on) << (bit))
#define FEATURES_MASK_VALUE (0u FEATURE_XLIST)

#define FEATURES_N (sizeof(feature_table) / sizeof(feature_table[0]))

uint8_t features_count(void) { return (uint8_t)FEATURES_N; }

const feature_info_t *features_at(uint8_t i)
{
    return i < FEATURES_N ? &feature_table[i] : NULL;
}

uint32_t features_mask(void) { return FEATURES_MASK_VALUE; }
