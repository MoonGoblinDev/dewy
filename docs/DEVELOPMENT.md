# Developing Dewy

## How it works

- **Face tracking.** On macOS, Apple Vision provides 76 landmarks with seeded tracking. The ONNX
  backend (Windows, or optionally macOS) runs MediaPipe Face Landmarker v2 (478 points) with
  OpenCV YuNet for re-detection. Both feed one portable core (`src/tracker/tracker-core.cpp`):
  One-Euro smoothing, frame history with interpolation, frame sync, and repeated-frame detection.
- **Segmentation.** MediaPipe's selfie multiclass model runs about 15 times a second. Its mask is
  snapped to the frame's real edges on the GPU with joint-bilateral upsampling.
- **Skin.** Frequency separation (base / blemish band / fine grain) handles smoothing. Colour
  edits (tone, foundation, redness, makeup) happen in optical density on the calibrated
  melanin and hemoglobin directions. See [research/pigment/](../research/pigment/).
- **Everything else** is GPU shaders in OBS's own render pipeline (`data/effects/`): face warp,
  makeup, LUTs, particles and relighting.

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

### Windows x64

Needs Visual Studio 2022+ (or its Build Tools) with the C++ workload, and CMake 3.24+. The CMake that
ships with Visual Studio works.

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
cpack --config build/CPackConfig.cmake -C Release -B dist
```

Unzip `dist/dewy-<version>-windows-x64.zip` into `C:\ProgramData\obs-studio\plugins\`, OBS's
per-machine plugin folder (no admin rights, survives OBS updates). ONNX Runtime with DirectML ships renamed as
`dewy-onnxruntime.dll` and is loaded by full path, so it can't clash with another plugin's copy.

To try the ONNX tracking backend on macOS: `cmake -S . -B build-onnx -DBEAUTY_TRACKER=onnx`.

For development on Windows, close OBS and run
`cmake --build build --config Release --target install-plugin`. It installs into
`C:\ProgramData\obs-studio\plugins\dewy`, which needs no admin rights.

Environment variables for diagnosing inference (set them before starting OBS):
`DEWY_PROFILE=1` logs the landmark pass's preprocessing and `Run` times, `DEWY_ORT_CPU=1` forces CPU
inference, and `DEWY_DML_ADAPTER=<n>` overrides the DirectML adapter, which otherwise matches OBS's own.

## Working on the plugin

`tools/obsctl.py` drives OBS through obs-websocket. It reads the password from OBS's own config.

```sh
python3 tools/obsctl.py set "Video Capture Device" smooth=80 eye_size=40
python3 tools/obsctl.py reload "Video Capture Device"     # after editing .effect files
python3 tools/obsctl.py shot "Video Capture Device" out.png 1280
```

- Shader edits only need `install-plugin` followed by `reload`, because effects are compiled from
  source and bypass libobs's effect cache.
- C, Objective-C and preset JSON changes need an OBS restart.
- To render every dock tab to PNG, create `plugin_config/dewy/snapshot/` in OBS's config folder
  (`~/Library/Application Support/obs-studio/` on macOS, `%APPDATA%\obs-studio\` on Windows) and
  restart OBS.

## Releasing

Releases are built by GitHub Actions (`.github/workflows/release.yml`) and only run when a version
tag is pushed:

1. Set the version in `CMakeLists.txt` (`project(dewy VERSION 0.1.0 ...)`) and commit it.
2. Tag and push: `git tag v0.1.0 && git push origin v0.1.0`. The tag must match the version.
3. The workflow builds Windows and macOS. On macOS it signs the plugin and its bundled ONNX Runtime
   (Developer ID, hardened runtime), builds a `.pkg`, has Apple notarize it and staples the ticket.
   It then creates a **draft** release with both downloads and `SHA256SUMS.txt`; review it and press
   **Publish**.

The macOS job needs these repository secrets (Settings → Secrets and variables → Actions):

| Secret | What it is |
|---|---|
| `MACOS_CERTIFICATE_P12_BASE64` | Your **Developer ID Application** certificate and private key, plus the **Developer ID Installer** one, exported together from Keychain Access as one `.p12`, base64-encoded |
| `MACOS_CERTIFICATE_PASSWORD` | The password you set when exporting the `.p12` |
| `MACOS_NOTARY_APPLE_ID` | The Apple ID email of your developer account |
| `MACOS_NOTARY_TEAM_ID` | Your 10-character team ID |
| `MACOS_NOTARY_PASSWORD` | An app-specific password for that Apple ID (appleid.apple.com → Sign-In and Security) |

Without a Developer ID Installer certificate in the `.p12`, the macOS download is a signed,
notarized `.zip` of the plugin instead of a `.pkg`. To test packaging locally without signing:
`VERSION=0.1.0 UNSIGNED_PKG=1 tools/package-macos.sh`.

## Automating the dock

Dewy registers an obs-websocket vendor, `dewy`, so scripts, Stream Deck or demo tools can drive the dock
(the filter's settings themselves are set with the regular `SetSourceFilterSettings`):

```json
{"requestType": "CallVendorRequest", "requestData": {"vendorName": "dewy", "requestType": "show",
  "requestData": {"tab": "atmosphere", "sub": "motion"}}}
{"requestType": "CallVendorRequest", "requestData": {"vendorName": "dewy", "requestType": "focus",
  "requestData": {"target": "face_slim"}}}
