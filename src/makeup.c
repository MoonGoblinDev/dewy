#include "makeup.h"

#include <util/platform.h>
#include <util/dstr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static obs_data_array_t *g_presets;

/* ---------------- custom fields ---------------- */

struct mk_slider {
	const char *key;
	const char *text;
	bool bipolar;
};

static const struct mk_slider grade_sliders[] = {
	{"fl_exposure", "Brightness", true}, {"fl_contrast", "Contrast", true}, {"fl_saturation", "Saturation", true},
	{"fl_warmth", "Warmth", true},       {"fl_tint", "Tint", true},         {"fl_fade", "Fade", false},
};

static const char *color_keys[] = {"mk_lip_color", "mk_blush_color", "mk_shadow_color"};
static const char *amount_keys[] = {"mk_lip_opacity", "mk_lip_gloss", "mk_blush_opacity", "mk_shadow_opacity",
				    "mk_liner",       "mk_contour",   "mk_highlight",     "mk_brow"};
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* OBS colours are 0xAABBGGRR */
static long long hex_to_obs_color(const char *hex)
{
	if (!hex || hex[0] != '#' || strlen(hex) < 7)
		return 0xFFFFFFFF;
	unsigned v = (unsigned)strtoul(hex + 1, NULL, 16);
	unsigned r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
	return 0xFF000000LL | ((long long)b << 16) | ((long long)g << 8) | r;
}

static void obs_color_to_rgb(long long c, float out[3])
{
	out[0] = (float)(c & 0xFF) / 255.0f;
	out[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	out[2] = (float)((c >> 16) & 0xFF) / 255.0f;
}

/* ---------------- presets ---------------- */

void makeup_presets_load(void)
{
	char *path = obs_module_file("makeup/presets.json");
	obs_data_t *root = path ? obs_data_create_from_json_file(path) : NULL;
	if (root) {
		g_presets = obs_data_get_array(root, "presets");
		obs_data_release(root);
	}
	blog(LOG_INFO, "[obs-beauty] %zu makeup presets loaded", g_presets ? obs_data_array_count(g_presets) : 0);
	bfree(path);
}

void makeup_presets_free(void)
{
	obs_data_array_release(g_presets);
	g_presets = NULL;
}

static obs_data_t *find_preset(const char *name)
{
	if (!g_presets || !name)
		return NULL;
	for (size_t i = 0; i < obs_data_array_count(g_presets); i++) {
		obs_data_t *p = obs_data_array_item(g_presets, i);
		if (strcmp(obs_data_get_string(p, "name"), name) == 0)
			return p;
		obs_data_release(p);
	}
	return NULL;
}

/* preset JSON uses 0..1 floats; settings use -100..100 ints */
static void set_pct(obs_data_t *settings, const char *key, obs_data_t *src, const char *src_key)
{
	double v = src ? obs_data_get_double(src, src_key) : 0.0;
	obs_data_set_int(settings, key, (long long)(v * 100.0 + (v >= 0 ? 0.5 : -0.5)));
}

static void apply_preset(obs_data_t *settings, const char *name)
{
	obs_data_t *p = find_preset(name);
	obs_data_t *fl = p ? obs_data_get_obj(p, "filter") : NULL;
	obs_data_t *mk = p ? obs_data_get_obj(p, "makeup") : NULL;

	set_pct(settings, "fl_exposure", fl, "exposure");
	set_pct(settings, "fl_contrast", fl, "contrast");
	set_pct(settings, "fl_saturation", fl, "saturation");
	set_pct(settings, "fl_warmth", fl, "warmth");
	set_pct(settings, "fl_tint", fl, "tint");
	set_pct(settings, "fl_fade", fl, "fade");
	obs_data_set_string(settings, "fl_lut", fl ? obs_data_get_string(fl, "lut") : "");

	obs_data_set_int(settings, "mk_lip_color", hex_to_obs_color(mk ? obs_data_get_string(mk, "lip_color") : NULL));
	obs_data_set_int(settings, "mk_blush_color",
			 hex_to_obs_color(mk ? obs_data_get_string(mk, "blush_color") : NULL));
	obs_data_set_int(settings, "mk_shadow_color",
			 hex_to_obs_color(mk ? obs_data_get_string(mk, "shadow_color") : NULL));
	set_pct(settings, "mk_lip_opacity", mk, "lip_opacity");
	set_pct(settings, "mk_lip_gloss", mk, "lip_gloss");
	set_pct(settings, "mk_blush_opacity", mk, "blush_opacity");
	set_pct(settings, "mk_shadow_opacity", mk, "shadow_opacity");
	set_pct(settings, "mk_liner", mk, "liner");
	set_pct(settings, "mk_contour", mk, "contour");
	set_pct(settings, "mk_highlight", mk, "highlight");
	set_pct(settings, "mk_brow", mk, "brow");
	obs_data_set_int(settings, "mk_blush_style", mk ? obs_data_get_int(mk, "blush_style") : 0);

	obs_data_release(mk);
	obs_data_release(fl);
	obs_data_release(p);
}

bool makeup_sync_preset(obs_data_t *settings)
{
	const char *preset = obs_data_get_string(settings, "makeup_preset");
	const char *applied = obs_data_get_string(settings, "makeup_applied");
	if (strcmp(preset, applied) == 0)
		return false;
	char *name = bstrdup(preset);
	apply_preset(settings, name);
	obs_data_set_string(settings, "makeup_applied", name);
	bfree(name);
	return true;
}

void makeup_reset(obs_data_t *settings)
{
	obs_data_set_string(settings, "makeup_preset", "None");
	obs_data_set_int(settings, "filter_amt", 70);
	obs_data_set_int(settings, "makeup_amt", 70);
	makeup_sync_preset(settings);
}

/* ---------------- settings / properties ---------------- */

void makeup_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "makeup_preset", "None");
	obs_data_set_default_int(settings, "filter_amt", 70);
	obs_data_set_default_int(settings, "makeup_amt", 70);
	for (size_t i = 0; i < COUNT(color_keys); i++)
		obs_data_set_default_int(settings, color_keys[i], 0xFFFFFFFF);
}

