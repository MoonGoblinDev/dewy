// Live preset thumbnails: a crop of the user's own face from the camera, with each
// filter / light / effect / ambience applied on the CPU (mirroring the GPU shaders).
#pragma once

#include <QtGui/QImage>
#include <obs-module.h>

#include "face-tracker.h"
#include "presets.h"

namespace beauty {

struct FaceCrop {
	QImage img;      /* square, ARGB32 */
	ft_face face{};  /* landmarks normalised to the crop (0..1) */
	QImage mask;     /* person mask cropped like img (Grayscale8), may be null */
	bool hasFace = false;
};

/* Grabs the latest frame from a Dewy filter and crops around the face (falls back to an
 * illustrated face when no camera frame or face is available). */
FaceCrop captureFaceCrop(obs_source_t *filter, int size, float zoom = 2.1f);

QImage filterThumb(const FaceCrop &c, obs_data_t *preset);
QImage lightThumb(const FaceCrop &c, obs_data_t *preset);
QImage effectThumb(const FaceCrop &c, obs_data_t *preset);
QImage atmoThumb(const FaceCrop &c, obs_data_t *preset);
QImage motionThumb(const FaceCrop &c, obs_data_t *preset);
QImage noneThumb(int size);

} // namespace beauty
