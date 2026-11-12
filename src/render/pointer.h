/*
 * kmscon - Arrow pointer shape
 *
 * Shared by the hardware cursor and the software pointer. The arrow is a
 * polygon in a 10 x 16 unit box with its tip, the hotspot, at (0, 0).
 */

#ifndef KMSCON_RENDER_POINTER_H
#define KMSCON_RENDER_POINTER_H

#include <stdbool.h>

#define POINTER_ARROW_W 10.0
#define POINTER_ARROW_H 16.0

static inline bool pointer_arrow_hit(double u, double v)
{
	static const double px[] = {0.0, 0.0, 3.2, 5.6, 7.6, 5.3, 9.6};
	static const double py[] = {0.0, 13.5, 10.6, 15.6, 14.7, 9.9, 9.9};
	const int n = sizeof(px) / sizeof(*px);
	bool in = false;
	int i, j;

	for (i = 0, j = n - 1; i < n; j = i++) {
		if ((py[i] > v) != (py[j] > v) &&
		    u < (px[j] - px[i]) * (v - py[i]) / (py[j] - py[i]) + px[i])
			in = !in;
	}
	return in;
}

/*
 * Fill a w x h mask with the arrow scaled by @scale, tip at pixel (ox, oy).
 * The tip pixel is always set so the hotspot is visible at any size.
 */
static inline void pointer_arrow_mask(bool *mask, unsigned int w, unsigned int h, unsigned int ox,
				      unsigned int oy, double scale)
{
	unsigned int x, y;
	double u, v;

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			u = ((double)x - ox + 0.5) / scale;
			v = ((double)y - oy + 0.5) / scale;
			mask[y * w + x] = (x == ox && y == oy) || pointer_arrow_hit(u, v);
		}
	}
}

#endif /* KMSCON_RENDER_POINTER_H */
