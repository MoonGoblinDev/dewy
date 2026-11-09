#include "widgets.hpp"

#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtWidgets/QColorDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QStyle>
#include <QtWidgets/QToolTip>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>

namespace beauty {

/* ============================== icons ============================== */

namespace {

struct Pen {
	QPainter &p;
	QRectF r;
	QPointF at(double x, double y) const { return {r.left() + x * r.width(), r.top() + y * r.height()}; }
};

void arrow(const Pen &g, double x0, double y0, double x1, double y1)
{
	QPointF a = g.at(x0, y0), b = g.at(x1, y1);
	g.p.drawLine(a, b);
	double ang = std::atan2(b.y() - a.y(), b.x() - a.x());
	double h = g.r.width() * 0.07;
	for (double s : {-1.0, 1.0}) {
		double t = ang + M_PI + s * 0.6;
		g.p.drawLine(b, b + QPointF(std::cos(t) * h, std::sin(t) * h));
	}
}

void star(const Pen &g, double cx, double cy, double s)
{
	QPainterPath path;
	path.moveTo(g.at(cx, cy - s));
	path.quadTo(g.at(cx, cy), g.at(cx + s, cy));
	path.quadTo(g.at(cx, cy), g.at(cx, cy + s));
	path.quadTo(g.at(cx, cy), g.at(cx - s, cy));
	path.quadTo(g.at(cx, cy), g.at(cx, cy - s));
	g.p.drawPath(path);
}

void faceOutline(const Pen &g, bool vshape)
{
	QPainterPath f;
	f.moveTo(g.at(0.27, 0.24));
	if (vshape) {
		f.cubicTo(g.at(0.26, 0.58), g.at(0.42, 0.80), g.at(0.5, 0.84));
		f.cubicTo(g.at(0.58, 0.80), g.at(0.74, 0.58), g.at(0.73, 0.24));
	} else {
		f.cubicTo(g.at(0.25, 0.64), g.at(0.38, 0.80), g.at(0.5, 0.80));
		f.cubicTo(g.at(0.62, 0.80), g.at(0.75, 0.64), g.at(0.73, 0.24));
	}
	g.p.drawPath(f);
	QPainterPath e;
	e.moveTo(g.at(0.36, 0.44));
	e.quadTo(g.at(0.40, 0.40), g.at(0.44, 0.44));
	e.moveTo(g.at(0.56, 0.44));
	e.quadTo(g.at(0.60, 0.40), g.at(0.64, 0.44));
	e.moveTo(g.at(0.44, 0.62));
	e.quadTo(g.at(0.5, 0.66), g.at(0.56, 0.62));
	g.p.drawPath(e);
}

void eyeShape(const Pen &g, bool bigIris)
{
	QPainterPath e;
	e.moveTo(g.at(0.16, 0.5));
	e.quadTo(g.at(0.5, 0.20), g.at(0.84, 0.5));
	e.quadTo(g.at(0.5, 0.80), g.at(0.16, 0.5));
	g.p.drawPath(e);
	double rr = bigIris ? 0.16 : 0.11;
	g.p.drawEllipse(g.at(0.5, 0.5), rr * g.r.width(), rr * g.r.height());
}

void noseShape(const Pen &g)
{
	QPainterPath n;
	n.moveTo(g.at(0.48, 0.18));
	n.cubicTo(g.at(0.46, 0.36), g.at(0.40, 0.48), g.at(0.40, 0.58));
	n.moveTo(g.at(0.32, 0.62));
	n.quadTo(g.at(0.34, 0.74), g.at(0.43, 0.71));
	n.quadTo(g.at(0.5, 0.78), g.at(0.57, 0.71));
	n.quadTo(g.at(0.66, 0.74), g.at(0.68, 0.62));
	g.p.drawPath(n);
}

void mouthShape(const Pen &g, bool smile)
{
	double cy = smile ? 0.46 : 0.52;
	QPainterPath m;
	m.moveTo(g.at(0.18, cy));
	m.cubicTo(g.at(0.32, 0.36), g.at(0.42, 0.37), g.at(0.5, 0.43));
	m.cubicTo(g.at(0.58, 0.37), g.at(0.68, 0.36), g.at(0.82, cy));
	m.cubicTo(g.at(0.68, 0.72), g.at(0.32, 0.72), g.at(0.18, cy));
	m.moveTo(g.at(0.20, cy + 0.01));
	m.quadTo(g.at(0.5, smile ? 0.60 : 0.55), g.at(0.80, cy + 0.01));
	g.p.drawPath(m);
}

void browShape(const Pen &g)
{
	QPainterPath b;
	b.moveTo(g.at(0.16, 0.58));
	b.quadTo(g.at(0.42, 0.30), g.at(0.84, 0.48));
	g.p.drawPath(b);
}

} // namespace

void paintIcon(QPainter &p, const QRectF &r, IconSpec s, const QColor &color)
{
	p.save();
	p.setRenderHint(QPainter::Antialiasing);
	QPen pen(color, std::max(1.2, r.width() * 0.055), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	Pen g{p, r};

	switch (s.base) {
	case Base::Face:
		faceOutline(g, false);
		break;
	case Base::VFace:
		faceOutline(g, true);
		break;
	case Base::Eye:
		eyeShape(g, s.mod == Mod::Iris);
		break;
	case Base::Nose:
		noseShape(g);
		break;
	case Base::Mouth:
		mouthShape(g, s.mod == Mod::Smile);
		break;
	case Base::Brow:
		browShape(g);
		break;
	case Base::Drop: {
		QPainterPath d;
		d.moveTo(g.at(0.5, 0.16));
		d.cubicTo(g.at(0.62, 0.36), g.at(0.76, 0.48), g.at(0.76, 0.62));
		d.cubicTo(g.at(0.76, 0.78), g.at(0.64, 0.86), g.at(0.5, 0.86));
		d.cubicTo(g.at(0.36, 0.86), g.at(0.24, 0.78), g.at(0.24, 0.62));
		d.cubicTo(g.at(0.24, 0.48), g.at(0.38, 0.36), g.at(0.5, 0.16));
		p.drawPath(d);
		break;
	}
	case Base::Compact:
		p.drawEllipse(g.at(0.5, 0.5), r.width() * 0.32, r.height() * 0.32);
		p.drawArc(QRectF(g.at(0.32, 0.32), g.at(0.68, 0.68)), 30 * 16, 120 * 16);
		star(g, 0.66, 0.34, 0.07);
		break;
	case Base::UnderEye: {
		QPainterPath e;
		e.moveTo(g.at(0.16, 0.44));
		e.quadTo(g.at(0.5, 0.16), g.at(0.84, 0.44));
		e.quadTo(g.at(0.5, 0.66), g.at(0.16, 0.44));
		p.drawPath(e);
		p.drawEllipse(g.at(0.5, 0.43), r.width() * 0.09, r.height() * 0.09);
		QPen d = pen;
		d.setStyle(Qt::DotLine);
		p.setPen(d);
		QPainterPath u;
		u.moveTo(g.at(0.24, 0.66));
		u.quadTo(g.at(0.5, 0.84), g.at(0.76, 0.66));
		p.drawPath(u);
		break;
	}
	case Base::Tooth: {
		QPainterPath t;
		t.moveTo(g.at(0.30, 0.22));
		t.cubicTo(g.at(0.18, 0.26), g.at(0.20, 0.48), g.at(0.30, 0.56));
		t.lineTo(g.at(0.36, 0.82));
		t.lineTo(g.at(0.44, 0.60));
		t.lineTo(g.at(0.56, 0.60));
		t.lineTo(g.at(0.64, 0.82));
		t.lineTo(g.at(0.70, 0.56));
		t.cubicTo(g.at(0.80, 0.48), g.at(0.82, 0.26), g.at(0.70, 0.22));
		t.cubicTo(g.at(0.60, 0.18), g.at(0.56, 0.26), g.at(0.5, 0.26));
		t.cubicTo(g.at(0.44, 0.26), g.at(0.40, 0.18), g.at(0.30, 0.22));
		p.drawPath(t);
		star(g, 0.78, 0.18, 0.08);
		break;
	}
	case Base::Contrast: {
		QRectF c(g.at(0.2, 0.2), g.at(0.8, 0.8));
		p.drawEllipse(c);
		QPainterPath half;
		half.moveTo(c.center());
		half.arcTo(c, 90, 180);
		half.closeSubpath();
		p.setBrush(color);
		p.drawPath(half);
		break;
	}
	case Base::Fold: {
		noseShape(g);
		QPainterPath f;
		f.moveTo(g.at(0.26, 0.62));
		f.quadTo(g.at(0.16, 0.76), g.at(0.24, 0.90));
		f.moveTo(g.at(0.74, 0.62));
		f.quadTo(g.at(0.84, 0.76), g.at(0.76, 0.90));
		p.drawPath(f);
		break;
	}
	case Base::Lips:
		mouthShape(g, false);
		break;
	case Base::Blush:
		faceOutline(g, false);
		p.setBrush(color);
		p.setOpacity(0.6);
		p.drawEllipse(g.at(0.36, 0.56), r.width() * 0.05, r.height() * 0.04);
		p.drawEllipse(g.at(0.64, 0.56), r.width() * 0.05, r.height() * 0.04);
		p.setOpacity(1.0);
		break;
	case Base::Shadow: {
		eyeShape(g, false);
		QPainterPath s2;
		s2.moveTo(g.at(0.18, 0.40));
		s2.quadTo(g.at(0.5, 0.06), g.at(0.82, 0.40));
		p.drawPath(s2);
		break;
	}
	case Base::Liner: {
		eyeShape(g, false);
		QPen thick = pen;
		thick.setWidthF(pen.widthF() * 1.8);
		p.setPen(thick);
		QPainterPath l;
		l.moveTo(g.at(0.22, 0.44));
		l.quadTo(g.at(0.5, 0.24), g.at(0.84, 0.46));
		l.lineTo(g.at(0.94, 0.36));
		p.drawPath(l);
		break;
	}
	case Base::Sculpt:
		faceOutline(g, false);
		p.drawLine(g.at(0.30, 0.50), g.at(0.38, 0.66));
		p.drawLine(g.at(0.70, 0.50), g.at(0.62, 0.66));
		break;
	case Base::Glow:
		faceOutline(g, false);
		star(g, 0.82, 0.20, 0.09);
		break;
	case Base::Palette: {
		QPainterPath pal;
		pal.addEllipse(QRectF(g.at(0.18, 0.2), g.at(0.82, 0.8)));
		p.drawPath(pal);
		for (auto [x, y] : {std::pair{0.38, 0.38}, {0.56, 0.32}, {0.66, 0.50}})
			p.drawEllipse(g.at(x, y), r.width() * 0.04, r.height() * 0.04);
		break;
	}
	case Base::SkinTone: { /* light and deep swatches */
		p.drawEllipse(g.at(0.38, 0.5), r.width() * 0.24, r.height() * 0.24);
		p.setBrush(color);
		p.drawEllipse(g.at(0.64, 0.5), r.width() * 0.20, r.height() * 0.20);
		break;
	}
	case Base::Wand: {
		p.drawLine(g.at(0.22, 0.80), g.at(0.62, 0.40));
		star(g, 0.72, 0.28, 0.16);
		star(g, 0.36, 0.26, 0.08);
		star(g, 0.80, 0.66, 0.07);
		break;
	}
	case Base::Smiley:
		p.drawEllipse(g.at(0.5, 0.5), r.width() * 0.36, r.height() * 0.36);
		p.drawArc(QRectF(g.at(0.32, 0.36), g.at(0.68, 0.68)), 200 * 16, 140 * 16);
		p.setBrush(color);
		p.drawEllipse(g.at(0.40, 0.42), r.width() * 0.03, r.height() * 0.03);
		p.drawEllipse(g.at(0.60, 0.42), r.width() * 0.03, r.height() * 0.03);
		break;
	case Base::Ambience: {
		QPainterPath f;
		f.addRoundedRect(QRectF(g.at(0.16, 0.24), g.at(0.70, 0.84)), 3, 3);
		p.drawPath(f);
		QPainterPath pl;
		pl.moveTo(g.at(0.36, 0.42));
		pl.lineTo(g.at(0.54, 0.54));
		pl.lineTo(g.at(0.36, 0.66));
		pl.closeSubpath();
		p.drawPath(pl);
		star(g, 0.80, 0.24, 0.10);
		break;
	}
	case Base::Filters:
		p.drawEllipse(g.at(0.5, 0.36), r.width() * 0.19, r.height() * 0.19);
		p.drawEllipse(g.at(0.37, 0.60), r.width() * 0.19, r.height() * 0.19);
		p.drawEllipse(g.at(0.63, 0.60), r.width() * 0.19, r.height() * 0.19);
		break;
	case Base::Bulb: {
		QPainterPath b;
		b.moveTo(g.at(0.40, 0.66));
		b.cubicTo(g.at(0.40, 0.56), g.at(0.26, 0.50), g.at(0.26, 0.38));
		b.cubicTo(g.at(0.26, 0.20), g.at(0.74, 0.20), g.at(0.74, 0.38));
		b.cubicTo(g.at(0.74, 0.50), g.at(0.60, 0.56), g.at(0.60, 0.66));
		b.closeSubpath();
		p.drawPath(b);
		p.drawLine(g.at(0.42, 0.76), g.at(0.58, 0.76));
		p.drawLine(g.at(0.45, 0.85), g.at(0.55, 0.85));
		break;
	}
	case Base::Body: {
		p.drawEllipse(g.at(0.5, 0.18), r.width() * 0.08, r.height() * 0.08);
		QPainterPath b;
		b.moveTo(g.at(0.32, 0.36));
		b.quadTo(g.at(0.40, 0.56), g.at(0.36, 0.86));
		b.moveTo(g.at(0.68, 0.36));
		b.quadTo(g.at(0.60, 0.56), g.at(0.64, 0.86));
		b.moveTo(g.at(0.32, 0.36));
		b.lineTo(g.at(0.68, 0.36));
		p.drawPath(b);
		break;
	}
	case Base::Arm: {
		QPainterPath a;
		a.moveTo(g.at(0.30, 0.20));
		a.quadTo(g.at(0.24, 0.50), g.at(0.42, 0.82));
		a.moveTo(g.at(0.46, 0.20));
		a.quadTo(g.at(0.42, 0.48), g.at(0.58, 0.78));
		p.drawPath(a);
		break;
	}
	case Base::Neck: {
		QPainterPath n;
		n.moveTo(g.at(0.40, 0.20));
		n.lineTo(g.at(0.40, 0.46));
		n.quadTo(g.at(0.24, 0.52), g.at(0.12, 0.62));
		n.moveTo(g.at(0.60, 0.20));
		n.lineTo(g.at(0.60, 0.46));
		n.quadTo(g.at(0.76, 0.52), g.at(0.88, 0.62));
		p.drawPath(n);
		break;
	}
	case Base::Leg: {
		QPainterPath l;
		l.moveTo(g.at(0.38, 0.14));
		l.lineTo(g.at(0.40, 0.86));
		l.moveTo(g.at(0.58, 0.14));
		l.lineTo(g.at(0.56, 0.86));
		p.drawPath(l);
		break;
	}
	}

	/* modifiers */
	QPen thin = pen;
	thin.setWidthF(pen.widthF() * 0.85);
	p.setPen(thin);
	p.setBrush(Qt::NoBrush);
	switch (s.mod) {
	case Mod::In:
		arrow(g, 0.02, 0.56, 0.16, 0.56);
		arrow(g, 0.98, 0.56, 0.84, 0.56);
		break;
	case Mod::Out:
		arrow(g, 0.16, 0.5, 0.02, 0.5);
		arrow(g, 0.84, 0.5, 0.98, 0.5);
		break;
	case Mod::InTop:
		arrow(g, 0.02, 0.36, 0.17, 0.36);
		arrow(g, 0.98, 0.36, 0.83, 0.36);
		break;
	case Mod::InBottom:
		arrow(g, 0.04, 0.72, 0.19, 0.68);
		arrow(g, 0.96, 0.72, 0.81, 0.68);
		break;
	case Mod::UpDown:
		arrow(g, 0.93, 0.50, 0.93, 0.24);
		arrow(g, 0.93, 0.50, 0.93, 0.76);
		break;
	case Mod::UpDownTop:
		arrow(g, 0.5, 0.14, 0.5, 0.0);
		arrow(g, 0.5, 0.14, 0.5, 0.28);
		break;
	case Mod::UpDownBottom:
		arrow(g, 0.5, 0.90, 0.5, 0.80);
		arrow(g, 0.5, 0.90, 0.5, 1.0);
		break;
	case Mod::Shrink:
		arrow(g, 0.04, 0.10, 0.16, 0.22);
		arrow(g, 0.96, 0.10, 0.84, 0.22);
		arrow(g, 0.04, 0.92, 0.16, 0.80);
		arrow(g, 0.96, 0.92, 0.84, 0.80);
		break;
	case Mod::Rotate: {
		QRectF a(g.at(0.62, 0.04), g.at(0.96, 0.38));
		p.drawArc(a, 10 * 16, 150 * 16);
		arrow(g, 0.95, 0.18, 0.95, 0.26);
		break;
	}
	case Mod::InnerCorner:
		arrow(g, 0.80, 0.5, 0.98, 0.5);
		break;
	case Mod::OuterCorner:
		arrow(g, 0.20, 0.5, 0.02, 0.5);
		break;
	case Mod::Down:
		arrow(g, 0.5, 0.78, 0.5, 0.98);
		break;
	case Mod::Sparkle:
		star(g, 0.84, 0.16, 0.10);
		break;
	case Mod::Smile:
		arrow(g, 0.12, 0.40, 0.12, 0.24);
		arrow(g, 0.88, 0.40, 0.88, 0.24);
		break;
	case Mod::Cheek:
		arrow(g, 0.02, 0.30, 0.17, 0.42);
		arrow(g, 0.98, 0.30, 0.83, 0.42);
		break;
	case Mod::HeadRing: {
		QPen dash = thin;
		dash.setStyle(Qt::DashLine);
		p.setPen(dash);
		p.drawEllipse(g.at(0.5, 0.5), r.width() * 0.47, r.height() * 0.47);
		break;
	}
	case Mod::Tip:
		p.drawEllipse(g.at(0.5, 0.86), r.width() * 0.05, r.height() * 0.05);
		break;
	default:
		break;
	}
	p.restore();
}

/* ============================== slider ============================== */

static const double KNOB = 8.0;

TTSlider::TTSlider(int mn, int mx, int d, QWidget *parent) : QWidget(parent), min(mn), max(mx), def(d), val(d)
{
	setMinimumHeight(26);
	setCursor(Qt::PointingHandCursor);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void TTSlider::setValue(int v)
{
	v = std::clamp(v, min, max);
	if (v != val) {
		val = v;
		update();
	}
}

double TTSlider::xOf(int v) const
{
	double w = width() - 2 * KNOB;
	return KNOB + w * (double)(v - min) / std::max(1, max - min);
}

int TTSlider::valueAt(double x) const
{
	double w = width() - 2 * KNOB;
	double t = std::clamp((x - KNOB) / std::max(1.0, w), 0.0, 1.0);
	return (int)std::lround(min + t * (max - min));
}

void TTSlider::userSet(int v)
{
	v = std::clamp(v, min, max);
	if (v == val)
		return;
	val = v;
	update();
	if (onChanged)
		onChanged(val);
}

void TTSlider::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	double cy = height() / 2.0;
	double x0 = KNOB, x1 = width() - KNOB;
	bool on = isEnabled();

	p.setPen(Qt::NoPen);
	p.setBrush(theme::line);
	p.drawRoundedRect(QRectF(x0, cy - 2, x1 - x0, 4), 2, 2);

	int origin = (min < 0 && max > 0) ? 0 : min;
	double xa = xOf(origin), xb = xOf(val);
	p.setBrush(on ? QColor(Qt::white) : theme::sub);
	p.drawRoundedRect(QRectF(std::min(xa, xb), cy - 2, std::abs(xb - xa), 4), 2, 2);

	if (def != origin) { /* default marker */
		p.setBrush(theme::sub);
		p.drawEllipse(QPointF(xOf(def), cy), 2.2, 2.2);
	}

	p.setBrush(QColor(0, 0, 0, 90));
	p.drawEllipse(QPointF(xb, cy + 1), KNOB + 1, KNOB + 1);
	p.setBrush(on ? QColor(Qt::white) : theme::sub);
	p.drawEllipse(QPointF(xb, cy), KNOB, KNOB);
}

void TTSlider::mousePressEvent(QMouseEvent *e)
{
	if (e->button() == Qt::LeftButton)
		userSet(valueAt(e->position().x()));
}

void TTSlider::mouseMoveEvent(QMouseEvent *e)
{
	if (e->buttons() & Qt::LeftButton)
		userSet(valueAt(e->position().x()));
}

void TTSlider::mouseDoubleClickEvent(QMouseEvent *)
{
	userSet(def);
}

void TTSlider::wheelEvent(QWheelEvent *e)
{
	e->ignore(); /* let the panel scroll */
}

SliderRow::SliderRow(int min, int max, int def, QWidget *parent) : QWidget(parent)
{
	auto *l = new QHBoxLayout(this);
	l->setContentsMargins(0, 0, 0, 0);
	l->setSpacing(10);
	slider = new TTSlider(min, max, def, this);
	readout = new QLabel(QString::number(def), this);
	readout->setMinimumWidth(30);
	readout->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	readout->setStyleSheet("color:#eeeef1; font-size:13px; font-weight:500;");
	l->addWidget(slider, 1);
	l->addWidget(readout);
	slider->onChanged = [this](int v) {
		readout->setText(QString::number(v));
		if (onChanged)
			onChanged(v);
	};
	slider->setToolTip(QStringLiteral("Double-click to reset"));
}

void SliderRow::setValue(int v)
{
	slider->setValue(v);
	readout->setText(QString::number(slider->value()));
}

void SliderRow::setRange(int mn, int mx, int def)
{
	slider->setRange(mn, mx, def);
	readout->setText(QString::number(slider->value()));
}

/* ============================== circle button ============================== */

CircleButton::CircleButton(IconSpec ic, const QString &tip, QWidget *parent) : QWidget(parent), icon(ic)
{
	setToolTip(tip);
	setFixedSize(sizeHint());
	setCursor(Qt::PointingHandCursor);
}

void CircleButton::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	if (!isEnabled())
		p.setOpacity(0.35);
	p.setRenderHint(QPainter::Antialiasing);
	QRectF c(3, 3, 46, 46);
	p.setPen(Qt::NoPen);
	p.setBrush(hover && !selected ? theme::chipHover : theme::chip);
	p.drawEllipse(c);
	if (selected) {
		p.setPen(QPen(Qt::white, 2.0));
		p.setBrush(Qt::NoBrush);
		p.drawEllipse(c.adjusted(-1, -1, 1, 1));
	}
	paintIcon(p, c.adjusted(11, 11, -11, -11), icon, selected ? QColor(Qt::white) : QColor(0xd8, 0xd8, 0xde));
	if (modified) {
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0xb0, 0xb0, 0xb8));
		p.drawEllipse(QPointF(26, 56), 2.0, 2.0);
	}
}

