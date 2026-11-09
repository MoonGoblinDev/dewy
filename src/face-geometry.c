/*
 * Turns face landmarks + slider values into GPU-ready warp primitives and region ellipses.
 *
 * Everything is defined in a "face frame": origin between the eye centres, x along the
 * eye axis (image-left eye -> image-right eye), y perpendicular towards the chin, unit =
 * inter-ocular distance (IOD). That makes every constant below independent of head roll,
 * camera distance and resolution.
 */

#include "face-geometry.h"

#include <math.h>
#include <string.h>

typedef struct {
	float x, y;
} v2;

static v2 V(float x, float y)
{
	return (v2){x, y};
}

struct stats {
	int n;
	v2 c;              /* centroid */
	float x0, x1, y0, y1; /* extents */
};

struct ctx {
	struct face_layout *out;
	v2 M, u, v; /* face frame origin and axes (pixels) */
	float d;    /* IOD in pixels */
	float weight;

	/* contour split by side, sorted by y (face frame) */
	v2 side_pts[2][FT_MAX_POINTS];
	int side_n[2];
};

/* ---------- frame conversion ---------- */

static v2 to_face(const struct ctx *c, v2 p)
{
	float dx = p.x - c->M.x, dy = p.y - c->M.y;
	return V((dx * c->u.x + dy * c->u.y) / c->d, (dx * c->v.x + dy * c->v.y) / c->d);
}

static v2 from_face(const struct ctx *c, v2 f)
{
	return V(c->M.x + (c->u.x * f.x + c->v.x * f.y) * c->d, c->M.y + (c->u.y * f.x + c->v.y * f.y) * c->d);
}

static v2 dir_px(const struct ctx *c, v2 f)
{
	return V(c->u.x * f.x + c->v.x * f.y, c->u.y * f.x + c->v.y * f.y);
}

static struct stats region_stats(const struct ctx *c, const ft_face *face, enum ft_region r, float W, float H)
{
	struct stats s = {0, {0, 0}, 1e9f, -1e9f, 1e9f, -1e9f};
	for (int i = 0; i < face->region_count[r]; i++) {
		ft_point p = face->pts[face->region_start[r] + i];
		v2 f = to_face(c, V(p.x * W, p.y * H));
		s.c.x += f.x;
		s.c.y += f.y;
		s.x0 = fminf(s.x0, f.x);
		s.x1 = fmaxf(s.x1, f.x);
		s.y0 = fminf(s.y0, f.y);
		s.y1 = fmaxf(s.y1, f.y);
		s.n++;
	}
	if (s.n) {
		s.c.x /= s.n;
		s.c.y /= s.n;
	}
	return s;
}

/* Face-frame x of the contour on one side (0 = image left, 1 = image right) at height y. */
static float contour_x(const struct ctx *c, int side, float y)
{
	int n = c->side_n[side];
	if (n < 2) {
		/* template face: ~1.0 IOD half width at the eyes, narrowing to the chin */
		float t = fminf(fmaxf(y / 1.8f, 0.0f), 1.0f);
		return (side ? 1.0f : -1.0f) * (1.0f - 0.75f * t * t);
	}
	const v2 *p = c->side_pts[side];
	if (y <= p[0].y)
		return p[0].x;
	for (int i = 1; i < n; i++) {
		if (y <= p[i].y) {
			float t = (y - p[i - 1].y) / fmaxf(p[i].y - p[i - 1].y, 1e-4f);
			return p[i - 1].x + (p[i].x - p[i - 1].x) * t;
		}
	}
	return p[n - 1].x;
}

/* ---------- emitters ---------- */

static void grow_bounds(struct ctx *c, v2 center_px, float r_px)
{
	float *b = c->out->bounds;
	b[0] = fminf(b[0], center_px.x - r_px);
	b[1] = fminf(b[1], center_px.y - r_px);
	b[2] = fmaxf(b[2], center_px.x + r_px);
	b[3] = fmaxf(b[3], center_px.y + r_px);
}

