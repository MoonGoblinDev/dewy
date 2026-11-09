#include "pigment-calib.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace beauty {
namespace {

struct V3 {
	double x, y, z;
};
V3 operator+(V3 a, V3 b)
{
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}
V3 operator-(V3 a, V3 b)
{
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}
V3 operator*(V3 a, double k)
{
	return {a.x * k, a.y * k, a.z * k};
}
double dot(V3 a, V3 b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}
V3 cross(V3 a, V3 b)
{
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
V3 unit(V3 a)
{
	double n = std::sqrt(dot(a, a));
	return n > 1e-12 ? a * (1.0 / n) : a;
}

const double S3 = 0.5773502691896258;
const V3 U = {S3, S3, S3};
/* Melanin absorbs more at short wavelengths (B > G > R); hemoglobin peaks in green. */
const V3 PRIOR_MEL = {0.33, 0.51, 0.79};
const V3 PRIOR_HEM = {0.16, 0.79, 0.59};
const double MAX_DEG = 15.0;

V3 in_plane(V3 v)
{
	return v - U * dot(v, U);
}

/* Give a chromatic-plane direction the prior's brightness (along-U) component. */
V3 lift(V3 dp, V3 prior)
{
	V3 pp = in_plane(prior);
	return unit(unit(dp) + U * (dot(prior, U) / std::sqrt(dot(pp, pp))));
}

V3 clamp_cone(V3 d, V3 prior_p)
{
	double ang = std::acos(std::clamp(dot(d, prior_p), -1.0, 1.0));
	double maxr = MAX_DEG * M_PI / 180.0;
	if (ang <= maxr)
		return d;
	double t = maxr / ang;
	return unit(prior_p * (std::sin((1 - t) * ang) / std::sin(ang)) + d * (std::sin(t * ang) / std::sin(ang)));
}

bool build_inverse(PigmentBasis &b)
{
	/* columns: mel, hem, U */
	double m[3][3] = {{b.mel[0], b.hem[0], S3}, {b.mel[1], b.hem[1], S3}, {b.mel[2], b.hem[2], S3}};
	double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
		     m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
	if (std::fabs(det) < 1e-6)
		return false;
	double inv[3][3];
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++) {
			int r1 = (c + 1) % 3, r2 = (c + 2) % 3, c1 = (r + 1) % 3, c2 = (r + 2) % 3;
			inv[r][c] = (m[r1][c1] * m[r2][c2] - m[r1][c2] * m[r2][c1]) / det;
		}
	for (int i = 0; i < 9; i++)
		b.inv[i] = (float)inv[i / 3][i % 3];
	return true;
}

void set(float out[3], V3 v)
{
	out[0] = (float)v.x;
	out[1] = (float)v.y;
	out[2] = (float)v.z;
}

double srgb_to_lin(double c)
{
	return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

} // namespace

PigmentBasis pigment_prior_basis()
{
	PigmentBasis b;
	set(b.mel, unit(PRIOR_MEL));
	set(b.hem, unit(PRIOR_HEM));
	build_inverse(b);
	return b;
}

bool pigment_calibrate(const uint8_t *bgra, int w, int h, int stride, const uint8_t *mask, int mw, int mh,
		       PigmentBasis &out)
{
	/* --- sample skin pixels (confident skin, not shiny, not clipped) --- */
	static double lut[256];
	static bool lut_ready = false;
	if (!lut_ready) {
		for (int i = 0; i < 256; i++)
			lut[i] = -std::log(std::max(srgb_to_lin(i / 255.0), 1e-3));
		lut_ready = true;
	}
	std::vector<V3> od;
	od.reserve(16384);
	int step = std::max(1, (int)std::sqrt((double)w * h / 20000.0));
	for (int y = 0; y < h; y += step) {
		int my = std::min(mh - 1, y * mh / h);
		const uint8_t *row = bgra + (size_t)y * stride;
		for (int x = 0; x < w; x += step) {
			int mx = std::min(mw - 1, x * mw / w);
			if (mask[((size_t)my * mw + mx) * 4] < 217) /* skin probability > 0.85 */
				continue;
			int B = row[x * 4], G = row[x * 4 + 1], R = row[x * 4 + 2];
			int mxc = std::max(R, std::max(G, B)), mnc = std::min(R, std::min(G, B));
			if (mxc > 247 || mxc < 12)
				continue;
			if (mxc > 190 && (mxc - mnc) < 0.25 * mxc) /* shine */
				continue;
			od.push_back({lut[R], lut[G], lut[B]});
		}
	}
	if (od.size() < 400)
		return false;

	/* --- centre, remove shading, 2-D coordinates in the chromatic plane --- */
	V3 mean = {0, 0, 0};
	for (auto &v : od)
		mean = mean + v;
	mean = mean * (1.0 / od.size());
	V3 e1 = unit(cross(U, {0, 0, 1}));
	V3 e2 = cross(U, e1);
	std::vector<double> px(od.size()), py(od.size());
	double cxx = 0, cxy = 0, cyy = 0;
	for (size_t i = 0; i < od.size(); i++) {
		V3 d = od[i] - mean;
		px[i] = dot(d, e1);
		py[i] = dot(d, e2);
		cxx += px[i] * px[i];
		cxy += px[i] * py[i];
		cyy += py[i] * py[i];
	}
	cxx /= od.size();
	cxy /= od.size();
	cyy /= od.size();

	/* --- whitening (2x2 eigen-decomposition) --- */
	double tr = cxx + cyy, dt = cxx * cyy - cxy * cxy;
	double disc = std::sqrt(std::max(tr * tr / 4 - dt, 0.0));
	double l1 = tr / 2 + disc, l2 = tr / 2 - disc;
	if (l2 < 1e-10)
		return false;
	double ang0 = 0.5 * std::atan2(2 * cxy, cxx - cyy); /* eigenvector of l1 */
	double c0 = std::cos(ang0), s0 = std::sin(ang0);
	/* whitened z = D^-1/2 E^T p */
	auto white = [&](double x, double y, double &zx, double &zy) {
		zx = (c0 * x + s0 * y) / std::sqrt(l1);
		zy = (-s0 * x + c0 * y) / std::sqrt(l2);
	};

	/* --- 2-D ICA: in whitened space the unmixing is a rotation; pick the angle that maximises
	 *     non-Gaussianity (log-cosh negentropy proxy), brute force over 90 degrees --- */
	std::vector<double> zx(od.size()), zy(od.size());
	for (size_t i = 0; i < od.size(); i++)
		white(px[i], py[i], zx[i], zy[i]);
	const double G_GAUSS = 0.3745672; /* E[log cosh(nu)] for a unit Gaussian */
	double best = -1, best_t = 0;
	for (int k = 0; k < 90; k++) {
		double t = k * M_PI / 180.0, ct = std::cos(t), st = std::sin(t);
		double g1 = 0, g2 = 0;
		for (size_t i = 0; i < od.size(); i++) {
			double y1 = ct * zx[i] + st * zy[i], y2 = -st * zx[i] + ct * zy[i];
			g1 += std::log(std::cosh(y1));
			g2 += std::log(std::cosh(y2));
		}
		g1 /= od.size();
		g2 /= od.size();
		double J = (g1 - G_GAUSS) * (g1 - G_GAUSS) + (g2 - G_GAUSS) * (g2 - G_GAUSS);
		if (J > best) {
			best = J;
			best_t = t;
		}
	}
	/* mixing directions in the plane: columns of (R W)^-1 = W^-1 R^T */
	double ct = std::cos(best_t), st = std::sin(best_t);
	auto unwhite = [&](double a, double b) { /* p = E D^1/2 z */
		double ux = a * std::sqrt(l1), uy = b * std::sqrt(l2);
		return V3{0, 0, 0} + e1 * (c0 * ux - s0 * uy) + e2 * (s0 * ux + c0 * uy);
	};
	V3 d1 = unit(unwhite(ct, st));   /* source 1 direction */
	V3 d2 = unit(unwhite(-st, ct));  /* source 2 direction */

	/* --- label + orient with the priors, clamp, lift --- */
	V3 pm = unit(in_plane(PRIOR_MEL)), ph = unit(in_plane(PRIOR_HEM));
	if (dot(d1, pm + ph) < 0)
		d1 = d1 * -1.0;
	if (dot(d2, pm + ph) < 0)
		d2 = d2 * -1.0;
	bool d1_is_mel = (std::fabs(dot(d1, pm)) - std::fabs(dot(d1, ph))) > (std::fabs(dot(d2, pm)) - std::fabs(dot(d2, ph)));
	V3 mel_p = d1_is_mel ? d1 : d2, hem_p = d1_is_mel ? d2 : d1;
	if (dot(mel_p, pm) < 0)
		mel_p = mel_p * -1.0;
	if (dot(hem_p, ph) < 0)
		hem_p = hem_p * -1.0;
	mel_p = clamp_cone(mel_p, pm);
	hem_p = clamp_cone(hem_p, ph);

	PigmentBasis b;
	set(b.mel, lift(mel_p, PRIOR_MEL));
	set(b.hem, lift(hem_p, PRIOR_HEM));
	if (!build_inverse(b))
		return false;
	out = b;
	return true;
}

void pigment_blend(PigmentBasis &cur, const PigmentBasis &next, float k)
{
	V3 m = unit(V3{cur.mel[0], cur.mel[1], cur.mel[2]} * (1 - k) + V3{next.mel[0], next.mel[1], next.mel[2]} * k);
	V3 h = unit(V3{cur.hem[0], cur.hem[1], cur.hem[2]} * (1 - k) + V3{next.hem[0], next.hem[1], next.hem[2]} * k);
	PigmentBasis b;
	set(b.mel, m);
	set(b.hem, h);
	if (build_inverse(b))
		cur = b;
}

} // namespace beauty