void CircleButton::mousePressEvent(QMouseEvent *e)
{
	if (e->button() == Qt::LeftButton && onClicked)
		onClicked();
}

/* ============================== toggle ============================== */

Toggle::Toggle(QWidget *parent) : QWidget(parent)
{
	setFixedSize(sizeHint());
	setCursor(Qt::PointingHandCursor);
}

void Toggle::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(on ? theme::blue : theme::line);
	p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 11, 11);
	p.setBrush(Qt::white);
	double x = on ? width() - 12.5 : 12.5;
	p.drawEllipse(QPointF(x, height() / 2.0), 9, 9);
}

void Toggle::mousePressEvent(QMouseEvent *)
{
	on = !on;
	update();
	if (onToggled)
		onToggled(on);
}

/* ============================== icon button ============================== */

IconButton::IconButton(std::function<void(QPainter &, const QRectF &, const QColor &)> d, const QString &tip,
		       QWidget *parent)
	: QWidget(parent),
	  draw(std::move(d))
{
	setToolTip(tip);
	setFixedSize(sizeHint());
	setCursor(Qt::PointingHandCursor);
}

void IconButton::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	if (hover) {
		p.setPen(Qt::NoPen);
		p.setBrush(theme::chip);
		p.drawEllipse(rect().adjusted(1, 1, -1, -1));
	}
	draw(p, QRectF(rect()).adjusted(6, 6, -6, -6), theme::text);
}