static void emit(struct ctx *c, int type, v2 center_f, float r_f, v2 vec_px, float s)
{
	struct face_layout *o = c->out;
	if (o->warp_count >= MAX_WARPS)
		return;
	v2 cp = from_face(c, center_f);
	float r = r_f * c->d;
	float *t = &o->data[o->warp_count * 8];
	t[0] = cp.x;
	t[1] = cp.y;
	t[2] = r;
	t[3] = (float)type;
	t[4] = vec_px.x;
	t[5] = vec_px.y;
	t[6] = s;
	t[7] = 0.0f;
	o->warp_count++;
	grow_bounds(c, cp, r);
}

/* Moves image content near `center` by `delta` (face units). */
static void translate(struct ctx *c, v2 center, float r, v2 delta)
{
	delta.x *= c->weight;
	delta.y *= c->weight;
	if (fabsf(delta.x) + fabsf(delta.y) < 1e-4f)
		return;
	v2 dp = dir_px(c, delta);
	emit(c, WARP_TRANSLATE, center, r, V(dp.x * c->d, dp.y * c->d), 0.0f);
}

/* Magnifies (s > 0) or shrinks (s < 0) content around `center`. */
static void scale(struct ctx *c, v2 center, float r, float s)
{
	s *= c->weight;
	if (fabsf(s) < 1e-4f)
		return;
	emit(c, WARP_SCALE, center, r, V(0, 0), s);
}

/* Like scale() but only along a face-frame direction (unit vector). */
static void scale_dir(struct ctx *c, v2 center, float r, v2 dir, float s)
{
	s *= c->weight;
	if (fabsf(s) < 1e-4f)
		return;
	emit(c, WARP_SCALE_DIR, center, r, dir_px(c, dir), s);
}

/* Region ellipse; `axis` is the face-frame direction of the ellipse's y radius. */
static void region(struct ctx *c, int idx, v2 center, float rx, float ry, v2 axis)
{
	v2 cp = from_face(c, center);
	v2 a = dir_px(c, axis);
	float len = hypotf(a.x, a.y);
	a.x /= len;
	a.y /= len;
	float *t = &c->out->data[(MAX_WARPS * 2 + idx * 2) * 4];
	t[0] = cp.x;
	t[1] = cp.y;
	t[2] = fmaxf(rx * c->d, 1.0f);
	t[3] = fmaxf(ry * c->d, 1.0f);
	/* shader: local = (dot(q, (cs, sn)), dot(q, (-sn, cs))) -> local y runs along `a` */
	t[4] = a.y;
	t[5] = -a.x;
	t[6] = 0.0f;
	t[7] = 0.0f;
}

static int cmp_y(const void *a, const void *b)
{
	float d = ((const v2 *)a)->y - ((const v2 *)b)->y;
	return (d > 0) - (d < 0);
}

#include <stdlib.h>

struct ang_pt {
	float a, x, y;
};

static int cmp_ang(const void *a, const void *b)
{
	float d = ((const struct ang_pt *)a)->a - ((const struct ang_pt *)b)->a;
	return (d > 0) - (d < 0);
}

int face_ring(const ft_face *face, enum ft_region r, float W, float H, float *xy, int max_points)
{
	int n = face->region_count[r];
	if (n > max_points)
		n = max_points;
	if (n < 3)
		return 0;
	struct ang_pt pts[FT_MAX_POINTS];
	float cx = 0, cy = 0;
	for (int i = 0; i < n; i++) {
		ft_point p = face->pts[face->region_start[r] + i];
		pts[i].x = p.x * W;
		pts[i].y = p.y * H;
		cx += pts[i].x;
		cy += pts[i].y;
	}
	cx /= n;
	cy /= n;
	for (int i = 0; i < n; i++)
		pts[i].a = atan2f(pts[i].y - cy, pts[i].x - cx);
	qsort(pts, n, sizeof(pts[0]), cmp_ang);
	for (int i = 0; i < n; i++) {
		xy[i * 2] = pts[i].x;
		xy[i * 2 + 1] = pts[i].y;
	}
	return n;
}

