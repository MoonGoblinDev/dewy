// Core ML plumbing for the macOS build (Objective-C++ only): compiled models (.mlmodelc, made
// from data/models/coreml/*.mlpackage at build time) are loaded once and shared by every Dewy
// filter; each user has its own BGRA input buffer and results.
#pragma once

#import <CoreML/CoreML.h>
#import <CoreVideo/CoreVideo.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace beauty {

struct CoreMLShared;

class CoreMLRunner {
public:
	CoreMLRunner() = default;
	~CoreMLRunner();
	CoreMLRunner(const CoreMLRunner &) = delete;
	CoreMLRunner &operator=(const CoreMLRunner &) = delete;

	/* Loads (or reuses) the compiled model and makes a w x h BGRA input for its "image" input. */
	bool open(const std::string &path, int w, int h);
	explicit operator bool() const { return (bool)shared; }

	/* Write the input pixels (B, G, R, A) between lock_input and unlock_input. */
	uint8_t *lock_input(size_t &stride);
	void unlock_input();

	/* Runs the model on the input; outputs stay readable until the next run. */
	bool run();
	/* Copies a multi-array output, any layout / fp16 / fp32, into a contiguous float vector. */
	bool read(const char *name, std::vector<float> &out) const;

private:
	std::shared_ptr<CoreMLShared> shared;
	CVPixelBufferRef pixels = nullptr;
	id<MLFeatureProvider> input = nil;
	id<MLFeatureProvider> result = nil;
};

} // namespace beauty
