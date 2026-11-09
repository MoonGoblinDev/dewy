/*
 * "Beauty Enhance" video filter.
 *
 * Per frame (once, even if OBS renders the source several times):
 *   1. render the filtered source into a texture
 *   2. downscale to ~640x360 BGRA, stage it, and hand the previous frame's copy to Apple
 *      Vision on a worker thread (async: never stalls the GPU)
 *   3. turn the latest landmarks + sliders into warp primitives / region ellipses
 *   4. half-res bilateral (smoothing) and 1/8-res gaussian (tone) passes
 *   5. final composite (beauty-final.effect), cached for any further renders this frame
 */

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/platform.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "face-geometry.h"
#include "face-tracker.h"
#include "makeup.h"
#include "fx-layers.h"
#include "beauty-api.h"
#include <util/threading.h>

#define TRACK_SHORT_SIDE 360.0f

struct beauty_filter {
	obs_source_t *source;

	gs_effect_t *blur_effect;
	gs_effect_t *final_effect;

	gs_texrender_t *tr_ring[2], *tr_small, *tr_half, *tr_tmp, *tr_smooth, *tr_q, *tr_e, *tr_tmp_e, *tr_tone, *tr_out;
	gs_stagesurf_t *stage[2];
	bool staged[2];
	int stage_idx;
	uint32_t stage_w, stage_h;
	gs_texture_t *data_tex;
	gs_texrender_t *tr_mask, *tr_mask_tmp; /* R lips, G eyeliner, B eye interior */
	gs_texture_t *lut_tex;
	int lut_size;
	char lut_loaded[512];

	face_tracker_t *tracker;
	struct face_layout layout;
	ft_face face;
	float face_weight;
	uint64_t last_ns;
	bool processed; /* output already rendered this frame */
	uint64_t frame;          /* render frame counter (tags tracker results) */
	uint64_t ring_id[2];     /* frame held in tr_ring[i] */
	uint32_t ring_w[2], ring_h[2];
	uint64_t stage_id[2];    /* frame staged in stage[i] */
	bool sync_tracking;

	/* settings (0..1 or -1..1) */
	float smooth, foundation, skin_tone, redness, pouch, fold, teeth, eyelight, clarity;
	struct beauty_shape shape;
	bool all_skin;
	bool show_landmarks;
	int debug_view;
	struct makeup_params mk;
	bool enhance_on; /* Enhance tab (appearance + makeup) switch */

	/* Effects / Atmosphere / Filters / Light */
	struct fx_layers *fx;
	gs_texture_t *present; /* texture shown this frame */
	uint64_t start_ns;

	/* latest small camera frame + its id, for the dock's live thumbnails */
	pthread_mutex_t snap_mutex;
	uint8_t *snap;
	uint32_t snap_w, snap_h;
	uint64_t snap_id;

	/* segmentation mask (R skin, G face skin, B hair, A person) */
	gs_texrender_t *tr_seg_ref, *tr_skin_q, *tr_skin_e, *tr_skin_t, *tr_skin_tone;
	gs_texrender_t *tr_tone_pb;
	gs_texture_t *person_tex;
	gs_texrender_t *tr_pm1, *tr_pm2;
	uint32_t pm_w, pm_h;
	uint64_t pm_serial;
	uint8_t *pm_cpu; /* latest mask copy for thumbnails (snap_mutex) */
	volatile uint64_t person_preview_until; /* dock is showing Light previews */
};

/* ---------------- settings table ---------------- */

enum group { G_SKIN, G_FACE, G_EYES, G_NOSE, G_MOUTH, G_BROWS, G_COUNT };
static const char *group_names[G_COUNT] = {"Skin", "Face", "Eyes", "Nose", "Mouth", "Eyebrows"};

struct slider {
	const char *key;
	const char *text;
	enum group grp;
	bool bipolar;
	int def;
	size_t off;
};

#define SK(f) offsetof(struct beauty_filter, f)
#define SH(f) offsetof(struct beauty_filter, shape.f)

static const struct slider sliders[] = {
	{"smooth", "Smooth", G_SKIN, false, 50, SK(smooth)},
	{"foundation", "Foundation", G_SKIN, false, 20, SK(foundation)},
	{"skin_tone", "Skin tone", G_SKIN, true, 0, SK(skin_tone)},
	{"redness", "Redness", G_SKIN, false, 0, SK(redness)},
	{"shadows", "Shadows (dark circles)", G_SKIN, false, 0, SK(pouch)},
	{"degree", "Degree (smile lines)", G_SKIN, false, 0, SK(fold)},
	{"teeth", "Brighten teeth", G_SKIN, false, 0, SK(teeth)},
	{"eyelight", "Bright eyes", G_SKIN, false, 30, SK(eyelight)},
	{"contrast", "Contrast (clarity)", G_SKIN, false, 0, SK(clarity)},

	{"face_natural", "Natural", G_FACE, false, 0, SH(face_natural)},
	{"face_slim", "Slim", G_FACE, false, 0, SH(face_slim)},
	{"face_v", "V-shape", G_FACE, false, 0, SH(face_v)},
	{"face_narrow", "Narrow", G_FACE, false, 0, SH(face_narrow)},
	{"face_small", "Small face", G_FACE, false, 0, SH(face_small)},
	{"cheekbone", "Cheekbones", G_FACE, false, 0, SH(cheekbone)},
	{"jawbone", "Jawbone", G_FACE, false, 0, SH(jawbone)},
	{"chin", "Chin length", G_FACE, true, 0, SH(chin)},
	{"forehead", "Forehead", G_FACE, true, 0, SH(forehead)},
	{"head", "Head size", G_FACE, false, 0, SH(head)},

	{"eye_size", "Enlarge", G_EYES, false, 0, SH(eye_size)},
	{"eye_spacing", "Distance", G_EYES, true, 0, SH(eye_spacing)},
	{"eye_position", "Position", G_EYES, true, 0, SH(eye_position)},
	{"eye_tilt", "Tilt", G_EYES, true, 0, SH(eye_tilt)},
	{"eye_inner", "Inner corner", G_EYES, false, 0, SH(eye_inner)},
	{"eye_outer", "Outer corner", G_EYES, true, 0, SH(eye_outer)},
	{"eye_lower_lid", "Lower eyelid", G_EYES, false, 0, SH(eye_lower_lid)},
	{"eye_pupil", "Iris size", G_EYES, false, 0, SH(eye_pupil)},

	{"nose_narrow", "Narrow", G_NOSE, false, 0, SH(nose_narrow)},
	{"nose_length", "Lift / length", G_NOSE, true, 0, SH(nose_length)},
	{"nose_bridge", "Bridge", G_NOSE, false, 0, SH(nose_bridge)},
	{"nose_tip", "Tip", G_NOSE, false, 0, SH(nose_tip)},

	{"mouth_size", "Size", G_MOUTH, true, 0, SH(mouth_size)},
	{"mouth_position", "Position", G_MOUTH, true, 0, SH(mouth_position)},
	{"mouth_smile", "Smile", G_MOUTH, false, 0, SH(mouth_smile)},

	{"brow_thickness", "Thickness", G_BROWS, true, 0, SH(brow_thickness)},
	{"brow_position", "Position", G_BROWS, true, 0, SH(brow_position)},
};
#define NUM_SLIDERS (sizeof(sliders) / sizeof(sliders[0]))

