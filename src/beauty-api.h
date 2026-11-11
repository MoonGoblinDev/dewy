#pragma once

/* Functions the dock uses to talk to a Dewy filter instance. */

#include <obs-module.h>
#include "face-tracker.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ask for a snapshot: the filter copies the next frame it analyses (cheap, one frame). */
void beauty_filter_request_snapshot(obs_source_t *filter);
/* Changes every time a snapshot is taken. */
uint32_t beauty_filter_snapshot_serial(obs_source_t *filter);

/* Copy of the last snapshot (BGRA, caller bfree()s it) and the face landmarks that belong to
 * it. Returns false if no snapshot has been taken yet. */
bool beauty_filter_snapshot(obs_source_t *filter, uint8_t **bgra, uint32_t *w, uint32_t *h, ft_face *face);

/* Latest person-segmentation mask (0..255, covers the whole frame; caller bfree()s it).
 * Only available while the Light tab is in use. */
bool beauty_filter_snapshot_mask(obs_source_t *filter, uint8_t **mask, uint32_t *w, uint32_t *h);

/* Keeps segmentation running for a few seconds so Light thumbnails can use the body mask. */
void beauty_filter_request_person_preview(obs_source_t *filter);

#ifdef __cplusplus
}
#endif