void IconButton::mousePressEvent(QMouseEvent *e)
{
	if (e->button() == Qt::LeftButton && onClicked)
		onClicked();
}

/* ============================== swatch ============================== */

Swatch::Swatch(QWidget *parent) : QWidget(parent)
{
	setFixedSize(sizeHint());
	setCursor(Qt::PointingHandCursor);
	setToolTip(QStringLiteral("Pick colour"));
}

void Swatch::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	if (!isEnabled())
		p.setOpacity(0.35);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(QPen(QColor(255, 255, 255, 160), 1.5));
	p.setBrush(color);
	p.drawEllipse(QRectF(rect()).adjusted(2, 2, -2, -2));
}

void Swatch::mousePressEvent(QMouseEvent *)
{
	QColor c = QColorDialog::getColor(color, this, QStringLiteral("Choose colour"));
	if (c.isValid()) {
		color = c;
		update();
		if (onPicked)
			onPicked(c);
	}
}

/* ============================== look art ============================== */

static QColor mix(const QColor &a, const QColor &b, double t)
{
	t = std::clamp(t, 0.0, 1.0);
	return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
				a.blueF() + (b.blueF() - a.blueF()) * t);
}

static QColor withAlpha(QColor c, double a)
{
	c.setAlphaF(std::clamp(a, 0.0, 1.0));
	return c;
}

