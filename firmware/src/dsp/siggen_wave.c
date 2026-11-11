/*
 * OpenScope 2C53T - signal generator: waveform, timer and DAC_CTRL math.
 * See siggen_wave.h for the stock model this reproduces.
 */

#include "siggen_wave.h"

#include <stddef.h>

/*
 * The eight waveform tables of stock V1.2.0, 100 x u16 each, copied from
 * flash 0x0804D848..0x0804DE87 (v120.bin file offset 0x46848). The same
 * 1600 bytes are present unchanged in V1.0.3, V1.0.7 and V1.1.2. The
 * builder addresses them as `base + 200 * idx - c` (0x08029E54), which
 * gives the wave -> table map in table_for_wave() below.
 *
 * Table 0 is exactly round(2047 + 2047 * sin(2 pi i / 100)); the others
 * are shape data (Half and Full sit on a 2047 floor, so at a given
 * amplitude they swing half as far as Sine, on a mid-scale pedestal —
 * kept that way for parity, not rescaled). The host test checks the
 * extraction against the emulated stock builder at every amplitude.
 */
static const uint16_t stock_tables[SIGGEN_TABLE_COUNT][SIGGEN_CYCLE_SAMPLES] = {
    /* 0: sine (wave 0), stock 0x0804D848 */
    {
        2047, 2176, 2304, 2431, 2556, 2680, 2801, 2919, 3033, 3144,
        3250, 3352, 3448, 3539, 3624, 3703, 3775, 3841, 3899, 3950,
        3994, 4030, 4058, 4078, 4090, 4094, 4090, 4078, 4058, 4030,
        3994, 3950, 3899, 3841, 3775, 3703, 3624, 3539, 3448, 3352,
        3250, 3144, 3033, 2919, 2801, 2680, 2556, 2431, 2304, 2176,
        2047, 1918, 1790, 1663, 1538, 1414, 1293, 1175, 1061,  950,
         844,  742,  646,  555,  470,  391,  319,  253,  195,  144,
         100,   64,   36,   16,    4,    0,    4,   16,   36,   64,
         100,  144,  195,  253,  319,  391,  470,  555,  646,  742,
         844,  950, 1061, 1175, 1293, 1414, 1538, 1663, 1790, 1918,
    },
    /* 1: half wave (wave 3), stock 0x0804D910 */
    {
        2047, 2176, 2304, 2431, 2556, 2680, 2801, 2919, 3033, 3144,
        3250, 3352, 3448, 3539, 3624, 3703, 3775, 3841, 3899, 3950,
        3994, 4030, 4058, 4078, 4090, 4094, 4090, 4078, 4058, 4030,
        3994, 3950, 3899, 3841, 3775, 3703, 3624, 3539, 3448, 3352,
        3250, 3144, 3033, 2919, 2801, 2680, 2556, 2431, 2304, 2176,
        2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048,
        2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048,
        2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048,
        2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048,
        2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048,
    },
    /* 2: full wave (wave 4), stock 0x0804D9D8 */
    {
        2047, 2111, 2176, 2240, 2304, 2367, 2431, 2494, 2556, 2618,
        2680, 2741, 2801, 2860, 2919, 2977, 3033, 3089, 3144, 3198,
        3250, 3302, 3352, 3401, 3449, 3495, 3540, 3583, 3625, 3665,
        3703, 3740, 3776, 3809, 3841, 3871, 3900, 3926, 3951, 3973,
        3994, 4013, 4030, 4045, 4058, 4069, 4078, 4085, 4090, 4093,
        4095, 4093, 4090, 4085, 4078, 4069, 4058, 4045, 4030, 4013,
        3994, 3973, 3951, 3926, 3900, 3871, 3841, 3809, 3776, 3740,
        3703, 3665, 3625, 3583, 3540, 3495, 3449, 3401, 3352, 3302,
        3250, 3198, 3144, 3089, 3033, 2977, 2919, 2860, 2801, 2741,
        2680, 2618, 2556, 2494, 2431, 2367, 2304, 2240, 2176, 2111,
    },
    /* 3: exponential rise (wave 7), stock 0x0804DAA0 */
    {
          30,   31,   33,   34,   36,   38,   40,   42,   44,   46,
          49,   51,   54,   57,   60,   63,   66,   69,   73,   77,
          80,   85,   89,   93,   98,  103,  109,  114,  120,  126,
         133,  139,  146,  154,  162,  170,  179,  188,  197,  207,
         218,  229,  241,  253,  266,  280,  294,  309,  325,  341,
         359,  377,  396,  416,  437,  460,  483,  508,  534,  561,
         589,  619,  651,  684,  719,  756,  794,  835,  877,  922,
         969, 1018, 1070, 1124, 1182, 1242, 1305, 1371, 1441, 1515,
        1592, 1673, 1758, 1848, 1942, 2040, 2144, 2254, 2368, 2489,
        2616, 2749, 2889, 3036, 3190, 3353, 3523, 3703, 3891, 4089,
    },
    /* 4: exponential fall (wave 8), stock 0x0804DB68 */
    {
        4065, 4063, 4061, 4060, 4058, 4056, 4054, 4052, 4050, 4048,
        4045, 4043, 4040, 4037, 4034, 4031, 4028, 4025, 4021, 4017,
        4014, 4009, 4005, 4001, 3996, 3991, 3985, 3980, 3974, 3968,
        3961, 3955, 3948, 3940, 3932, 3924, 3915, 3906, 3897, 3887,
        3876, 3865, 3853, 3841, 3828, 3814, 3800, 3785, 3769, 3753,
        3735, 3717, 3698, 3678, 3657, 3634, 3611, 3586, 3560, 3533,
        3505, 3475, 3443, 3410, 3375, 3338, 3300, 3259, 3217, 3172,
        3125, 3076, 3024, 2970, 2912, 2852, 2789, 2723, 2653, 2579,
        2502, 2421, 2336, 2246, 2152, 2054, 1950, 1840, 1726, 1605,
        1478, 1345, 1205, 1058,  904,  741,  571,  391,  203,   81,
    },
    /* 5: multi-tone (wave 10), stock 0x0804DC30 */
    {
        2049, 2532, 2986, 3386, 3709, 3940, 4069, 4095, 4023, 3865,
        3641, 3370, 3078, 2789, 2522, 2298, 2128, 2020, 1975, 1988,
        2049, 2145, 2261, 2379, 2485, 2566, 2613, 2620, 2588, 2520,
        2425, 2312, 2194, 2083, 1989, 1922, 1887, 1886, 1917, 1974,
        2049, 2133, 2214, 2282, 2329, 2349, 2338, 2297, 2231, 2145,
        2049, 1953, 1868, 1801, 1760, 1750, 1769, 1816, 1884, 1966,
        2049, 2124, 2182, 2212, 2211, 2176, 2109, 2015, 1904, 1786,
        1673, 1578, 1510, 1478, 1485, 1532, 1613, 1719, 1837, 1953,
        2049, 2110, 2123, 2078, 1970, 1800, 1576, 1310, 1020,  728,
         458,  233,   75,    3,   29,  158,  389,  712, 1112, 1567,
    },
    /* 6: sinc pulse (wave 11), stock 0x0804DCF8 */
    {
         729,  661,  601,  556,  535,  541,  575,  634,  709,  791,
         868,  929,  964,  966,  933,  869,  780,  678,  577,  492,
         437,  421,  450,  522,  631,  764,  903, 1028, 1120, 1162,
        1144, 1061,  918,  729,  517,  307,  131,   20,    0,   91,
         301,  630, 1063, 1574, 2127, 2682, 3194, 3622, 3929, 4089,
        4089, 3929, 3622, 3194, 2682, 2127, 1574, 1063,  630,  301,
          91,    0,   20,  131,  307,  517,  730,  918, 1061, 1144,
        1162, 1120, 1028,  903,  764,  631,  522,  450,  421,  437,
         492,  577,  678,  780,  869,  933,  966,  964,  929,  868,
         791,  709,  634,  575,  541,  535,  556,  601,  661,  729,
    },
    /* 7: lorentz (wave 12), stock 0x0804DDC0 */
    {
           1,   52,  105,  160,  217,  276,  337,  401,  466,  534,
         605,  678,  753,  832,  912,  996, 1082, 1171, 1263, 1357,
        1454, 1554, 1657, 1761, 1869, 1978, 2089, 2203, 2317, 2433,
        2549, 2666, 2783, 2898, 3013, 3125, 3235, 3342, 3444, 3542,
        3634, 3720, 3798, 3868, 3930, 3983, 4025, 4058, 4079, 4090,
        4090, 4079, 4058, 4025, 3983, 3930, 3868, 3798, 3720, 3634,
        3542, 3444, 3342, 3235, 3125, 3013, 2898, 2783, 2666, 2549,
        2433, 2317, 2203, 2089, 1978, 1869, 1761, 1657, 1554, 1454,
        1357, 1263, 1171, 1082,  996,  912,  832,  753,  678,  605,
         534,  466,  401,  337,  276,  217,  160,  105,   52,    1,
    },
};

