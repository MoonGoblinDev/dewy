/*
 * Dewy dock: the control panel for the Dewy filter.
 * Edits go straight into the filter's settings (obs_source_update), and the dock listens to
 * the filter's "update" signal so changes made elsewhere (Filters window, websocket) show up.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include <QtCore/QPointer>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtWidgets/QApplication>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QMenu>
#include <QtWidgets/QScrollBar>
#include <QtCore/QVariantAnimation>
#include <QtCore/QDateTime>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QVBoxLayout>

#include <climits>
#include <map>
#include <string>
#include <vector>

#include "widgets.hpp"
#include "http.hpp"
#include "thumbs.hpp"
#include "beauty-api.h"
#include "presets.h"
#include <obs-websocket-api.h>

using namespace beauty;

static const char *FILTER_ID = "obs_beauty_filter";

/* Dock pages, in tab order. */
enum DockPage { PG_PRESETS = 0, PG_ENHANCE, PG_EFFECTS, PG_ATMO, PG_FILTERS, PG_LIGHT, PG_COUNT };
static const char *PAGE_NAMES[PG_COUNT] = {"presets", "enhance", "effects", "atmosphere", "filters", "light"};
static const char *PAGE_SWITCH[PG_COUNT] = {nullptr, "enhance_on", "effects_on", "atmo_on", "filters_on", "light_on"};

/* Settings a preset never touches: per-source options and debugging. */
static bool look_key(const char *k)
{
	static const char *skip[] = {"all_skin", "show_landmarks", "sync_tracking", "debug_view", "dewy_look"};
	for (const char *x : skip)
		if (strcmp(k, x) == 0)
			return false;
	return true;
}

/* ---------------- model (keys/defaults must match beauty-filter.c) ---------------- */

struct Opt {
	const char *key;
	const char *name;
	IconSpec icon;
	bool bipolar;
	int def;
};

struct FeatureGroup {
	const char *title;
	IconSpec icon;
	std::vector<Opt> opts;
};

static const std::vector<FeatureGroup> &categories()
{
	static const std::vector<FeatureGroup> cats = {
		{"Face",
		 {Base::Face},
		 {
			 {"face_natural", "Natural", {Base::Face, Mod::Sparkle}, false, 0},
			 {"face_slim", "Slim", {Base::Face, Mod::In}, false, 0},
			 {"face_v", "V-shape", {Base::VFace}, false, 0},
			 {"face_narrow", "Narrow", {Base::Face, Mod::InTop}, false, 0},
			 {"face_small", "Small face", {Base::Face, Mod::Shrink}, false, 0},
			 {"cheekbone", "Cheekbones", {Base::Face, Mod::Cheek}, false, 0},
			 {"jawbone", "Jawbone", {Base::Face, Mod::InBottom}, false, 0},
			 {"chin", "Chin", {Base::Face, Mod::UpDownBottom}, true, 0},
			 {"forehead", "Forehead", {Base::Face, Mod::UpDownTop}, true, 0},
			 {"head", "Head size", {Base::Face, Mod::HeadRing}, false, 0},
		 }},
		{"Eyes",
		 {Base::Eye},
		 {
			 {"eye_size", "Enlarge", {Base::Eye, Mod::Out}, false, 0},
			 {"eye_spacing", "Distance", {Base::Eye, Mod::In}, true, 0},
			 {"eye_position", "Position", {Base::Eye, Mod::UpDown}, true, 0},
			 {"eye_tilt", "Tilt", {Base::Eye, Mod::Rotate}, true, 0},
			 {"eye_inner", "Inner corner", {Base::Eye, Mod::InnerCorner}, false, 0},
			 {"eye_outer", "Outer corner", {Base::Eye, Mod::OuterCorner}, true, 0},
			 {"eye_lower_lid", "Lower eyelid", {Base::Eye, Mod::Down}, false, 0},
			 {"eye_pupil", "Iris size", {Base::Eye, Mod::Iris}, false, 0},
			 {"eyelight", "Bright eyes", {Base::Eye, Mod::Sparkle}, false, 30},
		 }},
		{"Nose",
		 {Base::Nose},
		 {
			 {"nose_narrow", "Narrow", {Base::Nose, Mod::In}, false, 0},
			 {"nose_length", "Lift", {Base::Nose, Mod::UpDown}, true, 0},
			 {"nose_bridge", "Bridge", {Base::Nose, Mod::InTop}, false, 0},
			 {"nose_tip", "Tip", {Base::Nose, Mod::Tip}, false, 0},
		 }},
		{"Mouth",
		 {Base::Mouth},
		 {
			 {"mouth_size", "Size", {Base::Mouth, Mod::Out}, true, 0},
			 {"mouth_position", "Position", {Base::Mouth, Mod::UpDown}, true, 0},
			 {"mouth_smile", "Smile", {Base::Mouth, Mod::Smile}, false, 0},
		 }},
		{"Eyebrows",
		 {Base::Brow},
		 {
			 {"brow_thickness", "Thickness", {Base::Brow, Mod::UpDown}, true, 0},
			 {"brow_position", "Position", {Base::Brow, Mod::UpDownTop}, true, 0},
		 }},
	};
	return cats;
}

static const Opt SMOOTH{"smooth", "Smooth", {Base::Drop}, false, 50};
static const Opt FOUNDATION{"foundation", "Foundation", {Base::Compact}, false, 20};
static const Opt SKIN_TONE{"skin_tone", "Skin tone", {Base::SkinTone}, true, 0};
static const Opt REDNESS{"redness", "Redness", {Base::Redness}, false, 0};
static const std::vector<Opt> &tailSliders()
{
	static const std::vector<Opt> v = {
		{"shadows", "Shadows", {Base::UnderEye}, false, 0},
		{"teeth", "Brighten teeth", {Base::Tooth}, false, 0},
		{"contrast", "Contrast", {Base::Contrast}, false, 0},
		{"degree", "Smile lines", {Base::Fold}, false, 0},
	};
	return v;
}

/* ---------------- the dock ---------------- */

class BeautyDock : public QWidget {
public:
	BeautyDock();
	~BeautyDock() override;

	void bindToSelection();
	void unbind();
	void refresh();
	void frontendEvent(enum obs_frontend_event e);
	void showTab(int i)
	{
		topTabs->setCurrent(i);
		pages->setCurrentIndex(i);
		updateTabSwitch();
		refreshThumbs();
	}
	void showEnhanceSub(int i)
	{
		subTabs[i]->setChecked(true);
		enhanceStack->setCurrentIndex(i);
	}
	void setCustomOpen(bool open)
	{
		customPanel->setVisible(open);
		refresh();
	}
	void showAtmoSub(int i)
	{
		if (i < 0 || i >= (int)atmoTabs.size())
			return;
		atmoTabs[i]->setChecked(true);
		atmoStack->setCurrentIndex(i);
	}
	/* Automation (obs-websocket vendor requests): bring a control into view.
	 * Targets: a slider key ("smooth", "face_slim"), "look:<name>", or "<setting>:<preset name>"
	 * ("effect_preset:Kitty", "atmo_preset:Sparkles", "filter_preset:Fresh", "light_preset:Neon"). */
	bool hasFocusTarget(const std::string &t) const { return focusers.count(t) != 0; }
	void focusTarget(const std::string &t)
	{
		auto it = focusers.find(t);
		if (it != focusers.end())
			it->second();
	}

private:
	/* filter access */
	obs_source_t *filter = nullptr; /* strong ref while bound */
	obs_source_t *parentSrc = nullptr;
	long long getInt(const char *key);
	bool getBool(const char *key);
	std::string getString(const char *key);
	void setInt(const char *key, long long v);
	void setBool(const char *key, bool v);
	void setString(const char *key, const char *v);

	/* ui */
	QWidget *buildHeader();
	QWidget *buildAppearance();
	QWidget *buildMakeup();
	QWidget *buildEnhance();
	QWidget *buildEffects();
	QWidget *buildAtmosphere();
	QWidget *buildFilters();
	QWidget *buildLight();
	QWidget *intensityCard(const char *label, const char *key, int def);
	QWidget *presetSections(int page, obs_data_t *root, const char *itemsKey, const char *settingKey,
				QImage (*gen)(const FaceCrop &, obs_data_t *), const char *amtLabel, const char *amtKey,
				int amtDef);
	PresetGrid *simpleGrid(int page, obs_data_t *root, const char *itemsKey, const char *settingKey,
			       QImage (*gen)(const FaceCrop &, obs_data_t *), QWidget *panel);
	QWidget *chipRow(const std::vector<QString> &names, std::function<void(int)> onPick, int checked = 0);
	void refreshThumbs();
	const char *tabSwitchKey() const
	{
		int i = pages ? pages->currentIndex() : PG_ENHANCE;
		return PAGE_SWITCH[i < 0 || i >= PG_COUNT ? PG_ENHANCE : i];
	}
	/* header switch shows the current tab's state; switched-off tabs are greyed out */
	void updateTabSwitch()
	{
		if (!filter)
			return;
		const char *k = tabSwitchKey();
		enableToggle->setVisible(k != nullptr); /* Presets has no on/off of its own */
		if (k)
			enableToggle->setChecked(getBool(k));
		if (resetBtn)
			resetBtn->setToolTip(k ? QStringLiteral("Reset this tab") : QStringLiteral("Reset every tab"));
		for (int i = 0; i < PG_COUNT && i < (int)pageScrolls.size(); i++)
			if (QWidget *w = pageScrolls[i]->widget())
				w->setEnabled(!PAGE_SWITCH[i] || getBool(PAGE_SWITCH[i]));
	}

