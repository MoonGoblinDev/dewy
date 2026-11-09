#!/usr/bin/env python3
"""Skin pigment model prototype: melanin / hemoglobin / shading separation and edits.

Model (Tsumura et al. 1999/2003): in linear RGB, skin colour is pigment x light, so in
optical density (OD = -log linear RGB) it becomes a sum:

    OD = m * s_mel + h * s_hem + s * u + bias        u = (1,1,1)/sqrt(3)  (shading / intensity)

m, h are melanin / hemoglobin amounts, s is shading. Shading scales all channels equally,
so it is the (1,1,1) direction. We remove it, run ICA on the remaining 2-D chromatic
plane over the user's own skin pixels to find the two chromophore directions for *this*
camera and light, then edit m / h and recompose. Shading (shadows, 3-D form) and surface
shine are never touched.

Usage:  python3 pigment.py frames/day.png [more frames...]
Writes  out/<name>_maps.png, out/<name>_edits.png and prints metrics.
"""

import math
import os
import sys

import cv2
import numpy as np
import onnxruntime as ort
from sklearn.decomposition import FastICA

HERE = os.path.dirname(os.path.abspath(__file__))
MODELS = os.path.join(HERE, "..", "..", "data", "models")
OUT = os.path.join(HERE, "out")

# Priors used only to (1) tell the two ICA components apart and (2) give each chromophore
# its brightness component (ICA only sees the chromatic plane). Melanin absorbs more at
# short wavelengths (B > G > R); hemoglobin peaks in green (G > B > R).
PRIOR_MEL = np.array([0.33, 0.51, 0.79])
PRIOR_HEM = np.array([0.16, 0.79, 0.59])
U = np.ones(3) / math.sqrt(3)
TONE_OD = 0.70   # melanin change at slider +-1, in optical density along the melanin direction
BLUSH_OD = 0.12  # hemoglobin added at the cheek centre at blush 1.0


# ------------------------------------------------------------------ helpers