void face_layout_map(const struct face_layout *L, float fx, float fy, float *x, float *y)
{
	float ux = L->axis_u[0], uy = L->axis_u[1], vx = -uy, vy = ux, d = L->iod_px;
	*x = L->origin[0] + (ux * fx + vx * fy) * d;
	*y = L->origin[1] + (uy * fx + vy * fy) * d;
}

/* Closed Catmull-Rom spline through `n` ring points, `sub` samples per segment. */
static int spline_ring(const float *in, int n, int sub, float *out, int max_points)
{
	int m = 0;
	for (int i = 0; i < n && m < max_points; i++) {
		const float *p0 = &in[((i + n - 1) % n) * 2], *p1 = &in[i * 2], *p2 = &in[((i + 1) % n) * 2],
			    *p3 = &in[((i + 2) % n) * 2];
		for (int k = 0; k < sub && m < max_points; k++) {
			float t = (float)k / sub, t2 = t * t, t3 = t2 * t;
			for (int a = 0; a < 2; a++)
				out[m * 2 + a] = 0.5f * (2 * p1[a] + (-p0[a] + p2[a]) * t +
							 (2 * p0[a] - 5 * p1[a] + 4 * p2[a] - p3[a]) * t2 +
							 (-p0[a] + 3 * p1[a] - 3 * p2[a] + p3[a]) * t3);
			m++;
		}
	}
	return m;
}

static void sort_ring(float *xy, int n)
{
	struct ang_pt pts[FT_MAX_POINTS + 4];
	float cx = 0, cy = 0;
	for (int i = 0; i < n; i++) {
		cx += xy[i * 2];
		cy += xy[i * 2 + 1];
	}
	cx /= n;
	cy /= n;
	for (int i = 0; i < n; i++)
		pts[i] = (struct ang_pt){atan2f(xy[i * 2 + 1] - cy, xy[i * 2] - cx), xy[i * 2], xy[i * 2 + 1]};
	qsort(pts, n, sizeof(pts[0]), cmp_ang);
	for (int i = 0; i < n; i++) {
		xy[i * 2] = pts[i].x;
		xy[i * 2 + 1] = pts[i].y;
	}
}

int face_lips_outer(const ft_face *face, float W, float H, float *xy, int max_points)
{
	float ring[FT_MAX_POINTS * 2];
	int n = face_ring(face, FT_OUTER_LIPS, W, H, ring, FT_MAX_POINTS);
	if (n < 3)
		return 0;
	return spline_ring(ring, n, 6, xy, max_points);
}