	struct ThumbJob {
		PresetGrid *grid;
		int page;
		std::vector<obs_data_t *> presets; /* owned refs; nullptr = the "None" tile */
		QImage (*gen)(const FaceCrop &, obs_data_t *);
	};
	std::vector<ThumbJob> thumbJobs;
	TopTabs *topTabs = nullptr;
	std::vector<QPushButton *> subTabs;
	std::vector<QPushButton *> atmoTabs;
	QStackedWidget *atmoStack = nullptr;
	std::map<std::string, std::function<void()>> focusers;
	std::map<std::string, QPointer<QWidget>> locators; /* automation: where a control is on screen */
	QPushButton *resetBtn = nullptr, *saveBtn = nullptr;
	/* update notice */
	QFrame *updateBanner = nullptr;
	QLabel *updateLabel = nullptr;
	QString updateVersion, updateUrl;
	void checkForUpdates();
	void showUpdateIfNewer();
	/* presets */
	obs_data_array_t *myLooks = nullptr;
	PresetGrid *myGrid = nullptr;
	std::vector<PresetGrid *> lookGrids;
	QWidget *buildPresets();
	void applyLook(obs_data_t *settings, bool builtin, const QString &tag);
	void saveCurrentLook();
	void loadMyLooks();
	void storeMyLooks();
	void rebuildMyLooks();
	void refreshMyThumbs();
	/* scroll a control into view, animated */
	void reveal(int page, QWidget *w)
	{
		QPointer<QWidget> pw(w);
		QTimer::singleShot(30, this, [this, page, pw]() {
			if (!pw)
				return;
			QScrollArea *sa = pageScrolls[page];
			QWidget *content = sa->widget();
			if (!content || !content->isAncestorOf(pw))
				return;
			QScrollBar *bar = sa->verticalScrollBar();
			int top = pw->mapTo(content, QPoint(0, 0)).y(), h = pw->height(), view = sa->viewport()->height();
			int cur = bar->value(), target = cur;
			const int margin = 70;
			if (top - margin < cur)
				target = top - margin;
			else if (top + h + margin > cur + view)
				target = top + h + margin - view;
			target = std::clamp(target, bar->minimum(), bar->maximum());
			if (target == cur)
				return;
			auto *anim = new QVariantAnimation(sa);
			anim->setStartValue(cur);
			anim->setEndValue(target);
			anim->setDuration(450);
			anim->setEasingCurve(QEasingCurve::InOutCubic);
			QObject::connect(anim, &QVariantAnimation::valueChanged, bar,
					 [bar](const QVariant &v) { bar->setValue(v.toInt()); });
			anim->start(QAbstractAnimation::DeleteWhenStopped);
		});
	}

public:
	/* Automation: apply "preset:<name>" (built-in) or "mypreset:<name>"; save the current look. */
	bool applyNamed(const std::string &t)
	{
		if (t.rfind("preset:", 0) == 0) {
			obs_data_t *look = presets_find_in(presets_looks_root(), "looks", t.c_str() + 7);
			if (!look)
				return false;
			obs_data_t *st = obs_data_get_obj(look, "settings");
			applyLook(st, true, QStringLiteral("b:") + QString::fromStdString(t.substr(7)));
			obs_data_release(st);
			obs_data_release(look);
			return true;
		}
		if (t.rfind("mypreset:", 0) == 0 && myGrid && myGrid->tileNamed(QString::fromStdString(t.substr(9)))) {
			myGrid->onPicked(QString::fromStdString(t.substr(9)));
			return true;
		}
		return false;
	}
	void saveLook() { saveCurrentLook(); }
	void scrollTabs(bool right) { topTabs->scrollStep(right); }
	void resetTab() { resetCurrentTab(); }
	/* Automation: global (screen) rectangle of a control, and the knob of a slider. */
	bool locate(const std::string &t, QRect &r, QPointF &knob, bool &hasKnob, double &shown, int value = INT_MIN,
		    int *range = nullptr)
	{
		auto it = locators.find(t);
		if (it == locators.end() || !it->second || !it->second->isVisible())
			return false;
		QWidget *w = it->second;
		r = QRect(w->mapToGlobal(QPoint(0, 0)), w->size());
		/* how much of it is actually on screen (scroll areas clip it) */
		QRect vis = w->visibleRegion().boundingRect();
		shown = w->width() * w->height() > 0 ? (double)(vis.width() * vis.height()) / (w->width() * w->height()) : 0.0;
		hasKnob = false;
		if (auto *sl = dynamic_cast<TTSlider *>(w)) {
			QPointF k = value == INT_MIN ? sl->knobPos() : sl->knobAt(value);
			knob = QPointF(w->mapToGlobal(QPoint(0, 0))) + k;
			hasKnob = true;
			if (range) {
				range[0] = sl->minimum();
				range[1] = sl->maximum();
			}
		}
		return true;
	}

private:
	QStackedWidget *enhanceStack = nullptr;
	std::vector<QScrollArea *> pageScrolls;
	void renderThumbs(int page);
	void waitForSnapshot(int page, uint32_t s0, int tries);
	QWidget *sectionTitle(const QString &text, IconSpec icon, QLabel **detail = nullptr);
	QWidget *sliderSection(const Opt &o);
	QWidget *categorySection(const FeatureGroup &c);
	void loadLooks();
	void selectSourceByName(const QString &name);
	void refillSources();
	void resetCurrentTab();
	void saveConfig();
	std::string loadConfig();
	void introduceOnce();
	void snapshotIfRequested();

	QComboBox *sourceBox = nullptr;
	Toggle *enableToggle = nullptr;
	QStackedWidget *pages = nullptr;
	QWidget *content = nullptr;
	QWidget *emptyState = nullptr;
	QLabel *emptyLabel = nullptr;
	QPushButton *addButton = nullptr;
	std::vector<QPushButton *> tabs;
	std::vector<std::function<void()>> refreshers;
	std::vector<LookTile *> lookTiles;
	QLabel *lookName = nullptr;
	QWidget *customPanel = nullptr;
	bool refreshing = false;
	bool refreshQueued = false;
};

/* ---------------- obs callbacks ---------------- */

static void on_filter_changed(void *data, calldata_t *)
{
	QPointer<BeautyDock> dock = static_cast<BeautyDock *>(data);
	QMetaObject::invokeMethod(
		dock.data(),
		[dock]() {
			if (dock)
				dock->refresh();
		},
		Qt::QueuedConnection);
}

static void on_parent_filters_changed(void *data, calldata_t *)
{
	QPointer<BeautyDock> dock = static_cast<BeautyDock *>(data);
	QMetaObject::invokeMethod(
		dock.data(),
		[dock]() {
			if (dock)
				dock->bindToSelection();
		},
		Qt::QueuedConnection);
}

static void frontend_cb(enum obs_frontend_event e, void *data)
{
	static_cast<BeautyDock *>(data)->frontendEvent(e);
}

/* ---------------- settings helpers ---------------- */

long long BeautyDock::getInt(const char *key)
{
	if (!filter)
		return 0;
	obs_data_t *s = obs_source_get_settings(filter);
	long long v = obs_data_get_int(s, key);
	obs_data_release(s);
	return v;
}

bool BeautyDock::getBool(const char *key)
{
	if (!filter)
		return false;
	obs_data_t *s = obs_source_get_settings(filter);
	bool v = obs_data_get_bool(s, key);
	obs_data_release(s);
	return v;
}

std::string BeautyDock::getString(const char *key)
{
	if (!filter)
		return {};
	obs_data_t *s = obs_source_get_settings(filter);
	std::string v = obs_data_get_string(s, key);
	obs_data_release(s);
	return v;
}

void BeautyDock::setInt(const char *key, long long v)
{
	if (!filter || refreshing)
		return;
	obs_data_t *d = obs_data_create();
	obs_data_set_int(d, key, v);
	obs_source_update(filter, d);
	obs_data_release(d);
}

void BeautyDock::setBool(const char *key, bool v)
{
	if (!filter || refreshing)
		return;
	obs_data_t *d = obs_data_create();
	obs_data_set_bool(d, key, v);
	obs_source_update(filter, d);
	obs_data_release(d);
}

void BeautyDock::setString(const char *key, const char *v)
{
	if (!filter || refreshing)
		return;
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, key, v);
	obs_source_update(filter, d);
	obs_data_release(d);
}

/* ---------------- construction ---------------- */

static const char *STYLE = R"(
#BeautyDock { background: #161619; }
#BeautyDock QLabel { color: #eeeef1; }
#BeautyDock QLabel[role="sub"] { color: #8d8d96; font-size: 12px; }
#BeautyDock QLabel[role="title"] { font-size: 14px; font-weight: 500; }
#BeautyDock QScrollArea, #BeautyDock QScrollArea > QWidget > QWidget { background: transparent; border: none; }
#BeautyDock QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }
#BeautyDock QScrollBar::handle:vertical { background: #3a3a40; border-radius: 3px; min-height: 30px; }
#BeautyDock QScrollBar::add-line, #BeautyDock QScrollBar::sub-line { height: 0; }
#BeautyDock QComboBox { background: #212125; color: #eeeef1; border: 1px solid #2f2f35; border-radius: 8px;
  padding: 5px 10px; min-height: 18px; }
#BeautyDock QComboBox:hover { border-color: #45454c; }
#BeautyDock QComboBox QAbstractItemView { background: #212125; color: #eeeef1; selection-background-color: #36363c; }
#BeautyDock QPushButton[role="tab"] { background: transparent; color: #8d8d96; border: none; border-radius: 9px;
  padding: 7px 14px; font-size: 13px; font-weight: 600; }
#BeautyDock QPushButton[role="tab"]:hover { color: #c8c8d0; }
#BeautyDock QPushButton[role="tab"]:checked { background: #2b2b30; color: #ffffff; }
#BeautyDock QPushButton[role="toptab"] { background: transparent; color: #8d8d96; border: none;
  border-bottom: 2px solid transparent; padding: 8px 8px; font-size: 14px; font-weight: 600; }
#BeautyDock QPushButton[role="toptab"]:hover { color: #c8c8d0; }
#BeautyDock QPushButton[role="toptab"]:checked { color: #ffffff; border-bottom: 2px solid #ffffff; }
#BeautyDock QPushButton[role="primary"] { background: #fe2c55; color: white; border: none; border-radius: 9px;
  padding: 9px 16px; font-weight: 600; }
#BeautyDock QPushButton[role="primary"]:hover { background: #ff4a6e; }
#BeautyDock QPushButton[role="chip"] { background: #2b2b30; color: #c8c8d0; border: none; border-radius: 8px;
  padding: 5px 10px; font-size: 12px; }
#BeautyDock QPushButton[role="chip"]:checked { background: #eeeef1; color: #161619; }
#BeautyDock QFrame[role="card"] { background: #212125; border-radius: 12px; }
#BeautyDock QFrame[role="hint"] { background: #232328; border-radius: 10px; }
#BeautyDock QCheckBox { color: #c8c8d0; spacing: 8px; }
#BeautyDock QCheckBox::indicator { width: 16px; height: 16px; border-radius: 4px; border: 1px solid #45454c;
  background: #212125; }
#BeautyDock QCheckBox::indicator:checked { background: #2f7cf6; border-color: #2f7cf6; }
)";

BeautyDock::BeautyDock()
{
	setObjectName("BeautyDock");
	setAttribute(Qt::WA_StyledBackground, true);
	setStyleSheet(STYLE);
	setMinimumWidth(300);

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(0);
	root->addWidget(buildHeader());
	QTimer::singleShot(6000, this, [this]() { checkForUpdates(); });

	content = new QWidget(this);
	auto *cl = new QVBoxLayout(content);
	cl->setContentsMargins(0, 0, 0, 0);
	pages = new QStackedWidget(content);
	for (int i = 0; i < PG_COUNT; i++) {
		auto *scroll = new QScrollArea(pages);
		scroll->setWidgetResizable(true);
		scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		scroll->setFrameShape(QFrame::NoFrame);
		pageScrolls.push_back(scroll);
		pages->addWidget(scroll);
	}
	pageScrolls[PG_PRESETS]->setWidget(buildPresets());
	pageScrolls[PG_ENHANCE]->setWidget(buildEnhance());
	pageScrolls[PG_EFFECTS]->setWidget(buildEffects());
	pageScrolls[PG_ATMO]->setWidget(buildAtmosphere());
	pageScrolls[PG_FILTERS]->setWidget(buildFilters());
	pageScrolls[PG_LIGHT]->setWidget(buildLight());
	cl->addWidget(pages);
	root->addWidget(content, 1);

	/* shown when the selected source has no Beauty filter yet */
	emptyState = new QWidget(this);
	auto *el = new QVBoxLayout(emptyState);
	el->setContentsMargins(24, 40, 24, 24);
	el->setSpacing(14);
	auto *art = new QLabel(emptyState);
	QPixmap pm(120, 120);
	pm.fill(Qt::transparent);
	{
		QPainter p(&pm);
		LookArt a;
		a.name = "x";
		a.lipOpacity = 0.4;
		a.lip = QColor(0xe0, 0x56, 0x6e);
		a.blushOpacity = 0.25;
		a.highlight = 0.3;
		a.exposure = 0.1;
		paintLook(p, QRectF(10, 10, 100, 100), a);
	}
	art->setPixmap(pm);
	art->setAlignment(Qt::AlignHCenter);
	emptyLabel = new QLabel(emptyState);
	emptyLabel->setWordWrap(true);
	emptyLabel->setAlignment(Qt::AlignHCenter);
	emptyLabel->setProperty("role", "sub");
	addButton = new QPushButton(QStringLiteral("Add Dewy to this source"), emptyState);
	addButton->setProperty("role", "primary");
	addButton->setCursor(Qt::PointingHandCursor);
	el->addWidget(art);
	el->addWidget(emptyLabel);
	el->addWidget(addButton, 0, Qt::AlignHCenter);
	el->addStretch(1);
	root->addWidget(emptyState, 1);

	QObject::connect(addButton, &QPushButton::clicked, this, [this]() {
		QString name = sourceBox->currentText();
		obs_source_t *src = obs_get_source_by_name(name.toUtf8().constData());
		if (!src)
			return;
		obs_source_t *f = obs_source_create(FILTER_ID, obs_module_text("Dewy"), nullptr, nullptr);
		obs_source_filter_add(src, f);
		obs_source_release(f);
		obs_source_release(src);
		bindToSelection();
	});

	obs_frontend_add_event_callback(frontend_cb, this);
	QTimer::singleShot(0, this, [this]() {
		refillSources();
		std::string saved = loadConfig();
		if (!saved.empty())
			selectSourceByName(QString::fromStdString(saved));
		bindToSelection();
	});
}

