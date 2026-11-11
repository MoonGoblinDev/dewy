// ONNX Runtime backend (Windows; also buildable on macOS for testing).
//
// Models (data/models, all Apache-2.0 / MIT):
//   face_detection_yunet.onnx       OpenCV YuNet (2023) - only when the face is (re)acquired
//   face_landmarks_478.onnx         Google MediaPipe Face Landmarker v2 (468 mesh + 10 iris)
//   selfie_multiclass_256.onnx      MediaPipe selfie multiclass (hair / face skin / body skin /
//                                   clothes) - person mask, used with a GPU execution provider
//   selfie_segmentation_landscape.onnx  small 2-class fallback for CPU-only machines
//
// ONNX Runtime is loaded at runtime from an explicit path (ORT_API_MANUAL_INIT) so it can't
// clash with another plugin's onnxruntime.dll of a different version. On Windows we use the
// DirectML execution provider (any DX12 GPU), with CPU as fallback.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "backend.hpp"
#include "onnx-common.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace beauty {
namespace {

/* ---------- MediaPipe mesh -> ft_region layout ---------- */

const std::vector<int> &mesh_region(int r)
{
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

struct Crop {
	float cx, cy, size, roll;
};

class OnnxBackend : public TrackerBackend {
public:
	explicit OnnxBackend(const std::string &dir)
	{
		try {
			det = ort_model(dir + "/face_detection_yunet.onnx", false);
			lmk = ort_model(dir + "/face_landmarks_478.onnx", true);
			ready = det && lmk;
			ort_log(ready ? "face landmarks: MediaPipe Face Landmarker v2 (%s)" : "face models failed to load",
				lmk && lmk->accelerated ? ort_accel_name() : "CPU");
		} catch (const Ort::Exception &e) {
			ort_log("ONNX Runtime error: %s", e.what());
		}
	}
	const char *name() const override { return lmk && lmk->accelerated ? "MediaPipe (GPU)" : "MediaPipe (CPU)"; }

	bool detect(const uint8_t *bgra, int w, int h, int stride, ft_face &f, bool &full) override
	{
		full = false;
		if (!ready)
			return false;
		try {
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
		} catch (const Ort::Exception &e) {
			ort_log("landmarks failed: %s", e.what());
			have_prev = false;
			return false;
		}
	}

	bool segment(const uint8_t *, int, int, int, std::vector<uint8_t> &, int &, int &) override
	{
		return false; /* SkinSegmenter (segmenter.cpp) does this on every platform */
	}

private:
	/* YuNet: letterbox into 640x640, BGR 0..255 NCHW; per-stride score/box/keypoint heads. */
	bool detect_face(const uint8_t *bgra, int w, int h, int stride, Crop &crop)
	{
		const int S = 640;
		float sc = (float)S / std::max(w, h);
		std::vector<float> in((size_t)3 * S * S, 0.0f);
		int nw = (int)(w * sc), nh = (int)(h * sc);
		float px[3];
		for (int y = 0; y < nh; y++)
			for (int x = 0; x < nw; x++) {
				sample_bgra(bgra, w, h, stride, (x + 0.5f) / sc, (y + 0.5f) / sc, px);
				for (int c = 0; c < 3; c++)
					in[(size_t)c * S * S + (size_t)y * S + x] = px[c];
			}
		Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
		int64_t shape[4] = {1, 3, S, S};
		Ort::Value t = Ort::Value::CreateTensor<float>(mem, in.data(), in.size(), shape, 4);
		auto out = det->run(t);
		auto find = [&](const std::string &n) -> const float * {
			for (size_t i = 0; i < det->out_names.size(); i++)
				if (det->out_names[i] == n)
					return out[i].GetTensorData<float>();
			return nullptr;
		};
		float best = 0;
		for (int s : {8, 16, 32}) {
			const float *cls = find("cls_" + std::to_string(s)), *obj = find("obj_" + std::to_string(s));
			const float *bb = find("bbox_" + std::to_string(s)), *kp = find("kps_" + std::to_string(s));
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

	Crop crop_from_points() const
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

	/* Face Landmarker v2 on a rolled square crop: [1,256,256,3] RGB 0..1 -> 478 x (x,y,z) crop px. */
	bool landmarks(const uint8_t *bgra, int w, int h, int stride, const Crop &c, float &presence)
	{
		const int S = 256;
		float cs = std::cos(c.roll), sn = std::sin(c.roll), k = c.size / S;
		/* crop (u,v) -> image: centre + R(roll) * ((u,v) - 128) * k */
		auto t0 = std::chrono::steady_clock::now();
		std::vector<float> in((size_t)S * S * 3);
		float px[3];
		for (int v = 0; v < S; v++)
			for (int u = 0; u < S; u++) {
				float du = (u + 0.5f - S / 2.0f) * k, dv = (v + 0.5f - S / 2.0f) * k;
				float x = c.cx + du * cs - dv * sn, y = c.cy + du * sn + dv * cs;
				sample_bgra(bgra, w, h, stride, x, y, px);
				float *o = &in[((size_t)v * S + u) * 3];
				o[0] = px[2] / 255.f;
				o[1] = px[1] / 255.f;
				o[2] = px[0] / 255.f;
			}
		Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
		int64_t shape[4] = {1, S, S, 3};
		Ort::Value t = Ort::Value::CreateTensor<float>(mem, in.data(), in.size(), shape, 4);
		auto t1 = std::chrono::steady_clock::now();
		auto out = lmk->run(t);
		auto t2 = std::chrono::steady_clock::now();
		prof(std::chrono::duration<double, std::milli>(t1 - t0).count(),
		     std::chrono::duration<double, std::milli>(t2 - t1).count());
		const float *pts = out[0].GetTensorData<float>();
		presence = 1.0f / (1.0f + std::exp(-out[1].GetTensorData<float>()[0]));
		for (int i = 0; i < 478; i++) {
			float du = (pts[i * 3] - S / 2.0f) * k, dv = (pts[i * 3 + 1] - S / 2.0f) * k;
			mesh[i * 2] = c.cx + du * cs - dv * sn;
			mesh[i * 2 + 1] = c.cy + du * sn + dv * cs;
		}
		return true;
	}

	void prof(double pre, double run)
	{
		static const bool on = getenv("DEWY_PROFILE") != nullptr;
		if (!on)
			return;
		p_pre += pre;
		p_run += run;
		p_max = std::max(p_max, run);
		if (++p_n == 300) {
			char b[160];
			snprintf(b, sizeof(b), "landmarks profile: preprocess %.2f ms, Run %.2f ms (max %.2f) over 300 calls",
				 p_pre / p_n, p_run / p_n, p_max);
			ort_log("%s", b);
			p_pre = p_run = p_max = 0;
			p_n = 0;
		}
	}
	double p_pre = 0, p_run = 0, p_max = 0;
	int p_n = 0;

	OrtModelPtr det, lmk;
	bool ready = false, have_prev = false;
	float mesh[478 * 2] = {};
};

} // namespace

std::unique_ptr<TrackerBackend> create_onnx_backend(const std::string &models_dir)
{
	return std::make_unique<OnnxBackend>(models_dir);
}

} // namespace beauty
