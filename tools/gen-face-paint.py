#!/usr/bin/env python3
"""Bake Dewy's face-paint map and the face-mesh source file from MediaPipe's canonical face model.

    python3 tools/gen-face-paint.py art/face-mesh/canonical_face_model.obj [--preview out.png]

Writes
    data/makeup/face-paint.png   RGBA in the mesh's UV space: R contour, G highlight, B (free), A brow
    data/makeup/face-liner.png   the eyeliner styles, one per channel: R classic, G winged, B cat eye, A smoky
    data/makeup/face-normal.png  the face's surface direction (canonical frame) as RGB = n * 0.5 + 0.5, for lighting
    src/face-mesh-data.h         UVs, triangles and their front-view winding, for rendering the mesh

Every texel of the map knows where it sits on the canonical face (in centimetres: x towards the
subject's left, y up, z towards the camera; eye corners are 6.3 cm apart), so each product is
drawn the way a makeup artist would place it - measured on the face, along the face's own
structure - and then rides on the tracked mesh: it turns with the head and stays on the skin.
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image

N = 2048  # map size
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_obj(path):
    V, T, F = [], [], []
    for line in open(path):
        p = line.split()
        if not p:
            continue
        if p[0] == "v":
            V.append([float(x) for x in p[1:4]])
        elif p[0] == "vt":
            T.append([float(x) for x in p[1:3]])
        elif p[0] == "f":
            F.append([[int(x) for x in q.split("/")[:2]] for q in p[1:]])
    V = np.array(V)
    uv = np.zeros((len(V), 2))
    for f in F:
        for vi, ti in f:
            uv[vi - 1] = T[ti - 1]
    tris = np.array([[vi - 1 for vi, _ in f] for f in F])
    return V, uv, tris


def vertex_normals(V, tris):
    """Smooth outward normals (area-weighted), canonical frame."""
    a, b, c = V[tris[:, 0]], V[tris[:, 1]], V[tris[:, 2]]
    fn = np.cross(b - a, c - a)
    n = np.zeros_like(V)
    for k in range(3):
        np.add.at(n, tris[:, k], fn)
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
    if n[1, 2] < 0:  # the nose tip must face the camera (+z)
        n = -n
    return n


def rasterize(V, uv, tris):
    """Per texel: the attribute V (e.g. canonical 3D position) interpolated, NaN outside the mesh.
    Row 0 = top (v = 1)."""
    P = np.full((N, N, 3), np.nan)
    px = np.stack([uv[:, 0] * N, (1.0 - uv[:, 1]) * N], 1)
    for t in tris:
        a, b, c = px[t]
        x0, x1 = int(max(np.floor(min(a[0], b[0], c[0])), 0)), int(min(np.ceil(max(a[0], b[0], c[0])), N - 1))
        y0, y1 = int(max(np.floor(min(a[1], b[1], c[1])), 0)), int(min(np.ceil(max(a[1], b[1], c[1])), N - 1))
        if x1 < x0 or y1 < y0:
            continue
        xs, ys = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
        if abs(d) < 1e-9:
            continue
        l0 = ((b[1] - c[1]) * (xs - c[0]) + (c[0] - b[0]) * (ys - c[1])) / d
        l1 = ((c[1] - a[1]) * (xs - c[0]) + (a[0] - c[0]) * (ys - c[1])) / d
        l2 = 1 - l0 - l1
        inside = (l0 >= -1e-4) & (l1 >= -1e-4) & (l2 >= -1e-4)
        pos = l0[..., None] * V[t[0]] + l1[..., None] * V[t[1]] + l2[..., None] * V[t[2]]
        sub = P[y0:y1 + 1, x0:x1 + 1]
        sub[inside] = pos[inside]
    return P


def surface(V, tris, x, y):
    """Front-most point of the canonical face at front-view position (x, y)."""
    best = None
    for t in tris:
        a, b, c = V[t]
        d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
        if abs(d) < 1e-9:
            continue
        l0 = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / d
        l1 = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / d
        l2 = 1 - l0 - l1
        if min(l0, l1, l2) >= -1e-6:
            p = l0 * a + l1 * b + l2 * c
            if best is None or p[2] > best[2]:
                best = p
    if best is None:
        raise ValueError(f"({x}, {y}) is off the face")
    return best


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def polyline(P, pts, sigma0, sigma1, s0, s1):
    """Soft stroke along a polyline on the surface (3D distances), with soft round ends. Width
    (sigma) and strength run from the first point to the last."""
    pts = np.asarray(pts, float)
    seg = pts[1:] - pts[:-1]
    seglen = np.linalg.norm(seg, axis=1)
    cum = np.concatenate([[0], np.cumsum(seglen)])
    total = cum[-1]
    best_d = np.full(P.shape[:2], np.inf)
    best_t = np.zeros(P.shape[:2])
    for i in range(len(seg)):
        q = P - pts[i]
        u = np.clip(np.einsum("ijk,k->ij", q, seg[i]) / max(seglen[i] ** 2, 1e-9), 0, 1)
        d = np.linalg.norm(q - u[..., None] * seg[i], axis=2)
        m = d < best_d
        best_d[m] = d[m]
        best_t[m] = (cum[i] + u[m] * seglen[i]) / total
    sigma = sigma0 + (sigma1 - sigma0) * best_t
    s = s0 + (s1 - s0) * best_t
    return np.nan_to_num(s * np.exp(-0.5 * (best_d / sigma) ** 2))


def blob(P, c, sigma, s, squash=(1.0, 1.0, 1.0)):
    q = (P - np.asarray(c)) / np.asarray(squash)
    return np.nan_to_num(s * np.exp(-0.5 * (np.linalg.norm(q, axis=2) / sigma) ** 2))


def poly_fill(P, poly_xy, feather):
    """Inside a front-view polygon, with a soft edge `feather` cm wide."""
    x, y = P[..., 0], P[..., 1]
    poly = np.asarray(poly_xy, float)
    inside = np.zeros(P.shape[:2], bool)
    dmin = np.full(P.shape[:2], np.inf)
    n = len(poly)
    for i in range(n):
        (x0, y0), (x1, y1) = poly[i], poly[(i + 1) % n]
        cond = ((y0 > y) != (y1 > y)) & (x < (x1 - x0) * (y - y0) / ((y1 - y0) + 1e-12) + x0)
        inside ^= np.nan_to_num(cond).astype(bool)
        ex, ey = x1 - x0, y1 - y0
        u = np.clip(((x - x0) * ex + (y - y0) * ey) / (ex * ex + ey * ey), 0, 1)
        dmin = np.minimum(dmin, np.hypot(x - x0 - u * ex, y - y0 - u * ey))
    sd = np.where(inside, dmin, -dmin)
    return np.nan_to_num(smoothstep(-feather, feather, sd))


def eye_liner(P, V, s, style):
    """One eye's liner (s = -1 subject's right, +1 left) in one style, from the canonical lids."""
    R = s < 0
    up_ids = [133, 173, 157, 158, 159, 160, 161, 246, 33] if R else [362, 398, 384, 385, 386, 387, 388, 466, 263]
    lo_ids = [133, 155, 154, 153, 145, 144, 163, 7, 33] if R else [362, 382, 381, 380, 374, 373, 390, 249, 263]

    def curve(ids):  # front view, inner -> outer, x mirrored so "outwards" is +x for both eyes
        c = V[ids][:, :2]
        o = np.argsort(c[:, 0] * s)
        return c[o, 0] * s, c[o, 1]

    ux, uy = curve(up_ids)
    lx, ly = curve(lo_ids)
    x, y = P[..., 0] * s, P[..., 1]
    t = np.clip((x - ux[0]) / (ux[-1] - ux[0]), 0, 1)  # 0 inner corner .. 1 outer corner
    on_lid = np.nan_to_num((x >= ux[0]) & (x <= ux[-1])).astype(float)
    h = y - np.interp(x, ux, uy)  # height above the upper lash line
    corner = np.array([ux[-1], uy[-1]])

    def band(t0, t1, k):  # solid line on the lid, t0 thick at the inner corner growing by t1 outwards
        th = t0 + t1 * t ** k
        # from just under the lid line (it covers the lash fringe, so no skin shows between line and
        # lashes) up to the line's thickness
        return smoothstep(-0.09, -0.04, h) * (1 - smoothstep(th - 0.035, th + 0.035, h)) * smoothstep(0, 0.1, t) * on_lid

    def wing(length, lift, start, t0, t1, k):  # flick from the line's upper edge out past the corner
        x_top = corner[0] - start
        tt = np.clip((x_top - ux[0]) / (ux[-1] - ux[0]), 0, 1)
        poly = np.array([[x_top, np.interp(x_top, ux, uy) + t0 + t1 * tt ** k], corner + np.array([length, lift]),
                         corner + np.array([0.02, -0.02])])
        poly[:, 0] *= s
        return poly_fill(P, poly, 0.035)  # soft enough to stay smooth on a small face

    if style == "classic":  # thin along the lashes, a touch thicker outside, a short taper
        out = np.maximum(band(0.025, 0.06, 1.4), wing(0.14, 0.03, 0.22, 0.025, 0.06, 1.4))
    elif style == "winged":  # medium line, small lifted wing
        out = np.maximum(band(0.03, 0.085, 1.4), wing(0.42, 0.2, 0.32, 0.03, 0.085, 1.4))
    elif style == "cat":  # bold line, long wing swept towards the brow tail
        out = np.maximum(band(0.04, 0.12, 1.2), wing(0.78, 0.42, 0.55, 0.04, 0.12, 1.2))
    else:  # smoky: smudged soft upper line, smudged outer lower lash line, a soft smudge outwards
        x_ext = np.nan_to_num((x >= ux[0]) & (x <= ux[-1] + 0.25)).astype(float)
        upper = smoothstep(-0.04, 0.0, h) * np.exp(-np.maximum(h, 0) / 0.11) * smoothstep(0, 0.25, t) * x_ext
        hl = np.interp(x, lx, ly) - y  # depth below the lower lash line
        tl = np.clip((x - lx[0]) / (lx[-1] - lx[0]), 0, 1)
        lower = smoothstep(-0.03, 0.0, hl) * np.exp(-np.maximum(hl, 0) / 0.07) * 0.6 * smoothstep(0.35, 0.8, tl) * x_ext
        smudge = blob(P, np.array([(corner[0] + 0.22) * s, corner[1] + 0.06, V[up_ids[-1]][2] - 0.1]), 0.16, 0.55)
        out = 0.85 * np.maximum(np.maximum(upper, lower), smudge)
    return np.nan_to_num(out)


LINER_STYLES = ("classic", "winged", "cat", "smoky")  # face-liner.png channels R G B A


def bleed(rgba, P):
    """Spread values a few texels past the mesh's UV edge so bilinear sampling never pulls in black."""
    valid = ~np.isnan(P[..., 0])
    for _ in range(4):
        grown = rgba.copy()
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            sh = np.roll(np.roll(rgba, dy, 0), dx, 1)
            shv = np.roll(np.roll(valid, dy, 0), dx, 1)
            take = ~valid & shv
            grown[take] = np.maximum(grown[take], sh[take])
        valid = valid | np.roll(valid, 1, 0) | np.roll(valid, -1, 0) | np.roll(valid, 1, 1) | np.roll(valid, -1, 1)
        rgba = grown
    return rgba


