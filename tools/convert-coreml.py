#!/usr/bin/env python3
"""Convert Dewy's ONNX models to Core ML (macOS): ONNX -> PyTorch (onnx2torch) -> ML Program.

    python3 -m venv venv && venv/bin/pip install onnx onnx2torch "torch==2.7.0" coremltools
    venv/bin/python tools/convert-coreml.py

Reads data/models/*.onnx (downloaded by an ONNX Runtime build: Windows, or macOS with
-DBEAUTY_TRACKER=onnx) and writes data/models/coreml/*.mlpackage, which the macOS build compiles
into the plugin. coremltools has no ONNX importer any more, hence the trip through PyTorch.

All three take a 32BGRA pixel buffer as "image" (Core ML converts to the colour order below) and
run in fp16, almost entirely on the Neural Engine:
  face_detection_yunet   640x640 BGR 0..255 -> cls/obj/bbox/kps_{8,16,32} (as the ONNX model)
  face_landmarks_478     256x256 RGB 0..1   -> landmarks [1,1,1,1434] (crop px), presence [1,1,1,1] (logit)
  selfie_multiclass_256  256x256 RGB 0..1   -> mask [1,4,256,256] 0..1: skin, face skin, hair, person
                         (the softmax and class mix that the ONNX path does on the CPU are in the model)
"""
import os, sys
import onnx, torch, coremltools as ct
from onnx2torch import convert
from onnx2torch.node_converters import activations as _act

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "data", "models")
OUT = os.path.join(SRC, "coreml")


def _prelu(self, x, slope):
    # The TFLite-exported landmark model stores PReLU slopes as [1,C,1,1]; onnx2torch only takes
    # its fast path for [C] and otherwise emits a masked scatter that becomes ~20 Core ML ops per
    # activation (1471 ops, 40-80 ms instead of 296 ops, 0.5 ms).
    if slope.numel() == x.shape[1]:
        return torch.nn.functional.prelu(x, slope.reshape(-1))
    return torch.where(x >= 0, x, x * slope)


_act.OnnxPReLU.forward = _prelu


def fixed_batch(m, shape):
    for i in m.graph.input:
        d = i.type.tensor_type.shape.dim
        for k, v in enumerate(shape):
            d[k].ClearField("dim_param")
            d[k].dim_value = v
    for o in m.graph.output:
        d = o.type.tensor_type.shape.dim
        if len(d) and d[0].dim_param:
            d[0].ClearField("dim_param")
            d[0].dim_value = 1
    return m


class Landmarks(torch.nn.Module):
    def __init__(self, net):
        super().__init__()
        self.net = net

    def forward(self, x):  # image NCHW -> the model's NHWC
        y = self.net(x.permute(0, 2, 3, 1))
        return y[0], y[1]  # landmarks, presence logit (drop the unused third head)


class Segmentation(torch.nn.Module):
    def __init__(self, net):
        super().__init__()
        self.net = net

    def forward(self, x):
        p = torch.softmax(self.net(x.permute(0, 2, 3, 1)).permute(0, 3, 1, 2), dim=1)
        # classes: 0 background, 1 hair, 2 body skin, 3 face skin, 4 clothes, 5 accessories
        return torch.cat([p[:, 2:3] + p[:, 3:4], p[:, 3:4], p[:, 1:2], 1.0 - p[:, 0:1]], dim=1)


def build(name, wrap, in_shape, image, outs):
    print("==", name)
    m = fixed_batch(onnx.load(os.path.join(SRC, name + ".onnx")), in_shape)
    net = convert(m).eval()
    if wrap:
        net = wrap(net).eval()
        x = torch.rand(1, 3, in_shape[1], in_shape[2])
    else:
        x = torch.rand(*in_shape) * 255
    with torch.no_grad():
        traced = torch.jit.trace(net, x)
    mlm = ct.convert(traced, convert_to="mlprogram", minimum_deployment_target=ct.target.macOS13,
                     inputs=[ct.ImageType(name="image", shape=x.shape, **image)],
                     outputs=[ct.TensorType(name=o) for o in outs],
                     compute_precision=ct.precision.FLOAT16)
    mlm.short_description = f"Dewy: {name} converted from ONNX by tools/convert-coreml.py"
    mlm.save(os.path.join(OUT, name + ".mlpackage"))


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    rgb = dict(color_layout=ct.colorlayout.RGB, scale=1 / 255.0)
    which = sys.argv[1:] or ["landmarks", "segmentation", "yunet"]
    if "landmarks" in which:
        build("face_landmarks_478", Landmarks, [1, 256, 256, 3], rgb, ["landmarks", "presence"])
    if "segmentation" in which:
        build("selfie_multiclass_256", Segmentation, [1, 256, 256, 3], rgb, ["mask"])
    if "yunet" in which:
        build("face_detection_yunet", None, [1, 3, 640, 640], dict(color_layout=ct.colorlayout.BGR, scale=1.0),
              [f"{k}_{s}" for k in ("cls", "obj", "bbox", "kps") for s in (8, 16, 32)])
