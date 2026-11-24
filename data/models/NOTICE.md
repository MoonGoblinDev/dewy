# Bundled models

The ONNX files are downloaded at configure time by CMakeLists.txt (builds with ONNX Runtime:
Windows, or macOS with `-DBEAUTY_TRACKER=onnx`) and redistributed unchanged.

| File | Model | Source | License |
|---|---|---|---|
| `face_detection_yunet.onnx` | YuNet face detector (2023mar) | [OpenCV Zoo](https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet) | MIT |
| `face_landmarks_478.onnx` | Google MediaPipe Face Landmarker v2 (468 mesh + 10 iris points), ONNX conversion | [senty-au/face_landmarks_detector-ONNX](https://huggingface.co/senty-au/face_landmarks_detector-ONNX) | Apache-2.0 |
| `selfie_multiclass_256.onnx` | Google MediaPipe selfie multiclass segmenter (hair, face skin, body skin, clothes) | [senty-au/selfie_multiclass_256x256-ONNX](https://huggingface.co/senty-au/selfie_multiclass_256x256-ONNX) | Apache-2.0 |
| `selfie_segmentation_landscape.onnx` | Google MediaPipe selfie segmentation (landscape), CPU fallback | [onnx-community/mediapipe_selfie_segmentation_landscape](https://huggingface.co/onnx-community/mediapipe_selfie_segmentation_landscape) | Apache-2.0 |

`coreml/face_detection_yunet.mlpackage`, `coreml/face_landmarks_478.mlpackage` and
`coreml/selfie_multiclass_256.mlpackage` (macOS) are the first three converted to Core ML by
`tools/convert-coreml.py`: same weights in fp16, and segmentation with its softmax and class mix
built in. They keep the licenses above.

Model weights © Google LLC / OpenCV contributors, used under their licenses.

## Canonical face model

`art/face-mesh/canonical_face_model.obj` is Google MediaPipe's canonical face model (468
vertices, UVs, triangles) from
[mediapipe/modules/face_geometry/data](https://github.com/google-ai-edge/mediapipe/tree/master/mediapipe/modules/face_geometry/data),
© Google LLC, Apache-2.0. `src/face-mesh-data.h` and `data/makeup/face-paint.png` are generated
from it by `tools/gen-face-paint.py`.