BeautyDock::~BeautyDock()
{
	obs_frontend_remove_event_callback(frontend_cb, this);
	unbind();
	for (auto &j : thumbJobs)
		for (obs_data_t *d : j.presets)
			obs_data_release(d);
	obs_data_array_release(myLooks);
}

/* ---------------- update notice ---------------- */

/* plugin_config/dewy/update.json: {enabled, last_check, latest, url, dismissed} */
static obs_data_t *loadUpdateConfig()
{
	char *path = obs_module_config_path("update.json");
	obs_data_t *d = path ? obs_data_create_from_json_file(path) : nullptr;
	bfree(path);
	if (!d)
		d = obs_data_create();
	obs_data_set_default_bool(d, "enabled", true);
	return d;
}

static void saveUpdateConfig(obs_data_t *d)
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path("update.json");
	if (path)
		obs_data_save_json_safe(d, path, "tmp", "bak");
	bfree(path);
}

/* "v0.10.2" > "0.9" etc. */
static bool versionNewer(const QString &a, const QString &b)
{
	auto parts = [](QString s) {
		if (s.startsWith('v') || s.startsWith('V'))
			s.remove(0, 1);
		std::vector<int> v;
		for (const QString &p : s.split('.'))
			v.push_back(p.toInt());
		while (v.size() < 3)
			v.push_back(0);
		return v;
	};
	return parts(a) > parts(b);
}

void BeautyDock::showUpdateIfNewer()
{
	obs_data_t *c = loadUpdateConfig();
	QString latest = QString::fromUtf8(obs_data_get_string(c, "latest"));
	QString dismissed = QString::fromUtf8(obs_data_get_string(c, "dismissed"));
	bool on = obs_data_get_bool(c, "enabled");
	updateUrl = QString::fromUtf8(obs_data_get_string(c, "url"));
	obs_data_release(c);
	updateVersion = latest.startsWith('v') ? latest.mid(1) : latest;
	bool show = on && !latest.isEmpty() && versionNewer(latest, QStringLiteral(PLUGIN_VERSION)) &&
		    latest != dismissed && !updateUrl.isEmpty();
	if (show)
		updateLabel->setText(QStringLiteral("Dewy %1 is available").arg(updateVersion));
	updateBanner->setVisible(show);
}

/* Once a day, ask GitHub which release is the latest: github.com/<repo>/releases/latest redirects to
 * .../releases/tag/vX.Y.Z (published releases only; drafts never count). One anonymous HEAD request
 * through the OS's HTTPS client; nothing about the user is sent, and unlike the REST API it isn't
 * rate limited per IP. DEWY_UPDATE_URL overrides the address (testing). */
void BeautyDock::checkForUpdates()
{
	showUpdateIfNewer(); /* what we already know */
	obs_data_t *c = loadUpdateConfig();
	bool on = obs_data_get_bool(c, "enabled");
	long long last = obs_data_get_int(c, "last_check");
	obs_data_release(c);
	long long now = QDateTime::currentSecsSinceEpoch();
	const char *override = getenv("DEWY_UPDATE_URL");
	if (!on || (!override && now - last < 24 * 3600))
		return;
	std::string url = override ? override : "https://github.com/MoonGoblinDev/dewy/releases/latest";
	QPointer<BeautyDock> self(this);
	httpResolve(url, "Dewy/" PLUGIN_VERSION, [self, now](int status, std::string finalUrl) {
		QMetaObject::invokeMethod(qApp, [self, now, status, finalUrl]() {
			if (!self)
				return;
			if (status == 0) { /* offline: try again next time OBS starts */
				blog(LOG_INFO, "[dewy] update check: no connection");
				return;
			}
			obs_data_t *c = loadUpdateConfig();
			obs_data_set_int(c, "last_check", now);
			QString u = QString::fromStdString(finalUrl);
			int at = u.indexOf(QStringLiteral("/releases/tag/"));
			if (status == 200 && at >= 0) {
				QString tag = u.mid(at + 14).section('/', 0, 0).section('?', 0, 0);
				obs_data_set_string(c, "latest", tag.toUtf8().constData());
				obs_data_set_string(c, "url", finalUrl.c_str());
				blog(LOG_INFO, "[dewy] latest release: %s (this is %s)", tag.toUtf8().constData(),
				     PLUGIN_VERSION);
			}
			saveUpdateConfig(c);
			obs_data_release(c);
			self->showUpdateIfNewer();
		}, Qt::QueuedConnection);
	});
}

QWidget *BeautyDock::buildHeader()
{
	auto *w = new QWidget(this);
	auto *v = new QVBoxLayout(w);
	v->setContentsMargins(14, 12, 14, 8);
	v->setSpacing(10);

	auto *row = new QHBoxLayout();
	row->setSpacing(8);
	auto *title = new QLabel(QStringLiteral("Dewy"), w);
	title->setStyleSheet("font-size:17px; font-weight:700;");
	if (char *logo = obs_module_file("brand/dewy-wordmark-dark@2x.png")) {
		QPixmap pm(QString::fromUtf8(logo));
		if (!pm.isNull()) {
			pm = pm.scaledToHeight(68, Qt::SmoothTransformation);
			pm.setDevicePixelRatio(2.0);
			title->setPixmap(pm);
			title->setToolTip(QStringLiteral("Dewy"));
		}
		bfree(logo);
	}
	row->addWidget(title);
	row->addStretch(1);

	/* Reset: a labelled pill so it reads as a button */
	QPixmap ico(32, 32);
	ico.fill(Qt::transparent);
	{
		QPainter p(&ico);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(QPen(QColor(0xee, 0xee, 0xf1), 3.0, Qt::SolidLine, Qt::RoundCap));
		QRectF a(6, 6, 20, 20);
		p.drawArc(a, 100 * 16, 290 * 16);
		QPointF tip(a.center().x() - a.width() * 0.17, a.top());
		p.drawLine(tip, tip + QPointF(6.5, -4.5));
		p.drawLine(tip, tip + QPointF(6.5, 4.8));
	}
	ico.setDevicePixelRatio(2.0);
	resetBtn = new QPushButton(QIcon(ico), QStringLiteral("Reset"), w);
	resetBtn->setIconSize(QSize(16, 16));
	resetBtn->setCursor(Qt::PointingHandCursor);
	resetBtn->setToolTip(QStringLiteral("Reset this tab"));
	resetBtn->setStyleSheet(
		"QPushButton { color:#eeeef1; background:#26262b; border:1px solid #3a3a42; border-radius:13px;"
		" padding:4px 12px 4px 9px; font-size:12px; font-weight:600; }"
		"QPushButton:hover { background:#303036; border-color:#55555e; }"
		"QPushButton:pressed { background:#3a3a42; }");
	QObject::connect(resetBtn, &QPushButton::clicked, this, [this]() { resetCurrentTab(); });
	locators["reset"] = resetBtn;
	row->addWidget(resetBtn);

	/* "Dewy x.y.z is available" (hidden until an update check finds one) */
	updateBanner = new QFrame(w);
	updateBanner->setObjectName("UpdateBanner");
	updateBanner->setStyleSheet("#UpdateBanner { background:#2a1d24; border:1px solid #5a3446; border-radius:10px; }");
	{
		auto *ul = new QHBoxLayout(updateBanner);
		ul->setContentsMargins(12, 7, 6, 7);
		ul->setSpacing(8);
		updateLabel = new QLabel(updateBanner);
		updateLabel->setStyleSheet("color:#ffd0df; font-size:12px; font-weight:600;");
		ul->addWidget(updateLabel, 1);
		auto *dl = new QPushButton(QStringLiteral("Download"), updateBanner);
		dl->setCursor(Qt::PointingHandCursor);
		dl->setStyleSheet("QPushButton { color:#1e0e16; background:#ff96b8; border:none; border-radius:11px;"
				  " padding:3px 11px; font-size:12px; font-weight:700; }"
				  "QPushButton:hover { background:#ffaac6; }");
		QObject::connect(dl, &QPushButton::clicked, this,
				 [this]() { QDesktopServices::openUrl(QUrl(updateUrl)); });
		ul->addWidget(dl);
		auto *x = new QPushButton(QStringLiteral("✕"), updateBanner);
		x->setCursor(Qt::PointingHandCursor);
		x->setToolTip(QStringLiteral("Don't remind me about this version"));
		x->setStyleSheet("QPushButton { color:#c9a3b3; background:transparent; border:none; font-size:13px;"
				 " padding:2px 6px; } QPushButton:hover { color:#ffffff; }");
		QObject::connect(x, &QPushButton::clicked, this, [this]() {
			obs_data_t *c = loadUpdateConfig();
			obs_data_set_string(c, "dismissed", updateVersion.toUtf8().constData());
			saveUpdateConfig(c);
			obs_data_release(c);
			updateBanner->hide();
		});
		ul->addWidget(x);
	}
	updateBanner->hide();

	enableToggle = new Toggle(w);
	enableToggle->setToolTip(QStringLiteral("Turn this tab on / off"));
	enableToggle->onToggled = [this](bool on) {
		if (const char *k = tabSwitchKey())
			setBool(k, on);
		updateTabSwitch();
	};
	row->addWidget(enableToggle);
	v->addLayout(row);
	v->addWidget(updateBanner);

	sourceBox = new QComboBox(w);
	sourceBox->setToolTip(QStringLiteral("Camera / source to beautify"));
	QObject::connect(sourceBox, &QComboBox::activated, this, [this](int) {
		saveConfig();
		bindToSelection();
	});
	v->addWidget(sourceBox);

	topTabs = new TopTabs(w);
	topTabs->addTab(QStringLiteral("Presets"), {Base::Palette});
	topTabs->addTab(QStringLiteral("Enhance"), {Base::Wand});
	topTabs->addTab(QStringLiteral("Effects"), {Base::Smiley});
	topTabs->addTab(QStringLiteral("Atmosphere"), {Base::Ambience});
	topTabs->addTab(QStringLiteral("Filters"), {Base::Filters});
	topTabs->addTab(QStringLiteral("Light"), {Base::Bulb});
	topTabs->onChanged = [this](int i) {
		pages->setCurrentIndex(i);
		updateTabSwitch();
		refreshThumbs();
	};
	for (int i = 0; i < PG_COUNT; i++)
		locators[std::string("tab:") + PAGE_NAMES[i]] = topTabs->button(i);
	locators["tabs:next"] = topTabs->chevron(true);
	locators["tabs:prev"] = topTabs->chevron(false);
	v->addWidget(topTabs);

	auto *sep = new QFrame(w);
	sep->setFixedHeight(1);
	sep->setStyleSheet("background:#26262b;");
	v->addWidget(sep);
	return w;
}

