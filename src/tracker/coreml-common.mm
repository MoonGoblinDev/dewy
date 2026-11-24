#include "coreml-common.hpp"

#include <chrono>
#include <map>
#include <mutex>

#include "tracker-util.hpp"

/* ONNX Runtime hooks: the macOS build has no ONNX Runtime, so these do nothing here */
extern "C" void face_tracker_set_runtime_library(const char *) {}
extern "C" void face_tracker_set_render_device(void *) {}

namespace beauty {

struct CoreMLShared {
	MLModel *model = nil;
	std::mutex run_m; /* one prediction at a time per model, as with the ONNX Runtime sessions */
};

static std::shared_ptr<CoreMLShared> load_shared(const std::string &path)
{
	static std::mutex m;
	static std::map<std::string, std::weak_ptr<CoreMLShared>> cache;
	std::lock_guard<std::mutex> g(m);
	if (auto s = cache[path].lock())
		return s;
	@autoreleasepool {
		NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
		if (![[NSFileManager defaultManager] fileExistsAtPath:url.path]) {
			tracker_log("missing model %s", path.c_str());
			return nullptr;
		}
		MLModelConfiguration *cfg = [MLModelConfiguration new];
		cfg.computeUnits = MLComputeUnitsAll; /* in practice the Neural Engine */
		auto t0 = std::chrono::steady_clock::now();
		NSError *err = nil;
		MLModel *model = [MLModel modelWithContentsOfURL:url configuration:cfg error:&err];
		if (!model) {
			tracker_log("Core ML could not load %s: %s", path.c_str(), err.localizedDescription.UTF8String);
			return nullptr;
		}
		double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		tracker_log("Core ML loaded %s in %.0f ms", url.lastPathComponent.UTF8String, ms);
		auto s = std::make_shared<CoreMLShared>();
		s->model = model;
		cache[path] = s;
		return s;
	}
}

CoreMLRunner::~CoreMLRunner()
{
	input = nil;
	result = nil;
	if (pixels)
		CVPixelBufferRelease(pixels);
}

bool CoreMLRunner::open(const std::string &path, int w, int h)
{
	shared = load_shared(path);
	if (!shared)
		return false;
	@autoreleasepool {
		/* IOSurface-backed, so the Neural Engine reads it without a copy */
		NSDictionary *attrs = @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
		if (CVPixelBufferCreate(kCFAllocatorDefault, w, h, kCVPixelFormatType_32BGRA, (__bridge CFDictionaryRef)attrs,
					&pixels) != kCVReturnSuccess) {
			shared.reset();
			return false;
		}
		NSError *err = nil;
		input = [[MLDictionaryFeatureProvider alloc]
			initWithDictionary:@{@"image": [MLFeatureValue featureValueWithPixelBuffer:pixels]}
				     error:&err];
		if (!input) {
			tracker_log("Core ML input failed: %s", err.localizedDescription.UTF8String);
			shared.reset();
			return false;
		}
	}
	return true;
}

uint8_t *CoreMLRunner::lock_input(size_t &stride)
{
	CVPixelBufferLockBaseAddress(pixels, 0);
	stride = CVPixelBufferGetBytesPerRow(pixels);
	return (uint8_t *)CVPixelBufferGetBaseAddress(pixels);
}

void CoreMLRunner::unlock_input()
{
	CVPixelBufferUnlockBaseAddress(pixels, 0);
}

bool CoreMLRunner::run()
{
	@autoreleasepool {
		NSError *err = nil;
		{
			std::lock_guard<std::mutex> g(shared->run_m);
			result = [shared->model predictionFromFeatures:input error:&err];
		}
		if (!result) {
			static int logged = 0;
			if (logged++ < 5)
				tracker_log("Core ML prediction failed: %s", err.localizedDescription.UTF8String);
			return false;
		}
	}
	return true;
}

bool CoreMLRunner::read(const char *name, std::vector<float> &out) const
{
	@autoreleasepool {
		MLMultiArray *a = [result featureValueForName:[NSString stringWithUTF8String:name]].multiArrayValue;
		if (!a)
			return false;
		const int nd = (int)a.shape.count;
		if (nd == 0)
			return false;
		std::vector<NSInteger> shape(nd), strides(nd);
		for (int d = 0; d < nd; d++) {
			shape[d] = a.shape[d].integerValue;
			strides[d] = a.strides[d].integerValue;
		}
		const MLMultiArrayDataType type = a.dataType;
		if (type != MLMultiArrayDataTypeFloat32 && type != MLMultiArrayDataTypeFloat16)
			return false;
		out.resize((size_t)a.count);
		float *dst = out.data();
		const size_t total = out.size();
		[a getBytesWithHandler:^(const void *bytes, NSInteger) {
			/* walk every index except the last; copy the last axis with its stride */
			const NSInteger inner = shape[nd - 1], ist = strides[nd - 1];
			const size_t rows = total / (size_t)inner;
			std::vector<NSInteger> idx(nd, 0);
			size_t o = 0;
			for (size_t r = 0; r < rows; r++) {
				NSInteger off = 0;
				for (int d = 0; d < nd - 1; d++)
					off += idx[d] * strides[d];
				if (type == MLMultiArrayDataTypeFloat32) {
					const float *s = (const float *)bytes + off;
					for (NSInteger k = 0; k < inner; k++)
						dst[o++] = s[k * ist];
				} else {
					const _Float16 *s = (const _Float16 *)bytes + off;
					for (NSInteger k = 0; k < inner; k++)
						dst[o++] = (float)s[k * ist];
				}
				for (int d = nd - 2; d >= 0; d--) {
					if (++idx[d] < shape[d])
						break;
					idx[d] = 0;
				}
			}
		}];
	}
	return true;
}

} // namespace beauty
