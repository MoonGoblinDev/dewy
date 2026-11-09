// Platform face/person analysis behind the portable tracker core (tracker-core.cpp).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "face-tracker.h"

namespace beauty {

class TrackerBackend {
public:
	virtual ~TrackerBackend() = default;
	virtual const char *name() const = 0;

	/* Landmarks for the largest face in a BGRA frame. Points go into `out` in frame pixels
	 * (y down) using the ft_region layout; `full_detect` reports whether a (slow) face
	 * detection ran rather than cheap tracking from the previous frame. */
	virtual bool detect(const uint8_t *bgra, int w, int h, int stride, ft_face &out, bool &full_detect) = 0;

	/* Person segmentation mask (0..255) covering the whole frame, any resolution. */
	virtual bool segment(const uint8_t *bgra, int w, int h, int stride, std::vector<uint8_t> &mask, int &mw,
			     int &mh) = 0;
};

/* models_dir: folder holding the ONNX models (unused by the Vision backend). */
std::unique_ptr<TrackerBackend> create_vision_backend();
std::unique_ptr<TrackerBackend> create_onnx_backend(const std::string &models_dir);

} // namespace beauty