int face_mouth_opening(const ft_face *face, float W, float H, float *xy, int max_points)
{
	float outer[FT_MAX_POINTS * 2], ring[(FT_MAX_POINTS + 2) * 2];
	int no = face_ring(face, FT_OUTER_LIPS, W, H, outer, FT_MAX_POINTS);
	int ni = face_ring(face, FT_INNER_LIPS, W, H, ring, FT_MAX_POINTS);
	if (no < 3 || ni < 3 || !face->region_count[FT_LEFT_EYE] || !face->region_count[FT_RIGHT_EYE])
		return 0;

	/* mouth axis = eye axis */
	float ex[2] = {0, 0}, ey[2] = {0, 0};
	for (int s = 0; s < 2; s++) {
		enum ft_region r = s ? FT_RIGHT_EYE : FT_LEFT_EYE;
		for (int i = 0; i < face->region_count[r]; i++) {
			ex[s] += face->pts[face->region_start[r] + i].x * W;
			ey[s] += face->pts[face->region_start[r] + i].y * H;
		}
		ex[s] /= face->region_count[r];
		ey[s] /= face->region_count[r];
	}
	float ux = ex[1] - ex[0], uy = ey[1] - ey[0], ul = hypotf(ux, uy);
	if (ul < 1.0f)
		return 0;
	ux /= ul;
	uy /= ul;

	/* corners = outer-lip extremes along the mouth axis, nudged towards the opening */
	int lo = 0, hi = 0;
	for (int i = 1; i < no; i++) {
		float d = outer[i * 2] * ux + outer[i * 2 + 1] * uy;
		if (d < outer[lo * 2] * ux + outer[lo * 2 + 1] * uy)
			lo = i;
		if (d > outer[hi * 2] * ux + outer[hi * 2 + 1] * uy)
			hi = i;
	}
	float icx = 0, icy = 0;
	for (int i = 0; i < ni; i++) {
		icx += ring[i * 2];
		icy += ring[i * 2 + 1];
	}
	icx /= ni;
	icy /= ni;
	/* Only add the corners when the inner outline doesn't already reach them (Vision gives
	 * 6 inner points without corners; MediaPipe's mesh includes them). */
	float in_lo = 1e9f, in_hi = -1e9f;
	for (int i = 0; i < ni; i++) {
		float d = ring[i * 2] * ux + ring[i * 2 + 1] * uy;
		in_lo = fminf(in_lo, d);
		in_hi = fmaxf(in_hi, d);
	}
	float out_lo = outer[lo * 2] * ux + outer[lo * 2 + 1] * uy, out_hi = outer[hi * 2] * ux + outer[hi * 2 + 1] * uy;
	bool add_corners = (in_hi - in_lo) < 0.85f * (out_hi - out_lo);
	int idx[2] = {lo, hi};
	for (int k = 0; add_corners && k < 2; k++) {
		float px = outer[idx[k] * 2], py = outer[idx[k] * 2 + 1];
		ring[ni * 2] = px + (icx - px) * 0.06f;
		ring[ni * 2 + 1] = py + (icy - py) * 0.06f;
		ni++;
	}
	sort_ring(ring, ni);

	/* tiny dilation so the blurred mask edge stays on the lips, not on the teeth */
	float mw = hypotf(outer[hi * 2] - outer[lo * 2], outer[hi * 2 + 1] - outer[lo * 2 + 1]);
	for (int i = 0; i < ni; i++) {
		float dx = ring[i * 2] - icx, dy = ring[i * 2 + 1] - icy, l = hypotf(dx, dy);
		if (l > 1e-3f) {
			ring[i * 2] += dx / l * mw * 0.02f;
			ring[i * 2 + 1] += dy / l * mw * 0.02f;
		}
	}
	return spline_ring(ring, ni, 6, xy, max_points);
}