void paintLook(QPainter &p, const QRectF &r, const LookArt &a)
{
	p.save();
	p.setRenderHint(QPainter::Antialiasing);
	QPainterPath clip;
	clip.addRoundedRect(r, 12, 12);
	p.setClipPath(clip);

	auto at = [&](double x, double y) { return QPointF(r.left() + x * r.width(), r.top() + y * r.height()); };
	auto sz = [&](double w, double h) { return QSizeF(w * r.width(), h * r.height()); };
	auto ell = [&](double cx, double cy, double w, double h) {
		return QRectF(at(cx - w / 2, cy - h / 2), sz(w, h));
	};

	if (a.none || a.custom) {
		p.fillRect(r, theme::chip);
		paintIcon(p, ell(0.5, 0.5, 0.42, 0.42), a.none ? IconSpec{Base::Contrast} : IconSpec{Base::Palette},
			  QColor(0xc8, 0xc8, 0xd0));
		if (a.none) { /* draw a "no" symbol instead of the contrast glyph */
			p.fillRect(r, theme::chip);
			p.setPen(QPen(QColor(0xc8, 0xc8, 0xd0), 2.2));
			p.setBrush(Qt::NoBrush);
			QRectF c = ell(0.5, 0.5, 0.40, 0.40);
			p.drawEllipse(c);
			p.drawLine(at(0.36, 0.36), at(0.64, 0.64));
		}
		p.restore();
		return;
	}

	/* background tinted by the look's colour grade */
	QColor base(0xe8, 0xdd, 0xd8);
	QColor warm(0xf3, 0xcf, 0xb8), cool(0xcf, 0xdb, 0xec), pink(0xf1, 0xcf, 0xdc);
	QColor bg = mix(base, a.warmth >= 0 ? warm : cool, std::abs(a.warmth) * 4.0);
	bg = mix(bg, pink, std::max(0.0, a.tint) * 5.0);
	bg = mix(bg, QColor(Qt::white), std::max(0.0, a.exposure) * 2.5);
	bg = mix(bg, QColor(0xc8, 0xbe, 0xb4), a.fade * 1.5);
	QLinearGradient grad(r.topLeft(), r.bottomLeft());
	grad.setColorAt(0, bg.lighter(106));
	grad.setColorAt(1, bg.darker(104));
	p.fillRect(r, grad);

	/* zoom in on the head so lips / blush / lids read at tile size */
	QPointF zc = at(0.5, 0.50);
	p.translate(zc);
	p.scale(1.45, 1.45);
	p.translate(-zc - QPointF(0, r.height() * 0.02));

	QColor skin = mix(QColor(0xe2, 0xb2, 0x92), QColor(0xf2, 0xd2, 0xbc), a.exposure * 2.0 + a.highlight * 0.3);
	if (a.warmth > 0)
		skin = mix(skin, QColor(0xe4, 0xa8, 0x84), a.warmth);
	QColor hair(0x2b, 0x22, 0x20);

	p.setPen(Qt::NoPen);
	/* hair behind */
	p.setBrush(hair);
	if (a.men)
		p.drawEllipse(ell(0.5, 0.40, 0.56, 0.56));
	else {
		QPainterPath h;
		h.addRoundedRect(QRectF(at(0.20, 0.16), sz(0.60, 0.80)), 26, 26);
		p.drawPath(h);
	}
	/* neck + shoulders */
	p.setBrush(skin.darker(108));
	p.drawRect(QRectF(at(0.43, 0.70), sz(0.14, 0.18)));
	p.setBrush(a.men ? QColor(0x3d, 0x44, 0x52) : QColor(0xf4, 0xf0, 0xec));
	p.drawEllipse(ell(0.5, 1.05, 0.9, 0.36));
	/* face */
	p.setBrush(skin);
	QPainterPath face;
	face.moveTo(at(0.30, 0.42));
	face.cubicTo(at(0.30, 0.20), at(0.70, 0.20), at(0.70, 0.42));
	face.cubicTo(at(0.70, 0.62), at(0.58, 0.77), at(0.5, 0.77));
	face.cubicTo(at(0.42, 0.77), at(0.30, 0.62), at(0.30, 0.42));
	p.drawPath(face);
	/* fringe */
	p.setBrush(hair);
	QPainterPath fr;
	fr.moveTo(at(0.28, 0.40));
	fr.cubicTo(at(0.28, 0.14), at(0.72, 0.14), at(0.72, 0.40));
	fr.cubicTo(at(0.62, a.men ? 0.26 : 0.30), at(0.44, 0.24), at(0.28, 0.40));
	p.drawPath(fr);

	/* contour + highlight */
	if (a.contour > 0) {
		p.setBrush(withAlpha(skin.darker(135), a.contour * 0.8));
		p.drawEllipse(ell(0.35, 0.58, 0.06, 0.14));
		p.drawEllipse(ell(0.65, 0.58, 0.06, 0.14));
	}
	if (a.highlight > 0) {
		p.setBrush(withAlpha(QColor(Qt::white), a.highlight * 0.7));
		p.drawEllipse(ell(0.5, 0.52, 0.035, 0.12));
		p.drawEllipse(ell(0.38, 0.52, 0.07, 0.03));
		p.drawEllipse(ell(0.62, 0.52, 0.07, 0.03));
	}
	/* blush */
	if (a.blushOpacity > 0) {
		double bx = a.blushStyle == 1 ? 0.15 : a.blushStyle == 2 ? 0.10 : 0.12;
		double by = a.blushStyle == 1 ? 0.54 : 0.57;
		for (double s : {-1.0, 1.0}) {
			QPointF c = at(0.5 + s * bx, by);
			QRadialGradient rg(c, r.width() * (a.blushStyle == 2 ? 0.10 : 0.075));
			rg.setColorAt(0, withAlpha(a.blush, std::min(1.0, a.blushOpacity * 2.2)));
			rg.setColorAt(1, withAlpha(a.blush, 0));
			p.setBrush(rg);
			p.drawEllipse(c, r.width() * 0.11, r.height() * 0.08);
		}
		if (a.blushStyle == 2) {
			p.setBrush(withAlpha(a.blush, a.blushOpacity));
			p.drawEllipse(ell(0.5, 0.56, 0.08, 0.03));
		}
	}
	/* eyes */
	for (double s : {-1.0, 1.0}) {
		double ex = 0.5 + s * 0.085;
		if (a.shadowOpacity > 0) {
			p.setBrush(withAlpha(a.shadow, std::min(1.0, a.shadowOpacity * 1.8)));
			p.drawEllipse(ell(ex, 0.455, 0.085, 0.04));
		}
		p.setBrush(QColor(0x2a, 0x1e, 0x1a));
		p.drawEllipse(ell(ex, 0.475, 0.045, 0.022));
		if (a.liner > 0) {
			p.setPen(QPen(QColor(0x18, 0x12, 0x10), 0.6 + a.liner * 1.8, Qt::SolidLine, Qt::RoundCap));
			p.drawLine(at(ex - s * 0.025, 0.468), at(ex + s * 0.035, 0.460));
			if (a.liner > 0.3)
				p.drawLine(at(ex + s * 0.035, 0.460), at(ex + s * 0.05, 0.448));
			p.setPen(Qt::NoPen);
		}
		/* brows */
		p.setPen(QPen(hair, 1.0 + a.brow * 2.0 + (a.men ? 0.8 : 0.0), Qt::SolidLine, Qt::RoundCap));
		QPainterPath b;
		b.moveTo(at(ex - s * 0.04, 0.43));
		b.quadTo(at(ex, 0.405), at(ex + s * 0.045, 0.425));
		p.drawPath(b);
		p.setPen(Qt::NoPen);
	}
	/* nose */
	p.setPen(QPen(skin.darker(130), 1.0, Qt::SolidLine, Qt::RoundCap));
	p.drawLine(at(0.49, 0.56), at(0.51, 0.565));
	p.setPen(Qt::NoPen);
	/* lips */
	QColor natural(0xc6, 0x86, 0x78);
	QColor lip = mix(natural, a.lip, std::min(1.0, a.lipOpacity * 1.7));
	p.setBrush(lip);
	QPainterPath m;
	m.moveTo(at(0.455, 0.645));
	m.cubicTo(at(0.47, 0.625), at(0.49, 0.63), at(0.5, 0.635));
	m.cubicTo(at(0.51, 0.63), at(0.53, 0.625), at(0.545, 0.645));
	m.cubicTo(at(0.53, 0.675), at(0.47, 0.675), at(0.455, 0.645));
	p.drawPath(m);
	if (a.gloss > 0.2) {
		p.setBrush(withAlpha(QColor(Qt::white), a.gloss * 0.7));
		p.drawEllipse(ell(0.505, 0.655, 0.025, 0.008));
	}
	p.restore();
}

