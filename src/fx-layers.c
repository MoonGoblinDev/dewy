#include "fx-layers.h"

#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/platform.h>
#include <math.h>
#include <string.h>

#define FILTER_LUT_N 25

struct fx_layers {
	/* filter */
	char filter_name[96];
	float filter_amt;
	gs_texture_t *filter_lut;

	/* light */
	char light_name[96];
	float light_amt;
	struct light_params light;
	bool light_on;

	/* atmosphere + motion */
	char atmo_name[96], motion_name[96];
	float atmo_amt, motion_amt;
	struct atmo_params atmo;
	struct motion_params motion;

	/* effect */
	char effect_name[96];
	struct fx_params effect;
	gs_texture_t *sprite_tex[FX_MAX_SPRITES];
	obs_data_t *effect_shape;

	gs_effect_t *post;
	gs_texrender_t *tr_lut, *tr_fx, *tr_post;

	/* per-tab switches */
	bool effects_on, atmo_on, filters_on, lights_on;
};

static gs_effect_t *load_effect_src(const char *name)
{
	static long counter;
	char *path = obs_module_file(name);
	char *src = path ? os_quick_read_utf8_file(path) : NULL;
	char *err = NULL;
	gs_effect_t *e = NULL;
	if (src) {
		char unique[600];
		snprintf(unique, sizeof(unique), "%s#fx%ld", path, ++counter);
		e = gs_effect_create(src, unique, &err);
	}
	if (!e)
		blog(LOG_ERROR, "[obs-beauty] failed to load %s: %s", name, err ? err : "(missing)");
	bfree(err);
	bfree(src);
	bfree(path);
	return e;
}

struct fx_layers *fx_layers_create(void)
{
	struct fx_layers *fx = bzalloc(sizeof(*fx));
	obs_enter_graphics();
	fx->post = load_effect_src("effects/post.effect");
	fx->tr_lut = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	fx->tr_fx = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	fx->tr_post = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();
	return fx;
}

static void free_sprites(struct fx_layers *fx)
{
	for (int i = 0; i < FX_MAX_SPRITES; i++) {
		gs_texture_destroy(fx->sprite_tex[i]);
		fx->sprite_tex[i] = NULL;
	}
	obs_data_release(fx->effect.shape);
	fx->effect.shape = NULL;
	fx->effect.sprite_count = 0;
}

void fx_layers_destroy(struct fx_layers *fx)
{
	if (!fx)
		return;
	gs_effect_destroy(fx->post);
	gs_texrender_destroy(fx->tr_lut);
	gs_texrender_destroy(fx->tr_fx);
	gs_texrender_destroy(fx->tr_post);
	gs_texture_destroy(fx->filter_lut);
	free_sprites(fx);
	bfree(fx);
}

static bool changed(char *dst, size_t n, const char *src)
{
	if (strcmp(dst, src ? src : "") == 0)
		return false;
	snprintf(dst, n, "%s", src ? src : "");
	return true;
}

void fx_layers_update(struct fx_layers *fx, obs_data_t *s)
{
	fx->effects_on = obs_data_get_bool(s, "effects_on");
	fx->atmo_on = obs_data_get_bool(s, "atmo_on");
	fx->filters_on = obs_data_get_bool(s, "filters_on");
	fx->lights_on = obs_data_get_bool(s, "light_on");
	fx->filter_amt = fx->filters_on ? (float)obs_data_get_int(s, "filter_preset_amt") / 100.0f : 0.0f;
	fx->light_amt = fx->lights_on ? (float)obs_data_get_int(s, "light_amt") / 100.0f : 0.0f;
	fx->atmo_amt = fx->atmo_on ? (float)obs_data_get_int(s, "atmo_amt") / 100.0f : 0.0f;
	fx->motion_amt = fx->atmo_on ? (float)obs_data_get_int(s, "motion_amt") / 100.0f : 0.0f;

	if (changed(fx->filter_name, sizeof(fx->filter_name), obs_data_get_string(s, "filter_preset"))) {
		struct grade_params g;
		obs_enter_graphics();
		gs_texture_destroy(fx->filter_lut);
		fx->filter_lut = NULL;
		if (presets_filter(fx->filter_name, &g)) {
			uint8_t *px = bmalloc(FILTER_LUT_N * FILTER_LUT_N * FILTER_LUT_N * 4);
			grade_build_lut(&g, FILTER_LUT_N, px);
			const uint8_t *data = px;
			fx->filter_lut = gs_texture_create(FILTER_LUT_N * FILTER_LUT_N, FILTER_LUT_N, GS_RGBA, 1, &data, 0);
			bfree(px);
		}
		obs_leave_graphics();
	}
	if (changed(fx->light_name, sizeof(fx->light_name), obs_data_get_string(s, "light_preset")))
		fx->light_on = presets_light(fx->light_name, &fx->light);
	if (changed(fx->atmo_name, sizeof(fx->atmo_name), obs_data_get_string(s, "atmo_preset"))) {
		if (!presets_atmo(fx->atmo_name, &fx->atmo))
			memset(&fx->atmo, 0, sizeof(fx->atmo));
	}
	if (changed(fx->motion_name, sizeof(fx->motion_name), obs_data_get_string(s, "motion_preset"))) {
		if (!presets_motion(fx->motion_name, &fx->motion))
			memset(&fx->motion, 0, sizeof(fx->motion));
	}
	if (changed(fx->effect_name, sizeof(fx->effect_name), obs_data_get_string(s, "effect_preset"))) {
		obs_enter_graphics();
		free_sprites(fx);
		if (presets_effect(fx->effect_name, &fx->effect)) {
			for (int i = 0; i < fx->effect.sprite_count; i++) {
				char rel[128];
				snprintf(rel, sizeof(rel), "stickers/%s", fx->effect.sprites[i].image);
				char *path = obs_module_file(rel);
				if (path)
					fx->sprite_tex[i] = gs_texture_create_from_file(path);
				if (!fx->sprite_tex[i])
					blog(LOG_WARNING, "[obs-beauty] sticker not found: %s", rel);
				bfree(path);
			}
		}
		obs_leave_graphics();
	}
}

