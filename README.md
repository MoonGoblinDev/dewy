<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="art/brand/dewy-wordmark-white.png">
    <img src="art/brand/dewy-wordmark.png" alt="Dewy" width="300">
  </picture>
</p>

# Dewy

**Real-time beauty for OBS.** A face-aware beauty filter that runs natively inside OBS, as a
regular video filter, with no virtual camera. Skin smoothing, tone, face reshaping, makeup,
stickers, ambience, colour filters and virtual studio lighting, all controlled from one dock.

https://github.com/user-attachments/assets/15238be5-787a-48c5-b02a-7659ed85f954

## Why Dewy

Most beauty filters paint over the skin: they blur it, then blend a skin colour on top. That's what
makes faces look flat and plastic, because the paint also covers the light and shadow that give a
face its shape.

Dewy edits skin the way skin actually works. Every skin pixel is **melanin + hemoglobin + shading**,
and Dewy calibrates the first two on *your* skin and *your* camera. Skin tone, foundation, redness
and makeup change the pigments; the shading (your lighting, your shadows, the shape of your face) is
never touched.

## Features

| Tab | What it does |
|---|---|
| **Presets** | 26 one-click full looks (Everyday, Glam, Studio, Mood, Men) that set every tab at once, plus your own: save the current look in one click, right-click to rename or delete |
| **Enhance → Appearance** | Smooth (keeps pores and grain), Foundation, Skin tone (lighter / deeper), Redness, Shadows (dark circles), smile lines, teeth whitening, bright eyes, clarity, plus 27 reshape sliders for face, eyes, nose, mouth and brows |
| **Enhance → Makeup** | 15 looks, or build your own: lipstick and gloss, blush (3 placements), eyeshadow, eyeliner, contour, highlight, brow tint, colour grade and `.cube` LUTs |
| **Effects** | 23 face-tracked stickers and distortions (ears, glasses, crowns, freckles, …) |
| **Atmosphere** | 13 GPU particle ambiences and 6 camera-motion effects |
| **Filters** | 26 colour grades, baked to 3D LUTs at runtime |
| **Light** | 9 whole-body virtual lights (key, rim, fill, background dim) that follow your real silhouette |

- Every tab has its own on/off switch, and the dock's thumbnails are rendered from your own face.
- Makeup and stickers stay locked to your face as you move.
- Only skin is edited: walls, doors and clothes are left alone, and your neck and hands match your face.
- Control looks from a Stream Deck or any obs-websocket tool.

## Install

Download the latest build from [Releases](https://github.com/MoonGoblinDev/dewy/releases):

- **macOS (Apple Silicon):** run `dewy-<version>-macos-arm64.pkg`. It's signed and notarized by
  Apple and installs into your user's OBS plugins folder.
- **Windows (x64):** close OBS and unzip `dewy-<version>-windows-x64.zip` into
  `C:\ProgramData\obs-studio\plugins\`. You get a `dewy` folder there; no admin rights needed.

Then start OBS:

1. The **Dewy** dock opens the first time (later: **Docks → Dewy**).
2. Pick your camera in the dock and press **Add Dewy to this source**, or add the **Dewy** filter to
   any video source from its Filters window.

**Requirements:** OBS Studio 33 or later on macOS (Apple Silicon) or Windows 10/11 x64 with a
DirectX 12 GPU.

**Updates:** when a new version is out, the dock shows a notice with a download link. Dewy checks
GitHub's public releases at most once a day and sends nothing else; turn it off under
**Enhance → Appearance → Advanced → Check for updates**.

## Performance

On an M4 Pro MacBook Pro (1080p camera, 1080p60
canvas), Dewy with every tab on adds under 1 ms of OBS render time per frame and about 15 % of one
CPU core, with no skipped frames. Face tracking and segmentation run mostly on the Neural Engine on
macOS and on the GPU (DirectML) on Windows.

## Building from source

See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Credits

- [OBS Studio](https://obsproject.com) (GPL-2.0)
- [ONNX Runtime](https://onnxruntime.ai) (MIT)
- Google MediaPipe Face Landmarker and selfie segmentation models (Apache-2.0)
- OpenCV YuNet (MIT)

Model sources and licenses are listed in [data/models/NOTICE.md](data/models/NOTICE.md). The pigment
model follows Tsumura et al., *Image-based skin color and texture analysis/synthesis by extracting
hemoglobin and melanin information in the skin* (SIGGRAPH 2003).

Dewy is an independent project, not affiliated with the OBS Project.

## License

GPL-2.0-or-later, the same as OBS Studio. See [LICENSE](LICENSE).