LookTile::LookTile(const LookArt &a, QWidget *parent) : QWidget(parent), look(a)
{
	setFixedSize(sizeHint());
	setCursor(Qt::PointingHandCursor);
	setToolTip(a.name);
}

void LookTile::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	if (!isEnabled())
		p.setOpacity(0.35);
	p.setRenderHint(QPainter::Antialiasing);
	QRectF thumb(3, 3, width() - 6, width() - 6);
	paintLook(p, thumb, look);
	if (selected || hover) {
		p.setPen(QPen(selected ? QColor(Qt::white) : QColor(255, 255, 255, 70), selected ? 2.2 : 1.5));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(thumb.adjusted(-1.5, -1.5, 1.5, 1.5), 13, 13);
	}
	QFont f = font();
	f.setPixelSize(11);
	p.setFont(f);
	p.setPen(selected ? theme::text : theme::sub);
	QRectF tr(0, width() - 1, width(), height() - width() + 1);
	p.drawText(tr, Qt::AlignHCenter | Qt::AlignVCenter,
		   QFontMetrics(f).elidedText(look.name, Qt::ElideRight, width() - 2));
}

void LookTile::mousePressEvent(QMouseEvent *e)
{
	if (e->button() == Qt::LeftButton && onClicked)
		onClicked();
}

