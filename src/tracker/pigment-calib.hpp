// Skin pigment calibration (see research/pigment/FINDINGS.md).
//
// In optical density (OD = -log linear RGB) skin is  m*mel + h*hem + s*(1,1,1)/sqrt3 + bias.
// From the user's own skin pixels we remove the shading direction and run a 2-D ICA in the
// remaining chromatic plane to find melanin / hemoglobin for this camera and light, clamp each
// to within 15 degrees of its physical prior, and give it the prior's brightness component.
#pragma once

#include <cstdint>

namespace beauty {

struct PigmentBasis {
	float mel[3], hem[3]; /* unit OD directions */
	float inv[9];         /* inverse of [mel hem u] (row-major): row 0 -> m, row 1 -> h, row 2 -> s */
};

/* Prior-only basis (used until the first calibration). */
PigmentBasis pigment_prior_basis();

/* bgra: analysis frame; rgba_mask: segmentation (R = skin) of size mw x mh covering the frame. */
bool pigment_calibrate(const uint8_t *bgra, int w, int h, int stride, const uint8_t *rgba_mask, int mw, int mh,
		       PigmentBasis &out);

/* Temporal smoothing: blend `cur` towards `next` and rebuild the inverse. */
void pigment_blend(PigmentBasis &cur, const PigmentBasis &next, float k);

} // namespace beauty