static float *slider_ptr(struct beauty_filter *f, const struct slider *s)
{
	return (float *)((char *)f + s->off);
}

/* ---------------- lifecycle ---------------- */

static const char *beauty_get_name(void *unused)
{
	return obs_module_text("Beauty Enhance");
}

static void beauty_update(void *data, obs_data_t *settings)
{
	struct beauty_filter *f = data;
	makeup_sync_preset(settings);
	makeup_read(&f->mk, settings);
	if (strcmp(f->mk.lut_path, f->lut_loaded) != 0) {
		obs_enter_graphics();
		gs_texture_destroy(f->lut_tex);
		f->lut_tex = NULL;
		f->lut_size = 0;
		if (f->mk.lut_path[0])
			f->lut_tex = makeup_load_cube(f->mk.lut_path, &f->lut_size);
		obs_leave_graphics();
		snprintf(f->lut_loaded, sizeof(f->lut_loaded), "%s", f->mk.lut_path);
	}
	for (size_t i = 0; i < NUM_SLIDERS; i++)
		*slider_ptr(f, &sliders[i]) = (float)obs_data_get_int(settings, sliders[i].key) / 100.0f;
	f->all_skin = obs_data_get_bool(settings, "all_skin");
	f->show_landmarks = obs_data_get_bool(settings, "show_landmarks");
	f->sync_tracking = obs_data_get_bool(settings, "sync_tracking");
	f->debug_view = (int)obs_data_get_int(settings, "debug_view");
	f->enhance_on = obs_data_get_bool(settings, "enhance_on");
	if (f->fx)
		fx_layers_update(f->fx, settings);
}

/* Compiled from source rather than gs_effect_create_from_file(): libobs caches effects by
 * path for the whole session, which would make shader edits invisible until OBS restarts. */
static gs_effect_t *load_effect(const char *name)
{
	static long counter;
	char *path = obs_module_file(name);
	char *src = path ? os_quick_read_utf8_file(path) : NULL;
	char *err = NULL;
	gs_effect_t *e = NULL;
	if (src) {
		char unique[600];
		snprintf(unique, sizeof(unique), "%s#%ld", path, ++counter);
		e = gs_effect_create(src, unique, &err);
	}
	bfree(src);
	if (!e)
		blog(LOG_ERROR, "[obs-beauty] failed to load %s: %s", path ? path : name, err ? err : "(unknown)");
	bfree(err);
	bfree(path);
	return e;
}

static void *beauty_create(obs_data_t *settings, obs_source_t *source)
{
	struct beauty_filter *f = bzalloc(sizeof(*f));
	f->source = source;
	char *models = obs_module_file("models");
	f->tracker = face_tracker_create(models);
	bfree(models);
	f->fx = fx_layers_create();
	f->start_ns = os_gettime_ns();
	pthread_mutex_init(&f->snap_mutex, NULL);

	obs_enter_graphics();
	f->blur_effect = load_effect("effects/beauty-blur.effect");
	f->final_effect = load_effect("effects/beauty-final.effect");
	gs_texrender_t **trs[] = {&f->tr_ring[0], &f->tr_ring[1], &f->tr_half, &f->tr_tmp, &f->tr_smooth, &f->tr_q,
				  &f->tr_e,   &f->tr_tmp_e, &f->tr_tone, &f->tr_out};
	for (size_t i = 0; i < sizeof(trs) / sizeof(trs[0]); i++)
		*trs[i] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->tr_small = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	f->tr_mask = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->tr_mask_tmp = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->tr_pm1 = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->tr_seg_ref = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->tr_skin_q = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	f->tr_skin_e = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	f->tr_skin_t = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	f->tr_skin_tone = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	f->tr_tone_pb = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	f->tr_pm2 = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->data_tex = gs_texture_create(DATA_TEX_WIDTH, 1, GS_RGBA32F, 1, NULL, GS_DYNAMIC);
	obs_leave_graphics();

	beauty_update(f, settings);
	return f;
}

static void beauty_destroy(void *data)
{
	struct beauty_filter *f = data;
	obs_enter_graphics();
	gs_effect_destroy(f->blur_effect);
	gs_effect_destroy(f->final_effect);
	gs_texrender_t *trs[] = {f->tr_ring[0], f->tr_ring[1], f->tr_small, f->tr_half, f->tr_tmp,     f->tr_smooth,
				 f->tr_q,   f->tr_e,     f->tr_tmp_e, f->tr_tone, f->tr_out};
	for (size_t i = 0; i < sizeof(trs) / sizeof(trs[0]); i++)
		gs_texrender_destroy(trs[i]);
	gs_stagesurface_destroy(f->stage[0]);
	gs_stagesurface_destroy(f->stage[1]);
	gs_texture_destroy(f->data_tex);
	gs_texture_destroy(f->lut_tex);
	gs_texrender_destroy(f->tr_mask);
	gs_texrender_destroy(f->tr_mask_tmp);
	gs_texrender_destroy(f->tr_pm1);
	gs_texrender_destroy(f->tr_seg_ref);
	gs_texrender_destroy(f->tr_skin_q);
	gs_texrender_destroy(f->tr_skin_e);
	gs_texrender_destroy(f->tr_skin_t);
	gs_texrender_destroy(f->tr_skin_tone);
	gs_texrender_destroy(f->tr_tone_pb);
	gs_texrender_destroy(f->tr_pm2);
	gs_texture_destroy(f->person_tex);
	fx_layers_destroy(f->fx);
	obs_leave_graphics();
	face_tracker_destroy(f->tracker);
	pthread_mutex_destroy(&f->snap_mutex);
	bfree(f->snap);
	free(f->pm_cpu);
	bfree(f);
}