/* ============================== thumb tile ============================== */

ThumbTile::ThumbTile(const QString &n, const QImage &img, QWidget *parent) : QWidget(parent), label(n), image(img)
{
	setCursor(Qt::PointingHandCursor);
	setToolTip(n);
	QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Preferred);
	sp.setHeightForWidth(true);
	setSizePolicy(sp);
	setMinimumSize(64, 84);
}

void ThumbTile::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	if (!isEnabled())
		p.setOpacity(0.35);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	double side = std::min(width(), height() - 20) - 6.0;
	QRectF thumb((width() - side) / 2.0, 3, side, side);
	QPainterPath clip;
	clip.addRoundedRect(thumb, side * 0.16, side * 0.16);
	p.save();
	p.setClipPath(clip);
	if (!image.isNull())
		p.drawImage(thumb, image);
	else
		p.fillRect(thumb, theme::chip);
	p.restore();
	p.setBrush(Qt::NoBrush);
	if (selected) {
		p.setPen(QPen(Qt::white, 2.4));
		p.drawRoundedRect(thumb.adjusted(-1.5, -1.5, 1.5, 1.5), side * 0.17, side * 0.17);
	} else {
		p.setPen(QPen(hover ? QColor(255, 255, 255, 90) : QColor(255, 255, 255, 28), 1.2));
		p.drawRoundedRect(thumb, side * 0.16, side * 0.16);
	}
	QFont f = font();
	f.setPixelSize(11);
	p.setFont(f);
	p.setPen(selected ? theme::text : theme::sub);
	p.drawText(QRectF(0, thumb.bottom() + 2, width(), 16), Qt::AlignHCenter | Qt::AlignVCenter,
		   QFontMetrics(f).elidedText(label, Qt::ElideRight, width() - 2));
}

