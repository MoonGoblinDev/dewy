// ONNX Runtime backend (Windows; also buildable on macOS with -DBEAUTY_TRACKER=onnx).
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
// DirectML execution provider (any DX12 GPU), with CPU as fallback. The tracking itself
// (crops, lid snapping) is shared with the Core ML backend in mediapipe.cpp.

#include <chrono>
#include <string>
#include <vector>

#include "mediapipe.hpp"
#include "onnx-common.hpp"

namespace beauty {
namespace {

class OnnxBackend : public MediaPipeBackend {
public:
	explicit OnnxBackend(const std::string &dir)
	{
		try {
			det = ort_model(dir + "/face_detection_yunet.onnx", false);
#ifdef __APPLE__
			/* CPU on macOS: Core ML takes only part of this model (70 pieces bouncing to the CPU,
			 * ~40 ms a frame on an M4 Pro); ONNX Runtime's own CPU kernels run it in ~3 ms */
			lmk = ort_model(dir + "/face_landmarks_478.onnx", false);
#else
			lmk = ort_model(dir + "/face_landmarks_478.onnx", true);
#endif
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
		try {
			return MediaPipeBackend::detect(bgra, w, h, stride, f, full);
		} catch (const Ort::Exception &e) {
			ort_log("landmarks failed: %s", e.what());
			return false;
		}
	}

protected:
	/* letterbox into 640x640, BGR 0..255 NCHW */
	bool run_detector(const uint8_t *bgra, int w, int h, int stride, float sc, const float *heads[12]) override
	{
		const int S = DET_SIZE;
		det_in.assign((size_t)3 * S * S, 0.0f);
		int nw = (int)(w * sc), nh = (int)(h * sc);
		float px[3];
		for (int y = 0; y < nh; y++)
			for (int x = 0; x < nw; x++) {
				sample_bgra(bgra, w, h, stride, (x + 0.5f) / sc, (y + 0.5f) / sc, px);
				for (int c = 0; c < 3; c++)
					det_in[(size_t)c * S * S + (size_t)y * S + x] = px[c];
			}
		Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
		int64_t shape[4] = {1, 3, S, S};
		Ort::Value t = Ort::Value::CreateTensor<float>(mem, det_in.data(), det_in.size(), shape, 4);
		det_out = det->run(t);
		static const char *names[12] = {"cls_8",  "cls_16",  "cls_32",  "obj_8", "obj_16", "obj_32",
						"bbox_8", "bbox_16", "bbox_32", "kps_8", "kps_16", "kps_32"};
		for (int k = 0; k < 12; k++) {
			heads[k] = nullptr;
			for (size_t i = 0; i < det->out_names.size(); i++)
				if (det->out_names[i] == names[k])
					heads[k] = det_out[i].GetTensorData<float>();
		}
		return true;
	}

	/* [1,256,256,3] RGB 0..1 -> [1,1,1,1434] crop px, presence logit */
	bool run_landmarks(const uint8_t *bgra, int w, int h, int stride, const Crop &c, const float *&pts,
			   float &presence_logit) override
	{
		const int S = LMK_SIZE;
		auto t0 = std::chrono::steady_clock::now();
		lmk_in.resize((size_t)S * S * 3);
		float px[3], m[6];
		crop_affine(c, m);
		for (int v = 0; v < S; v++)
			for (int u = 0; u < S; u++) {
				float x = m[0] + m[1] * (u + 0.5f) + m[2] * (v + 0.5f);
				float y = m[3] + m[4] * (u + 0.5f) + m[5] * (v + 0.5f);
				sample_bgra(bgra, w, h, stride, x, y, px);
				float *o = &lmk_in[((size_t)v * S + u) * 3];
				o[0] = px[2] / 255.f;
				o[1] = px[1] / 255.f;
				o[2] = px[0] / 255.f;
			}
		Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
		int64_t shape[4] = {1, S, S, 3};
		Ort::Value t = Ort::Value::CreateTensor<float>(mem, lmk_in.data(), lmk_in.size(), shape, 4);
		auto t1 = std::chrono::steady_clock::now();
		lmk_out = lmk->run(t);
		auto t2 = std::chrono::steady_clock::now();
		prof(std::chrono::duration<double, std::milli>(t1 - t0).count(),
		     std::chrono::duration<double, std::milli>(t2 - t1).count());
		pts = lmk_out[0].GetTensorData<float>();
		presence_logit = lmk_out[1].GetTensorData<float>()[0];
		return true;
	}

private:
	OrtModelPtr det, lmk;
	std::vector<float> det_in, lmk_in;
	std::vector<Ort::Value> det_out, lmk_out;
};

} // namespace

std::unique_ptr<TrackerBackend> create_onnx_backend(const std::string &models_dir)
{
	return std::make_unique<OnnxBackend>(models_dir);
}

} // namespace beauty