```

- `show`: `tab` is `enhance`, `effects`, `atmosphere`, `filters` or `light`. The optional `sub` is
  `appearance`, `makeup`, `customise`, `ambience` or `motion`.
- `focus` brings a control into view. `target` is a slider key (`smooth`, `face_slim`, `eye_size`, …),
  `look:<name>` (`look:Rose`), `preset:<name>`, `mypreset:<name>`, or `<setting>:<preset>`
  (`effect_preset:Kitty`, `atmo_preset:Sparkles`, `filter_preset:Fresh`, `light_preset:Neon`).
  The response is `{"ok": true}` when the target exists.
- `apply` applies a full look: `{"target": "preset:Golden Hour"}` or `{"target": "mypreset:My look 1"}`.
  Handy on a Stream Deck button.
- `save` saves the current look as a new preset, `reset` presses Reset on the current tab, and
  `tabs_scroll` (`{"dir": 1}`) scrolls the tab strip.
- `locate` returns where a control is on screen (`x`, `y`, `w`, `h`, plus `kx`, `ky` for a slider's knob).
  It's used to script demo videos.

## Adding content

- **Filters:** add an entry to `data/filters.json` (exposure, contrast, curve, saturation, vibrance,
  warmth, tint, hue, fade, rolloff, per-channel gamma, shadows / highlights, mono).
- **Stickers:** draw an SVG in `art/stickers/`, render it to `data/stickers/<name>.png`, and add an
  effect to `data/effects.json`. Sprites anchor to `eyes`, `eye_l`, `eye_r`, `nose`, `mouth`, `chin`,
  `forehead`, `head_top`, `cheek_l` or `cheek_r`, with offsets and widths in eye-distance units, an
  optional `anim` (bob, pulse, spin, sway, twinkle) and `blend` (normal, add, multiply).
- **Lights and ambiences:** `data/lights.json` and `data/atmosphere.json`.
- **Presets:** `data/looks.json`. A look lists only the settings it changes; everything else is reset
  to its default when the look is applied.

## Project layout

```
src/beauty-filter.c        the OBS filter: passes, uniforms, tracking hand-off
src/tracker/               face tracking, segmentation, pigment calibration
src/dock/                  the Qt dock (no moc)
data/effects/              GPU shaders
data/*.json, data/makeup/  presets (looks.json: the Presets tab)
art/brand/                 the Dewy logo (SVG sources and PNGs)
research/pigment/          the pigment-model prototype and its findings
tests/                     tracking jitter / lag harnesses (need updating for the current tracker)
```
