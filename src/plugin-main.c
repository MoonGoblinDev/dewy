/*
Dewy — real-time beauty for OBS
Copyright (C) 2026 Bregas S. Wicaksono

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("dewy", "en-US")

#include "makeup.h"
#include "presets.h"
#include "face-tracker.h"
#include <util/dstr.h>
#include <string.h>

extern struct obs_source_info beauty_filter_info;
extern void beauty_dock_register(void);

const char *obs_module_name(void)
{
	return "Dewy";
}

const char *obs_module_description(void)
{
	return "Real-time skin smoothing, face reshaping and makeup.";
}

/* The ONNX backend loads its runtime from an explicit path next to the plugin binary (so it
 * can never pick up another plugin's onnxruntime of a different version). */
static void set_runtime_path(void)
{
#ifdef BEAUTY_ORT_FILE
	const char *bin = obs_get_module_binary_path(obs_current_module());
	if (!bin)
		return;
	struct dstr p = {0};
	dstr_copy(&p, bin);
	char *slash = strrchr(p.array, '/');
	char *bslash = strrchr(p.array, '\\');
	if (bslash > slash)
		slash = bslash;
	if (slash)
		*slash = 0;
	p.len = strlen(p.array);
#ifdef __APPLE__
	dstr_cat(&p, "/../Frameworks/" BEAUTY_ORT_FILE);
#else
	dstr_cat(&p, "/" BEAUTY_ORT_FILE);
#endif
	face_tracker_set_runtime_library(p.array);
	dstr_free(&p);
#endif
}

bool obs_module_load(void)
{
	set_runtime_path();
	makeup_presets_load();
	presets_load();
	obs_register_source(&beauty_filter_info);
	blog(LOG_INFO, "[dewy] loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_post_load(void)
{
	beauty_dock_register();
}

void obs_module_unload(void)
{
	makeup_presets_free();
	presets_free();
}