QWidget *BeautyDock::sectionTitle(const QString &text, IconSpec icon, QLabel **detail)
{
	auto *w = new QWidget();
	auto *l = new QHBoxLayout(w);
	l->setContentsMargins(0, 0, 0, 0);
	l->setSpacing(8);
	auto *ic = new QLabel(w);
	QPixmap pm(40, 40);
	pm.setDevicePixelRatio(2.0);
	pm.fill(Qt::transparent);
	{
		QPainter p(&pm);
		paintIcon(p, QRectF(1, 1, 18, 18), icon, theme::text);
	}
	ic->setPixmap(pm);
	ic->setFixedSize(20, 20);
	auto *t = new QLabel(text, w);
	t->setProperty("role", "title");
	l->addWidget(ic);
	l->addWidget(t);
	if (detail) {
		*detail = new QLabel(w);
		(*detail)->setProperty("role", "sub");
		l->addWidget(*detail);
	}
	l->addStretch(1);
	return w;
}

QWidget *BeautyDock::sliderSection(const Opt &o)
{
	auto *w = new QWidget();
	auto *v = new QVBoxLayout(w);
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(6);
	v->addWidget(sectionTitle(QString::fromUtf8(o.name), o.icon));
	auto *row = new SliderRow(o.bipolar ? -100 : 0, 100, o.def, w);
	const char *key = o.key;
	row->onChanged = [this, key](int val) { setInt(key, val); };
	refreshers.push_back([this, row, key]() { row->setValue((int)getInt(key)); });
	v->addWidget(row);
	focusers[key] = [this, w]() {
		showTab(PG_ENHANCE);
		showEnhanceSub(0);
		reveal(PG_ENHANCE, w);
	};
	locators[std::string("slider:") + key] = row->slider;
	return w;
}

QWidget *BeautyDock::categorySection(const FeatureGroup &c)
{
	auto *w = new QWidget();
	auto *v = new QVBoxLayout(w);
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(8);
	QLabel *detail = nullptr;
	v->addWidget(sectionTitle(QString::fromUtf8(c.title), c.icon, &detail));

	auto *chips = new QWidget(w);
	auto *flow = new FlowLayout(chips, 6, 4);
	auto *row = new SliderRow(0, 100, 0, w);

	struct State {
		int selected = 0;
	};
	auto state = std::make_shared<State>();
	std::vector<CircleButton *> buttons;
	const std::vector<Opt> *opts = &c.opts;

	auto apply = [this, state, opts, row, detail](std::vector<CircleButton *> btns) {
		const Opt &o = (*opts)[state->selected];
		for (size_t i = 0; i < btns.size(); i++) {
			btns[i]->setSelected((int)i == state->selected);
			btns[i]->setModified(getInt((*opts)[i].key) != (*opts)[i].def);
		}
		detail->setText(QStringLiteral("·  ") + QString::fromUtf8(o.name));
		row->setRange(o.bipolar ? -100 : 0, 100, o.def);
		row->setValue((int)getInt(o.key));
	};

	for (size_t i = 0; i < c.opts.size(); i++) {
		auto *b = new CircleButton(c.opts[i].icon, QString::fromUtf8(c.opts[i].name), chips);
		flow->addWidget(b);
		buttons.push_back(b);
	}
	for (size_t i = 0; i < buttons.size(); i++) {
		buttons[i]->onClicked = [state, i, apply, buttons]() {
			state->selected = (int)i;
			apply(buttons);
		};
	}
	row->onChanged = [this, state, opts, buttons](int val) {
		const Opt &o = (*opts)[state->selected];
		setInt(o.key, val);
		buttons[state->selected]->setModified(val != o.def);
	};
	refreshers.push_back([apply, buttons]() { apply(buttons); });
	for (size_t i = 0; i < c.opts.size(); i++) {
		focusers[c.opts[i].key] = [this, w, state, i, apply, buttons]() {
			showTab(PG_ENHANCE);
			showEnhanceSub(0);
			state->selected = (int)i;
			apply(buttons);
			reveal(PG_ENHANCE, w);
		};
		locators[std::string("opt:") + c.opts[i].key] = buttons[i];
		focusers[std::string("opt:") + c.opts[i].key] = [this, b = buttons[i]]() { /* scroll only */
			showTab(PG_ENHANCE);
			showEnhanceSub(0);
			reveal(PG_ENHANCE, b);
		};
		locators[std::string("slider:") + c.opts[i].key] = row->slider;
	}

	v->addWidget(chips);
	v->addWidget(row);
	return w;
}

static QWidget *spacer(int h)
{
	auto *s = new QWidget();
	s->setFixedHeight(h);
	return s;
}

QWidget *BeautyDock::buildAppearance()
{
	auto *page = new QWidget();
	auto *v = new QVBoxLayout(page);
	v->setContentsMargins(14, 12, 14, 16);
	v->setSpacing(18);
	v->addWidget(sliderSection(SMOOTH));
	v->addWidget(sliderSection(FOUNDATION));
	v->addWidget(sliderSection(SKIN_TONE));
	v->addWidget(sliderSection(REDNESS));
	for (const FeatureGroup &c : categories())
		v->addWidget(categorySection(c));
	for (const Opt &o : tailSliders())
		v->addWidget(sliderSection(o));

	/* advanced */
	auto *adv = new QFrame(page);
	adv->setProperty("role", "card");
	auto *al = new QVBoxLayout(adv);
	al->setContentsMargins(12, 10, 12, 12);
	al->setSpacing(8);
	auto *at = new QLabel(QStringLiteral("Advanced"), adv);
	at->setProperty("role", "title");
	al->addWidget(at);
	struct B {
		const char *key, *text;
	};
	for (B b : {B{"all_skin", "Smooth all visible skin"}, B{"sync_tracking", "Lock effects to the face (+1-3 frames)"},
		    B{"show_landmarks", "Show face landmarks"}}) {
		auto *cb = new QCheckBox(QString::fromUtf8(b.text), adv);
		const char *key = b.key;
		QObject::connect(cb, &QCheckBox::toggled, this, [this, key](bool on) { setBool(key, on); });
		refreshers.push_back([this, cb, key]() {
			QSignalBlocker block(cb);
			cb->setChecked(getBool(key));
		});
		al->addWidget(cb);
	}
	{
		auto *cb = new QCheckBox(QStringLiteral("Check for updates (once a day)"), adv);
		cb->setToolTip(QStringLiteral("Asks GitHub for the latest Dewy release. Nothing else is sent."));
		obs_data_t *c = loadUpdateConfig();
		cb->setChecked(obs_data_get_bool(c, "enabled"));
		obs_data_release(c);
		QObject::connect(cb, &QCheckBox::toggled, this, [this](bool on) {
			obs_data_t *c = loadUpdateConfig();
			obs_data_set_bool(c, "enabled", on);
			saveUpdateConfig(c);
			obs_data_release(c);
			if (on)
				checkForUpdates();
			else if (updateBanner)
				updateBanner->hide();
		});
		al->addWidget(cb);
	}
	auto *dvRow = new QHBoxLayout();
	auto *dvl = new QLabel(QStringLiteral("Debug view"), adv);
	dvl->setProperty("role", "sub");
	auto *dv = new QComboBox(adv);
	dv->addItems({"Off", "Skin mask", "Face regions", "Makeup masks"});
	QObject::connect(dv, &QComboBox::activated, this, [this](int i) { setInt("debug_view", i); });
	refreshers.push_back([this, dv]() {
		QSignalBlocker block(dv);
		dv->setCurrentIndex((int)getInt("debug_view"));
	});
	dvRow->addWidget(dvl);
	dvRow->addWidget(dv, 1);
	al->addLayout(dvRow);
	v->addWidget(adv);
	v->addStretch(1);
	return page;
}

/* ---------------- makeup ---------------- */

static QColor obsColor(long long c)
{
	return QColor((int)(c & 0xff), (int)((c >> 8) & 0xff), (int)((c >> 16) & 0xff));
}

static long long toObsColor(const QColor &c)
{
	return 0xFF000000LL | ((long long)c.blue() << 16) | ((long long)c.green() << 8) | c.red();
}

static std::vector<LookArt> readLooks()
{
	std::vector<LookArt> looks;
	char *path = obs_module_file("makeup/presets.json");
	obs_data_t *root = path ? obs_data_create_from_json_file(path) : nullptr;
	bfree(path);
	if (!root)
		return looks;
	obs_data_array_t *arr = obs_data_get_array(root, "presets");
	for (size_t i = 0; arr && i < obs_data_array_count(arr); i++) {
		obs_data_t *p = obs_data_array_item(arr, i);
		obs_data_t *fl = obs_data_get_obj(p, "filter");
		obs_data_t *mk = obs_data_get_obj(p, "makeup");
		LookArt a;
		a.name = QString::fromUtf8(obs_data_get_string(p, "name"));
		a.men = a.name.contains("(men)");
		auto col = [&](const char *k, QColor def) {
			const char *s = mk ? obs_data_get_string(mk, k) : "";
			return (s && *s) ? QColor(QString::fromUtf8(s)) : def;
		};
		if (mk) {
			a.lip = col("lip_color", a.lip);
			a.lipOpacity = obs_data_get_double(mk, "lip_opacity");
			a.gloss = obs_data_get_double(mk, "lip_gloss");
			a.blush = col("blush_color", a.blush);
			a.blushOpacity = obs_data_get_double(mk, "blush_opacity");
			a.blushStyle = (int)obs_data_get_int(mk, "blush_style");
			a.shadow = col("shadow_color", a.shadow);
			a.shadowOpacity = obs_data_get_double(mk, "shadow_opacity");
			a.liner = obs_data_get_double(mk, "liner");
			a.contour = obs_data_get_double(mk, "contour");
			a.highlight = obs_data_get_double(mk, "highlight");
			a.brow = obs_data_get_double(mk, "brow");
		}
		if (fl) {
			a.exposure = obs_data_get_double(fl, "exposure");
			a.warmth = obs_data_get_double(fl, "warmth");
			a.tint = obs_data_get_double(fl, "tint");
			a.saturation = obs_data_get_double(fl, "saturation");
			a.fade = obs_data_get_double(fl, "fade");
		}
		looks.push_back(a);
		obs_data_release(mk);
		obs_data_release(fl);
		obs_data_release(p);
	}
	obs_data_array_release(arr);
	obs_data_release(root);
	return looks;
}

