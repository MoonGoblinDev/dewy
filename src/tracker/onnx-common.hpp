// Shared ONNX Runtime plumbing: runtime loading by explicit path (ORT_API_MANUAL_INIT, so we
// never pick up another plugin's onnxruntime), sessions with the best execution provider
// (DirectML on Windows, Core ML on macOS, CPU fallback), and small image helpers.
#pragma once

#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace beauty {

bool ort_load();
Ort::Env &ort_env();

struct OrtModel {
	mutable Ort::Session session{nullptr}; /* Run() isn't const in the C++ API */
	std::vector<std::string> in_names, out_names;
	std::vector<const char *> in, out;
	bool accelerated = false; /* running on a GPU / Neural Engine provider */
	explicit operator bool() const { return (bool)session; }

	/* Sessions are shared by every Dewy filter, and DirectML does not allow two threads in
	 * Run() on one session at once (it crashes inside the GPU driver), so runs are
	 * serialised per model. Single input; the first n_out outputs (all if 0). */
	std::vector<Ort::Value> run(const Ort::Value &input, size_t n_out = 0) const
	{
		std::lock_guard<std::mutex> g(run_m);
		return session.Run(Ort::RunOptions{nullptr}, in.data(), &input, 1, out.data(),
				   n_out ? n_out : out.size());
	}

private:
	mutable std::mutex run_m;
};

using OrtModelPtr = std::shared_ptr<const OrtModel>;

/* Loads a model from disk; prefer_accel tries DirectML / Core ML first. Sessions are shared
 * between filter instances (OrtModel::run serialises calls), so a second Dewy filter costs no extra load
 * time or GPU memory. Returns null if the file is missing or the runtime can't be loaded. */
OrtModelPtr ort_model(const std::string &path, bool prefer_accel);

/* Runs a model once on zeros, so the GPU provider's first-run setup (~1 s on DirectML)
 * happens while loading rather than on the first live frame. */
void ort_warm_up(const OrtModel &m);

/* Name of the accelerated provider (and adapter) on this platform, for the log. */
const char *ort_accel_name();

void ort_log(const char *fmt, const std::string &arg = "");

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