def srgb_to_lin(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def lin_to_srgb(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055)


def unit(v):
    return v / np.linalg.norm(v)


def norm_blur(x, w, sigma):
    """Mask-weighted (normalised) gaussian blur: only skin contributes."""
    num = cv2.GaussianBlur(x * w, (0, 0), sigma)
    den = cv2.GaussianBlur(w, (0, 0), sigma)
    return num / np.maximum(den, 1e-4)


# ------------------------------------------------------------------ masks (same models as the plugin)

class Analyzer:
    def __init__(self):
        p = ["CPUExecutionProvider"]
        self.seg = ort.InferenceSession(os.path.join(MODELS, "selfie_multiclass_256.onnx"), providers=p)
        self.det = ort.InferenceSession(os.path.join(MODELS, "face_detection_yunet.onnx"), providers=p)
        self.lmk = ort.InferenceSession(os.path.join(MODELS, "face_landmarks_478.onnx"), providers=p)

    def segment(self, img):
        H, W = img.shape[:2]
        x = cv2.cvtColor(cv2.resize(img, (256, 256), interpolation=cv2.INTER_AREA), cv2.COLOR_BGR2RGB)
        lg = self.seg.run(None, {"input_29": x.astype(np.float32)[None] / 255.0})[0][0]
        e = np.exp(lg - lg.max(-1, keepdims=True))
        p = e / e.sum(-1, keepdims=True)
        up = lambda c: cv2.resize(p[..., c], (W, H), interpolation=cv2.INTER_LINEAR)
        return {"skin": np.clip(up(2) + up(3), 0, 1), "face": up(3), "hair": up(1)}

    def landmarks(self, img):
        H, W = img.shape[:2]
        sc = 640 / max(W, H)
        pad = np.zeros((640, 640, 3), np.uint8)
        pad[: int(H * sc), : int(W * sc)] = cv2.resize(img, (int(W * sc), int(H * sc)))
        out = dict(zip([o.name for o in self.det.get_outputs()],
                       self.det.run(None, {"input": pad.transpose(2, 0, 1)[None].astype(np.float32)})))
        best = None
        for s in (8, 16, 32):
            cols = 640 // s
            sc_ = np.sqrt(np.clip(out[f"cls_{s}"][0, :, 0], 0, 1) * np.clip(out[f"obj_{s}"][0, :, 0], 0, 1))
            i = int(sc_.argmax())
            if best is None or sc_[i] > best[0]:
                r, c = divmod(i, cols)
                bb, kp = out[f"bbox_{s}"][0, i], out[f"kps_{s}"][0, i]
                best = (sc_[i], (c + bb[0]) * s / sc, (r + bb[1]) * s / sc,
                        max(math.exp(bb[2]), math.exp(bb[3])) * s / sc,
                        math.atan2(kp[3] - kp[1], kp[2] - kp[0]))
        if best[0] < 0.6:
            return None
        _, cx, cy, size, roll = best
        for _ in range(2):  # crop from the detector, then refine once from the mesh itself
            size *= 1.5 if _ == 0 else 1.0
            c, s_ = math.cos(roll), math.sin(roll)
            k = size / 256
            A = np.array([[c * k, -s_ * k, cx - (c * 128 - s_ * 128) * k], [s_ * k, c * k, cy - (s_ * 128 + c * 128) * k]])
            crop = cv2.warpAffine(img, cv2.invertAffineTransform(A), (256, 256))
            o = self.lmk.run(None, {"input_12": cv2.cvtColor(crop, cv2.COLOR_BGR2RGB).astype(np.float32)[None] / 255})
            pts = o[0].reshape(478, 3)[:, :2]
            pts = (A[:, :2] @ pts.T).T + A[:, 2]
            x0, y0 = pts[:468].min(0)
            x1, y1 = pts[:468].max(0)
            cx, cy, size = (x0 + x1) / 2, (y0 + y1) / 2, max(x1 - x0, y1 - y0) * 1.5
            roll = math.atan2(pts[263, 1] - pts[33, 1], pts[263, 0] - pts[33, 0])
        return pts

    def protect(self, img, pts):
        """Eyes, lips, brows as a soft mask (features are not skin)."""
        H, W = img.shape[:2]
        m = np.zeros((H, W), np.float32)
        if pts is None:
            return m
        rings = [
            [263, 249, 390, 373, 374, 380, 381, 382, 362, 398, 384, 385, 386, 387, 388, 466],
            [33, 7, 163, 144, 145, 153, 154, 155, 133, 173, 157, 158, 159, 160, 161, 246],
            [61, 146, 91, 181, 84, 17, 314, 405, 321, 375, 291, 409, 270, 269, 267, 0, 37, 39, 40, 185],
            [276, 283, 282, 295, 285, 300, 293, 334, 296, 336],
            [46, 53, 52, 65, 55, 70, 63, 105, 66, 107],
        ]
        for r in rings:
            p = pts[r]
            c = p.mean(0)
            order = np.argsort(np.arctan2(p[:, 1] - c[1], p[:, 0] - c[0]))
            cv2.fillPoly(m, [p[order].astype(np.int32)], 1.0)
        iod = np.linalg.norm(pts[263] - pts[33])
        return cv2.GaussianBlur(m, (0, 0), max(1.0, iod * 0.02))


# ------------------------------------------------------------------ the pigment model

class PigmentModel:
    def __init__(self, img_bgr, skin_w):
        self.rgb = img_bgr[..., ::-1].astype(np.float32) / 255.0
        self.lin = srgb_to_lin(self.rgb)
        self.od = -np.log(np.maximum(self.lin, 1e-3))
        luma = (self.lin * [0.2126, 0.7152, 0.0722]).sum(-1)
        sat = self.lin.max(-1) - self.lin.min(-1)
        # shine (specular): bright and nearly colourless relative to skin
        self.spec = np.clip((luma - 0.45) / 0.25, 0, 1) * np.clip(1 - sat / np.maximum(luma, 1e-3) / 0.35, 0, 1)
        clipped = (self.lin.max(-1) > 0.97) | (luma < 0.01)
        self.w = skin_w * (1 - self.spec) * (~clipped)
        self.fit()

    def fit(self):
        sel = self.w > 0.85
        od = self.od[sel]
        if len(od) > 40000:
            od = od[np.random.default_rng(0).choice(len(od), 40000, replace=False)]
        self.ref = np.median(od, 0)
        d = od - self.ref
        plane = d - np.outer(d @ U, U)  # remove shading (intensity) direction
        # 2-D basis of the chromatic plane
        e1 = unit(np.cross(U, [0, 0, 1.0]))
        e2 = np.cross(U, e1)
        p2 = np.stack([plane @ e1, plane @ e2], 1)
        ica = FastICA(n_components=2, whiten="unit-variance", random_state=0, max_iter=1000)
        ica.fit(p2)
        cols = ica.mixing_.T  # each row: a component's direction in the plane
        dirs = [unit(c[0] * e1 + c[1] * e2) for c in cols]
        # orient + label with the priors (projected into the plane)
        pm = unit(PRIOR_MEL - (PRIOR_MEL @ U) * U)
        ph = unit(PRIOR_HEM - (PRIOR_HEM @ U) * U)
        dirs = [d if d @ (pm + ph) >= 0 else -d for d in dirs]
        score = abs(dirs[0] @ pm) - abs(dirs[0] @ ph)
        mel_p, hem_p = (dirs[0], dirs[1]) if score > 0 else (dirs[1], dirs[0])
        if mel_p @ pm < 0:
            mel_p = -mel_p
        if hem_p @ ph < 0:
            hem_p = -hem_p

        # Keep each estimate within MAX_DEG of its physical prior: ICA adapts to the camera,
        # but low light / noise reduction can make it latch onto something non-physical.
        self.raw_angles = (math.degrees(math.acos(np.clip(mel_p @ pm, -1, 1))),
                           math.degrees(math.acos(np.clip(hem_p @ ph, -1, 1))))

        def clamp_cone(d, prior, max_deg=15.0):
            ang = math.acos(np.clip(d @ prior, -1, 1))
            if ang <= math.radians(max_deg):
                return d
            t = math.radians(max_deg) / ang  # slerp from prior towards d, stop at the cone edge
            return unit(math.sin((1 - t) * ang) / math.sin(ang) * prior + math.sin(t * ang) / math.sin(ang) * d)

        mel_p = clamp_cone(mel_p, pm)
        hem_p = clamp_cone(hem_p, ph)

        # give each its brightness component from the prior (ratio of along-U to chromatic part)
        def lift(dp, prior):
            pr_p = prior - (prior @ U) * U
            return unit(dp + (prior @ U) / np.linalg.norm(pr_p) * U)

        self.mel = lift(mel_p, PRIOR_MEL)
        self.hem = lift(hem_p, PRIOR_HEM)
        self.B = np.stack([self.mel, self.hem, U], 1)
        self.Binv = np.linalg.inv(self.B)
        self.angle_check = {
            "mel_vs_prior_deg": math.degrees(math.acos(np.clip(self.mel @ unit(PRIOR_MEL), -1, 1))),
            "hem_vs_prior_deg": math.degrees(math.acos(np.clip(self.hem @ unit(PRIOR_HEM), -1, 1))),
        }
        c = (self.od - self.ref) @ self.Binv.T
        self.m, self.h, self.s = c[..., 0], c[..., 1], c[..., 2]
        sk = self.w > 0.5
        self.m_spread = float(np.std(self.m[sk]))
        self.h_spread = float(np.std(self.h[sk]))

    def recompose(self, dm, dh, weight):
        """Apply melanin / hemoglobin changes (per pixel) and return sRGB 0..1."""
        od = self.od + (dm[..., None] * self.mel + dh[..., None] * self.hem) * weight[..., None]
        return lin_to_srgb(np.exp(-od))


# ------------------------------------------------------------------ edits

def edits(pm, skin_soft, face_px):
    """face_px: rough face size in px (inter-ocular distance) for spatial scales."""
    w = skin_soft * (1 - pm.spec)  # never re-tint shine
    zero = np.zeros_like(pm.m)
    res = {}
    # Skin tone: shift melanin, uniformly over all skin (face, neck, hands alike)
    for t in (-1.0, -0.5, 0.5, 1.0):
        res[f"tone {t:+.1f}"] = pm.recompose(zero - t * TONE_OD, zero, w)
    # Even tone: flatten blotch-scale melanin + hemoglobin variation, keep large-scale variation
    sig = max(2.0, face_px * 0.25)
    m_low = norm_blur(pm.m, pm.w, sig)
    h_low = norm_blur(pm.h, pm.w, sig)
    # colour-led: move the blotches within the colour plane (+35% of melanin's brightness part),
    # so evening never reshapes the light on the face
    mel_c = pm.mel - (pm.mel @ U) * U
    hem_c = pm.hem - (pm.hem @ U) * U
    for a in (0.6, 1.0):
        dm, dh = -(pm.m - m_low) * a * 0.9, -(pm.h - h_low) * a * 0.9
        od = pm.od + (dm[..., None] * (mel_c + 0.35 * (pm.mel - mel_c)) + dh[..., None] * hem_c) * w[..., None]
        res[f"even {a:.1f}"] = lin_to_srgb(np.exp(-od))
    # Redness: remove hemoglobin excess (acne, flush) over its surroundings
    excess = np.maximum(pm.h - h_low, 0)
    res["redness 1.0"] = pm.recompose(zero, -excess * 0.95 - 0.04, w)
    return res


def blush(pm, skin_soft, pts, amount=1.0):
    H, W = pm.m.shape
    g = np.zeros((H, W), np.float32)
    if pts is not None:
        iod = np.linalg.norm(pts[263] - pts[33])
        for eye, mouth_corner in ((33, 61), (263, 291)):
            c = pts[eye] * 0.45 + pts[mouth_corner] * 0.55
            cv2.circle(g, (int(c[0]), int(c[1])), int(iod * 0.32), 1.0, -1)
        g = cv2.GaussianBlur(g, (0, 0), iod * 0.18)
    return pm.recompose(np.zeros_like(pm.m), g * amount * BLUSH_OD, skin_soft * (1 - pm.spec))


# ------------------------------------------------------------------ current shader (for comparison)

def to_ycc(rgb):
    y = rgb @ np.array([0.299, 0.587, 0.114])
    cb = 0.5 + rgb @ np.array([-0.168736, -0.331264, 0.5])
    cr = 0.5 + rgb @ np.array([0.5, -0.418688, -0.081312])
    return np.stack([y, cb, cr], -1)


def to_rgb(ycc):
    y, cb, cr = ycc[..., 0], ycc[..., 1] - 0.5, ycc[..., 2] - 0.5
    return np.stack([y + 1.402 * cr, y - 0.344136 * cb - 0.714136 * cr, y + 1.772 * cb], -1)


def current_tone(rgb, skin, t, iod):
    """beauty-final.effect's Skin tone (as shipped): edits a bilateral base layer in YCbCr."""
    base = cv2.bilateralFilter(rgb.astype(np.float32), 0, 0.09, max(2.0, iod * 0.08))
    det = rgb - base
    b = to_ycc(base)
    y0 = b[..., 0].copy()
    tt = b.copy()
    if t > 0:
        tt[..., 0] = b[..., 0] + (1 - b[..., 0]) * 0.24 * t
        tt[..., 1:] = 0.5 + (b[..., 1:] - 0.5) * (1 - 0.20 * t)
    else:
        d = -t
        tt[..., 0] = b[..., 0] * (1 - 0.40 * d)
        ratio = tt[..., 0] / np.maximum(b[..., 0], 0.02)
        tt[..., 1:] = 0.5 + (b[..., 1:] - 0.5) * (np.sqrt(ratio) * (1 + 0.10 * d))[..., None]
        tt[..., 1] -= 0.005 * d
    b = b + (tt - b) * skin[..., None]
    det = det * np.clip(b[..., 0] / np.maximum(y0, 0.02), 0.35, 1.0)[..., None]
    return np.clip(to_rgb(b) + det, 0, 1)


# ------------------------------------------------------------------ metrics

def log_luma(rgb):
    lin = srgb_to_lin(np.clip(rgb, 0, 1))
    return np.log(np.maximum((lin * [0.2126, 0.7152, 0.0722]).sum(-1), 1e-4))


def shading_preservation(orig, edited, mask, sigma=3.0):
    """Correlation of the log-luminance *structure* (shadows, form, pores) before vs after,
    inside skin. 1.0 = the light and 3-D form are untouched (a pure pigment change)."""
    a, b = log_luma(orig), log_luma(edited)
    a = a - cv2.GaussianBlur(a, (0, 0), sigma * 6)
    b = b - cv2.GaussianBlur(b, (0, 0), sigma * 6)
    m = mask > 0.8
    return float(np.corrcoef(a[m], b[m])[0, 1])


def contrast_ratio(orig, edited, mask):
    """Spread of log-luminance in skin after / before. 1.0 = shadows keep their depth;
    < 1 means the edit flattened (painted over) the shading."""
    m = mask > 0.8
    return float(np.std(log_luma(edited)[m]) / max(np.std(log_luma(orig)[m]), 1e-6))


# ------------------------------------------------------------------ figures

def label(img, text):
    img = img.copy()
    cv2.rectangle(img, (0, 0), (img.shape[1], 26), (0, 0, 0), -1)
    cv2.putText(img, text, (8, 18), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (255, 255, 255), 1, cv2.LINE_AA)
    return img


def to_bgr8(rgb):
    return (np.clip(rgb, 0, 1)[..., ::-1] * 255).astype(np.uint8)


def heat(x, mask, lo=None, hi=None):
    v = x[mask > 0.5]
    lo = np.percentile(v, 2) if lo is None else lo
    hi = np.percentile(v, 98) if hi is None else hi
    n = np.clip((x - lo) / max(hi - lo, 1e-6), 0, 1)
    c = cv2.applyColorMap((n * 255).astype(np.uint8), cv2.COLORMAP_INFERNO)
    c[mask < 0.5] = (c[mask < 0.5] * 0.15).astype(np.uint8)
    return c


def crop_box(pts, shape, scale=2.4):
    H, W = shape[:2]
    if pts is None:
        return 0, 0, W, H
    c = pts[:468].mean(0)
    side = int(np.ptp(pts[:468], 0).max() * scale)
    x0 = int(np.clip(c[0] - side / 2, 0, W - side)) if side < W else 0
    y0 = int(np.clip(c[1] - side * 0.45, 0, H - side)) if side < H else 0
    return x0, y0, min(W, x0 + side), min(H, y0 + side)


def run(path, an):
    name = os.path.splitext(os.path.basename(path))[0]
    img = cv2.imread(path)
    seg = an.segment(img)
    pts = an.landmarks(img)
    prot = an.protect(img, pts)
    skin = np.clip(seg["skin"], 0, 1) * (1 - prot) * (1 - np.clip(seg["hair"] * 2 - 0.4, 0, 1))
    skin_soft = cv2.GaussianBlur(skin, (0, 0), 1.5)
    pm = PigmentModel(img, skin)
    iod = float(np.linalg.norm(pts[263] - pts[33])) if pts is not None else img.shape[1] * 0.08

    res = edits(pm, skin_soft, iod)
    res["blush 1.0"] = blush(pm, skin_soft, pts)
    for t in (-1.0, 1.0):
        res[f"CURRENT {t:+.1f}"] = current_tone(pm.rgb, skin_soft, t, iod)

    print(f"\n== {name}  ({img.shape[1]}x{img.shape[0]}, skin px {(skin > 0.5).sum()})")
    print("   chromophore directions (OD, unit):  melanin", np.round(pm.mel, 3), " hemoglobin", np.round(pm.hem, 3))
    print("   ICA estimate vs prior (chromatic plane): melanin %.1f deg, hemoglobin %.1f deg%s" % (
        pm.raw_angles[0], pm.raw_angles[1], "  -> clamped to 15 deg" if max(pm.raw_angles) > 15 else ""))
    orig = pm.rgb
    for k, v in res.items():
        dbg = (np.abs(v - orig).sum(-1) * (skin < 0.02)).max()
        print(f"   {k:12s} shading-structure corr {shading_preservation(orig, v, skin):.4f}   "
              f"shadow-depth ratio {contrast_ratio(orig, v, skin):.3f}   max change outside skin {dbg:.4f}")

    x0, y0, x1, y1 = crop_box(pts, img.shape)
    crop = lambda a: a[y0:y1, x0:x1]
    size = (360, 360)
    tile = lambda a, t: label(cv2.resize(crop(a), size, interpolation=cv2.INTER_AREA), t)
    maps = np.hstack([
        tile(img, "original"),
        tile((np.stack([skin] * 3, -1) * 255).astype(np.uint8), "skin (learned + landmarks)"),
        tile(heat(pm.m, skin), "melanin"),
        tile(heat(pm.h, skin), "hemoglobin"),
        tile(heat(-pm.s, skin), "shading (light / 3-D form)"),
    ])
    cv2.imwrite(os.path.join(OUT, f"{name}_maps.png"), maps)
    row1 = [tile(img, "original")] + [tile(to_bgr8(res[k]), k) for k in ("tone -1.0", "tone -0.5", "tone +0.5", "tone +1.0")]
    row2 = [tile(img, "original")] + [tile(to_bgr8(res[k]), k) for k in ("even 0.6", "even 1.0", "redness 1.0", "blush 1.0")]
    cv2.imwrite(os.path.join(OUT, f"{name}_edits.png"), np.vstack([np.hstack(row1), np.hstack(row2)]))
    row3 = [tile(to_bgr8(res["CURRENT -1.0"]), "current shader -1"), tile(to_bgr8(res["tone -1.0"]), "pigment -1"),
            tile(img, "original"),
            tile(to_bgr8(res["tone +1.0"]), "pigment +1"), tile(to_bgr8(res["CURRENT +1.0"]), "current shader +1")]
    cv2.imwrite(os.path.join(OUT, f"{name}_vs_current.png"), np.hstack(row3))
    return pm, skin, res, (x0, y0, x1, y1)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    an = Analyzer()
    for p in sys.argv[1:]:
        run(p, an)