QWidget *BeautyDock::buildMakeup()
{
	auto *page = new QWidget();
	auto *v = new QVBoxLayout(page);
	v->setContentsMargins(14, 12, 14, 16);
	v->setSpacing(14);

	/* intensity card */
	auto *card = new QFrame(page);
	card->setProperty("role", "card");
	auto *cv = new QVBoxLayout(card);
	cv->setContentsMargins(14, 12, 14, 12);
	cv->setSpacing(6);
	lookName = new QLabel(card);
	lookName->setProperty("role", "title");
	cv->addWidget(lookName);
	struct K {
		const char *key, *label;
	};
	for (K k : {K{"filter_amt", "Filter"}, K{"makeup_amt", "Makeup"}}) {
		auto *lab = new QLabel(QString::fromUtf8(k.label), card);
		lab->setProperty("role", "sub");
		auto *row = new SliderRow(0, 100, 70, card);
		const char *key = k.key;
		row->onChanged = [this, key](int val) { setInt(key, val); };
		refreshers.push_back([this, row, key]() { row->setValue((int)getInt(key)); });
		cv->addWidget(lab);
		cv->addWidget(row);
	}
	v->addWidget(card);

	/* customise panel */
	customPanel = new QFrame(page);
	customPanel->setProperty("role", "card");
	auto *pv = new QVBoxLayout(customPanel);
	pv->setContentsMargins(14, 12, 14, 12);
	pv->setSpacing(8);
	auto *ct = new QLabel(QStringLiteral("Customise"), customPanel);
	ct->setProperty("role", "title");
	pv->addWidget(ct);

	auto colorRow = [this, pv](const char *label, IconSpec icon, const char *colorKey, const char *amountKey) {
		auto *head = new QHBoxLayout();
		auto *t = sectionTitle(QString::fromUtf8(label), icon);
		head->addWidget(t, 1);
		if (colorKey) {
			auto *sw = new Swatch(customPanel);
			sw->onPicked = [this, colorKey](QColor c) { setInt(colorKey, toObsColor(c)); };
			refreshers.push_back([this, sw, colorKey]() { sw->setColor(obsColor(getInt(colorKey))); });
			head->addWidget(sw);
		}
		pv->addLayout(head);
		auto *row = new SliderRow(0, 100, 0, customPanel);
		row->onChanged = [this, amountKey](int val) { setInt(amountKey, val); };
		refreshers.push_back([this, row, amountKey]() { row->setValue((int)getInt(amountKey)); });
		pv->addWidget(row);
	};
	colorRow("Lips", {Base::Lips}, "mk_lip_color", "mk_lip_opacity");
	colorRow("Gloss", {Base::Lips, Mod::Sparkle}, nullptr, "mk_lip_gloss");
	colorRow("Blush", {Base::Blush}, "mk_blush_color", "mk_blush_opacity");
	{
		auto *chips = new QHBoxLayout();
		chips->setSpacing(6);
		auto *grp = new QButtonGroup(customPanel);
		const char *styles[] = {"Cheeks", "Lifted", "Sun-kissed"};
		std::vector<QPushButton *> bs;
		for (int i = 0; i < 3; i++) {
			auto *b = new QPushButton(QString::fromUtf8(styles[i]), customPanel);
			b->setProperty("role", "chip");
			b->setCheckable(true);
			b->setCursor(Qt::PointingHandCursor);
			grp->addButton(b, i);
			chips->addWidget(b);
			bs.push_back(b);
		}
		chips->addStretch(1);
		QObject::connect(grp, &QButtonGroup::idClicked, this, [this](int id) { setInt("mk_blush_style", id); });
		refreshers.push_back([this, bs]() {
			int s = (int)getInt("mk_blush_style");
			for (int i = 0; i < 3; i++)
				bs[i]->setChecked(i == s);
		});
		pv->addLayout(chips);
	}
	colorRow("Eyeshadow", {Base::Shadow}, "mk_shadow_color", "mk_shadow_opacity");
	colorRow("Eyeliner", {Base::Liner}, nullptr, "mk_liner");
	colorRow("Contour", {Base::Sculpt}, nullptr, "mk_contour");
	colorRow("Highlight", {Base::Glow}, nullptr, "mk_highlight");
	colorRow("Brow tint", {Base::Brow}, nullptr, "mk_brow");

	auto *ft = new QLabel(QStringLiteral("Filter"), customPanel);
	ft->setProperty("role", "title");
	pv->addWidget(spacer(4));
	pv->addWidget(ft);
	struct G {
		const char *key, *label;
		bool bipolar;
	};
	for (G g : {G{"fl_exposure", "Brightness", true}, G{"fl_contrast", "Contrast", true},
		    G{"fl_saturation", "Saturation", true}, G{"fl_warmth", "Warmth", true}, G{"fl_tint", "Tint", true},
		    G{"fl_fade", "Fade", false}}) {
		auto *lab = new QLabel(QString::fromUtf8(g.label), customPanel);
		lab->setProperty("role", "sub");
		auto *row = new SliderRow(g.bipolar ? -100 : 0, 100, 0, customPanel);
		const char *key = g.key;
		row->onChanged = [this, key](int val) { setInt(key, val); };
		refreshers.push_back([this, row, key]() { row->setValue((int)getInt(key)); });
		pv->addWidget(lab);
		pv->addWidget(row);
	}
	auto *lutRow = new QHBoxLayout();
	auto *lutLabel = new QLabel(customPanel);
	lutLabel->setProperty("role", "sub");
	auto *lutBtn = new QPushButton(QStringLiteral("Load LUT…"), customPanel);
	lutBtn->setProperty("role", "chip");
	auto *lutClear = new QPushButton(QStringLiteral("Clear"), customPanel);
	lutClear->setProperty("role", "chip");
	QObject::connect(lutBtn, &QPushButton::clicked, this, [this]() {
		QString f = QFileDialog::getOpenFileName(this, QStringLiteral("Choose a .cube LUT"), QString(),
							 QStringLiteral("Cube LUT (*.cube)"));
		if (!f.isEmpty())
			setString("fl_lut", f.toUtf8().constData());
	});
	QObject::connect(lutClear, &QPushButton::clicked, this, [this]() { setString("fl_lut", ""); });
	refreshers.push_back([this, lutLabel]() {
		QString p = QString::fromStdString(getString("fl_lut"));
		lutLabel->setText(p.isEmpty() ? QStringLiteral("No LUT") : p.section('/', -1));
	});
	lutRow->addWidget(lutLabel, 1);
	lutRow->addWidget(lutBtn);
	lutRow->addWidget(lutClear);
	pv->addLayout(lutRow);
	customPanel->setVisible(false);

	/* look grid */
	auto *gridTitle = new QLabel(QStringLiteral("Looks"), page);
	gridTitle->setProperty("role", "title");
	v->addWidget(gridTitle);
	auto *grid = new QWidget(page);
	auto *flow = new FlowLayout(grid, 6, 8);
	std::vector<LookArt> looks;
	LookArt none;
	none.name = "None";
	none.none = true;
	LookArt custom;
	custom.name = "Customise";
	custom.custom = true;
	looks.push_back(none);
	looks.push_back(custom);
	for (auto &l : readLooks())
		looks.push_back(l);
	for (auto &l : looks) {
		auto *t = new LookTile(l, grid);
		flow->addWidget(t);
		lookTiles.push_back(t);
		focusers["look:" + l.name.toStdString()] = [this, t]() {
			showTab(PG_ENHANCE);
			showEnhanceSub(1);
			reveal(PG_ENHANCE, t);
		};
		locators["look:" + l.name.toStdString()] = t;
		if (l.custom) {
			t->onClicked = [this]() {
				customPanel->setVisible(!customPanel->isVisible());
				refresh();
			};
		} else {
			QString name = l.name;
			t->onClicked = [this, name]() {
				setString("makeup_preset", name.toUtf8().constData());
			};
		}
	}
	refreshers.push_back([this]() {
		QString cur = QString::fromStdString(getString("makeup_preset"));
		if (cur.isEmpty())
			cur = "None";
		for (auto *t : lookTiles)
			t->setSelected(t->art().custom ? customPanel->isVisible() : t->art().name == cur);
		lookName->setText(cur == "None" ? QStringLiteral("No look") : cur);
	});
	v->addWidget(grid);
	v->addWidget(customPanel);
	v->addStretch(1);
	return page;
}


/* ---------------- Enhance / Effects / Atmosphere / Filters / Light ---------------- */

static std::vector<obs_data_t *> arrayItems(obs_data_t *obj, const char *key)
{
	std::vector<obs_data_t *> out;
	obs_data_array_t *arr = obj ? obs_data_get_array(obj, key) : nullptr;
	for (size_t i = 0; arr && i < obs_data_array_count(arr); i++)
		out.push_back(obs_data_array_item(arr, i)); /* owned ref */
	obs_data_array_release(arr);
	return out;
}

/* Keeps a stacked widget only as tall as its current page. */
static void fitStackToCurrent(QStackedWidget *stack)
{
	QObject::connect(stack, &QStackedWidget::currentChanged, stack, [stack](int cur) {
		for (int i = 0; i < stack->count(); i++)
			stack->widget(i)->setSizePolicy(QSizePolicy::Preferred,
						       i == cur ? QSizePolicy::Preferred : QSizePolicy::Ignored);
		stack->adjustSize();
	});
	for (int i = 1; i < stack->count(); i++)
		stack->widget(i)->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
}

static QWidget *subTabRow(QWidget *parent, const std::vector<QString> &names, std::vector<QPushButton *> &out,
			  std::function<void(int)> onPick)
{
	auto *row = new QWidget(parent);
	auto *l = new QHBoxLayout(row);
	l->setContentsMargins(14, 10, 14, 0);
	l->setSpacing(4);
	auto *group = new QButtonGroup(row);
	for (size_t i = 0; i < names.size(); i++) {
		auto *b = new QPushButton(names[i], row);
		b->setProperty("role", "tab");
		b->setCheckable(true);
		b->setCursor(Qt::PointingHandCursor);
		group->addButton(b, (int)i);
		l->addWidget(b);
		out.push_back(b);
	}
	l->addStretch(1);
	out[0]->setChecked(true);
	QObject::connect(group, &QButtonGroup::idClicked, row, [onPick](int id) { onPick(id); });
	return row;
}

/* ---------------- Presets: built-in full looks + your own ---------------- */

