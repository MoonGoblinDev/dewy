// Apple Vision backend (macOS): 76-point face landmarks + person segmentation.
//
// A full face detection costs ~13 ms but landmarks seeded with a face box cost <1 ms, so we
// detect every DETECT_INTERVAL frames (or when tracking is lost) and otherwise move the last
// detection box along with the face.

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>

#include <cmath>
#include <cstring>

#include "backend.hpp"

namespace beauty {
namespace {

const int DETECT_INTERVAL = 90;

ft_point centroid(const ft_face &f, int r)
{
	ft_point c = {0, 0};
	for (int i = 0; i < f.region_count[r]; i++) {
		c.x += f.pts[f.region_start[r] + i].x;
		c.y += f.pts[f.region_start[r] + i].y;
	}
	if (f.region_count[r]) {
		c.x /= f.region_count[r];
		c.y /= f.region_count[r];
	}
	return c;
}

ft_point all_centroid(const ft_face &f)
{
	ft_point c = {0, 0};
	for (int i = 0; i < f.num_points; i++) {
		c.x += f.pts[i].x;
		c.y += f.pts[i].y;
	}
	if (f.num_points) {
		c.x /= f.num_points;
		c.y /= f.num_points;
	}
	return c;
}

void append_region(ft_face &f, int r, VNFaceLandmarkRegion2D *region, CGSize size)
{
	f.region_start[r] = f.num_points;
	f.region_count[r] = 0;
	if (!region)
		return;
	const CGPoint *pts = [region pointsInImageOfSize:size];
	for (NSUInteger i = 0; i < region.pointCount && f.num_points < FT_MAX_POINTS; i++) {
		f.pts[f.num_points].x = (float)pts[i].x;
		f.pts[f.num_points].y = (float)(size.height - pts[i].y); /* Vision origin is bottom-left */
		f.num_points++;
		f.region_count[r]++;
	}
}

class VisionBackend : public TrackerBackend {
public:
	VisionBackend()
	{
		request = [VNDetectFaceLandmarksRequest new];
		request.constellation = VNRequestFaceLandmarksConstellation76Points;
		seg_request = [VNGeneratePersonSegmentationRequest new];
		seg_request.qualityLevel = VNGeneratePersonSegmentationRequestQualityLevelBalanced;
		seg_request.outputPixelFormat = kCVPixelFormatType_OneComponent8;
	}
	~VisionBackend() override
	{
		if (pool)
			CVPixelBufferPoolRelease(pool);
		if (seg_pool)
			CVPixelBufferPoolRelease(seg_pool);
	}
	const char *name() const override { return "Apple Vision"; }

	bool detect(const uint8_t *bgra, int w, int h, int stride, ft_face &f, bool &full) override
	{
		@autoreleasepool {
			CVPixelBufferRef buf = wrap(bgra, w, h, stride, pool, pool_w, pool_h);
			if (!buf)
				return false;
			CGSize size = CGSizeMake(w, h);
			full = !tracking || !ref_obs || since_detect >= DETECT_INTERVAL;
			VNFaceObservation *seed = nil;
			if (!full) {
				ft_point pc = all_centroid(prev);
				ft_point e1 = centroid(prev, FT_LEFT_EYE), e2 = centroid(prev, FT_RIGHT_EYE);
				float iod = std::hypot(e2.x - e1.x, e2.y - e1.y);
				float k = ref_iod > 0 ? iod / ref_iod : 1.0f;
				CGRect rb = ref_obs.boundingBox; /* normalized, bottom-left */
				CGPoint cur = CGPointMake(pc.x, h - pc.y);
				CGFloat bw = rb.size.width * k, bh = rb.size.height * k;
				CGFloat bcx = (cur.x + (CGRectGetMidX(rb) * w - ref_center.x) * k) / w;
				CGFloat bcy = (cur.y + (CGRectGetMidY(rb) * h - ref_center.y) * k) / h;
				CGRect box = CGRectIntersection(CGRectMake(bcx - bw / 2, bcy - bh / 2, bw, bh),
								CGRectMake(0, 0, 1, 1));
				if (!CGRectIsEmpty(box))
					seed = [VNFaceObservation
						faceObservationWithRequestRevision:VNDetectFaceRectanglesRequestRevision3
								       boundingBox:box
									      roll:ref_obs.roll
									       yaw:ref_obs.yaw
									     pitch:nil];
				else
					full = true;
			}
			request.inputFaceObservations = seed ? @[seed] : nil;
			VNImageRequestHandler *handler = [[VNImageRequestHandler alloc] initWithCVPixelBuffer:buf
												      options:@{}];
			[handler performRequests:@[request] error:nil];
			CVPixelBufferRelease(buf);

			VNFaceObservation *best = nil;
			CGFloat best_area = 0;
			for (VNFaceObservation *obs in request.results) {
				CGFloat area = obs.boundingBox.size.width * obs.boundingBox.size.height;
				if (obs.landmarks && obs.landmarks.confidence > 0.3f && area > best_area) {
					best = obs;
					best_area = area;
				}
			}
			memset(&f, 0, sizeof(f));
			if (best) {
				VNFaceLandmarks2D *lm = best.landmarks;
				append_region(f, FT_CONTOUR, lm.faceContour, size);
				append_region(f, FT_LEFT_EYE, lm.leftEye, size);
				append_region(f, FT_RIGHT_EYE, lm.rightEye, size);
				append_region(f, FT_LEFT_BROW, lm.leftEyebrow, size);
				append_region(f, FT_RIGHT_BROW, lm.rightEyebrow, size);
				append_region(f, FT_NOSE, lm.nose, size);
				append_region(f, FT_NOSE_CREST, lm.noseCrest, size);
				append_region(f, FT_MEDIAN, lm.medianLine, size);
				append_region(f, FT_OUTER_LIPS, lm.outerLips, size);
				append_region(f, FT_INNER_LIPS, lm.innerLips, size);
				append_region(f, FT_LEFT_PUPIL, lm.leftPupil, size);
				append_region(f, FT_RIGHT_PUPIL, lm.rightPupil, size);
				if (!f.region_count[FT_LEFT_EYE] || !f.region_count[FT_RIGHT_EYE] || f.num_points < 10)
					best = nil;
			}
			if (!best) {
				tracking = false;
				return false;
			}
			if (full) {
				ref_obs = best;
				ft_point c = all_centroid(f);
				ref_center = CGPointMake(c.x, h - c.y);
				ft_point e1 = centroid(f, FT_LEFT_EYE), e2 = centroid(f, FT_RIGHT_EYE);
				ref_iod = std::hypot(e2.x - e1.x, e2.y - e1.y);
				since_detect = 0;
			} else {
				since_detect++;
			}
			tracking = true;
			prev = f;
			return true;
		}
	}

