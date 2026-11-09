#include "presets.h"

#include <math.h>
#include <string.h>
#include <stddef.h>

static obs_data_t *g_filters, *g_lights, *g_atmo, *g_effects;

static obs_data_t *load(const char *file)
{
	char *path = obs_module_file(file);
	obs_data_t *d = path ? obs_data_create_from_json_file(path) : NULL;
	if (!d)
		blog(LOG_WARNING, "[dewy] could not load %s", file);
	bfree(path);
	return d;
}

void presets_load(void)
{
	g_filters = load("filters.json");
	g_lights = load("lights.json");
	g_atmo = load("atmosphere.json");
	g_effects = load("effects.json");
}

void presets_free(void)
{
	obs_data_release(g_filters);
	obs_data_release(g_lights);
	obs_data_release(g_atmo);
	obs_data_release(g_effects);
	g_filters = g_lights = g_atmo = g_effects = NULL;
}

obs_data_t *presets_filters_root(void)
{
	return g_filters;
}
obs_data_t *presets_lights_root(void)
{
	return g_lights;
}
obs_data_t *presets_atmosphere_root(void)
{
	return g_atmo;
}
obs_data_t *presets_effects_root(void)
{
	return g_effects;
}

/* Finds {"name": name} in root[list] or, with categories, root.categories[*][list]. */
static obs_data_t *find_in(obs_data_t *root, const char *list, const char *name)
{
	if (!root || !name || !*name)
		return NULL;
	obs_data_array_t *cats = obs_data_get_array(root, "categories");
	obs_data_t *found = NULL;
	if (cats) {
		for (size_t c = 0; !found && c < obs_data_array_count(cats); c++) {
			obs_data_t *cat = obs_data_array_item(cats, c);
			obs_data_array_t *arr = obs_data_get_array(cat, list);
			for (size_t i = 0; !found && arr && i < obs_data_array_count(arr); i++) {
				obs_data_t *it = obs_data_array_item(arr, i);
				if (strcmp(obs_data_get_string(it, "name"), name) == 0)
					found = it;
				else
					obs_data_release(it);
			}
			obs_data_array_release(arr);
			obs_data_release(cat);
		}
		obs_data_array_release(cats);
		return found;
	}
	obs_data_array_t *arr = obs_data_get_array(root, list);
	for (size_t i = 0; !found && arr && i < obs_data_array_count(arr); i++) {
		obs_data_t *it = obs_data_array_item(arr, i);
		if (strcmp(obs_data_get_string(it, "name"), name) == 0)
			found = it;
		else
			obs_data_release(it);
	}
	obs_data_array_release(arr);
	return found;
}

static void hex(obs_data_t *d, const char *key, float out[3])
{
	const char *s = obs_data_get_string(d, key);
	if (s && *s)
		grade_parse_hex(s, out);
}

void presets_parse_grade(obs_data_t *d, struct grade_params *g)
{
	grade_defaults(g);
	g->exposure = (float)obs_data_get_double(d, "exposure");
	g->contrast = (float)obs_data_get_double(d, "contrast");
	g->curve = (float)obs_data_get_double(d, "curve");
	g->saturation = (float)obs_data_get_double(d, "saturation");
	g->vibrance = (float)obs_data_get_double(d, "vibrance");
	g->warmth = (float)obs_data_get_double(d, "warmth");
	g->tint = (float)obs_data_get_double(d, "tint");
	g->hue = (float)obs_data_get_double(d, "hue");
	g->fade = (float)obs_data_get_double(d, "fade");
	g->rolloff = (float)obs_data_get_double(d, "rolloff");
	g->gamma[0] *= 1.0f + (float)obs_data_get_double(d, "gamma_r");
	g->gamma[1] *= 1.0f + (float)obs_data_get_double(d, "gamma_g");
	g->gamma[2] *= 1.0f + (float)obs_data_get_double(d, "gamma_b");
	hex(d, "shadows", g->shadow_tint);
	g->shadow_amt = (float)obs_data_get_double(d, "shadows_amt");
	hex(d, "highlights", g->highlight_tint);
	g->highlight_amt = (float)obs_data_get_double(d, "highlights_amt");
	g->mono = obs_data_get_bool(d, "mono");
	hex(d, "mono_tint", g->mono_tint);
}

void presets_parse_light(obs_data_t *d, struct light_params *l)
{
	memset(l, 0, sizeof(*l));
	float az = (float)obs_data_get_double(d, "az") * (float)M_PI / 180.0f;
	float el = (float)obs_data_get_double(d, "el") * (float)M_PI / 180.0f;
	l->key_dir[0] = sinf(az) * cosf(el);
	l->key_dir[1] = -sinf(el);
	l->key_dir[2] = cosf(az) * cosf(el);
	l->key_color[0] = l->key_color[1] = l->key_color[2] = 1.0f;
	hex(d, "key", l->key_color);
	l->key_amt = (float)obs_data_get_double(d, "key_amt");
	l->rim_color[0] = l->rim_color[1] = l->rim_color[2] = 1.0f;
	hex(d, "rim", l->rim_color);
	l->rim_amt = (float)obs_data_get_double(d, "rim_amt");
	l->fill = (float)obs_data_get_double(d, "fill");
	l->bg_dim = (float)obs_data_get_double(d, "bg_dim");
	l->exposure = (float)obs_data_get_double(d, "exposure");
	l->warmth = (float)obs_data_get_double(d, "warmth");
}

int presets_anchor_from_name(const char *n)
{
	static const char *names[ANCHOR_COUNT] = {"eyes",     "eye_l",    "eye_r",   "nose",    "mouth",
						  "chin",     "forehead", "head_top", "cheek_l", "cheek_r"};
	for (int i = 0; n && i < ANCHOR_COUNT; i++)
		if (strcmp(n, names[i]) == 0)
			return i;
	return ANCHOR_EYES;
}