static void beauty_defaults(obs_data_t *settings)
{
	for (size_t i = 0; i < NUM_SLIDERS; i++)
		obs_data_set_default_int(settings, sliders[i].key, sliders[i].def);
	obs_data_set_default_bool(settings, "all_skin", true);
	obs_data_set_default_bool(settings, "show_landmarks", false);
	obs_data_set_default_bool(settings, "sync_tracking", true);
	obs_data_set_default_int(settings, "debug_view", 0);
	makeup_defaults(settings);
	obs_data_set_default_int(settings, "filter_preset_amt", 80);
	obs_data_set_default_bool(settings, "enhance_on", true);
	obs_data_set_default_bool(settings, "effects_on", true);
	obs_data_set_default_bool(settings, "atmo_on", true);
	obs_data_set_default_bool(settings, "filters_on", true);
	obs_data_set_default_bool(settings, "light_on", true);
	obs_data_set_default_int(settings, "light_amt", 60);
	obs_data_set_default_int(settings, "atmo_amt", 70);
	obs_data_set_default_int(settings, "motion_amt", 50);
}

static bool reset_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	struct beauty_filter *f = data;
	obs_data_t *settings = obs_source_get_settings(f->source);
	for (size_t i = 0; i < NUM_SLIDERS; i++)
		obs_data_set_int(settings, sliders[i].key, sliders[i].def);
	makeup_reset(settings);
	obs_source_update(f->source, settings);
	obs_data_release(settings);
	return true;
}

static obs_properties_t *beauty_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_t *groups[G_COUNT];
	for (int g = 0; g < G_COUNT; g++)
		groups[g] = obs_properties_create();

	for (size_t i = 0; i < NUM_SLIDERS; i++) {
		const struct slider *s = &sliders[i];
		obs_property_t *p = obs_properties_add_int_slider(groups[s->grp], s->key, obs_module_text(s->text),
								  s->bipolar ? -100 : 0, 100, 1);
		if (s->bipolar)
			obs_property_set_long_description(p, obs_module_text("Bipolar: 0 is unchanged"));
	}
	for (int g = 0; g < G_COUNT; g++) {
		char name[32];
		snprintf(name, sizeof(name), "group_%d", g);
		obs_properties_add_group(props, name, obs_module_text(group_names[g]), OBS_GROUP_NORMAL, groups[g]);
	}

	makeup_add_properties(props);

	obs_properties_t *adv = obs_properties_create();
	obs_properties_add_bool(adv, "all_skin", obs_module_text("Smooth all visible skin (not just the face)"));
	obs_property_t *sync = obs_properties_add_bool(
		adv, "sync_tracking", obs_module_text("Lock effects to the face (delays video by 1 frame)"));
	obs_property_set_long_description(
		sync, obs_module_text("Shows each frame together with its own face landmarks, so makeup and reshaping "
				      "never trail behind head movement. Adds one frame of video latency."));
	obs_properties_add_bool(adv, "show_landmarks", obs_module_text("Show face landmarks"));
	obs_property_t *dv = obs_properties_add_list(adv, "debug_view", obs_module_text("Debug view"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(dv, obs_module_text("Off"), 0);
	obs_property_list_add_int(dv, obs_module_text("Skin mask"), 1);
	obs_property_list_add_int(dv, obs_module_text("Face regions"), 2);
	obs_property_list_add_int(dv, obs_module_text("Makeup masks"), 3);
	obs_properties_add_group(props, "group_adv", obs_module_text("Advanced"), OBS_GROUP_NORMAL, adv);

	obs_properties_add_button2(props, "reset", obs_module_text("Reset to defaults"), reset_clicked, data);
	return props;
}

static void beauty_tick(void *data, float seconds)
{
	struct beauty_filter *f = data;
	f->processed = false;
}

/* ---------------- rendering helpers ---------------- */

static bool capture_source(struct beauty_filter *f, gs_texrender_t *tr, obs_source_t *target, obs_source_t *parent,
			   uint32_t w, uint32_t h)
{
	gs_texrender_reset(tr);
	if (!gs_texrender_begin(tr, w, h))
		return false;
	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	uint32_t flags = obs_source_get_output_flags(target);
	bool custom_draw = (flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
	bool async = (flags & OBS_SOURCE_ASYNC) != 0;
	if (target == parent && !custom_draw && !async)
		obs_source_default_render(target);
	else
		obs_source_video_render(target);
	gs_blend_state_pop();

	gs_texrender_end(tr);
	return true;
}

static gs_texture_t *blur_pass(struct beauty_filter *f, gs_texrender_t *tr, const char *tech, gs_texture_t *in,
			       uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h, float dx, float dy,
			       float step, float sigma)
{
	gs_effect_t *e = f->blur_effect;
	gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), in);
	struct vec2 texel, dir;
	vec2_set(&texel, 1.0f / (float)in_w, 1.0f / (float)in_h);
	vec2_set(&dir, dx, dy);
	gs_effect_set_vec2(gs_effect_get_param_by_name(e, "texel"), &texel);
	gs_effect_set_vec2(gs_effect_get_param_by_name(e, "dir"), &dir);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "step_px"), step);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "sigma_r"), sigma);

	gs_texrender_reset(tr);
	if (!gs_texrender_begin(tr, out_w, out_h))
		return NULL;
	gs_ortho(0.0f, (float)out_w, 0.0f, (float)out_h, -100.0f, 100.0f);
	while (gs_effect_loop(e, tech))
		gs_draw_sprite(in, 0, out_w, out_h);
	gs_texrender_end(tr);
	return gs_texrender_get_texture(tr);
}

