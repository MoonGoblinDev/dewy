// Shared ONNX Runtime plumbing: runtime loading by explicit path (ORT_API_MANUAL_INIT, so we
// never pick up another plugin's onnxruntime), sessions with the best execution provider
// (DirectML on Windows, Core ML on macOS, CPU fallback).
#pragma once

#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "tracker-util.hpp"

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

} // namespace beauty
