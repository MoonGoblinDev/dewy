#pragma once

/* Preset libraries for the Filters / Light / Atmosphere / Effects tabs, loaded from
 * data/{filters,lights,atmosphere,effects}.json. Shared by the filter and the dock. */

#include <obs-module.h>
#include "grade.h"
#include "face-geometry.h"

#ifdef __cplusplus
extern "C" {
#endif

struct light_params {
	float key_dir[3]; /* towards the light, face frame: x right, y down, z to camera */
	float key_color[3], key_amt;
	float rim_color[3], rim_amt;
	float fill;   /* how dark the unlit side gets (0..1) */
	float bg_dim; /* darken everything outside the person (0..1) */
	float exposure, warmth;
};

enum atmo_style {
	ATMO_NONE = 0,
	ATMO_SPARKLE,
	ATMO_BOKEH,
	ATMO_HEARTS,
	ATMO_PETALS,
	ATMO_SNOW,
	ATMO_FIREFLIES,
	ATMO_LEAK,
	ATMO_MIST,
	ATMO_GLITTER,
	ATMO_DREAMY,
	ATMO_CONFETTI,
	ATMO_BUBBLES,
};

enum motion_style { MOTION_NONE = 0, MOTION_SHAKE, MOTION_PULSE, MOTION_RGB, MOTION_DRIFT, MOTION_GLITCH, MOTION_WOBBLE };

struct atmo_params {
	int style;
	float color_a[3], color_b[3];
	float density, speed, size;
};

struct motion_params {
	int style;
	float speed;
};

enum fx_blend { FX_BLEND_NORMAL = 0, FX_BLEND_ADD, FX_BLEND_MULTIPLY };
enum fx_anim { FX_ANIM_NONE = 0, FX_ANIM_BOB, FX_ANIM_PULSE, FX_ANIM_SPIN, FX_ANIM_SWAY, FX_ANIM_TWINKLE };

#define FX_MAX_SPRITES 8

struct fx_sprite {
	char image[64]; /* file in data/stickers/ */
	int anchor;
	float offset[2]; /* face units (IOD), from the anchor */
	float width;     /* face units */
	float rotation;  /* degrees, on top of head roll */
	int blend, anim;
	float anim_amt, anim_speed, phase;
};

struct fx_params {
	int sprite_count;
	struct fx_sprite sprites[FX_MAX_SPRITES];
	obs_data_t *shape; /* slider-key -> value overrides for the warp engine (caller releases) */
};

void presets_load(void);
void presets_free(void);

/* Raw libraries for UIs (borrowed; do not release). */
obs_data_t *presets_filters_root(void);
obs_data_t *presets_lights_root(void);
obs_data_t *presets_atmosphere_root(void);
obs_data_t *presets_effects_root(void);

bool presets_filter(const char *name, struct grade_params *out);
bool presets_light(const char *name, struct light_params *out);
bool presets_atmo(const char *name, struct atmo_params *out);
bool presets_motion(const char *name, struct motion_params *out);
bool presets_effect(const char *name, struct fx_params *out);

/* Parsers usable on any preset object (the dock uses these for thumbnails). */
void presets_parse_grade(obs_data_t *d, struct grade_params *g);
void presets_parse_light(obs_data_t *d, struct light_params *l);
void presets_parse_effect(obs_data_t *d, struct fx_params *fx);
int presets_anchor_from_name(const char *name);

/* Adds {"eye_size": 2.0, ...} (reshape-slider keys, 0..1 scale, may exceed 1) to a shape. */
void presets_apply_shape(obs_data_t *shape_obj, struct beauty_shape *sh);

#ifdef __cplusplus
}
#endif