/* Like blur_pass, with a second "guide" texture (mask refinement / skin premultiply). */
static gs_texture_t *guided_pass(struct beauty_filter *f, gs_texrender_t *tr, const char *tech, gs_texture_t *in,
				 gs_texture_t *guide, float texel_x, float texel_y, uint32_t out_w, uint32_t out_h,
				 float sigma)
{
	gs_effect_t *e = f->blur_effect;
	gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), in);
	gs_effect_set_texture(gs_effect_get_param_by_name(e, "guide"), guide);
	struct vec2 texel;
	vec2_set(&texel, texel_x, texel_y);
	gs_effect_set_vec2(gs_effect_get_param_by_name(e, "texel"), &texel);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "sigma_r"), sigma);
	gs_texrender_reset(tr);
	if (!gs_texrender_begin(tr, out_w, out_h))
		return NULL;
	gs_ortho(0.0f, (float)out_w, 0.0f, (float)out_h, -100.0f, 100.0f);
	while (gs_effect_loop(e, tech))
		gs_draw_sprite(in, 0, out_w, out_h);
	gs_texrender_end(tr);
	return gs_texrender_get_texture(tr);
}

static void update_tracking(struct beauty_filter *f, gs_texture_t *src, uint32_t w, uint32_t h, uint64_t id)
{
	float s = fminf(1.0f, TRACK_SHORT_SIDE / (float)(w < h ? w : h));
	uint32_t sw = ((uint32_t)(w * s)) & ~1u, sh = ((uint32_t)(h * s)) & ~1u;
	if (sw < 16 || sh < 16)
		return;

	if (!f->stage[0] || f->stage_w != sw || f->stage_h != sh) {
		gs_stagesurface_destroy(f->stage[0]);
		gs_stagesurface_destroy(f->stage[1]);
		f->stage[0] = gs_stagesurface_create(sw, sh, GS_BGRA);
		f->stage[1] = gs_stagesurface_create(sw, sh, GS_BGRA);
		f->staged[0] = f->staged[1] = false;
		f->stage_w = sw;
		f->stage_h = sh;
	}

	/* Read back the frame staged last time (the GPU has finished with it by now) and analyse
	 * it. Tracking (not re-detecting) takes ~1 ms, so in sync mode we wait briefly for the
	 * result: the previous frame is then displayed with its own landmarks. */
	int prev = f->stage_idx ^ 1;
	if (f->staged[prev]) {
		uint8_t *ptr;
		uint32_t linesize;
		bool sent = false;
		if (gs_stagesurface_map(f->stage[prev], &ptr, &linesize)) {
			sent = face_tracker_submit_bgra(f->tracker, ptr, linesize, sw, sh, f->stage_id[prev],
							os_gettime_ns());
			if (f->stage_id[prev] % 15 == 0) { /* ~4x a second is plenty for thumbnails */
				pthread_mutex_lock(&f->snap_mutex);
				if (f->snap_w != sw || f->snap_h != sh) {
					bfree(f->snap);
					f->snap = bmalloc((size_t)sw * sh * 4);
					f->snap_w = sw;
					f->snap_h = sh;
				}
				for (uint32_t y = 0; y < sh; y++)
					memcpy(f->snap + (size_t)y * sw * 4, ptr + (size_t)y * linesize, (size_t)sw * 4);
				f->snap_id = f->stage_id[prev];
				pthread_mutex_unlock(&f->snap_mutex);
			}
			gs_stagesurface_unmap(f->stage[prev]);
		}
		f->staged[prev] = false;
		if (sent && f->sync_tracking)
			face_tracker_wait(f->tracker, f->stage_id[prev], 4.0f);
	}

	gs_texture_t *small = blur_pass(f, f->tr_small, "Draw", src, w, h, sw, sh, 0, 0, 0, 0);
	if (small) {
		gs_stage_texture(f->stage[f->stage_idx], small);
		f->staged[f->stage_idx] = true;
		f->stage_id[f->stage_idx] = id;
		f->stage_idx ^= 1;
	}
}

static bool makeup_active(const struct beauty_filter *f)
{
	const struct makeup_params *m = &f->mk;
	bool grade = m->filter_amt > 0 && (m->exposure != 0 || m->contrast != 0 || m->saturation != 0 ||
					   m->warmth != 0 || m->tint != 0 || m->fade != 0 || f->lut_tex);
	bool paint = m->makeup_amt > 0 && (m->lip_opacity > 0 || m->blush_opacity > 0 || m->shadow_opacity > 0 ||
					   m->liner > 0 || m->contour > 0 || m->highlight > 0 || m->brow > 0);
	return grade || paint;
}

static bool face_effects_active(const struct beauty_filter *f)
{
	if (makeup_active(f) || fx_layers_active(f->fx))
		return true;
	if (f->pouch > 0 || f->fold > 0 || f->teeth > 0 || f->eyelight > 0 || f->clarity > 0 || !f->all_skin ||
	    f->show_landmarks || f->debug_view)
		return true;
	const float *v = (const float *)&f->shape;
	for (size_t i = 0; i < sizeof(f->shape) / sizeof(float); i++)
		if (v[i] != 0.0f)
			return true;
	return f->smooth > 0 || f->foundation > 0 || f->skin_tone != 0 || f->redness > 0; /* skin colour is sampled from the face */
}

/* ---------------- makeup mask rasterisation ---------------- */

static void draw_fan(const float *xy, int n)
{
	float cx = 0, cy = 0;
	for (int i = 0; i < n; i++) {
		cx += xy[i * 2];
		cy += xy[i * 2 + 1];
	}
	cx /= n;
	cy /= n;
	gs_render_start(true);
	for (int i = 0; i < n; i++) {
		int j = (i + 1) % n;
		gs_vertex2f(cx, cy);
		gs_vertex2f(xy[i * 2], xy[i * 2 + 1]);
		gs_vertex2f(xy[j * 2], xy[j * 2 + 1]);
	}
	gs_render_stop(GS_TRIS);
}

struct lid_pt {
	float t, x, y;
};

static int cmp_lid(const void *a, const void *b)
{
	float d = ((const struct lid_pt *)a)->t - ((const struct lid_pt *)b)->t;
	return (d > 0) - (d < 0);
}

