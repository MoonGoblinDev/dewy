#pragma once

/* Colour-grade model shared by the GPU filter (baked into a 3D LUT) and the dock (CPU
 * thumbnails). All amounts are roughly -1..1. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct grade_params {
	float exposure, contrast, curve, saturation, vibrance;
	float warmth, tint, hue; /* hue in degrees */
	float fade, rolloff;
	float gamma[3]; /* per-channel gamma (1 = neutral) */
	float shadow_tint[3], shadow_amt;
	float highlight_tint[3], highlight_amt;
	bool mono;
	float mono_tint[3];
};

void grade_defaults(struct grade_params *g);
void grade_apply(const struct grade_params *g, float rgb[3]); /* in place, 0..1 */

/* n*n*n entries, laid out as an (n*n) x n RGBA8 strip: x = r + b*n, y = g */
void grade_build_lut(const struct grade_params *g, int n, uint8_t *rgba);

/* "#rrggbb" -> 0..1 */
void grade_parse_hex(const char *hex, float out[3]);

#ifdef __cplusplus
}
#endif