void ThumbTile::mousePressEvent(QMouseEvent *e)
{
	if (e->button() == Qt::LeftButton && onClicked)
		onClicked();
}

/* ============================== preset grid ============================== */

PresetGrid::PresetGrid(QWidget *parent, int columns) : QWidget(parent), cols(columns)
{
	grid = new QGridLayout(this);
	grid->setContentsMargins(0, 0, 0, 0);
	grid->setHorizontalSpacing(10);
	grid->setVerticalSpacing(8);
}

void PresetGrid::setItems(const std::vector<Item> &list)
{
	items = list;
	for (auto *t : tiles)
		t->deleteLater();
	tiles.clear();
	for (auto &it : items) {
		auto *t = new ThumbTile(it.name, it.thumb, this);
		QString n = it.name;
		t->onClicked = [this, n]() {
			if (onPicked)
				onPicked(n);
		};
		tiles.push_back(t);
	}
	relayout();
}

void PresetGrid::setThumbs(const std::vector<QImage> &thumbs)
{
	for (size_t i = 0; i < tiles.size() && i < thumbs.size(); i++) {
		items[i].thumb = thumbs[i];
		tiles[i]->setImage(thumbs[i]);
	}
}

void PresetGrid::setSelected(const QString &name)
{
	bool moved = name != selected;
	selected = name;
	for (auto *t : tiles)
		t->setSelected(t->name() == name);
	if (moved)
		relayout();
}

