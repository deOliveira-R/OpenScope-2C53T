#!/usr/bin/env python3
"""Generate the Q15 polyphase table in firmware/src/dsp/sinc_interp.c.

Design (see sinc_interp.h): 16 taps, output between taps 7 and 8, phase
d = k/20 (k = 0..19) so every factor that divides 20 (2, 4, 5, 10, 20 -- all
of stock's, 0x08030B64) indexes the same table. Tap k sits at t = k - 7 - d
samples from the output point; h = sinc(t) * Kaiser(t / 8, beta = 5),
normalised so each phase sums to exactly 32768 (DC gain 1). tests/test_sinc_interp.c
regenerates the table in double precision and fails if this file and the C
table disagree by more than 1 LSB.

Usage: python3 scripts/gen_sinc_interp.py   (prints the C initialiser)
"""
import math

TAPS, PHASES, BETA, HALF = 16, 20, 5.0, 8.0


def i0(x):
    s = t = 1.0
    for k in range(1, 50):
        t *= (x / 2.0 / k) ** 2
        s += t
    return s


def sinc(x):
    return 1.0 if abs(x) < 1e-12 else math.sin(math.pi * x) / (math.pi * x)


def phase_taps(k):
    d = k / PHASES
    h = []
    for j in range(TAPS):
        t = j - 7 - d
        u = 1.0 - (t / HALF) ** 2
        h.append(sinc(t) * i0(BETA * math.sqrt(max(u, 0.0))) / i0(BETA))
    s = sum(h)
    q = [int(round(v / s * 32768.0)) for v in h]
    # Put the rounding residue on the largest tap so the sum is exact.
    q[max(range(TAPS), key=lambda j: abs(h[j]))] += 32768 - sum(q)
    return q


def main():
    print("static const int32_t sinc_q15[SINC_INTERP_PHASES][SINC_INTERP_TAPS] = {")
    for k in range(PHASES):
        q = phase_taps(k)
        print("    { " + ", ".join("%6d" % v for v in q) + " },  /* d = %2d/20 */" % k)
    print("};")


if __name__ == "__main__":
    main()
