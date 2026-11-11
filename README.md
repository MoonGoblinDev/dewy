# Dewy

**Real-time beauty for OBS.** A face-aware beauty filter that runs natively inside OBS, as a
regular video filter, with no virtual camera. Skin smoothing, tone, face reshaping, makeup,
stickers, ambience, colour filters and virtual studio lighting, all controlled from one dock.

<p align="center">
  <img src="docs/media/demo.webp" alt="Dewy demo: a slider panel on the left raises one slider at a time (smooth, foundation, redness, skin tone, slim face, bigger eyes, bright eyes, Rose makeup, Soft Box light, Fresh filter) while the right shows Dewy on top and the original camera below" width="960">
</p>
<p align="center">Each slider rises one at a time: Dewy on top, the original camera below.<br>
<a href="docs/media/demo.mp4">▶ Watch the demo video (MP4, 30 s)</a></p>

![OBS with the Dewy dock: a streamer in the preview, the Enhance tab showing Smooth, Foundation, Skin tone and Redness sliders](docs/media/obs-dewy.png)

## Why Dewy

Most beauty filters paint over the skin: they blur it, then blend a skin colour on top. That's what
makes faces look flat and plastic, because the paint also covers the light and shadow that give a
face its shape.

Dewy edits skin the way skin actually works. In optical density, every skin pixel is
**melanin + hemoglobin + shading**, and Dewy calibrates the first two on *your* skin and *your*
camera every two seconds. Skin tone changes melanin. Foundation evens out melanin and hemoglobin
blotches. Redness removes excess hemoglobin. Makeup is deposited as a pigment layer. The
shading (your lighting, your shadows, the 3-D form of your face) is never touched. Measured, it
keeps 98–102 % of the original shadow depth; Dewy's own earlier paint-over version kept only 59–67 %.
The details are in [research/pigment/FINDINGS.md](research/pigment/FINDINGS.md).

## Features

| Tab | What it does |
|---|---|
| **Enhance → Appearance** | Smooth (keeps pores and grain), Foundation, Skin tone (lighter / deeper), Redness, Shadows (dark circles), smile lines, teeth whitening, bright eyes, clarity, plus 27 reshape sliders for face, eyes, nose, mouth and brows |
| **Enhance → Makeup** | 15 looks, or build your own: lipstick and gloss, blush (3 placements), eyeshadow, eyeliner, contour, highlight, brow tint, colour grade and `.cube` LUTs |
| **Effects** | 23 face-tracked stickers and distortions (ears, glasses, crowns, freckles, …) |
| **Atmosphere** | 13 GPU particle ambiences and 6 camera-motion effects |
| **Filters** | 26 colour grades, baked to 3D LUTs at runtime |
| **Light** | 9 whole-body virtual lights (key, rim, fill, background dim) that follow your real silhouette |

- Every tab has its own on/off switch, and the dock's thumbnails are rendered from your own face.
- Landmarks are tracked frame-synchronised, so makeup and stickers stay stuck to your face.
- Skin edits use a learned segmenter (face skin, body skin, hair, clothes), so the neck and hands match
  the face, and walls, doors and shirts are never treated as skin.

## Install

There are no prebuilt releases yet. Build from source (below), then restart OBS.

1. The **Dewy** dock opens the first time (later: **Docks → Dewy**).
2. Pick your camera in the dock and press **Add Dewy to this source**, or add the **Dewy** filter to
   any video source from its Filters window.

**Requirements:** OBS Studio 33 and macOS on Apple Silicon (tested). The Windows build compiles
in CI but **hasn't been run on a Windows PC yet**, so expect fixes to be needed.

## Performance

Measured on an M4 Pro MacBook Pro with a 1080p30 camera in a 1080p60 OBS canvas. The extra cost
of the filter barely depends on which features are on:

| | Filter off | Dewy on (every tab active) |
|---|---|---|
| OBS render time per frame | 5.4 ms | 6.0–6.2 ms |
| OBS CPU | baseline | about +15 % of one core |
| GPU | baseline | about +7–9 % |
| Skipped frames | 0 | 0 |

Face tracking runs on Apple Vision, and segmentation runs mostly on the Neural Engine (Core ML via
ONNX Runtime), so little of the work competes with a game for the GPU.

## How it works

- **Face tracking.** On macOS, Apple Vision provides 76 landmarks with seeded tracking. The ONNX
  backend (Windows, or optionally macOS) runs MediaPipe Face Landmarker v2 (478 points) with
  OpenCV YuNet for re-detection. Both feed one portable core (`src/tracker/tracker-core.cpp`):
  One-Euro smoothing, frame history with interpolation, frame sync, and repeated-frame detection.