void PresetGrid::relayout()
{
	while (grid->count())
		grid->takeAt(0);
	int sel = -1;
	for (size_t i = 0; i < tiles.size(); i++)
		if (tiles[i]->name() == selected)
			sel = (int)i;
	int panelRow = (inlinePanel && sel >= 0 && selected != QStringLiteral("None")) ? sel / cols + 1 : -1;
	for (size_t i = 0; i < tiles.size(); i++) {
		int r = (int)i / cols;
		if (panelRow >= 0 && r >= panelRow)
			r++;
		grid->addWidget(tiles[i], r, (int)i % cols);
	}
	for (int c = 0; c < cols; c++)
		grid->setColumnStretch(c, 1);
	if (inlinePanel) {
		inlinePanel->setVisible(panelRow >= 0);
		if (panelRow >= 0)
			grid->addWidget(inlinePanel, panelRow, 0, 1, cols);
	}
}

/* ============================== top tabs ============================== */

TopTabs::TopTabs(QWidget *parent) : QWidget(parent)
{
	auto *outer = new QHBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(2);
	auto chevron = [this](bool right) {
		auto *b = new IconButton(
			[right](QPainter &p, const QRectF &r, const QColor &c) {
				p.setRenderHint(QPainter::Antialiasing);
				p.setPen(QPen(c, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
				double cx = r.center().x(), cy = r.center().y(), s = r.width() * 0.22;
				double d = right ? 1 : -1;
				p.drawLine(QPointF(cx - d * s * 0.5, cy - s), QPointF(cx + d * s * 0.5, cy));
				p.drawLine(QPointF(cx + d * s * 0.5, cy), QPointF(cx - d * s * 0.5, cy + s));
			},
			right ? QStringLiteral("More") : QStringLiteral("Back"), this);
		b->onClicked = [this, right]() {
			auto *bar = scroll->horizontalScrollBar();
			bar->setValue(bar->value() + (right ? 140 : -140));
		};
		return b;
	};
	auto *left = chevron(false);
	scroll = new QScrollArea(this);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll->setWidgetResizable(true);
	scroll->setFixedHeight(40);
	strip = new QWidget(scroll);
	row = new QHBoxLayout(strip);
	row->setContentsMargins(0, 0, 0, 0);
	row->setSpacing(4);
	row->addStretch(1);
	scroll->setWidget(strip);
	auto *right = chevron(true);
	outer->addWidget(left);
	outer->addWidget(scroll, 1);
	outer->addWidget(right);
}

void TopTabs::addTab(const QString &text, IconSpec icon)
{
	auto *b = new QPushButton(text, strip);
	b->setCheckable(true);
	b->setCursor(Qt::PointingHandCursor);
	b->setProperty("role", "toptab");
	QPixmap pm(40, 40);
	pm.setDevicePixelRatio(2.0);
	pm.fill(Qt::transparent);
	{
		QPainter p(&pm);
		paintIcon(p, QRectF(1, 1, 18, 18), icon, theme::text);
	}
	b->setIcon(QIcon(pm));
	b->setIconSize(QSize(18, 18));
	int idx = (int)buttons.size();
	QObject::connect(b, &QPushButton::clicked, this, [this, idx]() {
		setCurrent(idx);
		if (onChanged)
			onChanged(idx);
	});
	row->insertWidget(row->count() - 1, b);
	buttons.push_back(b);
	if (idx == 0)
		b->setChecked(true);
}

void TopTabs::setCurrent(int i)
{
	cur = i;
	for (size_t k = 0; k < buttons.size(); k++)
		buttons[k]->setChecked((int)k == i);
	if (i >= 0 && i < (int)buttons.size())
		scroll->ensureWidgetVisible(buttons[i], 30, 0);
}

/* ============================== flow layout ============================== */

FlowLayout::FlowLayout(QWidget *parent, int h, int v) : QLayout(parent), hs(h), vs(v)
{
	setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout()
{
	while (QLayoutItem *item = takeAt(0))
		delete item;
}

QLayoutItem *FlowLayout::takeAt(int i)
{
	if (i < 0 || i >= count())
		return nullptr;
	QLayoutItem *it = items[i];
	items.erase(items.begin() + i);
	return it;
}

QSize FlowLayout::minimumSize() const
{
	QSize s;
	for (auto *it : items)
		s = s.expandedTo(it->minimumSize());
	return s;
}

int FlowLayout::doLayout(const QRect &rect, bool testOnly) const
{
	int x = rect.x(), y = rect.y(), lineH = 0;
	for (auto *it : items) {
		if (it->widget() && it->widget()->isHidden())
			continue;
		QSize s = it->sizeHint();
		if (x + s.width() > rect.right() + 1 && lineH > 0) {
			x = rect.x();
			y += lineH + vs;
			lineH = 0;
		}
		if (!testOnly)
			it->setGeometry(QRect(QPoint(x, y), s));
		x += s.width() + hs;
		lineH = std::max(lineH, s.height());
	}
	return y + lineH - rect.y();
}

} // namespace beauty