static bool post_active(const struct fx_layers *fx)
{
	return (fx->atmo.style && fx->atmo_amt > 0) || (fx->motion.style && fx->motion_amt > 0);
}

static bool effect_active(const struct fx_layers *fx)
{
	return fx->effects_on && (fx->effect.sprite_count > 0 || fx->effect.shape);
}

bool fx_layers_active(const struct fx_layers *fx)
{
	return (fx->filter_lut && fx->filter_amt > 0) || (fx->light_on && fx->light_amt > 0) || post_active(fx) ||
	       effect_active(fx);
}

bool fx_layers_needs_face(const struct fx_layers *fx)
{
	return (fx->light_on && fx->light_amt > 0) || effect_active(fx);
}

bool fx_layers_needs_person(const struct fx_layers *fx)
{
	return fx->light_on && fx->light_amt > 0;
}

/* Effect distortions use the same keys as the reshape sliders (as 0..1 floats; may exceed 1). */
void fx_layers_apply_shape(const struct fx_layers *fx, struct beauty_shape *sh)
{
	if (fx->effects_on)
		presets_apply_shape(fx->effect.shape, sh);
}

void fx_layers_set_final_params(struct fx_layers *fx, gs_effect_t *fe, gs_texture_t *fallback)
{
#define P(n) gs_effect_get_param_by_name(fe, n)
	(void)fallback;
	const struct light_params *l = &fx->light;
	float amt = fx->light_on ? fx->light_amt : 0.0f;
	struct vec4 v;
	gs_effect_set_float(P("light_amt"), amt);
	vec4_set(&v, l->key_dir[0], l->key_dir[1], l->key_dir[2], 0);
	gs_effect_set_vec4(P("light_dir"), &v);
	vec4_set(&v, l->key_color[0], l->key_color[1], l->key_color[2], l->key_amt);
	gs_effect_set_vec4(P("light_key"), &v);
	vec4_set(&v, l->rim_color[0], l->rim_color[1], l->rim_color[2], l->rim_amt);
	gs_effect_set_vec4(P("light_rim"), &v);
	vec4_set(&v, l->fill, l->bg_dim, l->exposure, l->warmth);
	gs_effect_set_vec4(P("light_misc"), &v);
#undef P
}

static void draw_sprites(struct fx_layers *fx, const struct face_layout *L, float face_weight, float t)
{
	gs_effect_t *e = fx->post;
	gs_eparam_t *img = gs_effect_get_param_by_name(e, "image");
	gs_eparam_t *opacity = gs_effect_get_param_by_name(e, "opacity");
	float roll = atan2f(L->axis_u[1], L->axis_u[0]);

	for (int i = 0; i < fx->effect.sprite_count; i++) {
		gs_texture_t *tex = fx->sprite_tex[i];
		const struct fx_sprite *s = &fx->effect.sprites[i];
		if (!tex || s->width <= 0)
			continue;
		float ph = t * s->anim_speed * 2.0f * (float)M_PI + s->phase * 6.283f;
		float ox = s->offset[0], oy = s->offset[1], scale = 1.0f, rot = s->rotation, alpha = face_weight;
		switch (s->anim) {
		case FX_ANIM_BOB:
			oy += sinf(ph) * s->anim_amt;
			break;
		case FX_ANIM_PULSE:
			scale += sinf(ph) * s->anim_amt;
			break;
		case FX_ANIM_SPIN:
			rot += t * s->anim_speed * 360.0f;
			break;
		case FX_ANIM_SWAY:
			rot += sinf(ph) * s->anim_amt * 25.0f;
			break;
		case FX_ANIM_TWINKLE:
			alpha *= 0.55f + 0.45f * sinf(ph);
			scale += 0.08f * sinf(ph);
			break;
		default:
			break;
		}
		const float *a = L->anchor[s->anchor];
		float cx, cy;
		face_layout_map(L, a[0] + ox, a[1] + oy, &cx, &cy);
		float wpx = s->width * L->iod_px * scale;
		float hpx = wpx * (float)gs_texture_get_height(tex) / (float)gs_texture_get_width(tex);

		switch (s->blend) {
		case FX_BLEND_ADD:
			gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_ONE);
			break;
		case FX_BLEND_MULTIPLY:
			gs_blend_function(GS_BLEND_DSTCOLOR, GS_BLEND_INVSRCALPHA);
			break;
		default:
			gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
		}
		gs_effect_set_texture(img, tex);
		gs_effect_set_float(opacity, alpha);
		gs_matrix_push();
		gs_matrix_translate3f(cx, cy, 0.0f);
		gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, roll + rot * (float)M_PI / 180.0f);
		gs_matrix_translate3f(-wpx * 0.5f, -hpx * 0.5f, 0.0f);
		while (gs_effect_loop(e, "Sprite"))
			gs_draw_sprite(tex, 0, (uint32_t)wpx, (uint32_t)hpx);
		gs_matrix_pop();
	}
}