void face_layout_build(struct face_layout *out, const ft_face *face, const struct beauty_shape *sh, float W, float H,
		       float weight, int blush_style)
{
	memset(out, 0, sizeof(*out));
	out->bounds[0] = out->bounds[1] = 1e9f;
	out->bounds[2] = out->bounds[3] = -1e9f;

	if (!face || !face->valid || !face->region_count[FT_LEFT_EYE] || !face->region_count[FT_RIGHT_EYE])
		return;

	struct ctx c;
	memset(&c, 0, sizeof(c));
	c.out = out;
	c.weight = weight;

	/* --- face frame from the eye centres --- */
	v2 e[2];
	for (int s = 0; s < 2; s++) {
		enum ft_region r = s ? FT_RIGHT_EYE : FT_LEFT_EYE;
		v2 acc = {0, 0};
		for (int i = 0; i < face->region_count[r]; i++) {
			acc.x += face->pts[face->region_start[r] + i].x * W;
			acc.y += face->pts[face->region_start[r] + i].y * H;
		}
		e[s] = V(acc.x / face->region_count[r], acc.y / face->region_count[r]);
	}
	if (e[0].x > e[1].x) {
		v2 t = e[0];
		e[0] = e[1];
		e[1] = t;
	}
	c.M = V((e[0].x + e[1].x) * 0.5f, (e[0].y + e[1].y) * 0.5f);
	c.d = hypotf(e[1].x - e[0].x, e[1].y - e[0].y);
	if (c.d < 4.0f)
		return;
	c.u = V((e[1].x - e[0].x) / c.d, (e[1].y - e[0].y) / c.d);
	c.v = V(-c.u.y, c.u.x);
	out->iod_px = c.d;
	out->origin[0] = c.M.x;
	out->origin[1] = c.M.y;
	out->axis_u[0] = c.u.x;
	out->axis_u[1] = c.u.y;
	out->has_face = true;

	/* --- contour, split into image-left / image-right halves --- */
	float chin_y = 1.75f, chin_x = 0.0f, half_w = 1.0f;
	for (int i = 0; i < face->region_count[FT_CONTOUR]; i++) {
		ft_point p = face->pts[face->region_start[FT_CONTOUR] + i];
		v2 f = to_face(&c, V(p.x * W, p.y * H));
		if (i == 0 || f.y > chin_y) {
			chin_y = f.y;
			chin_x = f.x;
		}
		int side = f.x >= 0.0f;
		c.side_pts[side][c.side_n[side]++] = f;
	}
	if (face->region_count[FT_CONTOUR] < 5) {
		c.side_n[0] = c.side_n[1] = 0;
		chin_y = 1.75f;
		chin_x = 0.0f;
	} else {
		qsort(c.side_pts[0], c.side_n[0], sizeof(v2), cmp_y);
		qsort(c.side_pts[1], c.side_n[1], sizeof(v2), cmp_y);
		half_w = fmaxf(fabsf(contour_x(&c, 0, 0.3f)), fabsf(contour_x(&c, 1, 0.3f)));
	}
	chin_y = fminf(fmaxf(chin_y, 1.2f), 2.4f);

	/* --- feature stats (face frame) --- */
	struct stats eye[2], brow[2], nose, lips, inner;
	{
		struct stats a = region_stats(&c, face, FT_LEFT_EYE, W, H);
		struct stats b = region_stats(&c, face, FT_RIGHT_EYE, W, H);
		eye[0] = a.c.x < b.c.x ? a : b;
		eye[1] = a.c.x < b.c.x ? b : a;
		a = region_stats(&c, face, FT_LEFT_BROW, W, H);
		b = region_stats(&c, face, FT_RIGHT_BROW, W, H);
		brow[0] = a.c.x < b.c.x ? a : b;
		brow[1] = a.c.x < b.c.x ? b : a;
	}
	nose = region_stats(&c, face, FT_NOSE, W, H);
	lips = region_stats(&c, face, FT_OUTER_LIPS, W, H);
	inner = region_stats(&c, face, FT_INNER_LIPS, W, H);

	v2 pupil[2] = {eye[0].c, eye[1].c};
	for (int r = FT_LEFT_PUPIL; r <= FT_RIGHT_PUPIL; r++) {
		if (!face->region_count[r])
			continue;
		ft_point p = face->pts[face->region_start[r]];
		v2 f = to_face(&c, V(p.x * W, p.y * H));
		pupil[f.x >= 0.0f] = f;
	}

	float nose_cx = nose.n ? nose.c.x : 0.0f;
	float tip_y = nose.n ? nose.y1 : 0.75f;
	float nose_w = nose.n ? fmaxf(nose.x1 - nose.x0, 0.25f) : 0.4f;
	v2 mc = lips.n ? lips.c : V(0, 1.15f);
	float mw = lips.n ? fmaxf(lips.x1 - lips.x0, 0.3f) : 0.75f;
	float mh = lips.n ? fmaxf(lips.y1 - lips.y0, 0.1f) : 0.3f;
	v2 corner[2] = {V(mc.x - mw * 0.5f, mc.y), V(mc.x + mw * 0.5f, mc.y)};
	v2 face_c = V(0.0f, chin_y * 0.45f);

	/* ================= FACE ================= */
	float slim = sh->face_slim + sh->face_natural * 0.5f;
	float small = sh->face_small + sh->face_natural * 0.5f;
	float vface = sh->face_v + sh->face_natural * 0.3f;

	for (int s = 0; s < 2; s++) {
		float in = s ? -1.0f : 1.0f; /* +x moves the left side inward */
		for (int i = 0; i < 4; i++) {
			float y = 0.35f + (chin_y * 0.85f - 0.35f) * (i / 3.0f);
			translate(&c, V(contour_x(&c, s, y), y), 0.5f, V(in * slim * 0.07f, 0));
		}
		for (int i = 0; i < 5; i++) {
			float y = chin_y * 0.92f * (i / 4.0f);
			v2 p = V(contour_x(&c, s, y), y);
			translate(&c, p, 0.55f, V((face_c.x - p.x) * small * 0.10f, (face_c.y - p.y) * small * 0.10f));
		}
		translate(&c, V(contour_x(&c, s, chin_y * 0.78f), chin_y * 0.78f), 0.42f,
			  V(in * vface * 0.09f, -vface * 0.02f));
		translate(&c, V(contour_x(&c, s, chin_y * 0.9f), chin_y * 0.9f), 0.38f,
			  V(in * vface * 0.07f, -vface * 0.02f));

		translate(&c, V(contour_x(&c, s, 0.3f), 0.3f), 0.4f, V(in * sh->cheekbone * 0.07f, 0));
		translate(&c, V(contour_x(&c, s, 0.5f), 0.5f), 0.35f, V(in * sh->cheekbone * 0.04f, 0));

		float jaw_y = fminf(mc.y + 0.2f, chin_y - 0.25f);
		translate(&c, V(contour_x(&c, s, jaw_y), jaw_y), 0.45f, V(in * sh->jawbone * 0.08f, 0));
	}
	translate(&c, V(chin_x, chin_y), 0.6f, V(0, -small * 0.08f));
	scale_dir(&c, face_c, 1.5f, V(1, 0), -sh->face_narrow * 0.10f);
	translate(&c, V(chin_x, chin_y), 0.55f, V(0, sh->chin * 0.12f));
	translate(&c, V(0, -0.95f), 0.75f, V(0, -sh->forehead * 0.10f));
	scale(&c, V(0, chin_y * 0.3f), 2.4f, -sh->head * 0.10f);

	/* ================= EYES ================= */
	for (int s = 0; s < 2; s++) {
		float out_dir = s ? 1.0f : -1.0f; /* away from the nose */
		v2 ec = eye[s].c;
		float ew = eye[s].n ? fmaxf(eye[s].x1 - eye[s].x0, 0.2f) : 0.45f;
		float eh = eye[s].n ? fmaxf(eye[s].y1 - eye[s].y0, 0.05f) : 0.15f;
		v2 outer = V(ec.x + out_dir * ew * 0.5f, ec.y);
		v2 innr = V(ec.x - out_dir * ew * 0.5f, ec.y);

		scale(&c, ec, ew * 1.15f, sh->eye_size * 0.20f);
		translate(&c, ec, 0.45f, V(out_dir * sh->eye_spacing * 0.05f, 0));
		translate(&c, ec, 0.45f, V(0, -sh->eye_position * 0.05f));
		translate(&c, outer, 0.22f, V(0, -sh->eye_tilt * 0.035f));
		translate(&c, innr, 0.20f, V(0, sh->eye_tilt * 0.02f));
		translate(&c, innr, 0.16f, V(-out_dir * sh->eye_inner * 0.035f, 0));
		translate(&c, outer, 0.18f, V(out_dir * sh->eye_outer * 0.035f, 0));
		translate(&c, V(ec.x, ec.y + eh * 0.5f), 0.20f, V(0, sh->eye_lower_lid * 0.03f));
		scale(&c, pupil[s], 0.14f, sh->eye_pupil * 0.22f);
	}

	/* ================= NOSE ================= */
	scale_dir(&c, V(nose_cx, tip_y - 0.08f), fmaxf(nose_w * 0.9f, 0.35f), V(1, 0), -sh->nose_narrow * 0.22f);
	translate(&c, V(nose_cx, tip_y), 0.30f, V(0, -sh->nose_length * 0.06f));
	scale_dir(&c, V(nose_cx, tip_y * 0.5f), 0.28f, V(1, 0), -sh->nose_bridge * 0.22f);
	scale(&c, V(nose_cx, tip_y - 0.04f), 0.16f, -sh->nose_tip * 0.22f);

	/* ================= MOUTH ================= */
	scale(&c, mc, mw * 0.85f, sh->mouth_size * 0.15f);
	translate(&c, mc, 0.5f, V(0, -sh->mouth_position * 0.06f));
	for (int s = 0; s < 2; s++)
		translate(&c, corner[s], 0.15f, V((s ? 1.0f : -1.0f) * sh->mouth_smile * 0.01f, -sh->mouth_smile * 0.035f));

	/* ================= BROWS ================= */
	for (int s = 0; s < 2; s++) {
		if (!brow[s].n)
			continue;
		float bw = fmaxf(brow[s].x1 - brow[s].x0, 0.3f);
		scale_dir(&c, brow[s].c, bw * 0.6f, V(0, 1), sh->brow_thickness * 0.30f);
		translate(&c, brow[s].c, bw * 0.7f, V(0, -sh->brow_position * 0.05f));
	}

	/* ================= REGIONS (for colour work) ================= */
	v2 up = V(0, 1);
	region(&c, REGION_FACE, V(0, (chin_y - 1.05f) * 0.5f), half_w * 1.05f, (chin_y + 1.05f) * 0.5f, up);
	for (int s = 0; s < 2; s++) {
		float side = s ? 1.0f : -1.0f;
		float ew = eye[s].n ? fmaxf(eye[s].x1 - eye[s].x0, 0.2f) : 0.45f;
		float eh = eye[s].n ? fmaxf(eye[s].y1 - eye[s].y0, 0.05f) : 0.15f;
		region(&c, REGION_EYE_L + s, eye[s].c, ew * 0.62f, fmaxf(eh * 0.9f, 0.07f) + 0.04f, up);
		if (brow[s].n)
			region(&c, REGION_BROW_L + s, brow[s].c, fmaxf(brow[s].x1 - brow[s].x0, 0.3f) * 0.58f, 0.10f,
			       up);
		region(&c, REGION_UNDER_EYE_L + s, V(eye[s].c.x + side * 0.02f, eye[s].c.y + eh * 0.5f + 0.15f),
		       ew * 0.6f, 0.11f, up);

		v2 wing = V(nose_cx + side * nose_w * 0.5f, tip_y - 0.02f);
		v2 fold_c = V((wing.x + corner[s].x) * 0.5f + side * 0.05f, (wing.y + corner[s].y) * 0.5f);
		v2 axis = V(corner[s].x - wing.x + side * 0.05f, corner[s].y - wing.y);
		float len = hypotf(axis.x, axis.y);
		region(&c, REGION_FOLD_L + s, fold_c, 0.06f, len * 0.6f + 0.05f, axis);

		/* ---- makeup ---- */
		switch (blush_style) {
		case BLUSH_LIFTED: /* swept up towards the temple */
			region(&c, REGION_CHEEK_L + s, V(eye[s].c.x + side * 0.22f, eye[s].c.y + 0.42f), 0.16f, 0.30f,
			       V(side * 0.8f, -0.6f));
			break;
		case BLUSH_SUNKISSED: /* band across the nose and cheeks */
			region(&c, REGION_CHEEK_L + s, V(eye[s].c.x * 0.8f, eye[s].c.y + 0.48f), 0.20f, 0.40f,
			       V(1, 0));
			break;
		default: /* apples of the cheeks */
			region(&c, REGION_CHEEK_L + s, V(eye[s].c.x + side * 0.08f, eye[s].c.y + 0.55f), 0.24f, 0.20f,
			       up);
		}
		region(&c, REGION_SHADOW_L + s, V(eye[s].c.x + side * 0.04f, eye[s].c.y - eh * 0.5f - 0.06f),
		       ew * 0.72f, 0.13f, up);

		v2 cheek_top = V(contour_x(&c, s, 0.45f), 0.45f);
		v2 hollow = V(contour_x(&c, s, 0.75f) * 0.80f, 0.78f);
		v2 hollow_axis = V(corner[s].x - cheek_top.x, corner[s].y - cheek_top.y);
		region(&c, REGION_HOLLOW_L + s, hollow, 0.12f, 0.30f, hollow_axis);

		float jy0 = chin_y * 0.62f, jy1 = chin_y * 0.95f;
		v2 j0 = V(contour_x(&c, s, jy0), jy0), j1 = V(contour_x(&c, s, jy1), jy1);
		v2 jaw_c = V((j0.x + j1.x) * 0.5f, (j0.y + j1.y) * 0.5f);
		region(&c, REGION_JAW_L + s, jaw_c, 0.10f, hypotf(j1.x - j0.x, j1.y - j0.y) * 0.6f,
		       V(j1.x - j0.x, j1.y - j0.y));

		region(&c, REGION_NOSE_SIDE_L + s, V(nose_cx + side * 0.13f, tip_y * 0.48f), 0.045f, tip_y * 0.36f, up);
		region(&c, REGION_HL_CHEEK_L + s, V(eye[s].c.x + side * 0.16f, eye[s].c.y + 0.36f), 0.26f, 0.12f,
		       V(-side * 0.35f, 1.0f));
	}
	/* rough person silhouette: head with hair, then shoulders / torso */
	region(&c, REGION_PERSON_HEAD, V(0, (chin_y - 1.6f) * 0.5f), half_w * 1.55f, (chin_y + 1.6f) * 0.5f * 1.1f, up);
	region(&c, REGION_PERSON_BODY, V(0, chin_y + 1.9f), half_w * 2.9f, 2.2f, up);

	/* sticker anchors */
	float (*an)[2] = out->anchor;
	an[ANCHOR_EYES][0] = 0;
	an[ANCHOR_EYES][1] = 0;
	an[ANCHOR_EYE_L][0] = eye[0].c.x;
	an[ANCHOR_EYE_L][1] = eye[0].c.y;
	an[ANCHOR_EYE_R][0] = eye[1].c.x;
	an[ANCHOR_EYE_R][1] = eye[1].c.y;
	an[ANCHOR_NOSE][0] = nose_cx;
	an[ANCHOR_NOSE][1] = tip_y;
	an[ANCHOR_MOUTH][0] = mc.x;
	an[ANCHOR_MOUTH][1] = mc.y;
	an[ANCHOR_CHIN][0] = chin_x;
	an[ANCHOR_CHIN][1] = chin_y;
	an[ANCHOR_FOREHEAD][0] = 0;
	an[ANCHOR_FOREHEAD][1] = -0.75f;
	an[ANCHOR_HEAD_TOP][0] = 0;
	an[ANCHOR_HEAD_TOP][1] = -1.45f;
	an[ANCHOR_CHEEK_L][0] = eye[0].c.x - 0.05f;
	an[ANCHOR_CHEEK_L][1] = eye[0].c.y + 0.55f;
	an[ANCHOR_CHEEK_R][0] = eye[1].c.x + 0.05f;
	an[ANCHOR_CHEEK_R][1] = eye[1].c.y + 0.55f;

	region(&c, REGION_HL_NOSE, V(nose_cx, tip_y * 0.45f), 0.045f, tip_y * 0.36f, up);
	region(&c, REGION_HL_FOREHEAD, V(0, -0.55f), 0.24f, 0.18f, up);
	region(&c, REGION_HL_CHIN, V(chin_x, chin_y - 0.16f), 0.12f, 0.08f, up);
	region(&c, REGION_LIPS, mc, mw * 0.58f, mh * 0.62f + 0.02f, up);
	if (inner.n)
		region(&c, REGION_MOUTH_INNER, V(mc.x, inner.c.y), fmaxf(fmaxf(inner.x1 - inner.x0, mw * 0.85f), 0.1f) * 0.55f,
		       fmaxf((inner.y1 - inner.y0) * 0.62f, 0.02f), up);
	region(&c, REGION_FOREHEAD, V(0, -0.75f), 0.8f, 0.3f, up);
}
