#include "segmenter.hpp"

#include <algorithm>
#include <cmath>

#include "onnx-common.hpp"

namespace beauty {

struct SkinSegmenter::Impl {
	OrtModel multiclass; /* [1,256,256,3] RGB 0..1 -> logits [1,256,256,6] */
	OrtModel person;     /* fallback: [1,3,144,256] -> alpha [1,1,144,256] (person only) */
	std::vector<float> input;
};

SkinSegmenter::SkinSegmenter(const std::string &dir) : impl(std::make_unique<Impl>())
{
	try {
		impl->multiclass = ort_model(dir + "/selfie_multiclass_256.onnx", true);
		if (!impl->multiclass)
			impl->person = ort_model(dir + "/selfie_segmentation_landscape.onnx", true);
		if (ready())
			ort_log("skin segmentation: %s", describe());
	} catch (const Ort::Exception &e) {
		ort_log("segmentation model failed: %s", e.what());
	}
}

SkinSegmenter::~SkinSegmenter() = default;

bool SkinSegmenter::ready() const
{
	return (bool)impl->multiclass || (bool)impl->person;
}

const char *SkinSegmenter::describe() const
{
	if (impl->multiclass)
		return impl->multiclass.accelerated ? "multiclass (GPU/ANE)" : "multiclass (CPU)";
	return impl->person ? "person only (fallback)" : "unavailable";
}

bool SkinSegmenter::run(const uint8_t *bgra, int w, int h, int stride, std::vector<uint8_t> &rgba, int &mw, int &mh)
{
	if (!ready())
		return false;
	try {
		Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
		float px[3];
		if (impl->multiclass) {
			const int S = 256;
			impl->input.resize((size_t)S * S * 3);
			/* area-ish downscale: average 4 bilinear taps per output pixel */
			for (int y = 0; y < S; y++)
				for (int x = 0; x < S; x++) {
					float acc[3] = {0, 0, 0};
					for (int k = 0; k < 4; k++) {
						float ox = (k & 1) ? 0.75f : 0.25f, oy = (k & 2) ? 0.75f : 0.25f;
						sample_bgra(bgra, w, h, stride, (x + ox) * w / S, (y + oy) * h / S, px);
						for (int c = 0; c < 3; c++)
							acc[c] += px[c];
					}
					float *o = &impl->input[((size_t)y * S + x) * 3];
					o[0] = acc[2] / (4 * 255.f);
					o[1] = acc[1] / (4 * 255.f);
					o[2] = acc[0] / (4 * 255.f);
				}
			int64_t shape[4] = {1, S, S, 3};
			Ort::Value t = Ort::Value::CreateTensor<float>(mem, impl->input.data(), impl->input.size(), shape, 4);
			auto out = impl->multiclass.session.Run(Ort::RunOptions{nullptr}, impl->multiclass.in.data(), &t, 1,
								 impl->multiclass.out.data(), 1);
			const float *lg = out[0].GetTensorData<float>();
			mw = mh = S;
			rgba.resize((size_t)S * S * 4);
			for (size_t i = 0; i < (size_t)S * S; i++) {
				const float *l = lg + i * 6;
				float mx = *std::max_element(l, l + 6), e[6], sum = 0;
				for (int c = 0; c < 6; c++) {
					e[c] = std::exp(l[c] - mx);
					sum += e[c];
				}
				/* 0 background, 1 hair, 2 body skin, 3 face skin, 4 clothes, 5 accessories */
				auto q = [](float v) { return (uint8_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
				rgba[i * 4 + 0] = q((e[2] + e[3]) / sum);
				rgba[i * 4 + 1] = q(e[3] / sum);
				rgba[i * 4 + 2] = q(e[1] / sum);
				rgba[i * 4 + 3] = q(1.0f - e[0] / sum);
			}
			return true;
		}
		const int W = 256, H = 144;
		impl->input.resize((size_t)W * H * 3);
		for (int y = 0; y < H; y++)
			for (int x = 0; x < W; x++) {
				sample_bgra(bgra, w, h, stride, (x + 0.5f) * w / W, (y + 0.5f) * h / H, px);
				impl->input[(size_t)0 * W * H + y * W + x] = px[2] / 255.f;
				impl->input[(size_t)1 * W * H + y * W + x] = px[1] / 255.f;
				impl->input[(size_t)2 * W * H + y * W + x] = px[0] / 255.f;
			}
		int64_t shape[4] = {1, 3, H, W};
		Ort::Value t = Ort::Value::CreateTensor<float>(mem, impl->input.data(), impl->input.size(), shape, 4);
		auto out = impl->person.session.Run(Ort::RunOptions{nullptr}, impl->person.in.data(), &t, 1,
						    impl->person.out.data(), 1);
		const float *a = out[0].GetTensorData<float>();
		mw = W;
		mh = H;
		rgba.assign((size_t)W * H * 4, 0);
		for (size_t i = 0; i < (size_t)W * H; i++)
			rgba[i * 4 + 3] = (uint8_t)std::lround(std::clamp(a[i], 0.0f, 1.0f) * 255.0f);
		return true;
	} catch (const Ort::Exception &e) {
		ort_log("segmentation failed: %s", e.what());
		return false;
	}
}

} // namespace beauty
