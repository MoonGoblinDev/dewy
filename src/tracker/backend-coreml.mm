// Core ML backend (macOS): the MediaPipe tracking of mediapipe.cpp with its two networks run by
// Core ML directly, almost entirely on the Neural Engine (~0.5 ms landmarks, ~1 ms YuNet on an
// M4 Pro, about a tenth of a millisecond of CPU each).
//
// Models: data/models/coreml/*.mlpackage, converted from the ONNX files by
// tools/convert-coreml.py and compiled into the plugin as .mlmodelc at build time.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "coreml-common.hpp"
#include "mediapipe.hpp"
#include "tracker-util.hpp"

namespace beauty {
namespace {

static inline uint8_t u8(float v)
{
	return (uint8_t)(v + 0.5f);
}

class CoreMLBackend : public MediaPipeBackend {
public:
	explicit CoreMLBackend(const std::string &dir)
	{
		bool d = det.open(dir + "/face_detection_yunet.mlmodelc", DET_SIZE, DET_SIZE);
		bool l = lmk.open(dir + "/face_landmarks_478.mlmodelc", LMK_SIZE, LMK_SIZE);
		ready = d && l;
		tracker_log(ready ? "face landmarks: MediaPipe Face Landmarker v2 (Core ML)" : "face models failed to load");
	}
	const char *name() const override { return "MediaPipe (Core ML)"; }

protected:
	/* frame scaled into the top-left of a black 640x640 square */
	bool run_detector(const uint8_t *bgra, int w, int h, int stride, float sc, const float *heads[12]) override
	{
		const int S = DET_SIZE;
		int nw = std::min(S, (int)(w * sc)), nh = std::min(S, (int)(h * sc));
		size_t ds;
		uint8_t *dst = det.lock_input(ds);
		float px[3];
		for (int y = 0; y < S; y++) {
			uint8_t *o = dst + (size_t)y * ds;
			if (y >= nh) {
				memset(o, 0, (size_t)S * 4);
				continue;
			}
			for (int x = 0; x < nw; x++, o += 4) {
				sample_bgra(bgra, w, h, stride, (x + 0.5f) / sc, (y + 0.5f) / sc, px);
				o[0] = u8(px[0]);
				o[1] = u8(px[1]);
				o[2] = u8(px[2]);
				o[3] = 255;
			}
			memset(o, 0, (size_t)(S - nw) * 4);
		}
		det.unlock_input();
		if (!det.run())
			return false;
		static const char *names[12] = {"cls_8",  "cls_16",  "cls_32",  "obj_8", "obj_16", "obj_32",
						"bbox_8", "bbox_16", "bbox_32", "kps_8", "kps_16", "kps_32"};
		for (int k = 0; k < 12; k++)
			heads[k] = det.read(names[k], det_out[k]) ? det_out[k].data() : nullptr;
		return true;
	}

	bool run_landmarks(const uint8_t *bgra, int w, int h, int stride, const Crop &c, const float *&pts,
			   float &presence_logit) override
	{
		const int S = LMK_SIZE;
		auto t0 = std::chrono::steady_clock::now();
		float px[3], m[6];
		crop_affine(c, m);
		size_t ds;
		uint8_t *dst = lmk.lock_input(ds);
		for (int v = 0; v < S; v++) {
			uint8_t *o = dst + (size_t)v * ds;
			for (int u = 0; u < S; u++, o += 4) {
				float x = m[0] + m[1] * (u + 0.5f) + m[2] * (v + 0.5f);
				float y = m[3] + m[4] * (u + 0.5f) + m[5] * (v + 0.5f);
				sample_bgra(bgra, w, h, stride, x, y, px);
				o[0] = u8(px[0]);
				o[1] = u8(px[1]);
				o[2] = u8(px[2]);
				o[3] = 255;
			}
		}
		lmk.unlock_input();
		auto t1 = std::chrono::steady_clock::now();
		bool ok = lmk.run() && lmk.read("landmarks", lmk_pts) && lmk.read("presence", lmk_pres) &&
			  lmk_pts.size() >= 478 * 3 && !lmk_pres.empty();
		auto t2 = std::chrono::steady_clock::now();
		prof(std::chrono::duration<double, std::milli>(t1 - t0).count(),
		     std::chrono::duration<double, std::milli>(t2 - t1).count());
		if (!ok)
			return false;
		pts = lmk_pts.data();
		presence_logit = lmk_pres[0];
		return true;
	}

private:
	CoreMLRunner det, lmk;
	std::vector<float> det_out[12], lmk_pts, lmk_pres;
};

} // namespace

std::unique_ptr<TrackerBackend> create_coreml_backend(const std::string &models_dir)
{
	return std::make_unique<CoreMLBackend>(models_dir);
}

} // namespace beauty