static bool preset_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	return makeup_sync_preset(settings);
}

void makeup_add_properties(obs_properties_t *props)
{
	obs_properties_t *g = obs_properties_create();
	obs_property_t *list = obs_properties_add_list(g, "makeup_preset", obs_module_text("Look"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, obs_module_text("None"), "None");
	if (g_presets) {
		for (size_t i = 0; i < obs_data_array_count(g_presets); i++) {
			obs_data_t *p = obs_data_array_item(g_presets, i);
			const char *name = obs_data_get_string(p, "name");
			obs_property_list_add_string(list, obs_module_text(name), name);
			obs_data_release(p);
		}
	}
	obs_property_set_modified_callback(list, preset_modified);
	obs_properties_add_int_slider(g, "filter_amt", obs_module_text("Filter"), 0, 100, 1);
	obs_properties_add_int_slider(g, "makeup_amt", obs_module_text("Makeup"), 0, 100, 1);
	obs_properties_add_group(props, "group_makeup", obs_module_text("Makeup"), OBS_GROUP_NORMAL, g);

	obs_properties_t *c = obs_properties_create();
	obs_properties_add_color(c, "mk_lip_color", obs_module_text("Lip colour"));
	obs_properties_add_int_slider(c, "mk_lip_opacity", obs_module_text("Lips"), 0, 100, 1);
	obs_properties_add_int_slider(c, "mk_lip_gloss", obs_module_text("Gloss"), 0, 100, 1);
	obs_properties_add_color(c, "mk_blush_color", obs_module_text("Blush colour"));
	obs_properties_add_int_slider(c, "mk_blush_opacity", obs_module_text("Blush"), 0, 100, 1);
	obs_property_t *bs = obs_properties_add_list(c, "mk_blush_style", obs_module_text("Blush placement"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(bs, obs_module_text("Apples of the cheeks"), 0);
	obs_property_list_add_int(bs, obs_module_text("Lifted (towards temples)"), 1);
	obs_property_list_add_int(bs, obs_module_text("Sun-kissed (across the nose)"), 2);
	obs_properties_add_color(c, "mk_shadow_color", obs_module_text("Eyeshadow colour"));
	obs_properties_add_int_slider(c, "mk_shadow_opacity", obs_module_text("Eyeshadow"), 0, 100, 1);
	obs_properties_add_int_slider(c, "mk_liner", obs_module_text("Eyeliner"), 0, 100, 1);
	obs_properties_add_int_slider(c, "mk_contour", obs_module_text("Contour"), 0, 100, 1);
	obs_properties_add_int_slider(c, "mk_highlight", obs_module_text("Highlight"), 0, 100, 1);
	obs_properties_add_int_slider(c, "mk_brow", obs_module_text("Brow tint"), 0, 100, 1);
	obs_properties_add_group(props, "group_makeup_custom", obs_module_text("Customise makeup"), OBS_GROUP_NORMAL,
				 c);

	obs_properties_t *f = obs_properties_create();
	for (size_t i = 0; i < COUNT(grade_sliders); i++)
		obs_properties_add_int_slider(f, grade_sliders[i].key, obs_module_text(grade_sliders[i].text),
					      grade_sliders[i].bipolar ? -100 : 0, 100, 1);
	obs_properties_add_path(f, "fl_lut", obs_module_text("LUT file (.cube)"), OBS_PATH_FILE,
				"Cube LUT (*.cube);;All files (*.*)", NULL);
	obs_properties_add_group(props, "group_filter_custom", obs_module_text("Customise filter"), OBS_GROUP_NORMAL,
				 f);
}

void makeup_read(struct makeup_params *mk, obs_data_t *s)
{
	memset(mk, 0, sizeof(*mk));
	mk->filter_amt = (float)obs_data_get_int(s, "filter_amt") / 100.0f;
	mk->makeup_amt = (float)obs_data_get_int(s, "makeup_amt") / 100.0f;
	mk->exposure = (float)obs_data_get_int(s, "fl_exposure") / 100.0f;
	mk->contrast = (float)obs_data_get_int(s, "fl_contrast") / 100.0f;
	mk->saturation = (float)obs_data_get_int(s, "fl_saturation") / 100.0f;
	mk->warmth = (float)obs_data_get_int(s, "fl_warmth") / 100.0f;
	mk->tint = (float)obs_data_get_int(s, "fl_tint") / 100.0f;
	mk->fade = (float)obs_data_get_int(s, "fl_fade") / 100.0f;

	const char *lut = obs_data_get_string(s, "fl_lut");
	if (lut && *lut && lut[0] != '/') { /* relative: bundled with the plugin */
		struct dstr rel = {0};
		dstr_printf(&rel, "luts/%s", lut);
		char *full = obs_module_file(rel.array);
		snprintf(mk->lut_path, sizeof(mk->lut_path), "%s", full ? full : "");
		bfree(full);
		dstr_free(&rel);
	} else {
		snprintf(mk->lut_path, sizeof(mk->lut_path), "%s", lut ? lut : "");
	}

	obs_color_to_rgb(obs_data_get_int(s, "mk_lip_color"), mk->lip);
	obs_color_to_rgb(obs_data_get_int(s, "mk_blush_color"), mk->blush);
	obs_color_to_rgb(obs_data_get_int(s, "mk_shadow_color"), mk->shadow);
	float *amounts[] = {&mk->lip_opacity, &mk->lip_gloss, &mk->blush_opacity, &mk->shadow_opacity,
			    &mk->liner,       &mk->contour,   &mk->highlight,     &mk->brow};
	for (size_t i = 0; i < COUNT(amount_keys); i++)
		*amounts[i] = (float)obs_data_get_int(s, amount_keys[i]) / 100.0f;
	mk->blush_style = (int)obs_data_get_int(s, "mk_blush_style");
}

/* ---------------- .cube LUT ---------------- */

gs_texture_t *makeup_load_cube(const char *path, int *size_out)
{
	*size_out = 0;
	FILE *f = os_fopen(path, "r");
	if (!f) {
		blog(LOG_WARNING, "[obs-beauty] cannot open LUT %s", path);
		return NULL;
	}
	int n = 0, count = 0, total = 0;
	uint8_t *pixels = NULL;
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
			continue;
		if (strncmp(line, "LUT_3D_SIZE", 11) == 0) {
			n = atoi(line + 11);
			if (n < 2 || n > 128)
				break;
			total = n * n * n;
			pixels = bzalloc((size_t)total * 4);
			continue;
		}
		float r, g, b;
		if (!pixels || sscanf(line, "%f %f %f", &r, &g, &b) != 3)
			continue;
		if (count >= total)
			break;
		/* red varies fastest, then green, then blue -> x = r + b*N, y = g */
		int ri = count % n, gi = (count / n) % n, bi = count / (n * n);
		uint8_t *px = pixels + ((size_t)gi * n * n + (size_t)bi * n + ri) * 4;
		px[0] = (uint8_t)(fminf(fmaxf(r, 0.0f), 1.0f) * 255.0f + 0.5f);
		px[1] = (uint8_t)(fminf(fmaxf(g, 0.0f), 1.0f) * 255.0f + 0.5f);
		px[2] = (uint8_t)(fminf(fmaxf(b, 0.0f), 1.0f) * 255.0f + 0.5f);
		px[3] = 255;
		count++;
	}
	fclose(f);

	gs_texture_t *tex = NULL;
	if (pixels && count == total) {
		const uint8_t *data = pixels;
		tex = gs_texture_create((uint32_t)(n * n), (uint32_t)n, GS_RGBA, 1, &data, 0);
		*size_out = n;
	} else {
		blog(LOG_WARNING, "[obs-beauty] invalid LUT %s (%d of %d entries)", path, count, total);
	}
	bfree(pixels);
	return tex;
}