/* Eyeliner: a band along the upper lid that thickens towards the outer corner, plus a wing. */
static void draw_liner(const struct face_layout *L, const float *xy, int n)
{
	float ux = L->axis_u[0], uy = L->axis_u[1], vx = -uy, vy = ux, d = L->iod_px;
	float cx = 0, cy = 0;
	for (int i = 0; i < n; i++) {
		cx += xy[i * 2];
		cy += xy[i * 2 + 1];
	}
	cx /= n;
	cy /= n;
	float out = ((cx - L->origin[0]) * ux + (cy - L->origin[1]) * uy) >= 0 ? 1.0f : -1.0f;

	struct lid_pt lid[FT_MAX_POINTS];
	int m = 0;
	for (int i = 0; i < n; i++) {
		float qx = xy[i * 2] - cx, qy = xy[i * 2 + 1] - cy;
		if (qx * vx + qy * vy <= 0.01f * d) /* on or above the eye's centre line */
			lid[m++] = (struct lid_pt){(qx * ux + qy * uy) * out, xy[i * 2], xy[i * 2 + 1]};
	}
	if (m < 2)
		return;
	qsort(lid, m, sizeof(lid[0]), cmp_lid); /* inner corner -> outer corner */

	gs_render_start(true);
	for (int i = 0; i + 1 < m; i++) {
		float t0 = (float)i / (m - 1), t1 = (float)(i + 1) / (m - 1);
		float w0 = d * (0.012f + 0.03f * t0), w1 = d * (0.012f + 0.03f * t1);
		float ax = lid[i].x, ay = lid[i].y, bx = lid[i + 1].x, by = lid[i + 1].y;
		gs_vertex2f(ax, ay);
		gs_vertex2f(bx, by);
		gs_vertex2f(ax - vx * w0, ay - vy * w0);
		gs_vertex2f(bx, by);
		gs_vertex2f(bx - vx * w1, by - vy * w1);
		gs_vertex2f(ax - vx * w0, ay - vy * w0);
	}
	struct lid_pt o = lid[m - 1];
	float wl = d * 0.042f;
	gs_vertex2f(o.x, o.y);
	gs_vertex2f(o.x - vx * wl, o.y - vy * wl);
	gs_vertex2f(o.x + ux * out * d * 0.11f - vx * d * 0.06f, o.y + uy * out * d * 0.11f - vy * d * 0.06f);
	gs_render_stop(GS_TRIS);
}

/* Thick band through points (sorted along the eye axis), half-width hw px. */
static void draw_band(const struct face_layout *L, const float *xy, int n, float hw)
{
	float ux = L->axis_u[0], uy = L->axis_u[1], vx = -uy, vy = ux;
	struct lid_pt pts[FT_MAX_POINTS];
	for (int i = 0; i < n; i++)
		pts[i] = (struct lid_pt){xy[i * 2] * ux + xy[i * 2 + 1] * uy, xy[i * 2], xy[i * 2 + 1]};
	qsort(pts, n, sizeof(pts[0]), cmp_lid);
	gs_render_start(true);
	for (int i = 0; i + 1 < n; i++) {
		float ax = pts[i].x, ay = pts[i].y, bx = pts[i + 1].x, by = pts[i + 1].y;
		gs_vertex2f(ax - vx * hw, ay - vy * hw);
		gs_vertex2f(bx - vx * hw, by - vy * hw);
		gs_vertex2f(ax + vx * hw, ay + vy * hw);
		gs_vertex2f(bx - vx * hw, by - vy * hw);
		gs_vertex2f(bx + vx * hw, by + vy * hw);
		gs_vertex2f(ax + vx * hw, ay + vy * hw);
	}
	gs_render_stop(GS_TRIS);
}