def build(V, uv, tris):
    P = rasterize(V, uv, tris)
    S = lambda x, y: surface(V, tris, x, y)
    contour = np.zeros((N, N))
    highlight = np.zeros((N, N))
    liners = [np.zeros((N, N)) for _ in LINER_STYLES]
    brow = np.zeros((N, N))

    for s in (-1.0, 1.0):  # subject's right, left
        R = s < 0
        # ---- contour: a shadow under the cheekbone from the ear towards the mouth corner,
        # stopping under the outer corner of the eye; the temples; the jaw edge; the nose sides
        contour = np.maximum(contour, polyline(P, [S(s * 7.1, 0.2), S(s * 6.0, -0.95), S(s * 4.7, -2.15)],
                                               0.66, 0.5, 1.0, 0.8))
        contour = np.maximum(contour, polyline(P, [S(s * 6.1, 5.0), S(s * 6.8, 3.6), S(s * 7.2, 2.0)],
                                               0.65, 0.55, 0.75, 0.6))
        jaw_ids = [132, 58, 172, 136, 150, 149] if R else [361, 288, 397, 365, 379, 378]
        jaw = [V[i] + np.array([-s * 0.25, 0.35, 0.25]) for i in jaw_ids]  # just inside the edge
        contour = np.maximum(contour, polyline(P, jaw, 0.55, 0.45, 0.8, 0.25))
        contour = np.maximum(contour, polyline(P, [S(s * 0.72, 2.6), S(s * 0.8, 1.3), S(s * 0.95, 0.15)],
                                               0.18, 0.2, 0.4, 0.35))

        # ---- highlight: cheekbone tops, under the brow tail, the inner eye corner
        highlight = np.maximum(highlight, polyline(P, [S(s * 3.2, 1.25), S(s * 4.6, 1.5), S(s * 6.0, 2.05)],
                                                   0.42, 0.38, 1.0, 0.7))
        lower_brow = [46, 53, 52] if R else [276, 283, 282]
        highlight = np.maximum(highlight, polyline(P, [V[i] + np.array([0, -0.42, 0]) for i in lower_brow],
                                                   0.2, 0.18, 0.55, 0.4))
        highlight = np.maximum(highlight, blob(P, S(s * 1.55, 2.55), 0.16, 0.45))

        # ---- eyeliner styles
        for k, style in enumerate(LINER_STYLES):
            liners[k] = np.maximum(liners[k], eye_liner(P, V, s, style))

        # ---- brow: the brow's own shape (upper and lower outline), softer and thinner at the tail
        up = [70, 63, 105, 66, 107] if R else [300, 293, 334, 296, 336]
        lo = [46, 53, 52, 65, 55] if R else [276, 283, 282, 295, 285]
        poly = [V[i][:2] for i in up] + [V[i][:2] for i in reversed(lo)]
        fill = poly_fill(P, poly, 0.1)
        xi = P[..., 0] * s
        tail = smoothstep(V[up[0]][0] * s - 0.2, V[up[0]][0] * s + 0.9, xi)  # 0 at the tail end
        head = 1 - 0.35 * smoothstep(V[up[-1]][0] * s + 0.45, V[up[-1]][0] * s - 0.1, xi)  # softer front
        brow = np.maximum(brow, np.nan_to_num(fill * (0.55 + 0.45 * tail) * head))

    # ---- centre-line highlights: nose bridge, forehead, cupid's bow, chin
    highlight = np.maximum(highlight, polyline(P, [V[168], V[6], V[197], V[195]], 0.18, 0.2, 0.75, 0.8))
    highlight = np.maximum(highlight, blob(P, S(0.0, 5.6), 0.95, 0.5, (1.0, 1.2, 1.0)))
    highlight = np.maximum(highlight, blob(P, V[0] + np.array([0, 0.28, 0.05]), 0.2, 0.6, (1.4, 1.0, 1.0)))
    highlight = np.maximum(highlight, blob(P, S(0.0, -7.6), 0.5, 0.55, (1.3, 1.0, 1.0)))

    contour *= np.clip(1.0 - highlight * 1.5, 0, 1)  # never both on one spot
    rgba = bleed(np.clip(np.nan_to_num(np.stack([contour, highlight, np.zeros((N, N)), brow], 2)), 0, 1), P)
    liner = bleed(np.clip(np.stack(liners, 2), 0, 1), P)
    return P, rgba, liner


