/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * The Domdly logo as geometry: a rounded square with a blue gradient and a
 * white "D". Shared by mkicon (application icon) and
 * mkcover (repository cover image).
 */
#ifndef DOMDLY_ICON_ART_H
#define DOMDLY_ICON_ART_H

#include <math.h>

/* Gradient from the top to the bottom of the square (0..1 per channel). */
static const double icon_top[3] = {0.20, 0.47, 0.96};
static const double icon_bottom[3] = {0.11, 0.30, 0.78};
static const double icon_radius = 0.22; /* corner radius, unit square */

/* Signed distance to a rounded rectangle centred at (cx,cy). */
static double sd_round_rect(double x, double y, double cx, double cy, double hw, double hh, double r)
{
    double qx = fabs(x - cx) - (hw - r);
    double qy = fabs(y - cy) - (hh - r);
    double ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    double outside = sqrt(ox * ox + oy * oy);
    double inside = (qx > qy ? qx : qy);
    if (inside > 0) inside = 0;
    return outside + inside - r;
}

/* Inside a "D": a straight stem joined to half an ellipse. (l, xc) is the
 * stem, (xc, cy) the centre of the bowl with radii rx, ry. */
static int in_d_shape(double x, double y, double l, double xc, double cy, double rx, double ry)
{
    if (y < cy - ry || y > cy + ry || x < l) return 0;
    if (x <= xc) return 1;
    double dx = (x - xc) / rx, dy = (y - cy) / ry;
    return dx * dx + dy * dy <= 1.0;
}

/* Coverage of the white "D" mark at unit coordinates (x,y), y grows
 * downwards: the outline of a bold D, drawn from geometry (no font). */
static int in_mark(double x, double y)
{
    const double stroke = 0.115;
    const double left = 0.29, xc = 0.47, cy = 0.5, rx = 0.25, ry = 0.28;
    return in_d_shape(x, y, left, xc, cy, rx, ry) &&
           !in_d_shape(x, y, left + stroke, xc, cy, rx - stroke, ry - stroke);
}

#endif
