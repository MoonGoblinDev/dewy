#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FT_MAX_POINTS 720

typedef struct {
	float x, y;
} ft_point;

/* Landmark regions (Apple Vision 76-point constellation layout). */
enum ft_region {
	FT_CONTOUR,
	FT_LEFT_EYE, /* "left"/"right" are whatever Vision reports; geometry re-sorts by image x */
	FT_RIGHT_EYE,
	FT_LEFT_BROW,
	FT_RIGHT_BROW,
	FT_NOSE,
	FT_NOSE_CREST,
	FT_MEDIAN,
	FT_OUTER_LIPS,
	FT_INNER_LIPS,
	FT_LEFT_PUPIL,
	FT_RIGHT_PUPIL,
	FT_MESH, /* MediaPipe face mesh, 468 points in mesh index order (MediaPipe backend; empty with Vision) */
	FT_REGION_COUNT
};

#define FT_MESH_POINTS 468

typedef struct {
	bool valid;   /* landmarks are usable (a face was seen at some point) */
	bool present; /* a face was detected in the last few hundred ms */
	uint64_t frame_id;     /* render frame these landmarks belong to */
	uint64_t timestamp_ns; /* when the analysed frame was captured */
	int region_start[FT_REGION_COUNT];
	int region_count[FT_REGION_COUNT];
	int num_points;
	ft_point pts[FT_MAX_POINTS]; /* normalized [0,1] image coords, y down, smoothed */

	bool skin_valid;
	float skin_cbcr[2]; /* mean cheek chroma (BT.601 full range, 0..1) */
} ft_face;

/* Eye anchor for building a face frame: the midpoint of the two eye corners when the mesh is
 * there (corners don't move when the lids do, so blinking can't shift the frame), otherwise the
 * centroid of the eye outline. eye: 0 = FT_LEFT_EYE, 1 = FT_RIGHT_EYE. */
static inline ft_point ft_eye_anchor(const ft_face *f, int eye)
{
	ft_point c = {0, 0};
	if (f->region_count[FT_MESH] >= FT_MESH_POINTS) {
		/* subject's left eye (FT_LEFT_EYE) corners 263 / 362, right eye 33 / 133 */
		const ft_point *m = &f->pts[f->region_start[FT_MESH]];
		ft_point a = eye ? m[33] : m[263], b = eye ? m[133] : m[362];
		c.x = (a.x + b.x) * 0.5f;
		c.y = (a.y + b.y) * 0.5f;
		return c;
	}
	int r = eye ? FT_RIGHT_EYE : FT_LEFT_EYE, n = f->region_count[r];
	for (int i = 0; i < n; i++) {
		c.x += f->pts[f->region_start[r] + i].x;
		c.y += f->pts[f->region_start[r] + i].y;
	}
	if (n) {
		c.x /= (float)n;
		c.y /= (float)n;
	}
	return c;
}

typedef struct face_tracker face_tracker_t;

/* models_dir: folder with the ONNX models (ONNX backend); ignored by Apple Vision. */
face_tracker_t *face_tracker_create(const char *models_dir);
const char *face_tracker_backend_name(face_tracker_t *ft);

/* ONNX backend: full path of the ONNX Runtime library to load (set once at module load). */
void face_tracker_set_runtime_library(const char *path);
/* Windows: OBS's ID3D11Device (gs_get_device_obj), so inference runs on the same GPU as OBS.
 * Call from the graphics thread before the first face_tracker_create. No-op elsewhere. */
void face_tracker_set_render_device(void *device);
void face_tracker_destroy(face_tracker_t *ft);

/* True while a previous frame is still being analysed. */
bool face_tracker_busy(face_tracker_t *ft);

/* Copies the BGRA frame and analyses it asynchronously. Returns false (frame dropped) if the
 * tracker is still busy with an earlier frame. */
bool face_tracker_submit_bgra(face_tracker_t *ft, const uint8_t *data, uint32_t linesize, uint32_t width,
			      uint32_t height, uint64_t frame_id, uint64_t timestamp_ns);

/* Blocks up to timeout_ms until frame `frame_id` has been analysed. */
bool face_tracker_wait(face_tracker_t *ft, uint64_t frame_id, float timeout_ms);

/* Landmarks for render frame `frame_id`: exact if that frame was analysed, otherwise
 * interpolated between neighbouring results or (briefly) extrapolated from the last two. */
bool face_tracker_get_at(face_tracker_t *ft, uint64_t frame_id, ft_face *out);

/* Skin / person segmentation on its own thread, only while enabled. */
void face_tracker_set_segmentation(face_tracker_t *ft, bool enabled);
/* How often segmentation may run: fast (~30 Hz, the light follows the body with it) or ~15 Hz. */
void face_tracker_set_segmentation_fast(face_tracker_t *ft, bool fast);

/* Newer segmentation mask than *serial? Returns a malloc'd RGBA8 copy (free() it), w x h,
 * covering the whole frame: R skin, G face skin, B hair, A person. Temporally smoothed.
 * NULL if nothing new. */
uint8_t *face_tracker_take_mask(face_tracker_t *ft, uint64_t *serial, uint32_t *w, uint32_t *h);

/* Skin pigment basis in optical density: melanin / hemoglobin unit directions and the inverse
 * of [mel hem (1,1,1)/sqrt3] (row-major; row 0 gives melanin, row 1 hemoglobin). Physical
 * priors until the first calibration from the user's own skin. */
typedef struct {
	bool calibrated;
	float mel[3], hem[3];
	float inv[9];
} ft_pigment;
bool face_tracker_get_pigment(face_tracker_t *ft, ft_pigment *out);

/* Running stats for the log: mean analysis time (ms) and fraction of frames re-detected. */
void face_tracker_stats(face_tracker_t *ft, float *mean_ms, float *detect_ratio);

#ifdef __cplusplus
}
#endif