def write_header(V, uv, tris, path):
    # front-view winding (image space: x right, y down) of every triangle; a tracked triangle
    # whose winding has flipped is facing away from the camera
    img = np.stack([V[:, 0], -V[:, 1]], 1)
    a, b, c = img[tris[:, 0]], img[tris[:, 1]], img[tris[:, 2]]
    area = (b[:, 0] - a[:, 0]) * (c[:, 1] - a[:, 1]) - (b[:, 1] - a[:, 1]) * (c[:, 0] - a[:, 0])
    with open(path, "w") as f:
        f.write("/* Generated by tools/gen-face-paint.py from MediaPipe's canonical face model\n"
                " * (Apache-2.0, see data/models/NOTICE.md). Do not edit. */\n#pragma once\n\n")
        f.write(f"#define FACE_MESH_VERTS {len(V)}\n#define FACE_MESH_TRIS {len(tris)}\n\n")
        f.write("/* texture coordinates (u, v), v = 0 at the top of face-paint.png */\n")
        f.write("static const float face_mesh_uv[FACE_MESH_VERTS][2] = {\n")
        for u, v in uv:
            f.write(f"\t{{{u:.6f}f, {1.0 - v:.6f}f}},\n")
        f.write("};\n\nstatic const unsigned short face_mesh_tris[FACE_MESH_TRIS][3] = {\n")
        for t in tris:
            f.write(f"\t{{{t[0]}, {t[1]}, {t[2]}}},\n")
        f.write("};\n\n/* canonical positions, cm: x towards the subject's left, y up, z towards the camera */\n")
        f.write("static const float face_mesh_pos[FACE_MESH_VERTS][3] = {\n")
        for x, y, z in V:
            f.write(f"\t{{{x:.4f}f, {y:.4f}f, {z:.4f}f}},\n")
        f.write("};\n\n/* +1 / -1: winding in a straight-on view (image space, y down) */\n")
        f.write("static const signed char face_mesh_front[FACE_MESH_TRIS] = {\n")
        for i in range(0, len(area), 24):
            f.write("\t" + ", ".join("1" if x > 0 else "-1" for x in area[i:i + 24]) + ",\n")
        f.write("};\n")


