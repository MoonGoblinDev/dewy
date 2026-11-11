#pragma once

#include <stdbool.h>
#include "face-tracker.h"

#ifdef __cplusplus
extern "C" {
#endif

/* All strengths are slider/100: 0..1 for one-sided controls, -1..1 for bipolar ones. */
struct beauty_shape {
	/* Face: natural, slim, V-shape, small, narrow, cheekbone, jawbone, chin, forehead, head size */
	float face_natural, face_slim, face_v, face_small, face_narrow;
	float cheekbone, jawbone, chin, forehead, head;
	/* Eyes (BigEye, Eye_Spacing, MoveEye, fd_eye (tilt), inner/outer corner, lower eyelid, pupil) */
	float eye_size, eye_spacing, eye_position, eye_tilt;
	float eye_inner, eye_outer, eye_lower_lid, eye_pupil;
	/* Nose (zoom nose, move nose, bridge, tip) */
	float nose_narrow, nose_length, nose_bridge, nose_tip;
	/* Mouth (size, position, smile corners) */
	float mouth_size, mouth_position, mouth_smile;
	/* Eyebrows (thickness, position) */
	float brow_thickness, brow_position;
};

/* Warp primitive types (must match beauty-final.effect). */
enum { WARP_TRANSLATE = 0, WARP_SCALE = 1, WARP_SCALE_DIR = 2 };

#define MAX_WARPS 64
#define MAX_REGIONS 32
#define DATA_TEX_WIDTH (MAX_WARPS * 2 + MAX_REGIONS * 2)

/* Region slots (must match beauty-final.effect). */
enum {
	REGION_FACE = 0,
	REGION_EYE_L,
	REGION_EYE_R,
	REGION_BROW_L,
	REGION_BROW_R,
	REGION_LIPS,
	REGION_MOUTH_INNER,
	REGION_UNDER_EYE_L,
	REGION_UNDER_EYE_R,
	REGION_FOLD_L,
	REGION_FOLD_R,
	REGION_FOREHEAD,
	REGION_CHEEK_L, /* blush (position depends on blush style) */
	REGION_CHEEK_R,
	REGION_SHADOW_L, /* eyeshadow */
	REGION_SHADOW_R,
	REGION_HOLLOW_L, /* contour under the cheekbone */
	REGION_HOLLOW_R,
	REGION_JAW_L, /* contour along the jaw */
	REGION_JAW_R,
	REGION_NOSE_SIDE_L, /* nose contour */
	REGION_NOSE_SIDE_R,
	REGION_HL_NOSE, /* highlights */
	REGION_HL_FOREHEAD,
	REGION_HL_CHIN,
	REGION_HL_CHEEK_L,
	REGION_HL_CHEEK_R,
	REGION_PERSON_HEAD, /* head incl. hair, for lighting / background dim */
	REGION_PERSON_BODY, /* shoulders + torso below the chin */
	REGION_COUNT
};

/* Sticker anchor points (face units, see face_layout.anchor). */
enum fx_anchor {
	ANCHOR_EYES = 0,
	ANCHOR_EYE_L,
	ANCHOR_EYE_R,
	ANCHOR_NOSE,
	ANCHOR_MOUTH,
	ANCHOR_CHIN,
	ANCHOR_FOREHEAD,
	ANCHOR_HEAD_TOP,
	ANCHOR_CHEEK_L,
	ANCHOR_CHEEK_R,
	ANCHOR_COUNT
};

struct face_layout {
	int warp_count;
	/* RGBA32F texels: warps at [0, 2*MAX_WARPS), regions at [2*MAX_WARPS, ...) */
	float data[DATA_TEX_WIDTH * 4];
	float bounds[4]; /* pixel rect containing every warp (x0, y0, x1, y1) */
	float iod_px;    /* inter-ocular distance in output pixels */
	float origin[2]; /* face frame origin (between the eyes) in pixels */
	float axis_u[2]; /* unit eye axis (image-left eye -> image-right eye) */
	float anchor[ANCHOR_COUNT][2]; /* face units (x along eyes, y down, IOD = 1) */
	bool has_face;
};

/* Face units -> output pixels. */
void face_layout_map(const struct face_layout *L, float fx, float fy, float *x, float *y);

enum blush_style { BLUSH_APPLE = 0, BLUSH_LIFTED = 1, BLUSH_SUNKISSED = 2 };

/* Builds warp primitives + region ellipses in output-pixel coordinates.
 * `weight` (0..1) fades everything in/out when the face appears or disappears. */
void face_layout_build(struct face_layout *out, const ft_face *face, const struct beauty_shape *shape,
		       float width, float height, float weight, int blush_style);

/* Points of a landmark region in pixels, sorted by angle around their centroid
 * (a closed ring suitable for a triangle fan). Returns the point count. */
int face_ring(const ft_face *face, enum ft_region r, float width, float height, float *xy, int max_points);

/* Smooth closed outlines (Catmull-Rom through the landmarks) for the lipstick mask.
 * The mouth opening adds the mouth corners, which Vision's 6 inner-lip points lack, so
 * an open mouth is cut out completely. Return point counts (0 if unavailable). */
int face_lips_outer(const ft_face *face, float width, float height, float *xy, int max_points);
int face_mouth_opening(const ft_face *face, float width, float height, float *xy, int max_points);

#ifdef __cplusplus
}
#endif
