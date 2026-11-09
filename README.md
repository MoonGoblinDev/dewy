# obs-beauty

A live-streaming apps–style beauty filter that runs natively inside OBS (no virtual camera).
It covers skin smoothing, foundation, dark-circle and smile-line removal, teeth whitening,
bright eyes and clarity, plus landmark-driven face, eye, nose, mouth and brow reshaping.

- Face tracking:
  - **macOS:** Apple Vision (76-point landmarks + person segmentation, Neural Engine).
  - **Windows:** ONNX Runtime + DirectML (any DX12 GPU, CPU fallback) running Google MediaPipe
    Face Landmarker v2 (478 points), MediaPipe selfie multiclass segmentation and OpenCV YuNet
    for (re)detection. See `data/models/NOTICE.md`.
  - Both backends feed the same portable core (`src/tracker/tracker-core.cpp`): smoothing,
    frame sync, skin sampling, mask smoothing.
- Skin: a learned segmenter (MediaPipe selfie multiclass on ONNX Runtime - Core ML on macOS,
  DirectML on Windows) labels face skin, body skin, hair, clothes and background every frame.
  The mask is snapped to the image's edges on the GPU (joint-bilateral upsampling); exact
  landmark outlines protect eyes, lips and brow hair. Retouching is frequency separation:
  base (shading + colour) / blemish band / fine grain. Smooth removes the blemish band and keeps
  the grain; Shadows fills the base layer.
- Skin colour is edited as pigment, not painted. In optical density (−log linear RGB) skin is
  melanin + hemoglobin + shading; the two pigment directions are calibrated on the user's own
  skin every 2 s (2-D ICA, clamped to physical priors). Skin tone changes melanin, Foundation
  evens melanin/hemoglobin blotches within the colour plane plus a little exposure, Redness
  removes excess hemoglobin, and Blush tints the cheeks the same way. Shading (light, shadows, the 3-D form) is never written, so
  shadow depth stays at 0.98–1.02 of the original (see
  [research/pigment/FINDINGS.md](research/pigment/FINDINGS.md)).
- Everything else runs as GPU shaders in OBS's render pipeline (`data/effects/`).
- Research notes and the streaming-app slider → technique mapping: [docs/RESEARCH.md](docs/RESEARCH.md)

## Build & install (macOS, against the installed OBS.app)

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build --target install-plugin   # copies into ~/Library/Application Support/obs-studio/plugins
```

Restart OBS. The **Beauty** dock opens the first time (later: Docks → Beauty). Pick your camera in the dock and press **Add Beauty to this source**, or add the **Beauty Enhance** filter from the Filters window.

The first configure downloads the OBS 33.0.0 headers, SIMDe and OBS's Qt 6.11 headers (obs-deps) into `.deps/`. The dock links against the Qt frameworks inside OBS.app and needs no moc (no `Q_OBJECT`).

Dev aid: create `~/Library/Application Support/obs-studio/plugin_config/obs-beauty/snapshot/` and restart OBS; the dock renders every tab into PNGs there.

## Build & install (Windows x64)

Needs Visual Studio 2022+ (C++ workload) and CMake 3.24+:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
cpack --config build/CPackConfig.cmake -C Release -B dist
```

The first configure downloads the OBS 33.0.0 release (its DLLs are turned into import libraries
with `dumpbin`/`lib`), OBS's Qt 6.11 package, ONNX Runtime DirectML 1.24.4, DirectML 1.15.4 and the
models. Unzip `dist/obs-beauty-<version>-windows-x64.zip` into your OBS folder
(`C:\Program Files\obs-studio`) so its `obs-plugins` and `data` folders merge, then restart OBS.

ONNX Runtime ships renamed (`obs-beauty-onnxruntime.dll`) and is loaded by full path, so it can't
clash with another plugin's `onnxruntime.dll`. GitHub Actions (`.github/workflows/build.yml`)
builds both platforms.

To try the ONNX backend on macOS: `cmake -S . -B build-onnx -DBEAUTY_TRACKER=onnx`.

## Dev loop

`tools/obsctl.py` drives OBS through obs-websocket (reads the password from OBS's config):

```sh
python3 tools/obsctl.py set "Video Capture Device" smooth=80 eye_size=40
python3 tools/obsctl.py reload "Video Capture Device"     # after editing .effect files
python3 tools/obsctl.py shot "Video Capture Device" out.png 1280
```

Shader edits only need `install-plugin` + `reload` (effects are compiled from source, bypassing libobs's per-path effect cache). C/ObjC and `presets.json` changes need an OBS restart.

## Adding content

- **Filter:** add an entry to `data/filters.json` (keys: exposure, contrast, curve, saturation, vibrance,
  warmth, tint, hue, fade, rolloff, gamma_r/g/b, shadows + shadows_amt, highlights + highlights_amt,
  mono, mono_tint).
- **Sticker effect:** draw an SVG in `art/stickers/`, render it to `data/stickers/<name>.png`, and add an
  effect to `data/effects.json` with sprites anchored to `eyes / eye_l / eye_r / nose / mouth / chin /
  forehead / head_top / cheek_l / cheek_r` (offsets and widths in inter-ocular-distance units; optional
  `anim`: bob, pulse, spin, sway, twinkle; `blend`: normal, add, multiply). Distortions use a `shape`
  object with reshape-slider keys.
- **Light / ambience:** `data/lights.json` (az/el in degrees, key/rim colours, fill, bg_dim) and
  `data/atmosphere.json`.

Preset files are read at startup, so restart OBS after editing them.

## Tracking tests

`tests/jitter.c` and `tests/lag.c` feed a still photo (with simulated sensor noise, and then a
pan) through the tracker and report landmark jitter and motion lag in 1080p pixels:

```sh
clang -O2 -fobjc-arc -I src -framework Foundation -framework Vision -framework CoreVideo \
  -framework CoreGraphics -framework ImageIO -o /tmp/jitter tests/jitter.c src/face-tracker-vision.m
/tmp/jitter face.png
```

## Status

- [x] Appearance tab: all 11 streaming-app controls (34 sliders)
- [x] Makeup tab: 15 looks (`data/makeup/presets.json`) with Filter + Makeup intensities; customisable lips/gloss, blush (3 placements), eyeshadow, eyeliner, contour, highlight, brow tint; colour grade + `.cube` LUT
- [x] Effects: 23 landmark stickers / distortions (`data/effects.json`, art in `art/stickers/*.svg` → `data/stickers/*.png`)
- [x] Atmosphere: 13 GPU particle ambiences + 6 motion effects (`data/atmosphere.json`, `data/effects/post.effect`)
- [x] Filters: 26 colour grades baked to 3D LUTs at runtime (`data/filters.json`, `src/grade.c`)
- [x] Light: 9 whole-body virtual lights: Apple Vision person segmentation gives the real silhouette
      (normals from the blurred mask, face ellipsoid blended in, rim along the outline, true background dim)
- [x] Per-tab on/off switch (header toggle follows the current tab: `enhance_on`, `effects_on`, `atmo_on`,
      `filters_on`, `light_on`)
- [x] Dock thumbnails rendered from your own face (CPU mirror of the shaders, `src/dock/thumbs.cpp`)
- [x] Windows (ONNX Runtime + DirectML backend), verified on macOS via `-DBEAUTY_TRACKER=onnx`
- [ ] Multiple faces, Linux
