/*
 * OpenScope 2C53T - the pane drawer of each view that can be a half of split
 *
 * One line per view: its view.mode value and the name of its drawer in
 * scope_split.c. scope_split.c builds its drawer table from this list, and
 * the host test of the view table checks that every view the table lets be
 * a half (view_desc_t.half) has a line here, and no other view: so the
 * table cannot offer a half split has no drawer for.
 *
 * Pure: names only, no drawing code (host tests include it).
 */
#ifndef SPLIT_HALVES_H
#define SPLIT_HALVES_H

#define SPLIT_HALF_DRAWERS(X) \
    X(VIEW_MODE_YT,  half_yt)  \
    X(VIEW_MODE_XY,  half_xy)  \
    X(VIEW_MODE_FFT, half_fft)

#endif /* SPLIT_HALVES_H */