- **Segmentation.** MediaPipe's selfie multiclass model runs about 15 times a second. Its mask is
  snapped to the frame's real edges on the GPU with joint-bilateral upsampling.
- **Skin.** Frequency separation (base / blemish band / fine grain) handles smoothing. Colour
  edits (tone, foundation, redness, makeup) happen in optical density on the calibrated
  melanin and hemoglobin directions. See [research/pigment/](research/pigment/).
- **Everything else** is GPU shaders in OBS's own render pipeline (`data/effects/`): face warp,
  makeup, LUTs, particles and relighting.

[docs/RESEARCH.md](docs/RESEARCH.md) maps each control to the technique behind it.

## Build from source

### macOS (Apple Silicon)

Needs Xcode command-line tools, CMake 3.24+ and OBS Studio 33 installed in `/Applications`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build --target install-plugin   # installs into ~/Library/Application Support/obs-studio/plugins
```

The first configure downloads the OBS 33.0.0 headers, SIMDe, OBS's Qt 6.11 headers, ONNX Runtime
and the models into `.deps/` and `data/models/`. The plugin links against the frameworks inside
OBS.app.

### Windows x64 (builds in CI, not yet tested on a PC)

Needs Visual Studio 2022+ with the C++ workload, and CMake 3.24+.

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
cpack --config build/CPackConfig.cmake -C Release -B dist
```

Unzip `dist/dewy-<version>-windows-x64.zip` into your OBS folder (`C:\Program Files\obs-studio`) so
that its `obs-plugins` and `data` folders merge. ONNX Runtime with DirectML ships renamed as
`dewy-onnxruntime.dll` and is loaded by full path, so it can't clash with another plugin's copy.

To try the ONNX tracking backend on macOS: `cmake -S . -B build-onnx -DBEAUTY_TRACKER=onnx`.

## Development

`tools/obsctl.py` drives OBS through obs-websocket. It reads the password from OBS's own config.

```sh
python3 tools/obsctl.py set "Video Capture Device" smooth=80 eye_size=40
python3 tools/obsctl.py reload "Video Capture Device"     # after editing .effect files
python3 tools/obsctl.py shot "Video Capture Device" out.png 1280
```

- Shader edits only need `install-plugin` followed by `reload`, because effects are compiled from
  source and bypass libobs's effect cache.
- C, Objective-C and preset JSON changes need an OBS restart.
- To render every dock tab to PNG, create
  `~/Library/Application Support/obs-studio/plugin_config/dewy/snapshot/` and restart OBS.

### Adding content

- **Filters:** add an entry to `data/filters.json` (exposure, contrast, curve, saturation, vibrance,
  warmth, tint, hue, fade, rolloff, per-channel gamma, shadows / highlights, mono).
- **Stickers:** draw an SVG in `art/stickers/`, render it to `data/stickers/<name>.png`, and add an
  effect to `data/effects.json`. Sprites anchor to `eyes`, `eye_l`, `eye_r`, `nose`, `mouth`, `chin`,
  `forehead`, `head_top`, `cheek_l` or `cheek_r`, with offsets and widths in eye-distance units, an
  optional `anim` (bob, pulse, spin, sway, twinkle) and `blend` (normal, add, multiply).
- **Lights and ambiences:** `data/lights.json` and `data/atmosphere.json`.

### Project layout

```
src/beauty-filter.c        the OBS filter: passes, uniforms, tracking hand-off
src/tracker/               face tracking, segmentation, pigment calibration
src/dock/                  the Qt dock (no moc)
data/effects/              GPU shaders
data/*.json, data/makeup/  presets
research/pigment/          the pigment-model prototype and its findings
tests/                     tracking jitter / lag harnesses (need updating for the current tracker)
```

## Roadmap

- Prebuilt releases for macOS
- Test the Windows build on real hardware
- Validate the pigment model across a wide range of skin tones
- Multiple faces, Linux

## Credits

- [OBS Studio](https://obsproject.com) (GPL-2.0)
- [ONNX Runtime](https://onnxruntime.ai) (MIT)
- Google MediaPipe Face Landmarker and selfie segmentation models (Apache-2.0)
- OpenCV YuNet (MIT)

Model sources and licenses are listed in [data/models/NOTICE.md](data/models/NOTICE.md). The pigment
model follows Tsumura et al., *Image-based skin color and texture analysis/synthesis by extracting
hemoglobin and melanin information in the skin* (SIGGRAPH 2003).

Dewy is an independent project, not affiliated with streaming-app or OBS.

## License

GPL-2.0-or-later, the same as OBS Studio. See [LICENSE](LICENSE).
