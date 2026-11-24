// MediaPipe face tracking shared by the ONNX Runtime (Windows) and Core ML (macOS) backends:
// YuNet finds the face when it is (re)acquired, then Face Landmarker v2 runs every frame on a
// rolled square crop around the previous frame's points; upper lids are snapped to the lash line.
// Subclasses only run the two networks.
#pragma once

#include "backend.hpp"

namespace beauty {

struct Crop {
	float cx, cy, size, roll;
};

class MediaPipeBackend : public TrackerBackend {
public:
	bool detect(const uint8_t *bgra, int w, int h, int stride, ft_face &f, bool &full) override;
	bool segment(const uint8_t *, int, int, int, std::vector<uint8_t> &, int &, int &) override
	{
		return false; /* SkinSegmenter does this */
	}

protected:
	static constexpr int DET_SIZE = 640; /* YuNet input */
	static constexpr int LMK_SIZE = 256; /* Face Landmarker input */

	/* YuNet on the frame scaled by sc into the top-left of a black DET_SIZE^2 square, BGR 0..255.
	 * heads[k * 3 + i]: k = cls, obj, bbox, kps; i = stride 8, 16, 32. Valid until the next call. */
	virtual bool run_detector(const uint8_t *bgra, int w, int h, int stride, float sc, const float *heads[12]) = 0;

	/* Face Landmarker on the LMK_SIZE^2 crop c, RGB 0..1 (see crop_to_image): 478 x (x, y, z) in
	 * crop pixels and the face presence logit. pts is valid until the next call. */
	virtual bool run_landmarks(const uint8_t *bgra, int w, int h, int stride, const Crop &c, const float *&pts,
				   float &presence_logit) = 0;

	/* crop position (u, v), in crop pixels, -> frame position:
	 * x = m[0] + m[1] u + m[2] v, y = m[3] + m[4] u + m[5] v */
	static void crop_affine(const Crop &c, float m[6]);
	static void crop_to_image(const Crop &c, float u, float v, float &x, float &y);

	/* DEWY_PROFILE=1: logs mean preprocess / inference time every 300 landmark runs */
	void prof(double pre_ms, double run_ms);

	bool ready = false;

private:
	bool detect_face(const uint8_t *bgra, int w, int h, int stride, Crop &crop);
	bool landmarks(const uint8_t *bgra, int w, int h, int stride, const Crop &c, float &presence);
	Crop crop_from_points() const;
	void snap_lids(const uint8_t *bgra, int w, int h, int stride);

	bool have_prev = false;
	float mesh[478 * 2] = {};
	double p_pre = 0, p_run = 0, p_max = 0;
	int p_n = 0;
};

} // namespace beauty