QWidget *BeautyDock::buildPresets()
{
	auto *w = new QWidget();
	auto *v = new QVBoxLayout(w);
	v->setContentsMargins(14, 12, 14, 16);
	v->setSpacing(10);

	/* your presets */
	auto *head = new QHBoxLayout();
	auto *t = new QLabel(QStringLiteral("My presets"), w);
	t->setProperty("role", "title");
	head->addWidget(t);
	head->addStretch(1);
	saveBtn = new QPushButton(QStringLiteral("＋  Save current look"), w);
	saveBtn->setCursor(Qt::PointingHandCursor);
	saveBtn->setToolTip(QStringLiteral("Save every tab's settings as a preset"));
	saveBtn->setStyleSheet(
		"QPushButton { color:#1e0e16; background:#ff96b8; border:none; border-radius:13px; padding:5px 12px;"
		" font-size:12px; font-weight:700; }"
		"QPushButton:hover { background:#ffaac6; } QPushButton:pressed { background:#f584a8; }");
	QObject::connect(saveBtn, &QPushButton::clicked, this, [this]() { saveCurrentLook(); });
	locators["save_look"] = saveBtn;
	focusers["save_look"] = [this]() { /* scroll only */
		showTab(PG_PRESETS);
		reveal(PG_PRESETS, saveBtn);
	};
	head->addWidget(saveBtn);
	v->addLayout(head);

	myGrid = new PresetGrid(w);
	myGrid->onPicked = [this](const QString &n) {
		for (size_t i = 0; myLooks && i < obs_data_array_count(myLooks); i++) {
			obs_data_t *e = obs_data_array_item(myLooks, i);
			if (n == QString::fromUtf8(obs_data_get_string(e, "name"))) {
				obs_data_t *st = obs_data_get_obj(e, "settings");
				applyLook(st, false, QStringLiteral("u:") + n);
				obs_data_release(st);
			}
			obs_data_release(e);
		}
	};
	myGrid->onContext = [this](const QString &n, const QPoint &g) {
		QMenu menu(this);
		QAction *ren = menu.addAction(QStringLiteral("Rename…"));
		QAction *del = menu.addAction(QStringLiteral("Delete"));
		QAction *got = menu.exec(g);
		if (!got || !myLooks)
			return;
		for (size_t i = 0; i < obs_data_array_count(myLooks); i++) {
			obs_data_t *e = obs_data_array_item(myLooks, i);
			bool hit = n == QString::fromUtf8(obs_data_get_string(e, "name"));
			if (hit && got == del) {
				obs_data_array_erase(myLooks, i);
			} else if (hit && got == ren) {
				bool ok = false;
				QString nn = QInputDialog::getText(this, QStringLiteral("Rename preset"), QStringLiteral("Name"),
								   QLineEdit::Normal, n, &ok)
						     .trimmed();
				if (ok && !nn.isEmpty())
					obs_data_set_string(e, "name", nn.toUtf8().constData());
			}
			obs_data_release(e);
			if (hit)
				break;
		}
		storeMyLooks();
		rebuildMyLooks();
	};
	refreshers.push_back([this]() {
		QString cur = QString::fromStdString(getString("dewy_look"));
		myGrid->setSelected(cur.startsWith("u:") ? cur.mid(2) : QString());
		for (PresetGrid *g : lookGrids)
			g->setSelected(cur.startsWith("b:") ? cur.mid(2) : QString());
	});
	v->addWidget(myGrid);
	loadMyLooks();
	rebuildMyLooks();

	/* built-in looks, by category */
	obs_data_t *root = presets_looks_root();
	std::vector<obs_data_t *> cats = arrayItems(root, "categories");
	std::vector<QString> chipNames;
	for (obs_data_t *c : cats)
		chipNames.push_back(QString::fromUtf8(obs_data_get_string(c, "name")));
	auto titles = std::make_shared<std::vector<QWidget *>>();
	v->addWidget(chipRow(chipNames, [this, titles](int idx) {
		if (idx < (int)titles->size())
			reveal(PG_PRESETS, (*titles)[idx]);
	}));
	for (obs_data_t *c : cats) {
		auto *ct = new QLabel(QString::fromUtf8(obs_data_get_string(c, "name")), w);
		ct->setProperty("role", "title");
		ct->setContentsMargins(0, 6, 0, 0);
		titles->push_back(ct);
		v->addWidget(ct);
		auto *grid = new PresetGrid(w);
		std::vector<obs_data_t *> items = arrayItems(c, "looks");
		std::vector<PresetGrid::Item> gi;
		for (obs_data_t *d : items)
			gi.push_back({QString::fromUtf8(obs_data_get_string(d, "name")), QImage()});
		grid->setItems(gi);
		for (auto &it : gi) {
			std::string id = "preset:" + it.name.toStdString();
			focusers[id] = [this, grid, n = it.name]() {
				showTab(PG_PRESETS);
				reveal(PG_PRESETS, grid->tileNamed(n));
			};
			locators[id] = grid->tileNamed(it.name);
		}
		grid->onPicked = [this, root](const QString &n) {
			obs_data_t *look = presets_find_in(root, "looks", n.toUtf8().constData());
			if (!look)
				return;
			obs_data_t *st = obs_data_get_obj(look, "settings");
			applyLook(st, true, QStringLiteral("b:") + n);
			obs_data_release(st);
			obs_data_release(look);
		};
		lookGrids.push_back(grid);
		thumbJobs.push_back({grid, PG_PRESETS, items, lookThumb});
		v->addWidget(grid);
		obs_data_release(c);
	}
	v->addStretch(1);
	return w;
}

/* Everything a look touches goes back to its default first, so a preset is the whole look. */
void BeautyDock::applyLook(obs_data_t *st, bool builtin, const QString &tag)
{
	if (!filter)
		return;
	obs_data_t *d = obs_data_create();
	obs_data_t *defs = obs_get_source_defaults(FILTER_ID);
	for (obs_data_item_t *it = obs_data_first(defs); it; obs_data_item_next(&it)) {
		const char *k = obs_data_item_get_name(it);
		if (!look_key(k))
			continue;
		switch (obs_data_item_gettype(it)) {
		case OBS_DATA_NUMBER:
			if (obs_data_item_numtype(it) == OBS_DATA_NUM_INT)
				obs_data_set_int(d, k, obs_data_item_get_default_int(it));
			else
				obs_data_set_double(d, k, obs_data_item_get_default_double(it));
			break;
		case OBS_DATA_BOOLEAN:
			obs_data_set_bool(d, k, obs_data_item_get_default_bool(it));
			break;
		case OBS_DATA_STRING:
			obs_data_set_string(d, k, obs_data_item_get_default_string(it));
			break;
		default:
			break;
		}
	}
	obs_data_release(defs);
	for (const char *k : {"effect_preset", "atmo_preset", "motion_preset", "filter_preset", "light_preset", "fl_lut"})
		obs_data_set_string(d, k, "");
	obs_data_set_string(d, "makeup_preset", "None");
	if (st)
		obs_data_apply(d, st);
	if (builtin) /* let the filter apply the makeup look's colours */
		obs_data_set_string(d, "makeup_applied", "");
	obs_data_set_string(d, "dewy_look", tag.toUtf8().constData());
	obs_source_update(filter, d);
	obs_data_release(d);
	/* applying a makeup look resets its intensities; restore the preset's own */
	if (st && (obs_data_has_user_value(st, "makeup_amt") || obs_data_has_user_value(st, "filter_amt"))) {
		obs_data_t *amt = obs_data_create();
		for (const char *k : {"makeup_amt", "filter_amt"})
			if (obs_data_has_user_value(st, k))
				obs_data_set_int(amt, k, obs_data_get_int(st, k));
		obs_source_update(filter, amt);
		obs_data_release(amt);
	}
}

void BeautyDock::saveCurrentLook()
{
	if (!filter)
		return;
	/* saved straight away with the next free name; right-click renames */
	auto taken = [this](const QString &n) {
		for (size_t i = 0; myLooks && i < obs_data_array_count(myLooks); i++) {
			obs_data_t *x = obs_data_array_item(myLooks, i);
			bool same = n == QString::fromUtf8(obs_data_get_string(x, "name"));
			obs_data_release(x);
			if (same)
				return true;
		}
		return false;
	};
	int n = 1;
	while (taken(QStringLiteral("My look %1").arg(n)))
		n++;
	QString name = QStringLiteral("My look %1").arg(n);
	obs_data_t *cur = obs_source_get_settings(filter);
	obs_data_t *st = obs_data_create();
	obs_data_apply(st, cur);
	obs_data_release(cur);
	for (const char *k : {"all_skin", "show_landmarks", "sync_tracking", "debug_view", "dewy_look"})
		obs_data_erase(st, k);
	obs_data_t *e = obs_data_create();
	obs_data_set_string(e, "name", name.toUtf8().constData());
	obs_data_set_obj(e, "settings", st);
	obs_data_release(st);
	if (!myLooks)
		myLooks = obs_data_array_create();
	bool replaced = false;
	for (size_t i = 0; i < obs_data_array_count(myLooks) && !replaced; i++) {
		obs_data_t *x = obs_data_array_item(myLooks, i);
		if (name == QString::fromUtf8(obs_data_get_string(x, "name"))) {
			obs_data_array_erase(myLooks, i);
			obs_data_array_insert(myLooks, i, e);
			replaced = true;
		}
		obs_data_release(x);
	}
	if (!replaced)
		obs_data_array_push_back(myLooks, e);
	obs_data_release(e);
	storeMyLooks();
	rebuildMyLooks();
	setString("dewy_look", (QStringLiteral("u:") + name).toUtf8().constData());
}

void BeautyDock::loadMyLooks()
{
	obs_data_array_release(myLooks);
	myLooks = nullptr;
	char *path = obs_module_config_path("my-presets.json");
	obs_data_t *d = path ? obs_data_create_from_json_file(path) : nullptr;
	bfree(path);
	if (d)
		myLooks = obs_data_get_array(d, "presets");
	obs_data_release(d);
	if (!myLooks)
		myLooks = obs_data_array_create();
}

void BeautyDock::storeMyLooks()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path("my-presets.json");
	if (!path)
		return;
	obs_data_t *d = obs_data_create();
	obs_data_set_array(d, "presets", myLooks);
	obs_data_save_json_safe(d, path, "tmp", "bak");
	obs_data_release(d);
	bfree(path);
}

void BeautyDock::rebuildMyLooks()
{
	std::vector<PresetGrid::Item> gi;
	for (size_t i = 0; myLooks && i < obs_data_array_count(myLooks); i++) {
		obs_data_t *e = obs_data_array_item(myLooks, i);
		gi.push_back({QString::fromUtf8(obs_data_get_string(e, "name")), QImage()});
		obs_data_release(e);
	}
	myGrid->setItems(gi);
	myGrid->setVisible(!gi.empty());
	for (auto it = locators.begin(); it != locators.end();)
		it = it->first.rfind("mypreset:", 0) == 0 ? locators.erase(it) : std::next(it);
	for (auto it = focusers.begin(); it != focusers.end();)
		it = it->first.rfind("mypreset:", 0) == 0 ? focusers.erase(it) : std::next(it);
	for (auto &it : gi) {
		std::string id = "mypreset:" + it.name.toStdString();
		locators[id] = myGrid->tileNamed(it.name);
		focusers[id] = [this, n = it.name]() { /* scroll only */
			showTab(PG_PRESETS);
			reveal(PG_PRESETS, myGrid->tileNamed(n));
		};
	}
	refreshMyThumbs();
	refresh();
}

void BeautyDock::refreshMyThumbs()
{
	if (!filter || !myGrid || !myLooks || !obs_data_array_count(myLooks))
		return;
	FaceCrop crop = captureFaceCrop(filter, 128, 2.1f);
	std::vector<QImage> imgs;
	for (size_t i = 0; i < obs_data_array_count(myLooks); i++) {
		obs_data_t *e = obs_data_array_item(myLooks, i);
		imgs.push_back(lookThumb(crop, e));
		obs_data_release(e);
	}
	myGrid->setThumbs(imgs);
}

QWidget *BeautyDock::buildEnhance()
{
	auto *page = new QWidget();
	auto *v = new QVBoxLayout(page);
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(0);
	enhanceStack = new QStackedWidget(page);
	v->addWidget(subTabRow(page, {"Appearance", "Makeup"}, subTabs,
			       [this](int i) { enhanceStack->setCurrentIndex(i); }));
	locators["sub:appearance"] = subTabs[0];
	locators["sub:makeup"] = subTabs[1];
	for (int i = 0; i < 2; i++)
		focusers[i ? "sub:makeup" : "sub:appearance"] = [this, i]() { /* scroll only */
			showTab(PG_ENHANCE);
			reveal(PG_ENHANCE, subTabs[i]);
		};
	enhanceStack->addWidget(buildAppearance());
	enhanceStack->addWidget(buildMakeup());
	fitStackToCurrent(enhanceStack);
	v->addWidget(enhanceStack);
	v->addStretch(1);
	return page;
}

QWidget *BeautyDock::intensityCard(const char *label, const char *key, int def)
{
	auto *card = new QFrame();
	card->setProperty("role", "card");
	auto *cv = new QVBoxLayout(card);
	cv->setContentsMargins(14, 10, 14, 10);
	cv->setSpacing(4);
	auto *lab = new QLabel(QString::fromUtf8(label), card);
	lab->setProperty("role", "sub");
	auto *row = new SliderRow(0, 100, def, card);
	row->onChanged = [this, key](int val) { setInt(key, val); };
	refreshers.push_back([this, row, key]() { row->setValue((int)getInt(key)); });
	cv->addWidget(lab);
	cv->addWidget(row);
	return card;
}

QWidget *BeautyDock::chipRow(const std::vector<QString> &names, std::function<void(int)> onPick, int)
{
	auto *row = new QWidget();
	auto *l = new FlowLayout(row, 6, 6);
	for (size_t i = 0; i < names.size(); i++) {
		auto *b = new QPushButton(names[i], row);
		b->setProperty("role", "chip");
		b->setCursor(Qt::PointingHandCursor);
		int idx = (int)i;
		QObject::connect(b, &QPushButton::clicked, row, [onPick, idx]() { onPick(idx); });
		l->addWidget(b);
	}
	return row;
}

