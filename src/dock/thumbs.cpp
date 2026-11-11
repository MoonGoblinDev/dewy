#include "thumbs.hpp"

#include <QtCore/QHash>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>
#include <cstring>

#include "beauty-api.h"
#include "face-geometry.h"
#include "grade.h"
#include "widgets.hpp"

namespace beauty {

/* ------------------------------------------------------------ capture */

FaceCrop captureFaceCrop(obs_source_t *filter, int size, float zoom)
{
	FaceCrop c;
	uint8_t *px = nullptr;
	uint32_t w = 0, h = 0;
	ft_face face;
	memset(&face, 0, sizeof(face));
	if (filter && beauty_filter_snapshot(filter, &px, &w, &h, &face) && px) {
		QImage frame(px, (int)w, (int)h, (int)w * 4, QImage::Format_ARGB32);
		QRectF crop;
		if (face.valid && face.num_points > 10) {
			float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
			for (int i = 0; i < face.num_points; i++) {
				x0 = std::min(x0, face.pts[i].x * w);
				x1 = std::max(x1, face.pts[i].x * w);
				y0 = std::min(y0, face.pts[i].y * h);
				y1 = std::max(y1, face.pts[i].y * h);
			}
			float fw = x1 - x0, fh = y1 - y0;
			float side = std::max(fw, fh) * zoom; /* headroom for crowns, halos, ears */
			float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2 - fh * 0.30f + (zoom - 2.1f) * fh * 0.35f;
			side = std::min(side, (float)std::min(w, h)); /* stay inside the frame */
			float x = std::clamp(cx - side / 2, 0.0f, (float)w - side);
			float y = std::clamp(cy - side / 2, 0.0f, (float)h - side);
			crop = QRectF(x, y, side, side);
			c.hasFace = true;
		} else {
			float side = (float)std::min(w, h);
			crop = QRectF((w - side) / 2, (h - side) / 2, side, side);
		}
		c.img = frame.copy(crop.toRect())
				.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
				.convertToFormat(QImage::Format_ARGB32);
		uint8_t *mk = nullptr;
		uint32_t mw = 0, mh = 0;
		if (beauty_filter_snapshot_mask(filter, &mk, &mw, &mh) && mk) {
			QImage m(mk, (int)mw, (int)mh, (int)mw, QImage::Format_Grayscale8);
			QImage full = m.scaled((int)w, (int)h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
			c.mask = full.copy(crop.toRect()).scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
			bfree(mk);
		}
		if (c.hasFace) {
			c.face = face;
			for (int i = 0; i < face.num_points; i++) {
				c.face.pts[i].x = (float)((face.pts[i].x * w - crop.x()) / crop.width());
				c.face.pts[i].y = (float)((face.pts[i].y * h - crop.y()) / crop.height());
			}
		}
		bfree(px);
	}
	if (c.img.isNull()) {
		c.img = QImage(size, size, QImage::Format_ARGB32);
		c.img.fill(theme::chip);
		QPainter p(&c.img);
		LookArt a;
		a.name = "x";
		paintLook(p, QRectF(0, 0, size, size), a);
	}
	return c;
}

QImage noneThumb(int size)
{
	QImage img(size, size, QImage::Format_ARGB32);
	img.fill(Qt::transparent);
	QPainter p(&img);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(theme::chip);
	p.drawRoundedRect(QRectF(0, 0, size, size), size * 0.16, size * 0.16);
	p.setPen(QPen(QColor(0xc8, 0xc8, 0xd0), size * 0.035));
	p.setBrush(Qt::NoBrush);
	QRectF r(size * 0.3, size * 0.3, size * 0.4, size * 0.4);
	p.drawEllipse(r);
	p.drawLine(QPointF(size * 0.36, size * 0.36), QPointF(size * 0.64, size * 0.64));
	return img;
}

/* ------------------------------------------------------------ helpers */

static inline float clampf(float v, float lo = 0.f, float hi = 1.f)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static bool cropLayout(const FaceCrop &c, struct face_layout *L, const struct beauty_shape *shape = nullptr)
{
	if (!c.hasFace)
		return false;
	struct beauty_shape zero;
	memset(&zero, 0, sizeof(zero));
	face_layout_build(L, &c.face, shape ? shape : &zero, (float)c.img.width(), (float)c.img.height(), 1.0f, 0);
	return L->has_face;
}

/* Soft ellipse mask for region idx (same as the shader). */
static float regionMask(const struct face_layout *L, int idx, float x, float y, float feather, float *lx = nullptr,
			float *ly = nullptr)
{
	const float *a = &L->data[(MAX_WARPS * 2 + idx * 2) * 4];
	const float *b = a + 4;
	if (a[2] <= 0)
		return 0;
	float qx = x - a[0], qy = y - a[1];
	float u = (qx * b[0] + qy * b[1]) / a[2], v = (-qx * b[1] + qy * b[0]) / a[3];
	if (lx)
		*lx = u;
	if (ly)
		*ly = v;
	float t = std::sqrt(u * u + v * v);
	float e0 = 1.0f - feather;
	float s = clampf((t - e0) / std::max(feather, 1e-3f));
	return 1.0f - s * s * (3 - 2 * s);
}

static float smooth(float e0, float e1, float x)
{
	float t = clampf((x - e0) / (e1 - e0));
	return t * t * (3 - 2 * t);
}

/* ------------------------------------------------------------ filter */

QImage filterThumb(const FaceCrop &c, obs_data_t *preset)
{
	struct grade_params g;
	presets_parse_grade(preset, &g);
	QImage out = c.img.copy();
	for (int y = 0; y < out.height(); y++) {
		QRgb *row = reinterpret_cast<QRgb *>(out.scanLine(y));
		for (int x = 0; x < out.width(); x++) {
			float px[3] = {qRed(row[x]) / 255.f, qGreen(row[x]) / 255.f, qBlue(row[x]) / 255.f};
			grade_apply(&g, px);
			row[x] = qRgb((int)(px[0] * 255 + .5f), (int)(px[1] * 255 + .5f), (int)(px[2] * 255 + .5f));
		}
	}
	return out;
}

/* ------------------------------------------------------------ light (mirrors beauty-final.effect) */

QImage lightThumb(const FaceCrop &c, obs_data_t *preset)
{
	struct light_params l;
	presets_parse_light(preset, &l);
	QImage out = c.img.copy();
	struct face_layout L;
	bool face = cropLayout(c, &L);
	float Ln = std::sqrt(l.key_dir[0] * l.key_dir[0] + l.key_dir[1] * l.key_dir[1] + l.key_dir[2] * l.key_dir[2]);
	float Lx = l.key_dir[0] / Ln, Ly = l.key_dir[1] / Ln, Lz = l.key_dir[2] / Ln;
	float la = 0.8f;
	bool hasMask = !c.mask.isNull();
	QImage blur;
	if (hasMask) /* cheap heavy blur: down to a few pixels and back up */
		blur = c.mask.scaled(14, 14, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
			       .scaled(out.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
			       .convertToFormat(QImage::Format_Grayscale8);
	auto mval = [&](const QImage &im, int x, int y) {
		x = std::clamp(x, 0, im.width() - 1);
		y = std::clamp(y, 0, im.height() - 1);
		return im.constScanLine(y)[x] / 255.0f;
	};
	for (int y = 0; y < out.height(); y++) {
		QRgb *row = reinterpret_cast<QRgb *>(out.scanLine(y));
		for (int x = 0; x < out.width(); x++) {
			float col[3] = {qRed(row[x]) / 255.f, qGreen(row[x]) / 255.f, qBlue(row[x]) / 255.f};
			float person = 0.4f;
			float nx = 0, ny = 0, nz = 1;
			if (hasMask) {
				person = mval(c.mask, x, y);
				float hx = mval(blur, x + 3, y) - mval(blur, x - 3, y);
				float hy = mval(blur, x, y + 3) - mval(blur, x, y - 3);
				nx = -hx * 3.5f;
				ny = -hy * 3.5f;
				nz = 0.55f;
			} else if (face) {
				person = std::max(regionMask(&L, REGION_PERSON_HEAD, x, y, 0.55f),
						  regionMask(&L, REGION_PERSON_BODY, x, y, 0.6f));
			}
			if (face) {
				float lx = 0, ly = 0;
				regionMask(&L, 0, (float)x, (float)y, 0.35f, &lx, &ly);
				ly *= 0.92f;
				float r = std::sqrt(lx * lx + ly * ly);
				float face_m = 1.0f - smooth(0.75f, 1.2f, r);
				float k = std::max(r, 1.0f), rn = std::min(r, 1.0f);
				float fx = lx / k, fy = ly / k, fz = std::sqrt(clampf(1 - rn * rn)) * 1.15f + 0.08f;
				const float *b = &L.data[(MAX_WARPS * 2) * 4 + 4]; /* face region rotation */
				float ix = fx * b[0] - fy * b[1], iy = fx * b[1] + fy * b[0];
				nx += (ix - nx) * face_m;
				ny += (iy - ny) * face_m;
				nz += (fz - nz) * face_m;
				person = std::max(person, face_m);
			}
			float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
			nx /= nl;
			ny /= nl;
			nz /= nl;
			float lam = clampf(nx * Lx + ny * Ly + nz * Lz);
			float shade = 1.0f - l.fill * (1.0f - lam) * person * la * 0.7f;
			for (int i = 0; i < 3; i++)
				col[i] *= (1.0f + l.key_color[i] * lam * lam * l.key_amt * 0.9f * person * la) * shade;
			float rsx = -Lx, rsy = Ly * 0.5f, rl = std::sqrt(rsx * rsx + rsy * rsy) + 1e-4f;
			float nn = std::sqrt(nx * nx + ny * ny) + 1e-4f;
			float rim_dir = clampf((nx / nn * rsx / rl + ny / nn * rsy / rl) * 0.8f + 0.3f);
			float edge = std::pow(clampf(1 - nz), 1.6f);
			for (int i = 0; i < 3; i++)
				col[i] += l.rim_color[i] * edge * rim_dir * person * l.rim_amt * 0.7f * la;
			float dim = 1.0f - l.bg_dim * (1.0f - person) * la;
			float ex = std::exp2(l.exposure * la * 0.6f);
			col[0] *= dim * ex * (1.0f + l.warmth * 0.1f * la);
			col[1] *= dim * ex;
			col[2] *= dim * ex * (1.0f - l.warmth * 0.1f * la);
			row[x] = qRgb((int)(clampf(col[0]) * 255), (int)(clampf(col[1]) * 255), (int)(clampf(col[2]) * 255));
		}
	}
	return out;
}

/* ------------------------------------------------------------ effects */

static QImage sticker(const char *file)
{
	static QHash<QString, QImage> cache;
	QString key = QString::fromUtf8(file);
	auto it = cache.find(key);
	if (it != cache.end())
		return *it;
	QImage img;
	std::string rel = std::string("stickers/") + file;
	char *path = obs_module_file(rel.c_str());
	if (path)
		img.load(QString::fromUtf8(path));
	bfree(path);
	cache.insert(key, img);
	return img;
}

static QImage warpCrop(const FaceCrop &c, const struct face_layout &L)
{
	QImage out(c.img.size(), QImage::Format_ARGB32);
	int W = c.img.width(), H = c.img.height();
	const QRgb *src = reinterpret_cast<const QRgb *>(c.img.constBits());
	for (int y = 0; y < H; y++) {
		QRgb *row = reinterpret_cast<QRgb *>(out.scanLine(y));
		for (int x = 0; x < W; x++) {
			float px = x + 0.5f, py = y + 0.5f, sx = px, sy = py;
			for (int i = 0; i < L.warp_count; i++) {
				const float *a = &L.data[i * 8], *b = a + 4;
				float qx = px - a[0], qy = py - a[1];
				float t2 = (qx * qx + qy * qy) / (a[2] * a[2]);
				if (t2 >= 1)
					continue;
				float f = (1 - t2) * (1 - t2);
				if (a[3] < 0.5f) {
					sx -= b[0] * f;
					sy -= b[1] * f;
				} else if (a[3] < 1.5f) {
					sx -= qx * b[2] * f;
					sy -= qy * b[2] * f;
				} else {
					float d = (qx * b[0] + qy * b[1]) * b[2] * f;
					sx -= b[0] * d;
					sy -= b[1] * d;
				}
			}
			int ix = std::clamp((int)sx, 0, W - 1), iy = std::clamp((int)sy, 0, H - 1);
			row[x] = src[iy * W + ix];
		}
	}
	return out;
}

QImage effectThumb(const FaceCrop &c, obs_data_t *preset)
{
	struct fx_params fx;
	presets_parse_effect(preset, &fx);
	QImage out = c.img.copy();
	struct face_layout L;
	struct beauty_shape shape;
	memset(&shape, 0, sizeof(shape));
	presets_apply_shape(fx.shape, &shape);
	bool face = cropLayout(c, &L, &shape);
	if (face && L.warp_count > 0)
		out = warpCrop(c, L);

	QPainter p(&out);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	p.setRenderHint(QPainter::Antialiasing);
	float roll = std::atan2(L.axis_u[1], L.axis_u[0]);
	for (int i = 0; i < fx.sprite_count; i++) {
		const struct fx_sprite &s = fx.sprites[i];
		QImage img = sticker(s.image);
		if (img.isNull())
			continue;
		float cx, cy, wpx;
		float rot = 0;
		if (face) {
			const float *a = L.anchor[s.anchor];
			face_layout_map(&L, a[0] + s.offset[0], a[1] + s.offset[1], &cx, &cy);
			wpx = s.width * L.iod_px;
			rot = roll * 180.0f / (float)M_PI + s.rotation;
		} else { /* no face: show the main sticker centred */
			if (i > 0)
				break;
			cx = out.width() / 2.0f;
			cy = out.height() / 2.0f;
			wpx = out.width() * 0.7f;
		}
		float hpx = wpx * img.height() / std::max(1, img.width());
		p.save();
		p.setCompositionMode(s.blend == FX_BLEND_MULTIPLY ? QPainter::CompositionMode_Multiply
					      : s.blend == FX_BLEND_ADD ? QPainter::CompositionMode_Plus
									: QPainter::CompositionMode_SourceOver);
		p.translate(cx, cy);
		p.rotate(rot);
		p.drawImage(QRectF(-wpx / 2, -hpx / 2, wpx, hpx), img);
		p.restore();
	}
	obs_data_release(fx.shape);
	return out;
}

/* ------------------------------------------------------------ atmosphere */

static QColor hexColor(obs_data_t *d, const char *key, QColor def)
{
	const char *s = obs_data_get_string(d, key);
	return (s && *s) ? QColor(QString::fromUtf8(s)) : def;
}

static void star4(QPainter &p, QPointF c, double r, const QColor &col)
{
	QPainterPath path;
	path.moveTo(c + QPointF(0, -r));
	path.quadTo(c, c + QPointF(r, 0));
	path.quadTo(c, c + QPointF(0, r));
	path.quadTo(c, c + QPointF(-r, 0));
	path.quadTo(c, c + QPointF(0, -r));
	p.setBrush(col);
	p.drawPath(path);
}

static void heart(QPainter &p, QPointF c, double s, const QColor &col)
{
	QPainterPath h;
	h.moveTo(c + QPointF(0, s * 0.35));
	h.cubicTo(c + QPointF(-s * 0.9, -s * 0.2), c + QPointF(-s * 0.4, -s * 0.8), c + QPointF(0, -s * 0.35));
	h.cubicTo(c + QPointF(s * 0.4, -s * 0.8), c + QPointF(s * 0.9, -s * 0.2), c + QPointF(0, s * 0.35));
	p.setBrush(col);
	p.drawPath(h);
}

QImage atmoThumb(const FaceCrop &c, obs_data_t *preset)
{
	QImage out = c.img.copy();
	QPainter p(&out);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	int W = out.width(), H = out.height();
	QString style = QString::fromUtf8(obs_data_get_string(preset, "style"));
	QColor a = hexColor(preset, "a", Qt::white), b = hexColor(preset, "b", Qt::white);
	uint32_t seed = qHash(QString::fromUtf8(obs_data_get_string(preset, "name")));
	auto rnd = [&seed]() {
		seed = seed * 1664525u + 1013904223u;
		return (seed >> 8) / 16777216.0;
	};
	auto pick = [&]() { return rnd() < 0.5 ? a : b; };

	if (style == "leak") {
		for (auto [x, y, col] : {std::tuple{0.1, 0.2, a}, std::tuple{0.95, 0.85, b}}) {
			QRadialGradient g(QPointF(W * x, H * y), W * 0.6);
			QColor c0 = col;
			c0.setAlphaF(0.75);
			QColor c1 = col;
			c1.setAlphaF(0);
			g.setColorAt(0, c0);
			g.setColorAt(1, c1);
			p.setCompositionMode(QPainter::CompositionMode_Plus);
			p.fillRect(out.rect(), g);
		}
	} else if (style == "mist") {
		for (int i = 0; i < 26; i++) {
			QColor col = pick();
			col.setAlphaF(0.22);
			p.setBrush(col);
			double x = rnd() * W, y = H * (0.62 + rnd() * 0.45);
			p.drawEllipse(QPointF(x, y), W * (0.18 + rnd() * 0.2), H * (0.08 + rnd() * 0.08));
		}
	} else if (style == "dreamy") {
		QColor haze = b;
		haze.setAlphaF(0.22);
		p.fillRect(out.rect(), haze);
		QRadialGradient g(QPointF(W / 2.0, H / 2.0), W * 0.75);
		QColor v0 = a, v1 = b;
		v0.setAlphaF(0);
		v1.setAlphaF(0.45);
		g.setColorAt(0.55, v0);
		g.setColorAt(1, v1);
		p.fillRect(out.rect(), g);
	} else {
		int n = style == "glitter" ? 60 : style == "snow" ? 40 : style == "bokeh" ? 10 : 18;
		for (int i = 0; i < n; i++) {
			QPointF c0(rnd() * W, rnd() * H);
			double s = W * (0.03 + rnd() * 0.05);
			QColor col = pick();
			if (style == "sparkle" || style == "glitter") {
				p.setCompositionMode(QPainter::CompositionMode_Plus);
				star4(p, c0, style == "glitter" ? s * 0.45 : s, col);
			} else if (style == "bokeh") {
				col.setAlphaF(0.35);
				p.setCompositionMode(QPainter::CompositionMode_Plus);
				p.setBrush(col);
				p.drawEllipse(c0, s * 2.2, s * 2.2);
			} else if (style == "hearts") {
				heart(p, c0, s * 1.3, col);
			} else if (style == "petals") {
				p.save();
				p.translate(c0);
				p.rotate(rnd() * 360);
				p.setBrush(col);
				p.drawEllipse(QPointF(0, 0), s * 0.45, s * 0.8);
				p.restore();
			} else if (style == "snow") {
				col.setAlphaF(0.9);
				p.setBrush(col);
				p.drawEllipse(c0, s * 0.3, s * 0.3);
			} else if (style == "fireflies") {
				QRadialGradient g(c0, s * 1.2);
				QColor c1 = col;
				c1.setAlphaF(0);
				g.setColorAt(0, col);
				g.setColorAt(1, c1);
				p.setCompositionMode(QPainter::CompositionMode_Plus);
				p.setBrush(g);
				p.drawEllipse(c0, s * 1.2, s * 1.2);
			} else if (style == "confetti") {
				p.save();
				p.translate(c0);
				p.rotate(rnd() * 360);
				p.setBrush(rnd() < 0.3 ? QColor(0xff, 0xd8, 0x3a) : col);
				p.drawRect(QRectF(-s * 0.5, -s * 0.22, s, s * 0.44));
				p.restore();
			} else if (style == "bubbles") {
				p.setBrush(Qt::NoBrush);
				p.setPen(QPen(col, 1.4));
				p.drawEllipse(c0, s * 1.1, s * 1.1);
				p.setPen(Qt::NoPen);
			}
		}
	}
	return out;
}

QImage motionThumb(const FaceCrop &c, obs_data_t *preset)
{
	QString style = QString::fromUtf8(obs_data_get_string(preset, "style"));
	const QImage &src = c.img;
	int W = src.width(), H = src.height();
	QImage out(src.size(), QImage::Format_ARGB32);
	out.fill(Qt::black);
	QPainter p(&out);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	if (style == "shake") {
		p.setOpacity(0.5);
		p.drawImage(QPointF(-W * 0.04, H * 0.02), src);
		p.drawImage(QPointF(W * 0.04, -H * 0.02), src);
	} else if (style == "pulse") {
		p.drawImage(QRectF(-W * 0.06, -H * 0.06, W * 1.12, H * 1.12), src);
		p.setPen(QPen(QColor(255, 255, 255, 140), 2));
		p.setBrush(Qt::NoBrush);
		for (int i = 1; i <= 2; i++)
			p.drawRoundedRect(QRectF(W * 0.06 * i, H * 0.06 * i, W * (1 - 0.12 * i), H * (1 - 0.12 * i)), 8, 8);
	} else if (style == "rgb" || style == "glitch") {
		QImage r = src.copy(), g = src.copy(), b = src.copy();
		for (QImage *im : {&r, &g, &b})
			im->convertTo(QImage::Format_ARGB32);
		auto keep = [](QImage &im, int ch) {
			for (int y = 0; y < im.height(); y++) {
				QRgb *row = reinterpret_cast<QRgb *>(im.scanLine(y));
				for (int x = 0; x < im.width(); x++)
					row[x] = qRgb(ch == 0 ? qRed(row[x]) : 0, ch == 1 ? qGreen(row[x]) : 0,
						      ch == 2 ? qBlue(row[x]) : 0);
			}
		};
		keep(r, 0);
		keep(g, 1);
		keep(b, 2);
		p.setCompositionMode(QPainter::CompositionMode_Plus);
		int o = style == "rgb" ? W / 28 : W / 18;
		p.drawImage(QPointF(o, 0), r);
		p.drawImage(QPointF(0, 0), g);
		p.drawImage(QPointF(-o, 0), b);
		if (style == "glitch") {
			p.setCompositionMode(QPainter::CompositionMode_SourceOver);
			for (int i = 0; i < 4; i++) {
				int y = (int)(H * (0.15 + 0.2 * i)), hh = H / 14;
				p.drawImage(QPointF((i % 2 ? 1 : -1) * W * 0.08, y), src.copy(0, y, W, hh));
			}
		}
	} else if (style == "drift") {
		p.drawImage(QRectF(-W * 0.12, -H * 0.08, W * 1.24, H * 1.24), src);
	} else if (style == "wobble") {
		for (int y = 0; y < H; y += 2)
			p.drawImage(QPointF(std::sin(y * 0.12) * W * 0.03, y), src.copy(0, y, W, 2));
	} else {
		p.drawImage(0, 0, src);
	}
	return out;
}

QImage lookThumb(const FaceCrop &c, obs_data_t *look)
{
	obs_data_t *s = obs_data_get_obj(look, "settings");
	if (!s)
		return c.img.copy();
	FaceCrop cur = c;
	/* skin: a soft-focus hint proportional to Smooth */
	long long sm = obs_data_get_int(s, "smooth");
	if (sm > 0) {
		QImage soft = cur.img.scaled(cur.img.width() / 6, cur.img.height() / 6, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
				      .scaled(cur.img.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
		QPainter p(&cur.img);
		p.setOpacity(0.28 * sm / 100.0);
		p.drawImage(0, 0, soft);
	}
	struct Layer {
		const char *key;
		obs_data_t *root;
		const char *list;
		QImage (*gen)(const FaceCrop &, obs_data_t *);
	} layers[] = {
		{"light_preset", presets_lights_root(), "lights", lightThumb},
		{"filter_preset", presets_filters_root(), "filters", filterThumb},
		{"atmo_preset", presets_atmosphere_root(), "ambience", atmoThumb},
		{"effect_preset", presets_effects_root(), "effects", effectThumb},
	};
	for (const Layer &l : layers) {
		const char *name = obs_data_get_string(s, l.key);
		obs_data_t *p = (name && *name) ? presets_find_in(l.root, l.list, name) : nullptr;
		if (p) {
			cur.img = l.gen(cur, p);
			obs_data_release(p);
		}
	}
	obs_data_release(s);
	return cur.img;
}

} // namespace beauty