static gs_texture_t *render_mask(struct beauty_filter *f, uint32_t w, uint32_t h, uint32_t hw, uint32_t hh)
{
	if (!f->layout.has_face || f->face_weight < 0.01f || !f->face.valid)
		return NULL;

	gs_texrender_reset(f->tr_mask_tmp);
	if (!gs_texrender_begin(f->tr_mask_tmp, hw, hh))
		return NULL;
	struct vec4 zero, one;
	vec4_zero(&zero);
	vec4_set(&one, 1, 1, 1, 1);
	gs_clear(GS_CLEAR_COLOR, &zero, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	float xy[FT_MAX_POINTS * 2];
#define LIP_MAX 160
	float lip_xy[LIP_MAX * 2];
	int n;

	while (gs_effect_loop(solid, "Solid")) {
		/* R: lips = outer polygon minus inner (open mouth) polygon */
		gs_enable_color(true, false, false, false);
		if ((n = face_lips_outer(&f->face, (float)w, (float)h, lip_xy, LIP_MAX)) >= 3) {
			gs_effect_set_vec4(color, &one);
			draw_fan(lip_xy, n);
			if ((n = face_mouth_opening(&f->face, (float)w, (float)h, lip_xy, LIP_MAX)) >= 3) {
				gs_effect_set_vec4(color, &zero);
				draw_fan(lip_xy, n);
				/* B: the mouth opening is a protected zone too (teeth, tongue) */
				gs_enable_color(false, false, true, false);
				gs_effect_set_vec4(color, &one);
				draw_fan(lip_xy, n);
				gs_enable_color(true, false, false, false);
			}
		}
		/* B: eye interiors (keeps eyeshadow off the eyeball); G: eyeliner */
		for (int r = FT_LEFT_EYE; r <= FT_RIGHT_EYE; r++) {
			if ((n = face_ring(&f->face, (enum ft_region)r, (float)w, (float)h, xy, FT_MAX_POINTS)) < 3)
				continue;
			gs_effect_set_vec4(color, &one);
			gs_enable_color(false, false, true, false);
			draw_fan(xy, n);
			gs_enable_color(false, true, false, false);
			draw_liner(&f->layout, xy, n);
		}
		/* A: eyebrows (a band along each brow) so smoothing and tone keep brow hair crisp */
		gs_enable_color(false, false, false, true);
		gs_effect_set_vec4(color, &one);
		for (int r = FT_LEFT_BROW; r <= FT_RIGHT_BROW; r++) {
			if ((n = face_ring(&f->face, (enum ft_region)r, (float)w, (float)h, xy, FT_MAX_POINTS)) < 2)
				continue;
			draw_band(&f->layout, xy, n, f->layout.iod_px * 0.05f);
		}
		gs_enable_color(true, true, true, true);
	}
	gs_texrender_end(f->tr_mask_tmp);
	return gs_texrender_get_texture(f->tr_mask_tmp);
}

static void process_frame(struct beauty_filter *f, gs_texture_t *src, uint32_t w, uint32_t h, uint64_t id)
{
	/* --- landmarks -> layout --- */
	uint64_t now = os_gettime_ns();
	float dt = f->last_ns ? (float)((double)(now - f->last_ns) / 1e9) : 0.0f;
	f->last_ns = now;

	bool have = face_tracker_get_at(f->tracker, id, &f->face);
	float target = f->face.present ? 1.0f : 0.0f;
	f->face_weight += (target - f->face_weight) * fminf(1.0f, dt * 6.0f);
	struct beauty_shape shape = f->shape;
	if (!f->enhance_on)
		memset(&shape, 0, sizeof(shape));
	fx_layers_apply_shape(f->fx, &shape);
	face_layout_build(&f->layout, have ? &f->face : NULL, &shape, (float)w, (float)h, f->face_weight,
			  f->mk.blush_style);
	gs_texture_set_image(f->data_tex, (const uint8_t *)f->layout.data, DATA_TEX_WIDTH * 4 * sizeof(float), false);

	/* --- smoothing + tone passes --- */
	uint32_t hw = w / 2 > 0 ? w / 2 : 1, hh = h / 2 > 0 ? h / 2 : 1;
	uint32_t qw = w / 4 > 0 ? w / 4 : 1, qh = h / 4 > 0 ? h / 4 : 1;
	uint32_t ew = w / 8 > 0 ? w / 8 : 1, eh = h / 8 > 0 ? h / 8 : 1;

	float iod_half = (f->layout.has_face ? f->layout.iod_px : (float)w * 0.1f) * 0.5f;

	/* Base layer for frequency separation: edge-preserving blur at a fixed, face-relative
	 * scale (removes pores / blemishes, keeps shading and features). The sliders decide how
	 * much of the detail on top of it to keep - they no longer change the blur itself. */
	float step = fmaxf(iod_half * 0.16f / 8.0f, 0.5f);
	float sigma = 0.09f;
	gs_texture_t *half = blur_pass(f, f->tr_half, "Draw", src, w, h, hw, hh, 0, 0, 0, 0);
	gs_texture_t *t1 = blur_pass(f, f->tr_tmp, "Bilateral", half, hw, hh, hw, hh, 1, 0, step, sigma);
	gs_texture_t *smooth = blur_pass(f, f->tr_smooth, "Bilateral", t1, hw, hh, hw, hh, 0, 1, step, sigma);
	gs_texture_t *q = blur_pass(f, f->tr_q, "Draw", smooth, hw, hh, qw, qh, 0, 0, 0, 0);
	gs_texture_t *e = blur_pass(f, f->tr_e, "Draw", q, qw, qh, ew, eh, 0, 0, 0, 0);
	gs_texture_t *t2 = blur_pass(f, f->tr_tmp_e, "Gauss", e, ew, eh, ew, eh, 1, 0, 1.5f, 0);
	gs_texture_t *tone = blur_pass(f, f->tr_tone, "Gauss", t2, ew, eh, ew, eh, 0, 1, 1.5f, 0);

	/* --- learned segmentation: R skin, G face skin, B hair, A person --- */
	face_tracker_set_segmentation(f->tracker, true);
	uint32_t mw = 0, mh = 0;
	uint8_t *segbuf = face_tracker_take_mask(f->tracker, &f->pm_serial, &mw, &mh);
	if (segbuf) {
		if (!f->person_tex || f->pm_w != mw || f->pm_h != mh) {
			gs_texture_destroy(f->person_tex);
			f->person_tex = gs_texture_create(mw, mh, GS_RGBA, 1, NULL, GS_DYNAMIC);
			f->pm_w = mw;
			f->pm_h = mh;
		}
		gs_texture_set_image(f->person_tex, segbuf, mw * 4, false);
		pthread_mutex_lock(&f->snap_mutex);
		free(f->pm_cpu);
		f->pm_cpu = segbuf; /* keep for thumbnails */
		pthread_mutex_unlock(&f->snap_mutex);
	}
	gs_texture_t *seg = NULL, *skin_od = NULL, *person_blur = NULL;
	ft_pigment pig;
	face_tracker_get_pigment(f->tracker, &pig); /* prior basis until calibrated */
	if (f->person_tex) {
		/* snap the 256px mask to the frame's real edges */
		seg = guided_pass(f, f->tr_seg_ref, "Refine", f->person_tex, half, 1.0f / (float)f->pm_w,
				  1.0f / (float)f->pm_h, hw, hh, 0.07f);
		/* skin-weighted optical density, blurred by normalised convolution: the local skin colour
		 * and pigment levels that foundation / redness / shadows work towards (blotch scale ~ a
		 * quarter of the eye distance). Background never bleeds into it. */
		float ps = fmaxf(iod_half * 2.0f * 0.25f / 8.0f / 2.0f, 0.75f);
		gs_texture_t *sq = guided_pass(f, f->tr_skin_q, "SkinOD", half, seg, 0, 0, qw, qh, 0);
		gs_texture_t *se = blur_pass(f, f->tr_skin_e, "Draw", sq, qw, qh, ew, eh, 0, 0, 0, 0);
		gs_texture_t *st = blur_pass(f, f->tr_skin_t, "Gauss", se, ew, eh, ew, eh, 1, 0, ps, 0);
		skin_od = blur_pass(f, f->tr_skin_tone, "Gauss", st, ew, eh, ew, eh, 0, 1, ps, 0);
		/* soft body height field for whole-body light normals (alpha channel) */
		float stp = fmaxf((float)f->pm_w * 0.012f, 1.0f);
		gs_texture_t *b1 = blur_pass(f, f->tr_pm1, "Gauss", f->person_tex, f->pm_w, f->pm_h, f->pm_w, f->pm_h, 1,
					     0, stp, 0);
		person_blur = blur_pass(f, f->tr_pm2, "Gauss", b1, f->pm_w, f->pm_h, f->pm_w, f->pm_h, 0, 1, stp, 0);
		/* packed into tone's alpha: the final effect is limited to 8 textures */
		gs_texture_t *tp = person_blur ? guided_pass(f, f->tr_tone_pb, "WithAlpha", tone, person_blur, 0, 0, ew, eh, 0)
					       : NULL;
		if (tp)
			tone = tp;
		else
			person_blur = NULL;
	}

	/* --- makeup masks (lips / eyeliner / eye interior), soft-edged --- */
	gs_texture_t *mask = render_mask(f, w, h, hw, hh);
	if (mask) {
		float ms = fmaxf(iod_half * 0.012f, 0.6f);
		gs_texture_t *m1 = blur_pass(f, f->tr_mask_tmp, "Gauss", mask, hw, hh, hw, hh, 1, 0, ms, 0);
		mask = blur_pass(f, f->tr_mask, "Gauss", m1, hw, hh, hw, hh, 0, 1, ms, 0);
	}

	/* --- final composite --- */
	gs_effect_t *fe = f->final_effect;
#define P(n) gs_effect_get_param_by_name(fe, n)
	gs_effect_set_texture(P("image"), src);
	gs_effect_set_texture(P("smooth_tex"), smooth);
	gs_effect_set_texture(P("tone_tex"), tone);
	gs_effect_set_texture(P("data_tex"), f->data_tex);
	struct vec2 size;
	vec2_set(&size, (float)w, (float)h);
	gs_effect_set_vec2(P("size"), &size);
	gs_effect_set_int(P("warp_count"), f->layout.warp_count);
	struct vec4 b;
	vec4_set(&b, f->layout.bounds[0], f->layout.bounds[1], f->layout.bounds[2], f->layout.bounds[3]);
	gs_effect_set_vec4(P("warp_bounds"), &b);
	gs_effect_set_float(P("face_weight"), f->layout.has_face ? f->face_weight : 0.0f);

	struct vec2 cbcr; /* generic skin cluster when no face has been sampled */
	vec2_set(&cbcr, 0.42f, 0.585f);
	float tol = 0.09f;
	if (f->face.skin_valid) {
		cbcr.x = f->face.skin_cbcr[0];
		cbcr.y = f->face.skin_cbcr[1];
		tol = 0.06f;
	}
	gs_effect_set_vec2(P("skin_cbcr"), &cbcr);
	gs_effect_set_float(P("skin_tol"), tol);
	gs_effect_set_float(P("all_skin"), f->all_skin ? 1.0f : 0.0f);
	float en = f->enhance_on ? 1.0f : 0.0f;
	gs_effect_set_float(P("smooth_amt"), en * f->smooth);
	gs_effect_set_float(P("foundation_amt"), en * f->foundation);
	gs_effect_set_float(P("skin_tone_amt"), en * f->skin_tone);
	gs_effect_set_float(P("pouch_amt"), en * f->pouch);
	gs_effect_set_float(P("fold_amt"), en * f->fold);
	gs_effect_set_float(P("teeth_amt"), en * f->teeth);
	gs_effect_set_float(P("eyelight_amt"), en * f->eyelight);
	gs_effect_set_float(P("clarity_amt"), en * f->clarity);
	gs_effect_set_int(P("debug_view"), f->debug_view);

	const struct makeup_params *m = &f->mk;
	gs_effect_set_texture(P("mask_tex"), mask ? mask : smooth);
	gs_effect_set_texture(P("lut_tex"), f->lut_tex ? f->lut_tex : tone);
	gs_effect_set_float(P("lut_size"), (float)f->lut_size);
	gs_effect_set_float(P("lut_amt"), f->lut_tex ? 1.0f : 0.0f);
	gs_effect_set_float(P("filter_amt"), en * m->filter_amt);
	struct vec4 g0, g1;
	vec4_set(&g0, m->exposure, m->contrast, m->saturation, m->warmth);
	vec4_set(&g1, m->tint, m->fade, 0.0f, 0.0f);
	gs_effect_set_vec4(P("grade0"), &g0);
	gs_effect_set_vec4(P("grade1"), &g1);
	gs_effect_set_float(P("makeup_amt"), mask ? en * m->makeup_amt : 0.0f);
	struct vec4 lc, bc, sc, extra;
	vec4_set(&lc, m->lip[0], m->lip[1], m->lip[2], m->lip_opacity);
	vec4_set(&bc, m->blush[0], m->blush[1], m->blush[2], m->blush_opacity);
	vec4_set(&sc, m->shadow[0], m->shadow[1], m->shadow[2], m->shadow_opacity);
	vec4_set(&extra, m->liner, m->contour, m->highlight, m->brow);
	gs_effect_set_vec4(P("lip_color"), &lc);
	gs_effect_set_float(P("lip_gloss"), m->lip_gloss);
	gs_effect_set_vec4(P("blush_color"), &bc);
	gs_effect_set_vec4(P("shadow_color"), &sc);
	gs_effect_set_vec4(P("makeup_extra"), &extra);
#undef P
#define P2(n) gs_effect_get_param_by_name(fe, n)
	fx_layers_set_final_params(f->fx, fe, tone);
	gs_effect_set_texture(P2("seg_tex"), seg ? seg : tone);
	gs_effect_set_texture(P2("skin_od_tex"), skin_od ? skin_od : tone);
	gs_effect_set_float(P2("has_seg"), seg ? 1.0f : 0.0f);
	gs_effect_set_float(P2("has_prot"), mask ? 1.0f : 0.0f);
	{
		struct vec3 v;
		vec3_set(&v, pig.mel[0], pig.mel[1], pig.mel[2]);
		gs_effect_set_vec3(P2("pig_mel"), &v);
		vec3_set(&v, pig.hem[0], pig.hem[1], pig.hem[2]);
		gs_effect_set_vec3(P2("pig_hem"), &v);
		vec3_set(&v, pig.inv[0], pig.inv[1], pig.inv[2]);
		gs_effect_set_vec3(P2("pig_r0"), &v);
		vec3_set(&v, pig.inv[3], pig.inv[4], pig.inv[5]);
		gs_effect_set_vec3(P2("pig_r1"), &v);
		gs_effect_set_float(P2("redness_amt"), f->enhance_on ? f->redness : 0.0f);
	}
	{
		bool hp = seg && person_blur;
		struct vec2 pt;
		vec2_set(&pt, hp ? 1.0f / (float)f->pm_w : 0.0f, hp ? 1.0f / (float)f->pm_h : 0.0f);
#undef P2
		gs_effect_set_vec2(gs_effect_get_param_by_name(fe, "person_texel"), &pt);
		gs_effect_set_float(gs_effect_get_param_by_name(fe, "has_person"), hp ? 1.0f : 0.0f);
	}

	gs_texrender_reset(f->tr_out);
	if (gs_texrender_begin(f->tr_out, w, h)) {
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		while (gs_effect_loop(fe, "Draw"))
			gs_draw_sprite(src, 0, w, h);
		gs_texrender_end(f->tr_out);
	}

	/* stickers, motion and atmosphere */
	float t = (float)fmod((double)(os_gettime_ns() - f->start_ns) / 1e9, 3600.0);
	f->present = fx_layers_render(f->fx, gs_texrender_get_texture(f->tr_out), w, h, &f->layout,
				      f->layout.has_face ? f->face_weight : 0.0f, t);
}

static void draw_landmarks(struct beauty_filter *f, uint32_t w, uint32_t h)
{
	if (!f->face.valid)
		return;
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	struct vec4 color;
	vec4_set(&color, 0.2f, 1.0f, 0.4f, 1.0f);
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &color);
	float dot = fmaxf(2.0f, (float)h / 300.0f);
	while (gs_effect_loop(solid, "Solid")) {
		for (int i = 0; i < f->face.num_points; i++) {
			gs_matrix_push();
			gs_matrix_translate3f(f->face.pts[i].x * w - dot * 0.5f, f->face.pts[i].y * h - dot * 0.5f,
					      0.0f);
			gs_draw_sprite(NULL, 0, (uint32_t)dot, (uint32_t)dot);
			gs_matrix_pop();
		}
	}
}

