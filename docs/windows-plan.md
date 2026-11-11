# Windows: findings, fixes and results

First run of Dewy on a real Windows PC, and the fixes that came out of it (2026-11-11).

**Test machine:** Windows 11 23H2, Intel i5-9300H (4C/8T), GeForce GTX 1050 Ti 4 GB (OBS renders on it,
with an Intel UHD 630 also present), 16 GB RAM, OBS 33.0.1, 1920×1080 canvas at 30 fps (and 60 fps for one test).
**Input:** a 640×360, 24 fps, 10 s talking-head clip (`Woman_streaming_to_camera_*.mp4`) as a looping
Media Source, with the Dewy filter on it. The source is small, so GPU pass costs here are low.
Re-measure with a 1080p camera before you quote numbers for 1080p sources.

## Results

| | Before | After |
|---|---|---|
| OBS render time, filter on (30 fps canvas, sync on) | 5.4 ms | 1.6–2.2 ms |
| OBS render time, filter on (60 fps canvas, sync on) | n/a | 2.2 ms |
| OBS render time, filter off | 1.0 ms | 1.0–1.3 ms |
| GPU used for inference | Intel UHD 630 (by accident) | GTX 1050 Ti, the GPU OBS renders on |
| Skin segmentation per run (standalone benchmark) | ~83 ms | ~17 ms |
| Face tracking per frame (worker thread) | 17–20 ms | 17–20 ms |
| Landmarks actually in sync with the video | no (the wait timed out) | yes, delayed 2 frames at 30 fps, 3 at 60 fps |
| Adding a Dewy filter (blocks the caller) | 1,259 ms | 53 ms |
| A second Dewy filter | loads every model again | reuses the loaded sessions |
| Two Dewy filters running at once | (untested) | stable (it crashed during development; fixed, see 4) |
| OBS CPU (share of one core) | 18 % | 22–24 % |
| Skipped frames | 0 | 0 |

OBS CPU went up by a few points because segmentation now really runs at its 15 Hz target, so its CPU
post-processing runs more often. On the iGPU each run took ~83 ms, which held it to ~10 Hz.

## What was fixed

### 1. Sync tracking blocked the render thread on every frame, and never synced
`update_tracking()` waited up to 4 ms for the landmarks of the frame it had just submitted. That
assumes a tracker that takes ~1 ms (Apple Vision). The ONNX tracker takes 17–20 ms, so the wait always timed out.
The cost was 3.7 ms of idle render thread per frame, and the landmarks still trailed by a frame.

**Fix** (`src/beauty-filter.c`): the captured frames are now a ring of `RING_N` = 4.
`update_sync_delay()` chooses how many frames to delay the video from the tracker's measured mean time
and OBS's frame interval: `1 + ceil((1.2 × mean + 2 ms) / interval)`, capped at 3. It grows at once
and shrinks only after 2 s, so timing jitter doesn't make the video stutter. Trackers faster than
3 ms (Apple Vision) keep the old behaviour, a 1-frame delay with a short wait. The periodic log line now
reports `sync delay N`.