static const char *const wave_names[SIGGEN_WAVEFORM_COUNT] = {
    "Sine", "Square", "Sawtooth", "Half Wave", "Full Wave",
    "Step", "Rev Step", "Exp Rise", "Exp Fall", "DC",
    "Multi-tone", "Sinc Pulse", "Lorentz",
};

const char *siggen_wave_name(uint8_t wave)
{
    return (wave < SIGGEN_WAVEFORM_COUNT) ? wave_names[wave] : "?";
}

bool siggen_wave_uses_duty(uint8_t wave)
{
    return wave == SIGGEN_SQUARE || wave == SIGGEN_SAWTOOTH;
}

/* Stock tbh cases 0, 3, 4, 7, 8, 10, 11, 12 read a flash table; the
 * offsets c in `base + 200 * idx - c` are 0, 400, 1000 for them. */
static const uint16_t *table_for_wave(uint8_t wave)
{
    switch (wave) {
    case SIGGEN_SINE:        return stock_tables[0];
    case SIGGEN_HALF_WAVE:   return stock_tables[1];
    case SIGGEN_FULL_WAVE:   return stock_tables[2];
    case SIGGEN_EXP_RISE:    return stock_tables[3];
    case SIGGEN_EXP_FALL:    return stock_tables[4];
    case SIGGEN_MULTI_TONE:  return stock_tables[5];
    case SIGGEN_SINC_PULSE:  return stock_tables[6];
    case SIGGEN_LORENTZ:     return stock_tables[7];
    default:                 return NULL;
    }
}