def preview(P, rgba, path):
    """Front view of the canonical face with the paint on it (for checking placement)."""
    W, H = 900, 1100
    img = np.full((H, W, 3), 245.0)
    zbuf = np.full((H, W), -np.inf)
    valid = ~np.isnan(P[..., 0])
    ys, xs = np.nonzero(valid)
    p = P[ys, xs]
    sx = ((p[:, 0] + 8.5) / 17.0 * W).astype(int)
    sy = ((9.8 - p[:, 1]) / 20.0 * H).astype(int)
    ok = (sx >= 0) & (sx < W) & (sy >= 0) & (sy < H)
    order = np.argsort(p[:, 2])
    for i in order[ok[order]]:
        if p[i, 2] > zbuf[sy[i], sx[i]]:
            zbuf[sy[i], sx[i]] = p[i, 2]
            c, hl, ln, br = rgba[ys[i], xs[i]]
            skin = np.array([224.0, 186, 160]) * (0.75 + 0.03 * p[i, 2])
            col = skin * (1 - 0.55 * c) + c * 0.55 * np.array([120.0, 80, 60])
            col = col * (1 - 0.6 * hl) + hl * 0.6 * np.array([255.0, 250, 240])
            col = col * (1 - 0.6 * br) + br * 0.6 * np.array([70.0, 50, 40])
            col = col * (1 - ln) + ln * np.array([15.0, 12, 12])
            img[sy[i], sx[i]] = col
    Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("obj")
    ap.add_argument("--preview")
    a = ap.parse_args()
    V, uv, tris = load_obj(a.obj)
    P, rgba, liner = build(V, uv, tris)
    Nm = rasterize(vertex_normals(V, tris), uv, tris)
    Nm /= np.maximum(np.linalg.norm(np.nan_to_num(Nm), axis=2, keepdims=True), 1e-9)
    # coverage: how much of the face mesh's own shape the light uses, fading out over ~2.5 cm towards
    # the mesh's outline (measured on the face), so the jaw, temples and upper forehead hand over
    # to the head's shape gradually instead of ending at a line
    inside = ~np.isnan(P[..., 0])
    oval = [10, 338, 297, 332, 284, 251, 389, 356, 454, 323, 361, 288, 397, 365, 379, 378, 400, 377, 152, 148,
            176, 149, 150, 136, 172, 58, 132, 93, 234, 127, 162, 21, 54, 103, 67, 109, 10]
    pts = V[oval]
    d = np.full(P.shape[:2], np.inf)
    for a_, b_ in zip(pts[:-1], pts[1:]):
        q = np.nan_to_num(P) - a_
        e = b_ - a_
        u = np.clip(np.einsum("ijk,k->ij", q, e) / (e @ e), 0, 1)
        d = np.minimum(d, np.linalg.norm(q - u[..., None] * e, axis=2))
    cover = smoothstep(0.0, 2.5, d)
    nrm = bleed(np.clip(np.nan_to_num(np.concatenate([Nm * 0.5 + 0.5, cover[..., None]], 2)), 0, 1)
                * inside[..., None], P)
    Image.fromarray((nrm * 255 + 0.5).astype(np.uint8), "RGBA").save(os.path.join(ROOT, "data", "makeup", "face-normal.png"),
                                                                     optimize=True)
    os.makedirs(os.path.join(ROOT, "data", "makeup"), exist_ok=True)
    Image.fromarray((rgba * 255 + 0.5).astype(np.uint8), "RGBA").save(os.path.join(ROOT, "data", "makeup", "face-paint.png"),
                                                                      optimize=True)
    Image.fromarray((liner * 255 + 0.5).astype(np.uint8), "RGBA").save(os.path.join(ROOT, "data", "makeup", "face-liner.png"),
                                                                       optimize=True)
    write_header(V, uv, tris, os.path.join(ROOT, "src", "face-mesh-data.h"))
    if a.preview:
        base, ext = os.path.splitext(a.preview)
        for k, style in enumerate(LINER_STYLES):
            shown = rgba.copy()
            shown[..., 2] = liner[..., k]
            preview(P, shown, f"{base}-{style}{ext}")
    print("wrote data/makeup/face-paint.png, face-liner.png, face-normal.png and src/face-mesh-data.h")


if __name__ == "__main__":
    sys.exit(main())
