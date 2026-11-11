#pragma once

#include <obs-module.h>

/* Makeup tab: a preset (filter grade + makeup look) with two global intensities (Filter and
 * Makeup), plus every part exposed for customising. */
struct makeup_params {
	float filter_amt, makeup_amt; /* 0..1 */

	/* colour grade, -1..1 */
	float exposure, contrast, saturation, warmth, tint, fade;
	char lut_path[512];

	/* makeup parts: colours are linear 0..1 RGB as stored, opacities 0..1 */
	float lip[3], lip_opacity, lip_gloss;
	float blush[3], blush_opacity;
	int blush_style;
	float shadow[3], shadow_opacity;
	float liner, contour, highlight, brow;
};

void makeup_presets_load(void);
void makeup_presets_free(void);

void makeup_defaults(obs_data_t *settings);
void makeup_add_properties(obs_properties_t *props);

/* If the chosen preset changed, copy its values into the custom fields. Returns true when
 * it did (so a properties view should refresh). */
bool makeup_sync_preset(obs_data_t *settings);

/* Resets the makeup tab to "None". */
void makeup_reset(obs_data_t *settings);

void makeup_read(struct makeup_params *mk, obs_data_t *settings);

/* Loads a .cube 3D LUT into an (N*N) x N RGBA texture. Call inside the graphics context. */
gs_texture_t *makeup_load_cube(const char *path, int *size_out);