void siggen_wave_build(uint16_t out[SIGGEN_CYCLE_SAMPLES],
                       uint8_t wave, uint8_t amp_dv, uint8_t duty_pct)
{
    if (amp_dv > SIGGEN_AMP_MAX_DV) amp_dv = SIGGEN_AMP_MAX_DV;
    if (duty_pct > SIGGEN_DUTY_MAX) duty_pct = SIGGEN_DUTY_MAX;

    const uint32_t amp = amp_dv;
    const uint32_t duty = duty_pct;
    const uint32_t base = SIGGEN_CODE_FLOOR;
    /* High level of the computed types: `rsb x, x, x, lsl #12` = x * 4095,
     * then /33 (0x0802A2A8). 3722 at 3.0 V, so the top code is 3742. */
    const uint32_t high = amp * 4095u / 33u;
    const uint16_t *tbl = table_for_wave(wave);

    for (uint32_t i = 0; i < SIGGEN_CYCLE_SAMPLES; i++) {
        uint32_t v;
        if (tbl != NULL) {
            v = base + (uint32_t)tbl[i] * amp / 33u;
        } else {
            switch (wave) {
            case SIGGEN_SQUARE:
                /* 0x0802A2C6: `i < duty ? high : 20`, duty in 1 % steps */
                v = (i < duty) ? base + high : base;
                break;
            case SIGGEN_SAWTOOTH:
                /* 0x0802A424: rise over `duty` samples (udiv), fall over
                 * the remaining 100 - duty (sdiv). duty 0 is a pure fall,
                 * 100 a pure rise, so each branch divides only when its
                 * own span is non-empty. */
                if (i < duty)
                    v = base + i * high / duty;
                else
                    v = base + (SIGGEN_CYCLE_SAMPLES - i) * high /
                                (SIGGEN_CYCLE_SAMPLES - duty);
                break;
            case SIGGEN_STEP:
                /* 0x0802A600: 8 levels of high/7, widths 13,12,13,12,...
                 * which is level = i * 8 / 100. */
                v = base + (i * 8u / SIGGEN_CYCLE_SAMPLES) * (high / 7u);
                break;
            case SIGGEN_REV_STEP:
                v = base + high - (i * 8u / SIGGEN_CYCLE_SAMPLES) * (high / 7u);
                break;
            case SIGGEN_DC:
                /* 0x0802A902: constant, so "amplitude" is the DC level */
                v = base + high;
                break;
            default:
                v = base;
                break;
            }
        }
        out[i] = (uint16_t)v;
    }
}

