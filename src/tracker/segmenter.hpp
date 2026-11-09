// Learned skin / person segmentation (MediaPipe selfie multiclass via ONNX Runtime).
// Output is an RGBA8 mask covering the whole frame:
//   R = skin (face + body), G = face skin, B = hair, A = person (1 - background)
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace beauty {

class SkinSegmenter {
public:
	explicit SkinSegmenter(const std::string &models_dir);
	~SkinSegmenter();
	bool ready() const;
	const char *describe() const;
	bool run(const uint8_t *bgra, int w, int h, int stride, std::vector<uint8_t> &rgba, int &mw, int &mh);

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace beauty