static void beauty_render(void *data, gs_effect_t *unused)
{
	struct beauty_filter *f = data;
	obs_source_t *target = obs_filter_get_target(f->source);
	obs_source_t *parent = obs_filter_get_parent(f->source);
	uint32_t w = target ? obs_source_get_base_width(target) : 0;
	uint32_t h = target ? obs_source_get_base_height(target) : 0;

	if (!target || !parent || !w || !h || !f->blur_effect || !f->final_effect || !f->data_tex ||
	    !face_effects_active(f)) {
		obs_source_skip_video_filter(f->source);
		return;
	}

	if (!f->processed) {
		uint64_t id = ++f->frame;
		int slot = (int)(id & 1);
		if (!capture_source(f, f->tr_ring[slot], target, parent, w, h)) {
			obs_source_skip_video_filter(f->source);
			return;
		}
		f->ring_id[slot] = id;
		f->ring_w[slot] = w;
		f->ring_h[slot] = h;
		gs_texture_t *src = gs_texrender_get_texture(f->tr_ring[slot]);

		gs_blend_state_push();
		gs_reset_blend_state();
		gs_enable_blending(false);
		update_tracking(f, src, w, h, id);

		/* Sync mode shows the previous frame, whose landmarks are now known. */
		int other = slot ^ 1;
		gs_texture_t *show = src;
		uint64_t show_id = id;
		if (f->sync_tracking && f->ring_id[other] == id - 1 && f->ring_w[other] == w && f->ring_h[other] == h) {
			show = gs_texrender_get_texture(f->tr_ring[other]);
			show_id = id - 1;
		}
		process_frame(f, show, w, h, show_id);
		gs_blend_state_pop();
		f->processed = true;

		if (id % 1800 == 0) {
			float ms, det;
			face_tracker_stats(f->tracker, &ms, &det);
			blog(LOG_INFO, "[obs-beauty] %s: %.2f ms/frame, %.0f%% full detections, face %s",
			     face_tracker_backend_name(f->tracker), ms, det * 100.0f, f->face.present ? "present" : "absent");
		}
	}

	gs_texture_t *out = f->present ? f->present : gs_texrender_get_texture(f->tr_out);
	if (!out) {
		obs_source_skip_video_filter(f->source);
		return;
	}
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), out);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite(out, 0, w, h);
	gs_blend_state_pop();

	if (f->show_landmarks)
		draw_landmarks(f, w, h);
}