bool siggen_timing_compute(uint32_t freq_hz, uint32_t tmr_clk_hz,
                           siggen_timing_t *t)
{
    t->div = 0;
    t->pr = 0;
    t->actual_mhz = 0;
    if (freq_hz == 0 || tmr_clk_hz == 0) return false;

    const uint64_t clk = tmr_clk_hz;
    const uint64_t upd = (uint64_t)freq_hz * SIGGEN_CYCLE_SAMPLES;

    /* Smallest DIV + 1 that keeps the period within the 16-bit PR. With
     * DIV = 0 every integer divider is reachable, so that is optimal. */
    const uint64_t psc_min = clk / (upd * 65536u) + 1u;
    /* Below ~37 Hz (DIV > 0) only products DIV+1 x PR+1 are reachable;
     * try a few DIV values so an exact factorisation is not missed
     * (2, 3, 6 and 15 Hz come out exact this way, as they do in stock). */
    const uint64_t psc_max = (psc_min == 1u) ? 1u : psc_min + 63u;
    uint64_t psc = psc_min, per = 2u;
    uint64_t best_num = UINT64_MAX, best_den = 1u;   /* error = num / den */
    for (uint64_t p = psc_min; p <= psc_max && p <= 65536u; p++) {
        /* Of the two periods around clk / (upd * p), keep the one whose
         * frequency is nearer f. That is not the nearer period, because
         * f goes as 1 / period: q wins over q + 1 iff
         * clk * (2q + 1) <= 2 * upd * p * q * (q + 1). */
        uint64_t q = clk / (upd * p);
        if (q < 1u) q = 1u;
        if (clk * (2u * q + 1u) > 2u * upd * p * q * (q + 1u)) q += 1u;
        if (q < 2u) q = 2u;                                  /* pr >= 1 */
        if (q > 65536u) continue;
        /* |f' - f| is proportional to |clk - upd * n| / n, n = p * q. */
        const uint64_t n = p * q;
        const uint64_t a = clk, b = upd * n;
        const uint64_t num = (a > b) ? a - b : b - a;
        if (best_num == UINT64_MAX || num * best_den < best_num * n) {
            best_num = num;
            best_den = n;
            psc = p;
            per = q;
        }
    }
    t->div = (uint16_t)(psc - 1u);
    t->pr = (uint16_t)(per - 1u);
    const uint64_t total = psc * per * SIGGEN_CYCLE_SAMPLES;
    t->actual_mhz = (uint32_t)((clk * 1000u + total / 2u) / total);
    return true;
}

bool siggen_timing_stock(uint32_t freq_hz, uint32_t tmr_clk_hz,
                         siggen_timing_t *t)
{
    t->div = 0;
    t->pr = 0;
    t->actual_mhz = 0;
    if (freq_hz == 0 || tmr_clk_hz == 0) return false;

    /* s = f * 200; below 4000 (f < 20 Hz) s *= 10 and PR = 19, else
     * PR = 1; DIV = clk / s - 1 truncated (vcvt.u32), stored into the
     * 16-bit DIV register. */
    uint32_t s = freq_hz * 200u;
    uint32_t pr = 1u;
    if (s < 4000u) { s *= 10u; pr = 19u; }
    uint32_t div = tmr_clk_hz / s;
    div = (div > 0u) ? div - 1u : 0u;
    t->div = (uint16_t)div;                    /* register keeps 16 bits */
    t->pr = (uint16_t)pr;
    const uint64_t total = ((uint64_t)t->div + 1u) * (pr + 1u) *
                           SIGGEN_CYCLE_SAMPLES;
    t->actual_mhz = (uint32_t)(((uint64_t)tmr_clk_hz * 1000u + total / 2u) / total);
    return true;
}

uint32_t siggen_dac_ctrl_ch2(uint32_t ctrl, uint8_t tsel, bool en)
{
    uint32_t v = ctrl & SIGGEN_DAC_CH1_MASK;
    v |= ((uint32_t)(tsel & 7u)) << SIGGEN_DAC_D2TRGSEL_POS;
    v |= SIGGEN_DAC_D2TRGEN | SIGGEN_DAC_D2DMAEN;
    if (en) v |= SIGGEN_DAC_D2EN;
    return v;           /* D2OBDIS, D2NM, D2NBSEL, bits 31:29 all 0 */
}