/* Category sections (title + grid each), with chips that jump to a section. */
QWidget *BeautyDock::presetSections(int page, obs_data_t *root, const char *itemsKey, const char *settingKey,
				    QImage (*gen)(const FaceCrop &, obs_data_t *), const char *amtLabel,
				    const char *amtKey, int amtDef)
{
	auto *w = new QWidget();
	auto *v = new QVBoxLayout(w);
	v->setContentsMargins(14, 12, 14, 16);
	v->setSpacing(10);
	std::vector<obs_data_t *> cats = arrayItems(root, "categories");
	std::vector<QString> chipNames{QStringLiteral("⊘  None")};
	for (obs_data_t *c : cats)
		chipNames.push_back(QString::fromUtf8(obs_data_get_string(c, "name")).replace("&", "&&"));
	auto titles = std::make_shared<std::vector<QWidget *>>();
	v->addWidget(chipRow(chipNames, [this, page, settingKey, titles](int idx) {
		if (idx == 0)
			setString(settingKey, "");
		else if (idx - 1 < (int)titles->size())
			pageScrolls[page]->ensureWidgetVisible((*titles)[idx - 1], 0, 60);
	}));
	for (obs_data_t *c : cats) {
		auto *t = new QLabel(QString::fromUtf8(obs_data_get_string(c, "name")), w);
		t->setProperty("role", "title");
		t->setContentsMargins(0, 6, 0, 0);
		titles->push_back(t);
		v->addWidget(t);
		auto *grid = new PresetGrid(w);
		if (amtKey)
			grid->setPanel(intensityCard(amtLabel, amtKey, amtDef));
		std::vector<obs_data_t *> items = arrayItems(c, itemsKey);
		std::vector<PresetGrid::Item> gi;
		for (obs_data_t *d : items)
			gi.push_back({QString::fromUtf8(obs_data_get_string(d, "name")), QImage()});
		grid->setItems(gi);
		for (auto &it : gi)
		{
			std::string id = std::string(settingKey) + ":" + it.name.toStdString();
			focusers[id] = [this, page, grid, n = it.name]() {
				showTab(page);
				reveal(page, grid->tileNamed(n));
			};
			locators[id] = grid->tileNamed(it.name);
		}
		grid->onPicked = [this, settingKey](const QString &n) { setString(settingKey, n.toUtf8().constData()); };
		refreshers.push_back([this, grid, settingKey]() {
			grid->setSelected(QString::fromStdString(getString(settingKey)));
		});
		thumbJobs.push_back({grid, page, items, gen});
		v->addWidget(grid);
		obs_data_release(c);
	}
	v->addStretch(1);
	return w;
}

/* One grid with a leading "None" tile. */
PresetGrid *BeautyDock::simpleGrid(int page, obs_data_t *root, const char *itemsKey, const char *settingKey,
				   QImage (*gen)(const FaceCrop &, obs_data_t *), QWidget *panel)
{
	auto *grid = new PresetGrid();
	if (panel)
		grid->setPanel(panel);
	std::vector<obs_data_t *> items{nullptr};
	for (obs_data_t *d : arrayItems(root, itemsKey))
		items.push_back(d);
	std::vector<PresetGrid::Item> gi;
	for (obs_data_t *d : items)
		gi.push_back({d ? QString::fromUtf8(obs_data_get_string(d, "name")) : QStringLiteral("None"),
			      d ? QImage() : noneThumb(128)});
	grid->setItems(gi);
	int sub = strcmp(settingKey, "motion_preset") == 0 ? 1 : 0;
	for (auto &it : gi)
	{
		std::string id = std::string(settingKey) + ":" + it.name.toStdString();
		focusers[id] = [this, page, sub, grid, n = it.name]() {
			showTab(page);
			if (page == PG_ATMO)
				showAtmoSub(sub);
			reveal(page, grid->tileNamed(n));
		};
		locators[id] = grid->tileNamed(it.name);
	}
	grid->onPicked = [this, settingKey](const QString &n) {
		setString(settingKey, n == "None" ? "" : n.toUtf8().constData());
	};
	refreshers.push_back([this, grid, settingKey]() {
		std::string cur = getString(settingKey);
		grid->setSelected(cur.empty() ? QStringLiteral("None") : QString::fromStdString(cur));
	});
	thumbJobs.push_back({grid, page, items, gen});
	return grid;
}

QWidget *BeautyDock::buildEffects()
{
	return presetSections(PG_EFFECTS, presets_effects_root(), "effects", "effect_preset", effectThumb, nullptr, nullptr, 0);
}

QWidget *BeautyDock::buildAtmosphere()
{
	auto *page = new QWidget();
	auto *v = new QVBoxLayout(page);
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(0);
	auto *stack = new QStackedWidget(page);
	atmoStack = stack;
	v->addWidget(subTabRow(page, {"Ambience", "Motion"}, atmoTabs, [stack](int i) { stack->setCurrentIndex(i); }));
	locators["sub:ambience"] = atmoTabs[0];
	locators["sub:motion"] = atmoTabs[1];
	for (int i = 0; i < 2; i++)
		focusers[i ? "sub:motion" : "sub:ambience"] = [this, i]() {
			showTab(PG_ATMO);
			reveal(PG_ATMO, atmoTabs[i]);
		};
	struct Sub {
		const char *items, *key, *amtKey;
		int def;
		QImage (*gen)(const FaceCrop &, obs_data_t *);
	};
	for (Sub sub : {Sub{"ambience", "atmo_preset", "atmo_amt", 70, atmoThumb},
			Sub{"motion", "motion_preset", "motion_amt", 50, motionThumb}}) {
		auto *w = new QWidget(stack);
		auto *wl = new QVBoxLayout(w);
		wl->setContentsMargins(14, 12, 14, 16);
		wl->addWidget(simpleGrid(PG_ATMO, presets_atmosphere_root(), sub.items, sub.key, sub.gen,
					 intensityCard("Intensity", sub.amtKey, sub.def)));
		wl->addStretch(1);
		stack->addWidget(w);
	}
	fitStackToCurrent(stack);
	v->addWidget(stack);
	v->addStretch(1);
	return page;
}

QWidget *BeautyDock::buildFilters()
{
	return presetSections(PG_FILTERS, presets_filters_root(), "filters", "filter_preset", filterThumb, "Filter",
			      "filter_preset_amt", 80);
}

QWidget *BeautyDock::buildLight()
{
	auto *page = new QWidget();
	auto *v = new QVBoxLayout(page);
	v->setContentsMargins(14, 12, 14, 16);
	v->setSpacing(14);
	v->addWidget(simpleGrid(PG_LIGHT, presets_lights_root(), "lights", "light_preset", lightThumb, nullptr));
	v->addWidget(intensityCard("Intensity", "light_amt", 60));
	v->addStretch(1);
	return page;
}

void BeautyDock::refreshThumbs()
{
	if (!filter || !pages)
		return;
	int page = pages->currentIndex();
	bool any = false;
	for (auto &j : thumbJobs)
		any |= j.page == page;
	if (!any)
		return;
	if (page == PG_LIGHT)
		beauty_filter_request_person_preview(filter);
	/* a fresh frame for this tab: ask the filter for one, render once it's there (or after
	 * ~0.6 s with whatever is available, e.g. when the source isn't showing) */
	uint32_t s0 = beauty_filter_snapshot_serial(filter);
	beauty_filter_request_snapshot(filter);
	waitForSnapshot(page, s0, 20);
}

void BeautyDock::waitForSnapshot(int page, uint32_t s0, int tries)
{
	if (!filter)
		return;
	if (beauty_filter_snapshot_serial(filter) != s0 || tries <= 0) {
		renderThumbs(page);
		return;
	}
	QTimer::singleShot(30, this, [this, page, s0, tries]() { waitForSnapshot(page, s0, tries - 1); });
}

void BeautyDock::renderThumbs(int page)
{
	if (!filter)
		return;
	if (page == PG_PRESETS)
		refreshMyThumbs();
	FaceCrop crop = captureFaceCrop(filter, 128, page == PG_LIGHT ? 3.4f : 2.1f); /* light: upper body */
	for (auto &j : thumbJobs) {
		if (j.page != page)
			continue;
		std::vector<QImage> imgs;
		for (obs_data_t *d : j.presets)
			imgs.push_back(d ? j.gen(crop, d) : noneThumb(128));
		j.grid->setThumbs(imgs);
	}
}

/* ---------------- binding ---------------- */

void BeautyDock::refillSources()
{
	QString cur = sourceBox->currentText();
	QSignalBlocker block(sourceBox);
	sourceBox->clear();
	struct Ctx {
		QComboBox *box;
		int firstWithFilter = -1;
	} ctx{sourceBox};
	obs_enum_sources(
		[](void *p, obs_source_t *s) {
			auto *c = static_cast<Ctx *>(p);
			if (!(obs_source_get_output_flags(s) & OBS_SOURCE_VIDEO))
				return true;
			bool has = false;
			obs_source_enum_filters(
				s,
				[](obs_source_t *, obs_source_t *f, void *hp) {
					if (strcmp(obs_source_get_id(f), FILTER_ID) == 0)
						*static_cast<bool *>(hp) = true;
				},
				&has);
			c->box->addItem(QString::fromUtf8(obs_source_get_name(s)));
			if (has && c->firstWithFilter < 0)
				c->firstWithFilter = c->box->count() - 1;
			return true;
		},
		&ctx);
	int idx = cur.isEmpty() ? -1 : sourceBox->findText(cur);
	if (idx < 0)
		idx = ctx.firstWithFilter >= 0 ? ctx.firstWithFilter : 0;
	sourceBox->setCurrentIndex(idx);
}

void BeautyDock::selectSourceByName(const QString &name)
{
	int i = sourceBox->findText(name);
	if (i >= 0)
		sourceBox->setCurrentIndex(i);
}

void BeautyDock::unbind()
{
	if (filter) {
		signal_handler_t *sh = obs_source_get_signal_handler(filter);
		signal_handler_disconnect(sh, "update", on_filter_changed, this);
		signal_handler_disconnect(sh, "enable", on_filter_changed, this);
		obs_source_release(filter);
		filter = nullptr;
	}
	if (parentSrc) {
		signal_handler_t *sh = obs_source_get_signal_handler(parentSrc);
		signal_handler_disconnect(sh, "filter_add", on_parent_filters_changed, this);
		signal_handler_disconnect(sh, "filter_remove", on_parent_filters_changed, this);
		obs_source_release(parentSrc);
		parentSrc = nullptr;
	}
}

void BeautyDock::bindToSelection()
{
	unbind();
	QString name = sourceBox->currentText();
	obs_source_t *src = name.isEmpty() ? nullptr : obs_get_source_by_name(name.toUtf8().constData());
	if (src) {
		parentSrc = src;
		signal_handler_t *sh = obs_source_get_signal_handler(src);
		signal_handler_connect(sh, "filter_add", on_parent_filters_changed, this);
		signal_handler_connect(sh, "filter_remove", on_parent_filters_changed, this);
		obs_source_enum_filters(
			src,
			[](obs_source_t *, obs_source_t *f, void *p) {
				auto **out = static_cast<obs_source_t **>(p);
				if (!*out && strcmp(obs_source_get_id(f), FILTER_ID) == 0)
					*out = obs_source_get_ref(f);
			},
			&filter);
	}
	if (filter) {
		signal_handler_t *sh = obs_source_get_signal_handler(filter);
		signal_handler_connect(sh, "update", on_filter_changed, this);
		signal_handler_connect(sh, "enable", on_filter_changed, this);
	}
	bool bound = filter != nullptr;
	content->setVisible(bound);
	emptyState->setVisible(!bound);
	enableToggle->setEnabled(bound);
	if (!src)
		emptyLabel->setText(QStringLiteral("Add a camera source to your scene, then pick it above."));
	else
		emptyLabel->setText(QStringLiteral("“%1” doesn't have Dewy yet.").arg(name));
	addButton->setVisible(src != nullptr);
	refresh();
	refreshThumbs();
}

