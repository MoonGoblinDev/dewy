# Bundled models (ONNX backend: Windows, optional on macOS)

Downloaded at configure time by CMakeLists.txt; redistributed unchanged.

| File | Model | Source | License |
|---|---|---|---|
| `face_detection_yunet.onnx` | YuNet face detector (2023mar) | [OpenCV Zoo](https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet) | MIT |
| `face_landmarks_478.onnx` | Google MediaPipe Face Landmarker v2 (468 mesh + 10 iris points), ONNX conversion | [senty-au/face_landmarks_detector-ONNX](https://huggingface.co/senty-au/face_landmarks_detector-ONNX) | Apache-2.0 |
| `selfie_multiclass_256.onnx` | Google MediaPipe selfie multiclass segmenter (hair, face skin, body skin, clothes) | [senty-au/selfie_multiclass_256x256-ONNX](https://huggingface.co/senty-au/selfie_multiclass_256x256-ONNX) | Apache-2.0 |
| `selfie_segmentation_landscape.onnx` | Google MediaPipe selfie segmentation (landscape), CPU fallback | [onnx-community/mediapipe_selfie_segmentation_landscape](https://huggingface.co/onnx-community/mediapipe_selfie_segmentation_landscape) | Apache-2.0 |

Model weights © Google LLC / OpenCV contributors, used under their licenses.