struct obs_source_info beauty_filter_info = {
	.id = "obs_beauty_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = beauty_get_name,
	.create = beauty_create,
	.destroy = beauty_destroy,
	.update = beauty_update,
	.get_defaults = beauty_defaults,
	.get_properties = beauty_properties,
	.video_tick = beauty_tick,
	.video_render = beauty_render,
};

/* ---------------- dock API ---------------- */

bool beauty_filter_snapshot(obs_source_t *filter, uint8_t **bgra, uint32_t *w, uint32_t *h, ft_face *face)
{
	if (!filter || strcmp(obs_source_get_id(filter), "obs_beauty_filter") != 0)
		return false;
	struct beauty_filter *f = obs_obj_get_data(filter);
	if (!f)
		return false;
	pthread_mutex_lock(&f->snap_mutex);
	bool ok = f->snap != NULL;
	uint64_t id = f->snap_id;
	if (ok) {
		size_t n = (size_t)f->snap_w * f->snap_h * 4;
		*bgra = bmalloc(n);
		memcpy(*bgra, f->snap, n);
		*w = f->snap_w;
		*h = f->snap_h;
	}
	pthread_mutex_unlock(&f->snap_mutex);
	if (ok && face && !face_tracker_get_at(f->tracker, id, face))
		memset(face, 0, sizeof(*face));
	return ok;
}

bool beauty_filter_snapshot_mask(obs_source_t *filter, uint8_t **mask, uint32_t *w, uint32_t *h)
{
	if (!filter || strcmp(obs_source_get_id(filter), "obs_beauty_filter") != 0)
		return false;
	struct beauty_filter *f = obs_obj_get_data(filter);
	if (!f)
		return false;
	pthread_mutex_lock(&f->snap_mutex);
	bool ok = f->pm_cpu != NULL;
	if (ok) { /* person channel (alpha) of the RGBA segmentation */
		size_t n = (size_t)f->pm_w * f->pm_h;
		*mask = bmalloc(n);
		for (size_t i = 0; i < n; i++)
			(*mask)[i] = f->pm_cpu[i * 4 + 3];
		*w = f->pm_w;
		*h = f->pm_h;
	}
	pthread_mutex_unlock(&f->snap_mutex);
	return ok;
}

void beauty_filter_request_person_preview(obs_source_t *filter)
{
	if (!filter || strcmp(obs_source_get_id(filter), "obs_beauty_filter") != 0)
		return;
	struct beauty_filter *f = obs_obj_get_data(filter);
	if (f)
		f->person_preview_until = os_gettime_ns() + 8000000000ULL; /* 8 s */
}