	bool segment(const uint8_t *bgra, int w, int h, int stride, std::vector<uint8_t> &mask, int &mw,
		     int &mh) override
	{
		@autoreleasepool {
			CVPixelBufferRef buf = wrap(bgra, w, h, stride, seg_pool, seg_pool_w, seg_pool_h);
			if (!buf)
				return false;
			VNImageRequestHandler *handler = [[VNImageRequestHandler alloc] initWithCVPixelBuffer:buf
												      options:@{}];
			BOOL ok = [handler performRequests:@[seg_request] error:nil];
			CVPixelBufferRelease(buf);
			VNPixelBufferObservation *obs = ok ? seg_request.results.firstObject : nil;
			if (!obs)
				return false;
			CVPixelBufferRef m = obs.pixelBuffer;
			CVPixelBufferLockBaseAddress(m, kCVPixelBufferLock_ReadOnly);
			mw = (int)CVPixelBufferGetWidth(m);
			mh = (int)CVPixelBufferGetHeight(m);
			const uint8_t *src = (const uint8_t *)CVPixelBufferGetBaseAddress(m);
			size_t ms = CVPixelBufferGetBytesPerRow(m);
			mask.resize((size_t)mw * mh);
			for (int y = 0; y < mh; y++)
				memcpy(mask.data() + (size_t)y * mw, src + y * ms, mw);
			CVPixelBufferUnlockBaseAddress(m, kCVPixelBufferLock_ReadOnly);
			return true;
		}
	}

private:
	static CVPixelBufferRef wrap(const uint8_t *bgra, int w, int h, int stride, CVPixelBufferPoolRef &pool,
				     int &pw, int &ph)
	{
		if (!pool || pw != w || ph != h) {
			if (pool)
				CVPixelBufferPoolRelease(pool);
			pool = nullptr;
			NSDictionary *attrs = @{
				(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
				(id)kCVPixelBufferWidthKey: @(w),
				(id)kCVPixelBufferHeightKey: @(h),
				(id)kCVPixelBufferIOSurfacePropertiesKey: @{},
			};
			CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, (__bridge CFDictionaryRef)attrs, &pool);
			pw = w;
			ph = h;
		}
		CVPixelBufferRef buf = nullptr;
		if (!pool || CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &buf) != kCVReturnSuccess)
			return nullptr;
		CVPixelBufferLockBaseAddress(buf, 0);
		uint8_t *dst = (uint8_t *)CVPixelBufferGetBaseAddress(buf);
		size_t ds = CVPixelBufferGetBytesPerRow(buf);
		for (int y = 0; y < h; y++)
			memcpy(dst + y * ds, bgra + (size_t)y * stride, (size_t)w * 4);
		CVPixelBufferUnlockBaseAddress(buf, 0);
		return buf;
	}

	VNDetectFaceLandmarksRequest *request;
	VNGeneratePersonSegmentationRequest *seg_request;
	VNFaceObservation *ref_obs = nil;
	CGPoint ref_center = {0, 0};
	float ref_iod = 0;
	int since_detect = 0;
	bool tracking = false;
	ft_face prev{};
	CVPixelBufferPoolRef pool = nullptr, seg_pool = nullptr;
	int pool_w = 0, pool_h = 0, seg_pool_w = 0, seg_pool_h = 0;
};

} // namespace

std::unique_ptr<TrackerBackend> create_vision_backend()
{
	return std::make_unique<VisionBackend>();
}

} // namespace beauty
