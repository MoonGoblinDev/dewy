// Small helpers shared by every tracker backend (ONNX Runtime, Core ML, Vision).
#pragma once

#include <cstddef>
#include <cstdint>

namespace beauty {

/* printf-style line in the OBS log ("[dewy] ..."), or stderr outside OBS. */
void tracker_log(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 1, 2)))
#endif
	;

/* Bilinear BGRA sample (pixel-centre coordinates), returns B, G, R in 0..255. */
inline void sample_bgra(const uint8_t *bgra, int w, int h, int stride, float x, float y, float out[3])
{
	x = x - 0.5f;
	y = y - 0.5f;
	x = x < 0 ? 0 : (x > w - 1.001f ? w - 1.001f : x);
	y = y < 0 ? 0 : (y > h - 1.001f ? h - 1.001f : y);
	int x0 = (int)x, y0 = (int)y;
	float fx = x - x0, fy = y - y0;
	const uint8_t *p00 = bgra + (size_t)y0 * stride + x0 * 4, *p01 = p00 + 4, *p10 = p00 + stride, *p11 = p10 + 4;
	for (int c = 0; c < 3; c++) {
		float a = p00[c] + (p01[c] - p00[c]) * fx;
		float b = p10[c] + (p11[c] - p10[c]) * fx;
		out[c] = a + (b - a) * fy;
	}
}

} // namespace beauty
