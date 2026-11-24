// SkinSegmenter on Core ML (macOS): MediaPipe selfie multiclass, converted by
// tools/convert-coreml.py with the softmax and class mix inside the model, so the Neural Engine
// returns the four mask channels directly (~2.5 ms, ~0.6 ms CPU on an M4 Pro).

#include "segmenter.hpp"

#import <Accelerate/Accelerate.h>

#include <algorithm>
#include <cmath>

#include "coreml-common.hpp"
#include "tracker-util.hpp"

namespace beauty {

struct SkinSegmenter::Impl {
	CoreMLRunner net; /* 256x256 RGB -> mask [1,4,256,256]: skin, face skin, hair, person */
	std::vector<float> out;
};

static const int S = 256;

SkinSegmenter::SkinSegmenter(const std::string &dir) : impl(std::make_unique<Impl>())
{
	if (impl->net.open(dir + "/selfie_multiclass_256.mlmodelc", S, S))
		tracker_log("skin segmentation: %s", describe());
}

SkinSegmenter::~SkinSegmenter() = default;

bool SkinSegmenter::ready() const
{
	return (bool)impl->net;
}

const char *SkinSegmenter::describe() const
{
	return ready() ? "multiclass (Core ML)" : "unavailable";
}

bool SkinSegmenter::run(const uint8_t *bgra, int w, int h, int stride, std::vector<uint8_t> &rgba, int &mw, int &mh)
{
	if (!ready())
		return false;
	size_t ds;
	uint8_t *dst = impl->net.lock_input(ds);
	vImage_Buffer src_b = {(void *)bgra, (vImagePixelCount)h, (vImagePixelCount)w, (size_t)stride};
	vImage_Buffer dst_b = {dst, (vImagePixelCount)S, (vImagePixelCount)S, ds};
	vImage_Error e = vImageScale_ARGB8888(&src_b, &dst_b, nullptr, kvImageNoFlags);
	impl->net.unlock_input();
	if (e != kvImageNoError || !impl->net.run() || !impl->net.read("mask", impl->out) ||
	    impl->out.size() != (size_t)4 * S * S)
		return false;
	mw = mh = S;
	rgba.resize((size_t)S * S * 4);
	const float *o = impl->out.data();
	for (size_t i = 0; i < (size_t)S * S; i++)
		for (int c = 0; c < 4; c++)
			rgba[i * 4 + c] = (uint8_t)std::lround(std::clamp(o[(size_t)c * S * S + i], 0.0f, 1.0f) * 255.0f);
	return true;
}

} // namespace beauty