static int index_of(const char *s, const char *const *names, int count)
{
	for (int i = 0; s && i < count; i++)
		if (strcmp(s, names[i]) == 0)
			return i;
	return 0;
}

void presets_parse_effect(obs_data_t *d, struct fx_params *fx)
{
	memset(fx, 0, sizeof(*fx));
	static const char *blends[] = {"normal", "add", "multiply"};
	static const char *anims[] = {"none", "bob", "pulse", "spin", "sway", "twinkle"};
	obs_data_array_t *sp = obs_data_get_array(d, "sprites");
	for (size_t i = 0; sp && i < obs_data_array_count(sp) && fx->sprite_count < FX_MAX_SPRITES; i++) {
		obs_data_t *s = obs_data_array_item(sp, i);
		struct fx_sprite *o = &fx->sprites[fx->sprite_count++];
		snprintf(o->image, sizeof(o->image), "%s", obs_data_get_string(s, "image"));
		o->anchor = presets_anchor_from_name(obs_data_get_string(s, "anchor"));
		o->offset[0] = (float)obs_data_get_double(s, "x");
		o->offset[1] = (float)obs_data_get_double(s, "y");
		o->width = (float)obs_data_get_double(s, "width");
		o->rotation = (float)obs_data_get_double(s, "rotation");
		o->blend = index_of(obs_data_get_string(s, "blend"), blends, 3);
		o->anim = index_of(obs_data_get_string(s, "anim"), anims, 6);
		o->anim_amt = (float)obs_data_get_double(s, "anim_amt");
		o->anim_speed = (float)obs_data_get_double(s, "anim_speed");
		o->phase = (float)obs_data_get_double(s, "phase");
		obs_data_release(s);
	}
	obs_data_array_release(sp);
	fx->shape = obs_data_get_obj(d, "shape");
}

bool presets_filter(const char *name, struct grade_params *out)
{
	obs_data_t *d = find_in(g_filters, "filters", name);
	if (!d)
		return false;
	presets_parse_grade(d, out);
	obs_data_release(d);
	return true;
}

bool presets_light(const char *name, struct light_params *out)
{
	obs_data_t *d = find_in(g_lights, "lights", name);
	if (!d)
		return false;
	presets_parse_light(d, out);
	obs_data_release(d);
	return true;
}

static const char *atmo_names[] = {"none",  "sparkle", "bokeh",   "hearts", "petals",   "snow",   "fireflies",
				   "leak",  "mist",    "glitter", "dreamy", "confetti", "bubbles"};
static const char *motion_names[] = {"none", "shake", "pulse", "rgb", "drift", "glitch", "wobble"};

bool presets_atmo(const char *name, struct atmo_params *out)
{
	obs_data_t *d = find_in(g_atmo, "ambience", name);
	if (!d)
		return false;
	memset(out, 0, sizeof(*out));
	out->style = index_of(obs_data_get_string(d, "style"), atmo_names, 13);
	out->color_a[0] = out->color_a[1] = out->color_a[2] = 1.0f;
	out->color_b[0] = out->color_b[1] = out->color_b[2] = 1.0f;
	hex(d, "a", out->color_a);
	hex(d, "b", out->color_b);
	obs_data_set_default_double(d, "density", 1.0);
	obs_data_set_default_double(d, "speed", 1.0);
	obs_data_set_default_double(d, "size", 1.0);
	out->density = (float)obs_data_get_double(d, "density");
	out->speed = (float)obs_data_get_double(d, "speed");
	out->size = (float)obs_data_get_double(d, "size");
	obs_data_release(d);
	return true;
}

bool presets_motion(const char *name, struct motion_params *out)
{
	obs_data_t *d = find_in(g_atmo, "motion", name);
	if (!d)
		return false;
	out->style = index_of(obs_data_get_string(d, "style"), motion_names, 7);
	obs_data_set_default_double(d, "speed", 1.0);
	out->speed = (float)obs_data_get_double(d, "speed");
	obs_data_release(d);
	return true;
}

bool presets_effect(const char *name, struct fx_params *out)
{
	obs_data_t *d = find_in(g_effects, "effects", name);
	if (!d)
		return false;
	presets_parse_effect(d, out);
	obs_data_release(d);
	return true;
}

void presets_apply_shape(obs_data_t *shape_obj, struct beauty_shape *sh)
{
	if (!shape_obj)
		return;
	static const struct {
		const char *key;
		size_t off;
	} map[] = {
#define M(k, f) {k, offsetof(struct beauty_shape, f)}
		M("face_natural", face_natural), M("face_slim", face_slim),     M("face_v", face_v),
		M("face_small", face_small),     M("face_narrow", face_narrow), M("cheekbone", cheekbone),
		M("jawbone", jawbone),           M("chin", chin),               M("forehead", forehead),
		M("head", head),                 M("eye_size", eye_size),       M("eye_spacing", eye_spacing),
		M("eye_position", eye_position), M("eye_tilt", eye_tilt),       M("eye_inner", eye_inner),
		M("eye_outer", eye_outer),       M("eye_lower_lid", eye_lower_lid), M("eye_pupil", eye_pupil),
		M("nose_narrow", nose_narrow),   M("nose_length", nose_length), M("nose_bridge", nose_bridge),
		M("nose_tip", nose_tip),         M("mouth_size", mouth_size),   M("mouth_position", mouth_position),
		M("mouth_smile", mouth_smile),   M("brow_thickness", brow_thickness), M("brow_position", brow_position),
#undef M
	};
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (obs_data_has_user_value(shape_obj, map[i].key))
			*(float *)((char *)sh + map[i].off) += (float)obs_data_get_double(shape_obj, map[i].key);
}