void BeautyDock::refresh()
{
	if (!filter)
		return;
	refreshing = true; /* refreshers set widget values; don't echo them back */
	updateTabSwitch();
	for (auto &r : refreshers)
		r();
	refreshing = false;
}

void BeautyDock::resetCurrentTab()
{
	if (!filter)
		return;
	obs_data_t *d = obs_data_create();
	int page = pages->currentIndex();
	if (page == PG_PRESETS) { /* everything back to defaults */
		obs_data_release(d);
		obs_data_t *empty = obs_data_create();
		obs_data_set_string(empty, "dewy_look", "");
		applyLook(empty, true, QString());
		obs_data_release(empty);
		return;
	}
	if (page == PG_ENHANCE && enhanceStack->currentIndex() == 0) {
		obs_data_set_int(d, SMOOTH.key, SMOOTH.def);
		obs_data_set_int(d, FOUNDATION.key, FOUNDATION.def);
		obs_data_set_int(d, SKIN_TONE.key, SKIN_TONE.def);
		obs_data_set_int(d, REDNESS.key, REDNESS.def);
		for (const FeatureGroup &c : categories())
			for (const Opt &o : c.opts)
				obs_data_set_int(d, o.key, o.def);
		for (const Opt &o : tailSliders())
			obs_data_set_int(d, o.key, o.def);
	} else if (page == PG_ENHANCE) {
		obs_data_set_string(d, "makeup_preset", "None");
		obs_data_set_int(d, "filter_amt", 70);
		obs_data_set_int(d, "makeup_amt", 70);
	} else if (page == PG_EFFECTS) {
		obs_data_set_string(d, "effect_preset", "");
	} else if (page == PG_ATMO) {
		obs_data_set_string(d, "atmo_preset", "");
		obs_data_set_string(d, "motion_preset", "");
	} else if (page == PG_FILTERS) {
		obs_data_set_string(d, "filter_preset", "");
	} else if (page == PG_LIGHT) {
		obs_data_set_string(d, "light_preset", "");
	}
	obs_source_update(filter, d);
	obs_data_release(d);
}

void BeautyDock::frontendEvent(enum obs_frontend_event e)
{
	switch (e) {
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
	case OBS_FRONTEND_EVENT_EXIT:
		unbind();
		break;
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		introduceOnce();
		snapshotIfRequested();
		[[fallthrough]];
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_CHANGED: {
		std::string saved = loadConfig();
		refillSources();
		if (!saved.empty())
			selectSourceByName(QString::fromStdString(saved));
		if (e != OBS_FRONTEND_EVENT_SCENE_CHANGED || !filter)
			bindToSelection();
		break;
	}
	default:
		break;
	}
}

/* ---------------- persistence ---------------- */

void BeautyDock::saveConfig()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path("dock.json");
	if (!path)
		return;
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "source", sourceBox->currentText().toUtf8().constData());
	obs_data_save_json(d, path);
	obs_data_release(d);
	bfree(path);
}

std::string BeautyDock::loadConfig()
{
	char *path = obs_module_config_path("dock.json");
	obs_data_t *d = path ? obs_data_create_from_json_file(path) : nullptr;
	bfree(path);
	std::string s = d ? obs_data_get_string(d, "source") : "";
	obs_data_release(d);
	return s;
}

/* Show the dock the first time the plugin is installed (OBS adds new docks hidden). */
void BeautyDock::introduceOnce()
{
	char *path = obs_module_config_path("introduced");
	if (!path)
		return;
	if (!os_file_exists(path)) {
		if (auto *dw = qobject_cast<QDockWidget *>(parentWidget())) {
			dw->setVisible(true);
			dw->raise();
		}
		char *dir = obs_module_config_path("");
		os_mkdirs(dir);
		bfree(dir);
		os_quick_write_utf8_file(path, "1", 1, false);
	}
	bfree(path);
}

/* Dev aid: if <plugin config>/snapshot/ exists, render every tab offscreen into PNGs there. */
void BeautyDock::snapshotIfRequested()
{
	char *dir = obs_module_config_path("snapshot");
	bool want = dir && os_file_exists(dir);
	QString out = QString::fromUtf8(dir ? dir : "");
	bfree(dir);
	if (!want)
		return;
	QTimer::singleShot(20000, this, [out]() {
		auto *d = new BeautyDock();
		d->setAttribute(Qt::WA_DontShowOnScreen);
		d->resize(380, 1800);
		d->show();
		QApplication::processEvents();
		d->bindToSelection();
		for (int i = 0; i < PG_COUNT; i++) {
			d->showTab(i);
			QApplication::processEvents();
			d->grab().save(out + QString("/tab-%1.png").arg(PAGE_NAMES[i]));
		}
		d->showTab(PG_ENHANCE);
		d->showEnhanceSub(1);
		d->setCustomOpen(true);
		QApplication::processEvents();
		d->grab().save(out + "/tab-makeup.png");
		d->showEnhanceSub(0);
		d->resize(380, 760);
		QApplication::processEvents();
		d->grab().save(out + "/dock-default-size.png");
		d->deleteLater();
	});
}

/* ---------------- registration (called from plugin-main.c) ---------------- */

/* obs-websocket vendor "dewy": drive the dock from scripts, Stream Deck or demos.
 *   CallVendorRequest {vendorName: "dewy", requestType: "show",  requestData: {tab: "effects", sub: "motion"}}
 *   CallVendorRequest {vendorName: "dewy", requestType: "focus", requestData: {target: "face_slim"}}
 * Requests arrive on obs-websocket's thread; the dock is touched on the UI thread. */
static QPointer<BeautyDock> g_dock;

static void vendor_show(obs_data_t *req, obs_data_t *res, void *)
{
	std::string tab = obs_data_get_string(req, "tab"), sub = obs_data_get_string(req, "sub");
	int ti = -1;
	for (int i = 0; i < PG_COUNT; i++)
		if (tab == PAGE_NAMES[i])
			ti = i;
	obs_data_set_bool(res, "ok", ti >= 0 && g_dock);
	if (ti < 0 || !g_dock)
		return;
	QMetaObject::invokeMethod(g_dock, [ti, sub]() {
		if (!g_dock)
			return;
		g_dock->showTab(ti);
		if (sub == "appearance" || sub == "makeup")
			g_dock->showEnhanceSub(sub == "makeup" ? 1 : 0);
		if (sub == "ambience" || sub == "motion")
			g_dock->showAtmoSub(sub == "motion" ? 1 : 0);
		if (sub == "customise")
			g_dock->setCustomOpen(true);
	}, Qt::QueuedConnection);
}

static void vendor_focus(obs_data_t *req, obs_data_t *res, void *)
{
	std::string target = obs_data_get_string(req, "target");
	bool ok = false;
	if (g_dock)
		QMetaObject::invokeMethod(g_dock, [target, &ok]() {
			if (g_dock && g_dock->hasFocusTarget(target)) {
				g_dock->focusTarget(target);
				ok = true;
			}
		}, Qt::BlockingQueuedConnection);
	obs_data_set_bool(res, "ok", ok);
}

/* Where a control is on screen (global coordinates, logical points): {x, y, w, h} and, for
 * sliders, the knob {kx, ky}. Targets: anything "focus" takes, "slider:<key>", "opt:<key>",
 * "tab:<page>", "sub:<subtab>", "reset", "save_look". */
static void vendor_locate(obs_data_t *req, obs_data_t *res, void *)
{
	std::string target = obs_data_get_string(req, "target");
	bool ok = false, hasKnob = false;
	QRect r;
	QPointF knob;
	double shown = 0;
	int value = obs_data_has_user_value(req, "value") ? (int)obs_data_get_int(req, "value") : INT_MIN;
	int range[2] = {0, 100};
	if (g_dock)
		QMetaObject::invokeMethod(g_dock,
					  [&]() { ok = g_dock && g_dock->locate(target, r, knob, hasKnob, shown, value, range); },
					  Qt::BlockingQueuedConnection);
	obs_data_set_bool(res, "ok", ok);
	if (!ok)
		return;
	obs_data_set_int(res, "x", r.x());
	obs_data_set_int(res, "y", r.y());
	obs_data_set_int(res, "w", r.width());
	obs_data_set_int(res, "h", r.height());
	obs_data_set_double(res, "shown", shown);
	if (hasKnob) {
		obs_data_set_double(res, "kx", knob.x());
		obs_data_set_double(res, "ky", knob.y());
		obs_data_set_int(res, "min", range[0]);
		obs_data_set_int(res, "max", range[1]);
	}
}

/* {target: "preset:<name>" | "mypreset:<name>"}: apply a full look, like clicking its tile */
static void vendor_apply(obs_data_t *req, obs_data_t *res, void *)
{
	std::string target = obs_data_get_string(req, "target");
	bool ok = false;
	if (g_dock)
		QMetaObject::invokeMethod(g_dock, [&]() { ok = g_dock && g_dock->applyNamed(target); },
					  Qt::BlockingQueuedConnection);
	obs_data_set_bool(res, "ok", ok);
}

/* the Reset button: the current tab (Presets: everything) */
static void vendor_reset(obs_data_t *, obs_data_t *res, void *)
{
	if (g_dock)
		QMetaObject::invokeMethod(g_dock, []() {
			if (g_dock)
				g_dock->resetTab();
		}, Qt::QueuedConnection);
	obs_data_set_bool(res, "ok", g_dock != nullptr);
}

/* {dir: 1 | -1}: the tab strip's arrows */
static void vendor_tabs_scroll(obs_data_t *req, obs_data_t *res, void *)
{
	bool right = obs_data_get_int(req, "dir") >= 0;
	if (g_dock)
		QMetaObject::invokeMethod(g_dock, [right]() {
			if (g_dock)
				g_dock->scrollTabs(right);
		}, Qt::QueuedConnection);
	obs_data_set_bool(res, "ok", g_dock != nullptr);
}

/* save the current look as "My look N", like the Save button */
static void vendor_save(obs_data_t *, obs_data_t *res, void *)
{
	bool ok = false;
	if (g_dock)
		QMetaObject::invokeMethod(g_dock, [&]() {
			if (g_dock) {
				g_dock->saveLook();
				ok = true;
			}
		}, Qt::BlockingQueuedConnection);
	obs_data_set_bool(res, "ok", ok);
}

extern "C" void beauty_dock_register(void)
{
	auto *dock = new BeautyDock();
	if (!obs_frontend_add_dock_by_id("obs-beauty-dock", obs_module_text("Dewy"), dock) /* id kept: saved dock layouts */) {
		delete dock;
		return;
	}
	g_dock = dock;
	if (obs_websocket_vendor v = obs_websocket_register_vendor("dewy")) {
		obs_websocket_vendor_register_request(v, "show", vendor_show, nullptr);
		obs_websocket_vendor_register_request(v, "focus", vendor_focus, nullptr);
		obs_websocket_vendor_register_request(v, "locate", vendor_locate, nullptr);
		obs_websocket_vendor_register_request(v, "apply", vendor_apply, nullptr);
		obs_websocket_vendor_register_request(v, "save", vendor_save, nullptr);
		obs_websocket_vendor_register_request(v, "tabs_scroll", vendor_tabs_scroll, nullptr);
		obs_websocket_vendor_register_request(v, "reset", vendor_reset, nullptr);
	}
}
