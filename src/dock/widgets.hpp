// Custom-painted widgets for the Dewy dock. No Q_OBJECT anywhere (callbacks are
// std::function), so the plugin builds without moc.
#pragma once

#include <QtWidgets/QLayout>
#include <QtWidgets/QWidget>
#include <QtGui/QColor>
#include <functional>
#include <vector>

class QLabel;
class QScrollArea;
class QHBoxLayout;
class QPushButton;
class QGridLayout;

namespace beauty {

namespace theme {
inline const QColor bg{0x16, 0x16, 0x19};
inline const QColor card{0x21, 0x21, 0x25};
inline const QColor chip{0x2b, 0x2b, 0x30};
inline const QColor chipHover{0x36, 0x36, 0x3c};
inline const QColor line{0x3a, 0x3a, 0x40};
inline const QColor text{0xee, 0xee, 0xf1};
inline const QColor sub{0x8d, 0x8d, 0x96};
inline const QColor accent{0xfe, 0x2c, 0x55};
inline const QColor blue{0x2f, 0x7c, 0xf6};
} // namespace theme

/* ---------- icons ---------- */

enum class Base {
	Face, VFace, Eye, Nose, Mouth, Brow, Drop, Compact, UnderEye, Tooth, Contrast, Fold,
	Lips, Blush, Shadow, Liner, Sculpt, Glow, Body, Arm, Neck, Leg, Palette,
	Wand, Smiley, Ambience, Filters, Bulb, SkinTone, Redness
};
enum class Mod {
	None, In, Out, InTop, InBottom, UpDown, UpDownTop, UpDownBottom, Shrink, Rotate,
	InnerCorner, OuterCorner, Down, Iris, Sparkle, Smile, Tip, Cheek, HeadRing
};
struct IconSpec {
	Base base;
	Mod mod = Mod::None;
};

void paintIcon(QPainter &p, const QRectF &r, IconSpec spec, const QColor &color);

/* ---------- slider: thin track, white fill (from the centre when bipolar), round knob ---------- */

class TTSlider : public QWidget {
public:
	TTSlider(int min, int max, int def, QWidget *parent = nullptr);
	void setValue(int v); /* programmatic: no callback */
	int value() const { return val; }
	QPointF knobPos() const { return QPointF(xOf(val), height() / 2.0); } /* automation */
	QPointF knobAt(int v) const { return QPointF(xOf(v), height() / 2.0); }
	int minimum() const { return min; }
	int maximum() const { return max; }
	void setRange(int mn, int mx, int d)
	{
		min = mn;
		max = mx;
		def = d;
		val = val < min ? min : val > max ? max : val;
		update();
	}
	std::function<void(int)> onChanged; /* user edits (while dragging too) */

	QSize sizeHint() const override { return {200, 26}; }

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseDoubleClickEvent(QMouseEvent *) override;
	void wheelEvent(QWheelEvent *e) override;

private:
	int valueAt(double x) const;
	double xOf(int v) const;
	void userSet(int v);
	int min, max, def, val;
};

/* Slider + numeric readout. */
class SliderRow : public QWidget {
public:
	SliderRow(int min, int max, int def, QWidget *parent = nullptr);
	void setValue(int v);
	void setRange(int min, int max, int def);
	TTSlider *slider;
	QLabel *readout;
	std::function<void(int)> onChanged;
};

/* ---------- round feature button ("face / eyes / nose" chips) ---------- */

class CircleButton : public QWidget {
public:
	CircleButton(IconSpec icon, const QString &tip, QWidget *parent = nullptr);
	void setSelected(bool s)
	{
		selected = s;
		update();
	}
	void setModified(bool m)
	{
		modified = m;
		update();
	}
	std::function<void()> onClicked;
	QSize sizeHint() const override { return {52, 60}; }

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void enterEvent(QEnterEvent *) override
	{
		hover = true;
		update();
	}
	void leaveEvent(QEvent *) override
	{
		hover = false;
		update();
	}

private:
	IconSpec icon;
	bool selected = false, modified = false, hover = false;
};

/* ---------- iOS-style switch ---------- */

class Toggle : public QWidget {
public:
	explicit Toggle(QWidget *parent = nullptr);
	void setChecked(bool c)
	{
		on = c;
		update();
	}
	bool isChecked() const { return on; }
	std::function<void(bool)> onToggled;
	QSize sizeHint() const override { return {42, 24}; }

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;

private:
	bool on = false;
};

/* ---------- small round icon button (reset, etc.) ---------- */

class IconButton : public QWidget {
public:
	IconButton(std::function<void(QPainter &, const QRectF &, const QColor &)> draw, const QString &tip,
		   QWidget *parent = nullptr);
	std::function<void()> onClicked;
	QSize sizeHint() const override { return {28, 28}; }

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void enterEvent(QEnterEvent *) override
	{
		hover = true;
		update();
	}
	void leaveEvent(QEvent *) override
	{
		hover = false;
		update();
	}

private:
	std::function<void(QPainter &, const QRectF &, const QColor &)> draw;
	bool hover = false;
};

/* ---------- colour swatch ---------- */

class Swatch : public QWidget {
public:
	explicit Swatch(QWidget *parent = nullptr);
	void setColor(const QColor &c)
	{
		color = c;
		update();
	}
	QColor colorValue() const { return color; }
	std::function<void(QColor)> onPicked;
	QSize sizeHint() const override { return {24, 24}; }

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;

private:
	QColor color{Qt::white};
};

/* ---------- makeup look tile with an illustrated face ---------- */

struct LookArt {
	QString name;
	bool none = false, custom = false, men = false;
	QColor lip{0xc9, 0x87, 0x7a};
	double lipOpacity = 0, gloss = 0;
	QColor blush{0xf2, 0xa0, 0xa0};
	double blushOpacity = 0;
	int blushStyle = 0;
	QColor shadow{0xa0, 0x70, 0x60};
	double shadowOpacity = 0, liner = 0, contour = 0, highlight = 0, brow = 0;
	double exposure = 0, warmth = 0, tint = 0, saturation = 0, fade = 0;
};

void paintLook(QPainter &p, const QRectF &r, const LookArt &a);

class LookTile : public QWidget {
public:
	LookTile(const LookArt &art, QWidget *parent = nullptr);
	void setSelected(bool s)
	{
		selected = s;
		update();
	}
	const LookArt &art() const { return look; }
	std::function<void()> onClicked;
	QSize sizeHint() const override { return {70, 88}; }

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void enterEvent(QEnterEvent *) override
	{
		hover = true;
		update();
	}
	void leaveEvent(QEvent *) override
	{
		hover = false;
		update();
	}

private:
	LookArt look;
	bool selected = false, hover = false;
};

/* ---------- preset tile with a live thumbnail ---------- */

class ThumbTile : public QWidget {
public:
	ThumbTile(const QString &name, const QImage &img, QWidget *parent = nullptr);
	void setImage(const QImage &img)
	{
		image = img;
		update();
	}
	void setSelected(bool s)
	{
		selected = s;
		update();
	}
	const QString &name() const { return label; }
	std::function<void()> onClicked;
	std::function<void(const QPoint &)> onContext; /* right click, global position */
	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int w) const override { return w + 20; }
	QSize sizeHint() const override { return {96, 116}; }

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void enterEvent(QEnterEvent *) override
	{
		hover = true;
		update();
	}
	void leaveEvent(QEvent *) override
	{
		hover = false;
		update();
	}

private:
	QString label;
	QImage image;
	bool selected = false, hover = false;
};

