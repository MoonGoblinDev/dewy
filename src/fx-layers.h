#pragma once

/* Effects / Atmosphere / Filters / Light layers that sit on top of the beauty pipeline. */

#include <obs-module.h>
#include "face-geometry.h"
#include "presets.h"

struct fx_layers;

struct fx_layers *fx_layers_create(void);
void fx_layers_destroy(struct fx_layers *fx); /* inside graphics context */

/* Reads filter_preset / light_preset / atmo_preset / motion_preset / effect_preset and their
 * amounts; (re)builds LUTs and loads sticker textures when presets change. */
void fx_layers_update(struct fx_layers *fx, obs_data_t *settings);

bool fx_layers_active(const struct fx_layers *fx);
bool fx_layers_needs_face(const struct fx_layers *fx);
bool fx_layers_needs_person(const struct fx_layers *fx); /* light uses the body mask */

/* Adds the selected effect's distortion to the user's reshape sliders. */
void fx_layers_apply_shape(const struct fx_layers *fx, struct beauty_shape *shape);

/* Light + filter-LUT uniforms for beauty-final.effect. */
void fx_layers_set_final_params(struct fx_layers *fx, gs_effect_t *final_effect, gs_texture_t *fallback);

/* Stickers, then motion + atmosphere. Returns the texture to show (may be `in`). */
gs_texture_t *fx_layers_render(struct fx_layers *fx, gs_texture_t *in, uint32_t w, uint32_t h,
			       const struct face_layout *layout, float face_weight, float time);