### 2. All inference ran on the integrated GPU (the most important find)
DirectML was always created on adapter `0`. In plain DXGI order (`EnumAdapters1`, which is what
DirectML's `device_id` means), index 0 on this laptop is the **Intel UHD 630**. OBS sorts adapters
differently and lists the GTX 1050 Ti first, so the log looked fine. Measured standalone, the landmark model
takes 18.7 ms on the iGPU vs 8.5 ms on the GTX, and the segmenter 83 ms vs 17 ms.

**Fix** (`src/tracker/onnx-common.cpp`, `face_tracker_set_render_device`): the filter passes OBS's
`ID3D11Device` (`gs_get_device_obj()`) to the tracker. The tracker takes that device's adapter LUID
and finds the same adapter in `EnumAdapters1` order. The log now names the GPU:
`multiclass (DirectML on NVIDIA GeForce GTX 1050 Ti)`. `DEWY_DML_ADAPTER=<n>` overrides it for testing.

### 3. Adding a filter froze OBS for 1.3 s
`beauty_create()` built three ORT sessions synchronously, and DirectML session creation is slow.
**Fix** (`src/tracker/tracker-core.cpp`): models now load on the tracker's own worker threads. Each worker
reports "busy" until its models are ready, so frames submitted meanwhile are dropped and the filter shows
the unmodified face. Accelerated sessions also do one warm-up run on zeros while loading (`ort_warm_up`). The
first real DirectML run otherwise took ~840 ms.

### 4. One set of models for every filter instance, and a driver crash
Sessions are now cached by model path and shared through `std::shared_ptr` (`ort_model` in
`onnx-common.cpp`). A second Dewy filter, or the same camera in two scenes, costs no load time or GPU
memory.

During testing, sharing a session between two filters **crashed OBS inside the NVIDIA driver**
(`nvwgf2umx.dll`, access violation). DirectML doesn't allow two threads in `Run()` on one session
at the same time. Every run now goes through `OrtModel::run`, which serialises calls per model. Then
three instances (two on one source, one on another, toggled every 10 s) ran for 60 s with
0 skipped frames, and 15 rapid create/remove cycles ran with the worst case at 37 ms.

### 5. Papercuts
- The log said `GPU/ANE` on Windows. It now names the provider and adapter (`DirectML on …`, `Core ML`).
- `tools/obsctl.py` found the obs-websocket config only on macOS. It now handles Windows (`%APPDATA%`) and Linux.
- New `install-plugin` target on Windows. It installs into `C:\ProgramData\obs-studio\plugins\dewy`, which
  OBS 32+ loads with no admin rights needed and which survives OBS updates. The README documents it as the
  no-admin install path.
- The sync tracking setting's text in the filter properties and the dock now says 1–3 frames.
- The README's snapshot path now covers Windows.

## Optimisations evaluated and not done

| Idea | Measured / reason | Decision |
|---|---|---|
| `Ort::IoBinding` for the landmark model | 14.71 → 14.19 ms per run (−3 %) on the GTX 1050 Ti | Not worth the code |
| CPU inference instead of DirectML | Landmarks no faster (19–26 ms in OBS), and OBS CPU 88–135 % of a core | Keep DirectML; `DEWY_ORT_CPU=1` stays as a diagnostic |
| GPU-side face crop (skip CPU resampling) | Preprocessing is 1.8 ms of the 17–20 ms, on a worker thread, and doesn't change the chosen delay | Not worth a new render pass |
| FP16 landmark model | Pascal (GTX 10xx) has no fast FP16, so it can't be measured here | Open; needs an RTX / Radeon / Arc machine |
| Caching the optimised graph on disk | Loading is now off the UI thread and shared, and DirectML graphs are adapter-specific | Not needed |

## Remaining work

- Measure on other hardware: an RTX card, a Radeon card, and an iGPU-only laptop. The adaptive delay should
  settle at 1–2 frames on faster GPUs.
- Measure with a real 1080p camera, which costs more in the GPU passes than this 640×360 clip.
- The landmark `Run` takes ~15 ms on the GTX 1050 Ti in OBS (8.5 ms standalone) because it shares the GPU
  with segmentation and OBS rendering. If that matters on slow GPUs, segmentation could run less often
  while the face is still, or drop to 10 Hz automatically when tracking is slow.
- The segmentation softmax and mask post-processing on the CPU could be vectorised. That would win back the
  ~4 points of CPU added by the full 15 Hz rate.
- The skin mask shows a small false positive on the headphone. It's a model issue; check it on macOS too.

## How it was measured

These scripts drive OBS through obs-websocket (`tools/obsctl.py` for the connection): `GetStats` for render time
and skipped frames, `psutil` for OBS process CPU, and `nvidia-smi` for GPU. Each phase is 25–70 s: filter off,
on, on without sync, and every tab active. Model timings come from `DEWY_PROFILE=1` in OBS and a
standalone `onnxruntime-directml` benchmark per DXGI adapter.
