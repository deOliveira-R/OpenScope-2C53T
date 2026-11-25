/*
 * Where a label may go on a plot (src/ui/label_place.h): the first candidate
 * inside the plot that meets no label placed before it, or none.
 *
 * [1] boxes meet when they share a pixel; side by side they do not.
 * [2] nothing in the way: the first candidate, so the look is unchanged.
 * [3] the spectrum in a half of split: the H1 cursor label's place above its
 *     line is on the window name ("Hann") of the header row, so it goes under
 *     the line.
 * [4] the harmonic tags of a 1 kHz square at a wide span, by peak rank: the
 *     strongest is placed, a tag on top of it is left out, one clear of it
 *     is placed.
 * [5] a candidate outside the plot is passed over; none left: -1.
 * [6] the cursor labels' places in that half (scope_cursor_draw.c): V1 under
 *     the source name and H1 on the peak (6 dB of headroom under the top,
 *     about 8 rows at 80 dB over 103) both miss the header row and take the
 *     clear row under it; a V line at the right edge keeps its label on the
 *     plot (label_x_within).
 */
#include <stdio.h>
#include "label_place.h"

static int fails = 0;
#define CHECK(cond, ...) do { if (cond) { printf("  PASS: "); } else { printf("  FAIL: "); fails++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

/* font_small: 12 rows (font_small.c); a fixed CW columns a character stands
 * in for its proportional widths (the rule only sees boxes). */
#define FH 12
#define CW 6
static label_box_t box(int x, int y, int chars)
{
    return (label_box_t){ (int16_t)x, (int16_t)y, (int16_t)(chars * CW), FH };
}

int main(void)
{
    /* A half of split: the pane from row 18, 103 rows, the full width. */
    const label_box_t pane = { 0, 18, 320, 103 };

    /* [1] */
    label_box_t a = box(10, 30, 2), b = box(10 + 2 * CW, 30, 2), c = box(10 + 2 * CW - 1, 30 + FH - 1, 2);
    CHECK(!label_boxes_meet(&a, &b) && !label_boxes_meet(&b, &a), "[1] side by side: they do not meet");
    CHECK(label_boxes_meet(&a, &c) && label_boxes_meet(&c, &a), "[1] one shared corner pixel: they meet");

    /* The header row: the source on the left, the window name on the right. */
    const label_box_t hdr[2] = { box(4, 20, 8), box(320 - 4 - 4 * CW, 20, 4) };

    /* [2] */
    const label_box_t far[2] = { box(320 - 1 - 2 * CW, 80 - 13, 2), box(320 - 1 - 2 * CW, 80 + 2, 2) };
    CHECK(label_place(&pane, far, 2, hdr, 2) == 0, "[2] H1 low in the plot: above its line, as before");

    /* [3] the line 8 rows under the header: above it is the header row */
    const int hy = 20 + FH + 8;
    const label_box_t h1[2] = { box(320 - 1 - 2 * CW, hy - 13, 2), box(320 - 1 - 2 * CW, hy + 2, 2) };
    CHECK(label_boxes_meet(&h1[0], &hdr[1]), "[3] (the place above meets \"Hann\")");
    CHECK(label_place(&pane, h1, 2, hdr, 2) == 1, "[3] H1 under the header: under its line");

    /* [4] tags by rank: Fund at x 100, H3 two columns right of it, H5 at 140 */
    label_box_t placed[5] = { hdr[0], hdr[1] };
    uint8_t np = 2;
    const label_box_t fund = box(100 - 8, 50, 4), h3 = box(102 - 8, 60, 2), h5 = box(140 - 8, 70, 2);
    const int f = label_place(&pane, &fund, 1, placed, np);
    if (f == 0) placed[np++] = fund;
    const int t3 = label_place(&pane, &h3, 1, placed, np);
    if (t3 == 0) placed[np++] = h3;
    const int t5 = label_place(&pane, &h5, 1, placed, np);
    CHECK(f == 0 && t3 == -1 && t5 == 0, "[4] Fund placed, H3 on it left out, H5 clear of it placed");

    /* [5] */
    const label_box_t top_line[2] = { box(300, 18 + 3 - 13, 2), box(300, 18 + 3 + 2, 2) };
    CHECK(label_place(&pane, top_line, 2, NULL, 0) == 1, "[5] above a line at the plot's top: off the plot, under it");
    const label_box_t off[1] = { box(316, 40, 2) };
    CHECK(label_place(&pane, off, 1, NULL, 0) == -1, "[5] past the right edge and nothing else: not placed");

    /* [6] the clear row: under the header row and one clear row */
    const int clear = 20 + FH + 1;
    const label_box_t v1[2] = { box(30 - 10, 18, 2), box(30 - 10, clear, 2) };
    CHECK(label_place(&pane, v1, 2, hdr, 2) == 1, "[6] V1 under \"LIVE CH1\": on the clear row");
    const int peak = 18 + 8;
    const label_box_t hp[3] = { box(320 - 1 - 2 * CW, peak - 13, 2), box(320 - 1 - 2 * CW, peak + 2, 2),
                                box(320 - 1 - 2 * CW, clear, 2) };
    CHECK(label_place(&pane, hp, 3, hdr, 2) == 2, "[6] H1 on the peak: off the top, on \"Hann\", so the clear row");
    const int16_t w2 = 2 * CW;
    CHECK(label_x_within(319 - 10, w2, &pane) == 320 - w2 && label_x_within(-3, w2, &pane) == 0 &&
          label_x_within(100, w2, &pane) == 100, "[6] a label at either edge is kept on the plot, one inside stays");
    const label_box_t edge[2] = { box(label_x_within(319 - 10, w2, &pane), 18, 2), box(0, 0, 0) };
    CHECK(label_place(&pane, edge, 1, hdr, 0) == 0, "[6] V at the right edge: placed");

    printf("test_label_place: %d failed\n", fails);
    return fails ? 1 : 0;
}
