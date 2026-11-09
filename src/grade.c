#include "grade.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

void grade_defaults(struct grade_params *g)
{
	memset(g, 0, sizeof(*g));
	g->gamma[0] = g->gamma[1] = g->gamma[2] = 1.0f;
	g->shadow_tint[0] = g->shadow_tint[1] = g->shadow_tint[2] = 0.5f;
	g->highlight_tint[0] = g->highlight_tint[1] = g->highlight_tint[2] = 0.5f;
	g->mono_tint[0] = g->mono_tint[1] = g->mono_tint[2] = 1.0f;
}

void grade_parse_hex(const char *hex, float out[3])
{
	if (!hex || hex[0] != '#' || strlen(hex) < 7)
		return;
	unsigned v = (unsigned)strtoul(hex + 1, NULL, 16);
	out[0] = ((v >> 16) & 0xff) / 255.0f;
	out[1] = ((v >> 8) & 0xff) / 255.0f;
	out[2] = (v & 0xff) / 255.0f;
}

static float luma(const float c[3])
{
	return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
}

void grade_apply(const struct grade_params *g, float c[3])
{
	/* exposure + white balance */
	float e = exp2f(g->exposure * 0.8f);
	c[0] *= e * (1.0f + g->warmth * 0.12f);
	c[1] *= e * (1.0f - g->tint * 0.07f);
	c[2] *= e * (1.0f - g->warmth * 0.12f);
	c[0] *= 1.0f + g->tint * 0.03f;
	c[2] *= 1.0f + g->tint * 0.03f;

	/* highlight roll-off (filmic shoulder) */
	for (int i = 0; i < 3; i++) {
		float x = c[i];
		if (g->rolloff > 0 && x > 0.7f) {
			float over = x - 0.7f;
			x = 0.7f + over / (1.0f + over * g->rolloff * 4.0f);
		}
		c[i] = clampf(x, 0.0f, 1.0f);
	}

	/* contrast: S-curve for positive values, flatten towards mid grey for negative */
	float k = g->contrast + g->curve;
	for (int i = 0; i < 3; i++) {
		float x = c[i];
		float s = x * x * (3.0f - 2.0f * x);
		c[i] = k >= 0 ? x + (s - x) * k : x + (0.5f - x) * (-k * 0.4f);
	}

	/* hue rotation (YIQ) */
	if (g->hue != 0.0f) {
		float y = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
		float i_ = 0.596f * c[0] - 0.274f * c[1] - 0.322f * c[2];
		float q = 0.211f * c[0] - 0.523f * c[1] + 0.312f * c[2];
		float a = g->hue * (float)M_PI / 180.0f, cs = cosf(a), sn = sinf(a);
		float i2 = i_ * cs - q * sn, q2 = i_ * sn + q * cs;
		c[0] = y + 0.956f * i2 + 0.621f * q2;
		c[1] = y - 0.272f * i2 - 0.647f * q2;
		c[2] = y - 1.106f * i2 + 1.703f * q2;
	}

	/* saturation + vibrance (boosts muted colours more, protects saturated ones) */
	float y = luma(c);
	float mx = fmaxf(c[0], fmaxf(c[1], c[2])), mn = fminf(c[0], fminf(c[1], c[2]));
	float sat_now = mx - mn;
	float f = (1.0f + g->saturation) * (1.0f + g->vibrance * (1.0f - clampf(sat_now * 2.0f, 0.0f, 1.0f)));
	for (int i = 0; i < 3; i++)
		c[i] = y + (c[i] - y) * f;

	/* split toning */
	y = clampf(luma(c), 0.0f, 1.0f);
	float ws = (1.0f - y) * (1.0f - y), wh = y * y;
	for (int i = 0; i < 3; i++)
		c[i] += (g->shadow_tint[i] - 0.5f) * g->shadow_amt * ws + (g->highlight_tint[i] - 0.5f) * g->highlight_amt * wh;

	/* per-channel gamma */
	for (int i = 0; i < 3; i++)
		c[i] = powf(clampf(c[i], 0.0f, 1.0f), 1.0f / fmaxf(g->gamma[i], 0.1f));

	if (g->mono) {
		float m = luma(c);
		for (int i = 0; i < 3; i++)
			c[i] = m * g->mono_tint[i] / fmaxf(luma(g->mono_tint), 0.05f) * 0.92f + m * 0.08f;
	}

	/* fade: lifted blacks, slightly lowered whites */
	for (int i = 0; i < 3; i++)
		c[i] = clampf(c[i] * (1.0f - g->fade * 0.2f) + g->fade * 0.11f, 0.0f, 1.0f);
}

void grade_build_lut(const struct grade_params *g, int n, uint8_t *rgba)
{
	for (int b = 0; b < n; b++)
		for (int gg = 0; gg < n; gg++)
			for (int r = 0; r < n; r++) {
				float c[3] = {(float)r / (n - 1), (float)gg / (n - 1), (float)b / (n - 1)};
				grade_apply(g, c);
				uint8_t *px = rgba + ((size_t)gg * n * n + (size_t)b * n + r) * 4;
				for (int i = 0; i < 3; i++)
					px[i] = (uint8_t)(clampf(c[i], 0.0f, 1.0f) * 255.0f + 0.5f);
				px[3] = 255;
			}
}