/* ---------- 3-column preset grid with an optional panel under the selected row ---------- */

class PresetGrid : public QWidget {
public:
	struct Item {
		QString name;
		QImage thumb;
	};
	explicit PresetGrid(QWidget *parent = nullptr, int columns = 3);
	void setItems(const std::vector<Item> &items);
	void setThumbs(const std::vector<QImage> &thumbs);
	void setSelected(const QString &name);
	void setPanel(QWidget *panel) { inlinePanel = panel; }
	std::function<void(const QString &)> onPicked;
	std::function<void(const QString &, const QPoint &)> onContext;
	const std::vector<Item> &itemList() const { return items; }
	QWidget *tileNamed(const QString &name) const
	{
		for (size_t i = 0; i < items.size() && i < tiles.size(); i++)
			if (items[i].name == name)
				return (QWidget *)tiles[i];
		return nullptr;
	}

private:
	void relayout();
	std::vector<Item> items;
	std::vector<ThumbTile *> tiles;
	QString selected;
	QWidget *inlinePanel = nullptr;
	QGridLayout *grid;
	int cols;
};

/* ---------- top tab bar (icon + label, underline, scrolls with chevrons) ---------- */

class TopTabs : public QWidget {
public:
	explicit TopTabs(QWidget *parent = nullptr);
	void addTab(const QString &text, IconSpec icon);
	void setCurrent(int i);
	int current() const { return cur; }
	QWidget *button(int i) const { return i >= 0 && i < (int)buttons.size() ? (QWidget *)buttons[i] : nullptr; }
	QWidget *chevron(bool right) const { return right ? rightBtn : leftBtn; }
	void scrollStep(bool right);
	void animateTo(int x);
	std::function<void(int)> onChanged;

private:
	QScrollArea *scroll;
	QWidget *strip;
	QHBoxLayout *row;
	std::vector<QPushButton *> buttons;
	QWidget *leftBtn = nullptr, *rightBtn = nullptr;
	int cur = 0;
};

/* ---------- wrapping layout (Qt's FlowLayout example, trimmed) ---------- */

class FlowLayout : public QLayout {
public:
	explicit FlowLayout(QWidget *parent = nullptr, int hSpacing = 8, int vSpacing = 8);
	~FlowLayout() override;
	void addItem(QLayoutItem *item) override { items.push_back(item); }
	int count() const override { return (int)items.size(); }
	QLayoutItem *itemAt(int i) const override { return i >= 0 && i < count() ? items[i] : nullptr; }
	QLayoutItem *takeAt(int i) override;
	Qt::Orientations expandingDirections() const override { return {}; }
	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int w) const override { return doLayout(QRect(0, 0, w, 0), true); }
	QSize minimumSize() const override;
	QSize sizeHint() const override { return minimumSize(); }
	void setGeometry(const QRect &r) override
	{
		QLayout::setGeometry(r);
		doLayout(r, false);
	}

private:
	int doLayout(const QRect &rect, bool testOnly) const;
	std::vector<QLayoutItem *> items;
	int hs, vs;
};

} // namespace beauty
