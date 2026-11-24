#include "mediapipe.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "tracker-util.hpp"

namespace beauty {
namespace {

/* ---------- MediaPipe mesh -> ft_region layout ---------- */

const std::vector<int> &mesh_region(int r)
{
	if (r == FT_MESH) { /* the whole mesh, in mesh order */
		static const std::vector<int> all = [] {
			std::vector<int> v(FT_MESH_POINTS);
			for (int i = 0; i < FT_MESH_POINTS; i++)
				v[i] = i;
			return v;
		}();
		return all;
	}
	static const std::vector<int> tables[FT_REGION_COUNT] = {
		/* FT_CONTOUR (jaw line, temple to temple like Vision) */
		{356, 454, 323, 361, 288, 397, 365, 379, 378, 400, 377, 152, 148, 176, 149, 150, 136, 172, 58, 132, 93, 234,
		 127},
		/* FT_LEFT_EYE / FT_RIGHT_EYE (subject's left = image right) */
		{263, 249, 390, 373, 374, 380, 381, 382, 362, 398, 384, 385, 386, 387, 388, 466},
		{33, 7, 163, 144, 145, 153, 154, 155, 133, 173, 157, 158, 159, 160, 161, 246},
		/* brows */
		{276, 283, 282, 295, 285, 300, 293, 334, 296, 336},
		{46, 53, 52, 65, 55, 70, 63, 105, 66, 107},
		/* FT_NOSE (wings + tip + lower bridge), FT_NOSE_CREST, FT_MEDIAN */
		{98, 327, 129, 358, 64, 294, 2, 94, 19, 1, 4, 5, 195},
		{168, 6, 197, 195, 5, 4},
		{10, 151, 9, 8, 168, 6, 197, 195, 5, 4, 1, 19, 94, 2, 164, 0, 11, 12, 13, 14, 15, 16, 17, 18, 200, 199, 175, 152},
		/* lips (outer, inner incl. mouth corners 78 / 308) */
		{61, 146, 91, 181, 84, 17, 314, 405, 321, 375, 291, 409, 270, 269, 267, 0, 37, 39, 40, 185},
		{78, 95, 88, 178, 87, 14, 317, 402, 318, 324, 308, 415, 310, 311, 312, 13, 82, 81, 80, 191},
		/* iris centres */
		{473},
		{468},
	};
	return tables[r];
}

} // namespace

bool MediaPipeBackend::detect(const uint8_t *bgra, int w, int h, int stride, ft_face &f, bool &full)
{
	full = false;
	if (!ready)
		return false;
	Crop crop;
	if (!have_prev) {
		full = true;
		if (!detect_face(bgra, w, h, stride, crop))
			return false;
		float pres = 0;
		if (!landmarks(bgra, w, h, stride, crop, pres) || pres < 0.5f)
			return false;
		crop = crop_from_points(); /* refine once from its own points */
	} else {
		crop = crop_from_points();
	}
	float pres = 0;
	if (!landmarks(bgra, w, h, stride, crop, pres) || pres < 0.5f) {
		have_prev = false;
		return false;
	}
	have_prev = true;
	snap_lids(bgra, w, h, stride);
	memset(&f, 0, sizeof(f));
	for (int r = 0; r < FT_REGION_COUNT; r++) {
		f.region_start[r] = f.num_points;
		for (int idx : mesh_region(r)) {
			if (f.num_points >= FT_MAX_POINTS)
				break;
			f.pts[f.num_points++] = {mesh[idx * 2], mesh[idx * 2 + 1]};
		}
		f.region_count[r] = f.num_points - f.region_start[r];
	}
	return true;
}

/* YuNet: best face over the per-stride score / box / keypoint heads. */
bool MediaPipeBackend::detect_face(const uint8_t *bgra, int w, int h, int stride, Crop &crop)
{
	const int S = DET_SIZE;
	float sc = (float)S / std::max(w, h);
	const float *heads[12] = {};
	if (!run_detector(bgra, w, h, stride, sc, heads))
		return false;
	float best = 0;
	for (int si = 0; si < 3; si++) {
		int s = 8 << si;
		const float *cls = heads[si], *obj = heads[3 + si], *bb = heads[6 + si], *kp = heads[9 + si];
		if (!cls || !obj || !bb || !kp)
			continue;
		int cols = S / s, n = cols * cols;
		for (int i = 0; i < n; i++) {
			float score = std::sqrt(std::clamp(cls[i], 0.f, 1.f) * std::clamp(obj[i], 0.f, 1.f));
			if (score <= best)
				continue;
			best = score;
			int r = i / cols, c = i % cols;
			float bw = std::exp(bb[i * 4 + 2]) * s, bh = std::exp(bb[i * 4 + 3]) * s;
			crop.cx = (c + bb[i * 4]) * s / sc;
			crop.cy = (r + bb[i * 4 + 1]) * s / sc;
			crop.size = std::max(bw, bh) / sc * 1.5f;
			float ex1 = (c + kp[i * 10]) * s, ey1 = (r + kp[i * 10 + 1]) * s;
			float ex2 = (c + kp[i * 10 + 2]) * s, ey2 = (r + kp[i * 10 + 3]) * s;
			crop.roll = std::atan2(ey2 - ey1, ex2 - ex1);
		}
	}
	return best > 0.6f;
}

Crop MediaPipeBackend::crop_from_points() const
{
	float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
	for (int i = 0; i < 468; i++) {
		x0 = std::min(x0, mesh[i * 2]);
		x1 = std::max(x1, mesh[i * 2]);
		y0 = std::min(y0, mesh[i * 2 + 1]);
		y1 = std::max(y1, mesh[i * 2 + 1]);
	}
	Crop c;
	c.cx = (x0 + x1) * 0.5f;
	c.cy = (y0 + y1) * 0.5f;
	c.size = std::max(x1 - x0, y1 - y0) * 1.5f;
	c.roll = std::atan2(mesh[263 * 2 + 1] - mesh[33 * 2 + 1], mesh[263 * 2] - mesh[33 * 2]);
	return c;
}

void MediaPipeBackend::crop_affine(const Crop &c, float m[6])
{
	/* centre + R(roll) * ((u, v) - S/2) * size/S */
	float k = c.size / LMK_SIZE, cs = std::cos(c.roll) * k, sn = std::sin(c.roll) * k, h = LMK_SIZE / 2.0f;
	m[0] = c.cx - h * (cs - sn);
	m[1] = cs;
	m[2] = -sn;
	m[3] = c.cy - h * (sn + cs);
	m[4] = sn;
	m[5] = cs;
}

void MediaPipeBackend::crop_to_image(const Crop &c, float u, float v, float &x, float &y)
{
	float m[6];
	crop_affine(c, m);
	x = m[0] + m[1] * u + m[2] * v;
	y = m[3] + m[4] * u + m[5] * v;
}

bool MediaPipeBackend::landmarks(const uint8_t *bgra, int w, int h, int stride, const Crop &c, float &presence)
{
	const float *pts = nullptr;
	float logit = 0;
	if (!run_landmarks(bgra, w, h, stride, c, pts, logit) || !pts)
		return false;
	presence = 1.0f / (1.0f + std::exp(-logit));
	float m[6];
	crop_affine(c, m);
	for (int i = 0; i < 478; i++) {
		float u = pts[i * 3], v = pts[i * 3 + 1];
		mesh[i * 2] = m[0] + m[1] * u + m[2] * v;
		mesh[i * 2 + 1] = m[3] + m[4] * u + m[5] * v;
	}
	return true;
}

/* The mesh's upper-lid points often sit a little inside the eye, and eyeliner drawn on them
 * then covers the top of the iris. Slide each one along the lid's normal to the lash line in
 * the picture: the strongest step from bright lid skin (above) to the dark lashes (below),
 * searched within a few percent of the eye distance. The corners stay where they are. */
void MediaPipeBackend::snap_lids(const uint8_t *bgra, int w, int h, int stride)
{
	static const int lids[2][9] = {{133, 173, 157, 158, 159, 160, 161, 246, 33},
				       {362, 398, 384, 385, 386, 387, 388, 466, 263}};
	static const int brow[2] = {105, 334};
	auto P = [&](int i) { return std::make_pair(mesh[i * 2], mesh[i * 2 + 1]); };
	auto luma = [&](float x, float y) {
		float px[3];
		sample_bgra(bgra, w, h, stride, x, y, px);
		return 0.114f * px[0] + 0.587f * px[1] + 0.299f * px[2];
	};
	float d = std::hypot(mesh[263 * 2] - mesh[33 * 2], mesh[263 * 2 + 1] - mesh[33 * 2 + 1]) / 1.45f; /* ~IOD */
	if (d < 12.0f)
		return; /* face too small to see a lash line */
	float step = std::max(1.0f, 0.012f * d), half = std::max(1.0f, 0.01f * d);
	for (int e = 0; e < 2; e++) {
		auto a = P(lids[e][0]), b = P(lids[e][8]);
		float tx = b.first - a.first, ty = b.second - a.second, tl = std::hypot(tx, ty);
		if (tl < 4.0f)
			continue;
		tx /= tl;
		ty /= tl;
		float nx = -ty, ny = tx; /* normal; make it point up, towards the brow */
		auto br = P(brow[e]);
		if (nx * (br.first - a.first) + ny * (br.second - a.second) < 0) {
			nx = -nx;
			ny = -ny;
		}
		float off[9] = {}, wgt[9] = {};
		for (int k = 1; k < 8; k++) {
			auto p = P(lids[e][k]);
			float best = 0, best_g = 0;
			for (float s = -0.05f * d; s <= 0.035f * d; s += 0.25f) {
				float g = 0;
				for (int j = -1; j <= 1; j++) {
					float x = p.first + tx * j * half, y = p.second + ty * j * half;
					g += luma(x + nx * (s + step), y + ny * (s + step)) -
					     luma(x + nx * (s - step), y + ny * (s - step));
				}
				g = g / 3.0f - 0.25f * std::fabs(s); /* prefer the closest strong edge */
				if (g > best_g) {
					best_g = g;
					best = s;
				}
			}
			off[k] = best;
			wgt[k] = best_g > 10.0f ? best_g : 0.0f;
		}
		/* one smooth correction along the lid (a parabola through the corners' zero offset,
		 * fitted to the clear edges), so single points can't kink the line */
		double sxx = 0, sxy = 0;
		for (int k = 1; k < 8; k++) {
			double u = k / 8.0, basis = u * (1.0 - u) * 4.0; /* 0 at both corners, 1 mid-lid */
			sxx += wgt[k] * basis * basis;
			sxy += wgt[k] * basis * off[k];
		}
		if (sxx < 30.0) /* too few clear edges: leave the lid as tracked */
			continue;
		double amp = sxy / sxx;
		double sw = 0, sr = 0; /* plus a straight tilt term for the remaining residual */
		for (int k = 1; k < 8; k++) {
			double u = k / 8.0;
			sw += wgt[k] * (u - 0.5) * (u - 0.5);
			sr += wgt[k] * (u - 0.5) * (off[k] - amp * u * (1.0 - u) * 4.0);
		}
		double tilt = sw > 1e-6 ? sr / sw : 0.0;
		for (int k = 1; k < 8; k++) {
			double u = k / 8.0;
			float o = (float)(amp * u * (1.0 - u) * 4.0 + tilt * (u - 0.5) * u * (1.0 - u) * 4.0);
			mesh[lids[e][k] * 2] += nx * o;
			mesh[lids[e][k] * 2 + 1] += ny * o;
		}
	}
}

void MediaPipeBackend::prof(double pre, double run)
{
	static const bool on = getenv("DEWY_PROFILE") != nullptr;
	if (!on)
		return;
	p_pre += pre;
	p_run += run;
	p_max = std::max(p_max, run);
	if (++p_n == 300) {
		tracker_log("landmarks profile: preprocess %.2f ms, inference %.2f ms (max %.2f) over 300 calls",
			    p_pre / p_n, p_run / p_n, p_max);
		p_pre = p_run = p_max = 0;
		p_n = 0;
	}
}

} // namespace beauty
