// Portable implementation of face-tracker.h.
//
// Two worker threads (landmarks, person segmentation) each take at most one frame at a
// time; the platform backend does the analysis. Here we keep everything that must behave
// identically on every OS:
//   * rigid + shape One-Euro smoothing (head motion follows instantly, shape stays steady)
//   * per-frame result history with interpolation / short extrapolation (frame sync)
//   * cheek skin-colour sampling for the adaptive skin mask
//   * temporal smoothing of the person mask

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "backend.hpp"
#include "segmenter.hpp"
#include "pigment-calib.hpp"
#include "face-tracker.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace beauty;

namespace {

uint64_t now_ns()
{
	return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
		       std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

const uint64_t PRESENT_TIMEOUT_NS = 350000000ULL;
const int HISTORY = 8;

/* ---------- One-Euro filter (Casiez et al. 2012) ---------- */

struct Euro {
	float x = 0, dx = 0;
};
struct EuroCfg {
	float min_cutoff, beta;
};

float euro_alpha(float cutoff, float dt)
{
	float tau = 1.0f / (2.0f * (float)M_PI * cutoff);
	return 1.0f / (1.0f + tau / dt);
}

float euro_step(Euro &s, EuroCfg c, float x, float dt)
{
	float dx = (x - s.x) / dt;
	s.dx += euro_alpha(1.0f, dt) * (dx - s.dx);
	float cutoff = c.min_cutoff + c.beta * std::fabs(s.dx);
	s.x += euro_alpha(cutoff, dt) * (x - s.x);
	return s.x;
}

/* Tuned with tests/jitter.c + tests/lag.c (analysis px, radians, IOD units). */
const EuroCfg CFG_CENTER = {1.5f, 0.20f};
const EuroCfg CFG_ANGLE = {1.0f, 1.0f};
const EuroCfg CFG_SCALE = {0.8f, 0.05f};
const EuroCfg CFG_SHAPE = {0.6f, 0.35f};

ft_point region_centroid(const ft_face &f, int r)
{
	ft_point c = {0, 0};
	int n = f.region_count[r];
	for (int i = 0; i < n; i++) {
		c.x += f.pts[f.region_start[r] + i].x;
		c.y += f.pts[f.region_start[r] + i].y;
	}
	if (n) {
		c.x /= n;
		c.y /= n;
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

/* Mean Cb/Cr of two cheek patches (points in frame pixels). */
bool sample_skin(const ft_face &f, const uint8_t *bgra, int stride, int w, int h, float out[2])
{
	if (!f.region_count[FT_LEFT_EYE] || !f.region_count[FT_RIGHT_EYE] || !f.region_count[FT_OUTER_LIPS])
		return false;
	ft_point e1 = region_centroid(f, FT_LEFT_EYE), e2 = region_centroid(f, FT_RIGHT_EYE);
	ft_point m = region_centroid(f, FT_OUTER_LIPS);
	ft_point mid = {(e1.x + e2.x) * 0.5f, (e1.y + e2.y) * 0.5f};
	float iod = std::hypot(e2.x - e1.x, e2.y - e1.y);
	int half = (int)std::fmax(1.0f, iod * 0.06f);
	double sum_cb = 0, sum_cr = 0;
	int count = 0;
	for (ft_point e : {e1, e2}) {
		int px = (int)(e.x + (m.x - mid.x) * 0.55f + (e.x - mid.x) * 0.2f);
		int py = (int)(e.y + (m.y - mid.y) * 0.55f + (e.y - mid.y) * 0.2f);
		for (int y = py - half; y <= py + half; y++) {
			if (y < 0 || y >= h)
				continue;
			const uint8_t *row = bgra + (size_t)y * stride;
			for (int x = px - half; x <= px + half; x++) {
				if (x < 0 || x >= w)
					continue;
				float b = row[x * 4] / 255.0f, g = row[x * 4 + 1] / 255.0f, r = row[x * 4 + 2] / 255.0f;
				float luma = 0.299f * r + 0.587f * g + 0.114f * b;
				if (luma < 0.06f || luma > 0.97f)
					continue;
				sum_cb += 0.5 - 0.168736 * r - 0.331264 * g + 0.5 * b;
				sum_cr += 0.5 + 0.5 * r - 0.418688 * g - 0.081312 * b;
				count++;
			}
		}
	}
	if (count < 8)
		return false;
	out[0] = (float)(sum_cb / count);
	out[1] = (float)(sum_cr / count);
	return true;
}

/* Single-slot worker: at most one queued frame, newest wins. */
struct Worker {
	std::thread thread;
	std::mutex m;
	std::condition_variable cv;
	bool has_job = false, quit = false;
	std::vector<uint8_t> buf;
	int w = 0, h = 0;
	uint64_t id = 0, ts = 0;
	std::atomic<bool> busy{false};
};

} // namespace

struct face_tracker {
	std::unique_ptr<TrackerBackend> backend;
	std::unique_ptr<SkinSegmenter> segmenter;
	Worker lm, seg;
	std::atomic<bool> seg_enabled{false};

	/* results */
	std::mutex res_m;
	std::condition_variable res_cv;
	uint64_t processed_id = 0;
	ft_face hist[HISTORY];
	int hist_count = 0, hist_head = 0;
	uint64_t last_seen_ns = 0;
	float mean_ms = 0, detect_ratio = 0;

	/* smoothing state (landmark worker only) */
	Euro e_cx, e_cy, e_ang, e_scale;
	Euro e_shape[FT_MAX_POINTS * 2];
	Euro e_cbcr[2];
	int euro_count = 0;
	uint64_t euro_ts = 0;
	bool skin_valid = false;

	/* skin / person mask: RGBA8 (R skin, G face skin, B hair, A person) */
	std::mutex mask_m;
	std::vector<uint8_t> mask;
	uint32_t mask_w = 0, mask_h = 0;
	uint64_t mask_serial = 0;

	/* skin pigment basis (melanin / hemoglobin), recalibrated every ~2 s */
	PigmentBasis pigment = pigment_prior_basis();
	bool pigment_calibrated = false;
	uint64_t pigment_next_ns = 0;

	void smooth(ft_face &f, uint64_t ts);
	void run_landmarks();
	void run_segmentation();
};

void face_tracker::smooth(ft_face &f, uint64_t ts)
{
	ft_point c = all_centroid(f);
	ft_point e1 = region_centroid(f, FT_LEFT_EYE), e2 = region_centroid(f, FT_RIGHT_EYE);
	if (e1.x > e2.x)
		std::swap(e1, e2);
	float scale = std::fmax(std::hypot(e2.x - e1.x, e2.y - e1.y), 1.0f);
	float ang = std::atan2(e2.y - e1.y, e2.x - e1.x);
	float dt = euro_ts ? (float)((double)(ts - euro_ts) / 1e9) : 0.0f;
	bool reset = euro_count != f.num_points || dt <= 0.0f || dt > 0.5f;

	if (reset) {
		e_cx = {c.x, 0};
		e_cy = {c.y, 0};
		e_ang = {ang, 0};
		e_scale = {scale, 0};
	} else {
		while (ang - e_ang.x > (float)M_PI)
			ang -= 2.0f * (float)M_PI;
		while (ang - e_ang.x < -(float)M_PI)
			ang += 2.0f * (float)M_PI;
	}
	float cs = std::cos(ang), sn = std::sin(ang);
	float sx[FT_MAX_POINTS], sy[FT_MAX_POINTS];
	for (int i = 0; i < f.num_points; i++) {
		float dx = f.pts[i].x - c.x, dy = f.pts[i].y - c.y;
		sx[i] = (dx * cs + dy * sn) / scale;
		sy[i] = (-dx * sn + dy * cs) / scale;
	}
	if (reset) {
		for (int i = 0; i < f.num_points; i++) {
			e_shape[i * 2] = {sx[i], 0};
			e_shape[i * 2 + 1] = {sy[i], 0};
		}
	} else {
		c.x = euro_step(e_cx, CFG_CENTER, c.x, dt);
		c.y = euro_step(e_cy, CFG_CENTER, c.y, dt);
		ang = euro_step(e_ang, CFG_ANGLE, ang, dt);
		scale = euro_step(e_scale, CFG_SCALE, scale, dt);
		for (int i = 0; i < f.num_points; i++) {
			sx[i] = euro_step(e_shape[i * 2], CFG_SHAPE, sx[i], dt);
			sy[i] = euro_step(e_shape[i * 2 + 1], CFG_SHAPE, sy[i], dt);
		}
		cs = std::cos(ang);
		sn = std::sin(ang);
	}
	for (int i = 0; i < f.num_points; i++) {
		f.pts[i].x = c.x + (sx[i] * cs - sy[i] * sn) * scale;
		f.pts[i].y = c.y + (sx[i] * sn + sy[i] * cs) * scale;
	}
	euro_count = f.num_points;
	euro_ts = ts;
}

void face_tracker::run_landmarks()
{
	for (;;) {
		std::unique_lock<std::mutex> lk(lm.m);
		lm.cv.wait(lk, [this] { return lm.has_job || lm.quit; });
		if (lm.quit)
			return;
		lm.has_job = false;
		lk.unlock();

		uint64_t t0 = now_ns();
		const uint8_t *buf = lm.buf.data();
		int w = lm.w, h = lm.h;
		ft_face f;
		memset(&f, 0, sizeof(f));
		bool full = false;
		bool ok = backend && backend->detect(buf, w, h, w * 4, f, full) && f.num_points > 10 &&
			  f.region_count[FT_LEFT_EYE] && f.region_count[FT_RIGHT_EYE];
		if (ok)
			f.skin_valid = sample_skin(f, buf, w * 4, w, h, f.skin_cbcr);

		std::lock_guard<std::mutex> g(res_m);
		if (ok) {
			smooth(f, lm.ts);
			for (int i = 0; i < f.num_points; i++) {
				f.pts[i].x /= w;
				f.pts[i].y /= h;
			}
			if (f.skin_valid) { /* skin colour drifts slowly; heavy smoothing avoids mask flicker */
				if (!skin_valid) {
					e_cbcr[0].x = f.skin_cbcr[0];
					e_cbcr[1].x = f.skin_cbcr[1];
				} else {
					e_cbcr[0].x += 0.08f * (f.skin_cbcr[0] - e_cbcr[0].x);
					e_cbcr[1].x += 0.08f * (f.skin_cbcr[1] - e_cbcr[1].x);
				}
				skin_valid = true;
			}
			f.skin_valid = skin_valid;
			f.skin_cbcr[0] = e_cbcr[0].x;
			f.skin_cbcr[1] = e_cbcr[1].x;
			f.valid = true;
			f.frame_id = lm.id;
			f.timestamp_ns = lm.ts;
			hist[hist_head] = f;
			hist_head = (hist_head + 1) % HISTORY;
			if (hist_count < HISTORY)
				hist_count++;
			last_seen_ns = now_ns();
		}
		float ms = (float)((double)(now_ns() - t0) / 1e6);
		mean_ms += 0.05f * (ms - mean_ms);
		detect_ratio += 0.05f * ((full ? 1.0f : 0.0f) - detect_ratio);
		processed_id = lm.id;
		lm.busy = false;
		res_cv.notify_all();
	}
}

void face_tracker::run_segmentation()
{
	for (;;) {
		std::unique_lock<std::mutex> lk(seg.m);
		seg.cv.wait(lk, [this] { return seg.has_job || seg.quit; });
		if (seg.quit)
			return;
		seg.has_job = false;
		lk.unlock();

		std::vector<uint8_t> m;
		int mw = 0, mh = 0;
		bool ok = false;
		if (segmenter && segmenter->ready()) {
			ok = segmenter->run(seg.buf.data(), seg.w, seg.h, seg.w * 4, m, mw, mh);
		} else if (backend) { /* person-only fallback (e.g. Apple Vision) -> alpha channel */
			std::vector<uint8_t> p;
			ok = backend->segment(seg.buf.data(), seg.w, seg.h, seg.w * 4, p, mw, mh);
			if (ok) {
				m.assign(p.size() * 4, 0);
				for (size_t i = 0; i < p.size(); i++)
					m[i * 4 + 3] = p[i];
			}
		}
		if (ok && mw > 0 && mh > 0) {
			std::lock_guard<std::mutex> g(mask_m);
			bool blend = mask_w == (uint32_t)mw && mask_h == (uint32_t)mh && mask.size() == m.size();
			if (!blend) {
				mask = m;
				mask_w = mw;
				mask_h = mh;
			} else { /* temporal smoothing against flicker */
				for (size_t i = 0; i < mask.size(); i++)
					mask[i] = (uint8_t)((mask[i] + m[i] + 1) / 2);
			}
			mask_serial++;
		}
		/* pigment calibration from confident skin pixels of this frame */
		if (ok && (int)m.size() == mw * mh * 4 && now_ns() >= pigment_next_ns) {
			PigmentBasis b;
			if (pigment_calibrate(seg.buf.data(), seg.w, seg.h, seg.w * 4, m.data(), mw, mh, b)) {
				std::lock_guard<std::mutex> g(mask_m);
				if (pigment_calibrated)
					pigment_blend(pigment, b, 0.3f);
				else
					pigment = b;
				pigment_calibrated = true;
			}
			pigment_next_ns = now_ns() + 2000000000ULL;
		}
		seg.busy = false;
	}
}

/* ---------- C API ---------- */

extern "C" face_tracker_t *face_tracker_create(const char *models_dir)
{
	auto *t = new face_tracker();
	(void)models_dir;
#if defined(BEAUTY_TRACKER_VISION)
	t->backend = create_vision_backend();
#else
	t->backend = create_onnx_backend(models_dir ? models_dir : "");
#endif
	if (models_dir && *models_dir)
		t->segmenter = std::make_unique<SkinSegmenter>(models_dir);
	t->lm.thread = std::thread([t] { t->run_landmarks(); });
	t->seg.thread = std::thread([t] { t->run_segmentation(); });
	return t;
}

extern "C" void face_tracker_destroy(face_tracker_t *t)
{
	if (!t)
		return;
	for (Worker *w : {&t->lm, &t->seg}) {
		{
			std::lock_guard<std::mutex> g(w->m);
			w->quit = true;
		}
		w->cv.notify_all();
		if (w->thread.joinable())
			w->thread.join();
	}
	delete t;
}

extern "C" const char *face_tracker_backend_name(face_tracker_t *t)
{
	return t && t->backend ? t->backend->name() : "none";
}

extern "C" bool face_tracker_busy(face_tracker_t *t)
{
	return t->lm.busy.load();
}

static bool queue_frame(Worker &w, const uint8_t *data, uint32_t linesize, uint32_t width, uint32_t height,
			uint64_t id, uint64_t ts)
{
	bool expected = false;
	if (!w.busy.compare_exchange_strong(expected, true))
		return false;
	{
		std::lock_guard<std::mutex> g(w.m);
		w.buf.resize((size_t)width * height * 4);
		for (uint32_t y = 0; y < height; y++)
			memcpy(w.buf.data() + (size_t)y * width * 4, data + (size_t)y * linesize, (size_t)width * 4);
		w.w = (int)width;
		w.h = (int)height;
		w.id = id;
		w.ts = ts;
		w.has_job = true;
	}
	w.cv.notify_one();
	return true;
}

extern "C" bool face_tracker_submit_bgra(face_tracker_t *t, const uint8_t *data, uint32_t linesize, uint32_t width,
					 uint32_t height, uint64_t frame_id, uint64_t timestamp_ns)
{
	/* segmentation every other frame on its own thread, never blocking landmarks */
	if (t->seg_enabled.load())
		queue_frame(t->seg, data, linesize, width, height, frame_id, timestamp_ns);
	return queue_frame(t->lm, data, linesize, width, height, frame_id, timestamp_ns);
}

extern "C" bool face_tracker_wait(face_tracker_t *t, uint64_t frame_id, float timeout_ms)
{
	std::unique_lock<std::mutex> lk(t->res_m);
	return t->res_cv.wait_for(lk, std::chrono::microseconds((int64_t)(timeout_ms * 1000.0f)),
				  [&] { return t->processed_id >= frame_id; });
}

static void lerp_face(ft_face *out, const ft_face *a, const ft_face *b, float k)
{
	*out = *a;
	for (int i = 0; i < a->num_points; i++) {
		out->pts[i].x = a->pts[i].x + (b->pts[i].x - a->pts[i].x) * k;
		out->pts[i].y = a->pts[i].y + (b->pts[i].y - a->pts[i].y) * k;
	}
}

extern "C" bool face_tracker_get_at(face_tracker_t *t, uint64_t id, ft_face *out)
{
	std::lock_guard<std::mutex> g(t->res_m);
	memset(out, 0, sizeof(*out));
	const ft_face *lo = nullptr, *hi = nullptr, *lo2 = nullptr;
	for (int i = 0; i < t->hist_count; i++) {
		const ft_face *f = &t->hist[(t->hist_head + HISTORY - 1 - i) % HISTORY]; /* newest first */
		if (f->frame_id >= id)
			hi = f;
		if (f->frame_id <= id) {
			if (!lo)
				lo = f;
			else if (!lo2)
				lo2 = f;
		}
	}
	const uint64_t MAX_GAP = 12;
	if (lo && hi && lo != hi && lo->num_points == hi->num_points && hi->frame_id - lo->frame_id <= MAX_GAP) {
		lerp_face(out, lo, hi, (float)(id - lo->frame_id) / (float)(hi->frame_id - lo->frame_id));
	} else if (lo && !hi && lo2 && lo2->num_points == lo->num_points && lo->frame_id - lo2->frame_id <= MAX_GAP &&
		   id - lo->frame_id <= 3) {
		float k = 0.8f * (float)(id - lo->frame_id) / (float)(lo->frame_id - lo2->frame_id);
		lerp_face(out, lo, lo2, -k);
	} else if (lo) {
		*out = *lo;
	} else if (hi) {
		*out = *hi;
	}
	out->frame_id = id;
	out->present = out->valid && (now_ns() - t->last_seen_ns) < PRESENT_TIMEOUT_NS;
	return out->valid;
}

extern "C" void face_tracker_set_segmentation(face_tracker_t *t, bool enabled)
{
	t->seg_enabled = enabled;
}

extern "C" uint8_t *face_tracker_take_mask(face_tracker_t *t, uint64_t *serial, uint32_t *w, uint32_t *h)
{
	std::lock_guard<std::mutex> g(t->mask_m);
	if (t->mask.empty() || t->mask_serial == *serial)
		return nullptr;
	auto *copy = (uint8_t *)malloc(t->mask.size());
	memcpy(copy, t->mask.data(), t->mask.size());
	*w = t->mask_w;
	*h = t->mask_h;
	*serial = t->mask_serial;
	return copy;
}

extern "C" void face_tracker_stats(face_tracker_t *t, float *mean_ms, float *detect_ratio)
{
	std::lock_guard<std::mutex> g(t->res_m);
	*mean_ms = t->mean_ms;
	*detect_ratio = t->detect_ratio;
}

extern "C" bool face_tracker_get_pigment(face_tracker_t *t, ft_pigment *out)
{
	if (!t) {
		PigmentBasis b = pigment_prior_basis();
		std::copy(b.mel, b.mel + 3, out->mel);
		std::copy(b.hem, b.hem + 3, out->hem);
		std::copy(b.inv, b.inv + 9, out->inv);
		out->calibrated = false;
		return false;
	}
	std::lock_guard<std::mutex> g(t->mask_m);
	for (int i = 0; i < 3; i++) {
		out->mel[i] = t->pigment.mel[i];
		out->hem[i] = t->pigment.hem[i];
	}
	for (int i = 0; i < 9; i++)
		out->inv[i] = t->pigment.inv[i];
	out->calibrated = t->pigment_calibrated;
	return true;
}
