# Pigment model prototype: findings

Prototype: `pigment.py` (run: `python3 pigment.py frames/*.png`, outputs in `out/`).
Frames: raw (unfiltered) captures of one person in daylight and in warm lamp light at night.
The same learned skin mask and MediaPipe landmarks as the plugin are used.

## Model

In linear RGB a skin pixel is roughly pigment × light, so in optical density
(OD = −log linear RGB) it is a sum:

    OD = m·σ_melanin + h·σ_hemoglobin + s·(1,1,1)/√3 + bias

Shading `s` (light, shadows, 3-D form) is the intensity direction. Removing it leaves a
2-D chromatic plane where ICA over the user's own skin pixels finds the two chromophore
directions for this camera and light (Tsumura et al., JOSA 1999 / SIGGRAPH 2003).

## Results

**1. The decomposition is plausible.** The melanin map holds freckles, acne marks and darker
patches; hemoglobin holds redness (cheeks, nose, ears); shading holds the 3-D form.

**2. Skin tone as a melanin change keeps the light and form; the current shader does not.**
"Shadow depth" = spread of log-luminance in skin after / before (1.00 = shadows untouched).

| | structure corr | shadow depth |
|---|---|---|
| pigment, tone ±1 (0.7 OD) | 0.987 – 0.997 | 0.98 – 1.02 |
| current shader, tone +1 | 0.89 – 0.90 | **0.59 – 0.67** |
| current shader, tone −1 | 0.88 – 0.97 | 0.90 – 0.91 |

The current shader lightens by flattening up to 41 % of the face's shading. That's the
"painting over" look, measured.

**3. Chromophore directions need priors.** Melanin is found reliably (1–17° from the
physical prior). Hemoglobin from 2-component ICA is unstable (28°–90° off between frames: noise
reduction, warm light, low signal), which is a known weakness of the method. Fix: clamp each
estimate to within 15° of its prior. ICA still adapts to the camera, but never goes
non-physical.

**4. Edits must use fixed physical step sizes** (OD units), not per-frame statistics. The
first version scaled steps by in-frame melanin spread and over-shot badly at night.
Chosen: skin tone ±1 = ∓0.7 OD of melanin; blush = +0.12 OD hemoglobin at the cheek centre.

**5. Even tone should be colour-led.** Flattening melanin/hemoglobin blotches with their full
direction (which includes a brightness part) disturbed shading at night (corr 0.90, depth
1.09). Moving them within the colour plane plus 35 % of melanin's brightness part keeps
shading (corr 0.992–0.997, depth 1.00–1.02) and still removes redness and marks.

**6. Redness** (hemoglobin excess over its surroundings) is subtle and safe (corr ≥ 0.989).
**Blush** as added hemoglobin works, but the strength and cheek shape need tuning (too orange
at 1.0 under warm light).

## Not yet covered

- One person / one skin tone only. Needs frames across a wide range of skin tones before
  shipping (melanin dominates on deeper skin; hemoglobin separation gets harder).
- Specular shine is excluded by a heuristic (bright and colourless). This is fine for tone, but a
  proper shine layer belongs to the shading phase.
- Camera processing (gamma, tone curve, noise reduction) is assumed sRGB. It works on these
  frames but isn't calibrated.
- Smooth still belongs to the frequency-separation pipeline. Next phase: split pigment spots
  (remove) from shading micro-bumps (keep pores).

## Port plan (shader)

1. **CPU, every ~2 s on the segmentation thread:** sample skin pixels from the analysis
   frame → OD → remove (1,1,1) → ICA (or fixed-point 2-D FastICA) → clamp to priors → lift
   → 3×3 basis + its inverse + median OD → smoothed over time → uniforms.
2. **GPU, per pixel:** linearise → OD → coefficients (m, h, s) via the inverse basis → edit
   m, h → recompose with the basis → exp → sRGB. Shading is never written.
3. **Even tone / redness** need low-passed m and h: a quarter-res pass writing (m, h) premultiplied
   by skin weight, then blurred (same normalised convolution as `skin_tone_tex` today).
4. **Controls:** Skin tone = melanin; Foundation = colour-led evening + optional fill-light
   later; new Redness; Blush (makeup) = hemoglobin.
5. **Validate:** these metrics as an automated test on recorded frames (structure corr ≥ 0.98,
   shadow depth 0.95–1.05, zero change outside skin).

## Port results (GPU, in OBS)

Same frames through the plugin (1280×720 day, 960×540 night), measured on OBS's output:

| | structure corr | shadow depth |
|---|---|---|
| tone +1 / −1 | 0.987 – 0.996 | 0.997 – 1.001 |
| foundation 1.0 | 0.994 – 0.996 | 0.989 – 1.014 |
| redness 1.0 | 0.996 – 0.999 | 0.978 – 0.994 |

No pixel outside skin changes. Redness measures the hemoglobin excess on the pixel minus its
1–2 px grain: half-rectifying per-pixel sensor noise otherwise reads as a texture change
(night corr 0.977 → 0.996).

The port also found that libobs tracks only 8 texture units per effect (`GS_MAX_TEXTURES`);
the final effect had grown to 13, which corrupted the GL backend's texture/sampler cache
(per-frame GL errors and stale masks sampled after a source change). The final effect is now
held at 8: shared masks are bound once, skin colour and pigment share one skin-weighted OD
texture, the body height field lives in tone's alpha, and the Filters LUT is its own pass.