gs_texture_t *fx_layers_render(struct fx_layers *fx, gs_texture_t *in, uint32_t w, uint32_t h,
			       const struct face_layout *L, float face_weight, float t)
{
	if (!fx->post)
		return in;
	gs_texture_t *cur = in;

	/* Filters tab LUT (its own pass: the final effect is at libobs's 8-texture limit) */
	if (fx->filter_lut && fx->filter_amt > 0) {
		gs_effect_t *e = fx->post;
		gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), cur);
		gs_effect_set_texture(gs_effect_get_param_by_name(e, "filter_lut_tex"), fx->filter_lut);
		gs_effect_set_float(gs_effect_get_param_by_name(e, "filter_lut_size"), (float)FILTER_LUT_N);
		gs_effect_set_float(gs_effect_get_param_by_name(e, "filter_lut_amt"), fx->filter_amt);
		gs_texrender_reset(fx->tr_lut);
		if (gs_texrender_begin(fx->tr_lut, w, h)) {
			gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
			gs_enable_blending(false);
			while (gs_effect_loop(e, "FilterLut"))
				gs_draw_sprite(cur, 0, w, h);
			gs_texrender_end(fx->tr_lut);
			cur = gs_texrender_get_texture(fx->tr_lut);
		}
	}

	/* stickers */
	if (fx->effects_on && fx->effect.sprite_count > 0 && L->has_face && face_weight > 0.01f) {
		gs_texrender_reset(fx->tr_fx);
		if (gs_texrender_begin(fx->tr_fx, w, h)) {
			gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
			gs_enable_blending(false);
			gs_effect_set_texture(gs_effect_get_param_by_name(fx->post, "image"), cur);
			while (gs_effect_loop(fx->post, "Copy"))
				gs_draw_sprite(cur, 0, w, h);
			gs_enable_blending(true);
			draw_sprites(fx, L, face_weight, t);
			gs_enable_blending(false);
			gs_texrender_end(fx->tr_fx);
			cur = gs_texrender_get_texture(fx->tr_fx);
		}
	}

	/* motion + atmosphere */
	if (post_active(fx)) {
		gs_effect_t *e = fx->post;
#define P(n) gs_effect_get_param_by_name(e, n)
		struct vec2 size;
		vec2_set(&size, (float)w, (float)h);
		gs_effect_set_texture(P("image"), cur);
		gs_effect_set_vec2(P("size"), &size);
		gs_effect_set_float(P("time"), t);
		gs_effect_set_int(P("motion_style"), fx->motion_amt > 0 ? fx->motion.style : 0);
		gs_effect_set_float(P("motion_amt"), fx->motion_amt);
		gs_effect_set_float(P("motion_speed"), fx->motion.speed);
		gs_effect_set_int(P("atmo_style"), fx->atmo_amt > 0 ? fx->atmo.style : 0);
		gs_effect_set_float(P("atmo_amt"), fx->atmo_amt);
		struct vec4 a, b, p;
		vec4_set(&a, fx->atmo.color_a[0], fx->atmo.color_a[1], fx->atmo.color_a[2], 1);
		vec4_set(&b, fx->atmo.color_b[0], fx->atmo.color_b[1], fx->atmo.color_b[2], 1);
		vec4_set(&p, fx->atmo.density, fx->atmo.speed, fx->atmo.size, 0);
		gs_effect_set_vec4(P("color_a"), &a);
		gs_effect_set_vec4(P("color_b"), &b);
		gs_effect_set_vec4(P("atmo_params"), &p);
#undef P
		gs_texrender_reset(fx->tr_post);
		if (gs_texrender_begin(fx->tr_post, w, h)) {
			gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
			gs_enable_blending(false);
			while (gs_effect_loop(e, "Post"))
				gs_draw_sprite(cur, 0, w, h);
			gs_texrender_end(fx->tr_post);
			cur = gs_texrender_get_texture(fx->tr_post);
		}
	}
	return cur;
}
