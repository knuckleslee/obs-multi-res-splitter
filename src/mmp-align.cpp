#include "mmp-align.hpp"
#include "mmp-core.hpp"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

/* ------------------------------ 預覽 ------------------------------ */

AlignPreview::AlignPreview(QWidget *parent) : MmpDisplay(parent)
{
	setMinimumSize(320, 120);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	setMouseTracking(false);
	setCursor(Qt::SizeAllCursor);
	source = obs_source_get_ref(MmpCore::Get()->CaptureSource());
	if (source)
		obs_source_inc_showing(source);
}

AlignPreview::~AlignPreview()
{
	DestroyDisplay();
	if (source) {
		obs_source_dec_showing(source);
		obs_source_release(source);
	}
}

void AlignPreview::SetFrame(int sw_, int sh_, double rx_, double ry_, double rw_, double rh_)
{
	std::lock_guard<std::mutex> lock(mtx);
	sw = sw_;
	sh = sh_;
	rx = rx_;
	ry = ry_;
	rw = rw_;
	rh = rh_;
}

void AlignPreview::Render(uint32_t cx, uint32_t cy)
{
	int msw, msh;
	double frx, fry, frw, frh;
	{
		std::lock_guard<std::mutex> lock(mtx);
		msw = sw;
		msh = sh;
		frx = rx;
		fry = ry;
		frw = rw;
		frh = rh;
	}
	if (msw <= 0 || msh <= 0)
		return;

	double scale = std::min(double(cx) / msw, double(cy) / msh);
	int vw = int(msw * scale + 0.5);
	int vh = int(msh * scale + 0.5);
	int vx = (int(cx) - vw) / 2;
	int vy = (int(cy) - vh) / 2;
	{
		std::lock_guard<std::mutex> lock(mtx);
		viewX = vx;
		viewY = vy;
		viewScale = scale;
	}

	// 以主螢幕座標繪製
	MmpStartRegion(vx, vy, vw, vh, 0.0f, float(msw), 0.0f, float(msh));
	MmpDrawRectFill(0, 0, float(msw), float(msh), 0xFF202020);
	if (source) {
		uint32_t srcW = obs_source_get_width(source);
		uint32_t srcH = obs_source_get_height(source);
		if (srcW && srcH) {
			gs_matrix_push();
			gs_matrix_scale3f(float(msw) / srcW, float(msh) / srcH, 1.0f);
			obs_source_video_render(source);
			gs_matrix_pop();
		}
	}
	// 紅框外變暗
	uint32_t shade = 0x99000000;
	MmpDrawRectFill(0, 0, float(msw), float(fry), shade);
	MmpDrawRectFill(0, float(fry + frh), float(msw), float(msh - fry - frh), shade);
	MmpDrawRectFill(0, float(fry), float(frx), float(frh), shade);
	MmpDrawRectFill(float(frx + frw), float(fry), float(msw - frx - frw), float(frh), shade);
	// 紅框
	float t = float(std::max(2.0, 2.5 / scale));
	MmpDrawRectOutline(float(frx), float(fry), float(frw), float(frh), t, 0xFF3030FF);
	MmpEndRegion();
}

bool AlignPreview::MapToMain(const QPointF &p, double &x, double &y)
{
	std::lock_guard<std::mutex> lock(mtx);
	if (viewScale <= 0)
		return false;
	double dpr = devicePixelRatioF();
	x = (p.x() * dpr - viewX) / viewScale;
	y = (p.y() * dpr - viewY) / viewScale;
	return true;
}

void AlignPreview::mousePressEvent(QMouseEvent *e)
{
	double x, y;
	if (e->button() != Qt::LeftButton || !MapToMain(e->position(), x, y))
		return;
	std::lock_guard<std::mutex> lock(mtx);
	if (x >= rx && x <= rx + rw && y >= ry && y <= ry + rh) {
		grabDX = x - rx;
		grabDY = y - ry;
	} else {
		// 點在框外：讓框的中心移到點擊處
		grabDX = rw / 2;
		grabDY = rh / 2;
	}
	dragging = true;
	double nx = x - grabDX, ny = y - grabDY;
	QMetaObject::invokeMethod(this, [this, nx, ny]() { emit Dragged(nx, ny); }, Qt::QueuedConnection);
}

void AlignPreview::mouseMoveEvent(QMouseEvent *e)
{
	double x, y;
	if (!dragging || !MapToMain(e->position(), x, y))
		return;
	emit Dragged(x - grabDX, y - grabDY);
}

void AlignPreview::mouseReleaseEvent(QMouseEvent *)
{
	dragging = false;
}

/* ------------------------------ 對話框 ------------------------------ */

AlignDialog::AlignDialog(int rowId_, QWidget *parent) : QDialog(parent), rowId(rowId_)
{
	MmpCore *core = MmpCore::Get();
	OutputRow *row = core->FindRow(rowId);
	origAx = row ? row->ax : 0.5;
	origAy = row ? row->ay : 1.0;

	setWindowTitle(T("對齊：%1").arg(row ? row->name : QString()));
	resize(900, 560);

	auto *lay = new QVBoxLayout(this);
	info = new QLabel(this);
	info->setWordWrap(true);
	lay->addWidget(info);

	preview = new AlignPreview(this);
	lay->addWidget(preview, 1);

	auto *bottom = new QHBoxLayout();
	lay->addLayout(bottom);

	// 九宮格
	auto *gridBox = new QGroupBox(T("快速對齊"), this);
	auto *grid = new QGridLayout(gridBox);
	static const char *names[3][3] = {{"左上", "中上", "右上"}, {"左中", "置中", "右中"}, {"左下", "中下", "右下"}};
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++) {
			auto *b = new QPushButton(T(names[r][c]), gridBox);
			b->setMinimumWidth(56);
			double ax = c * 0.5, ay = r * 0.5;
			connect(b, &QPushButton::clicked, this, [this, ax, ay]() { SetRatios(ax, ay); });
			grid->addWidget(b, r, c);
		}
	}
	bottom->addWidget(gridBox);

	// 裁切數字
	auto *cropBox = new QGroupBox(T("裁切（像素）"), this);
	auto *form = new QGridLayout(cropBox);
	auto mkSpin = [&](int which) {
		auto *s = new QSpinBox(cropBox);
		s->setRange(0, 16384);
		s->setKeyboardTracking(false);
		connect(s, &QSpinBox::valueChanged, this, [this, which](int) { OnSpinEdited(which); });
		return s;
	};
	spinL = mkSpin(0);
	spinR = mkSpin(1);
	spinT = mkSpin(2);
	spinB = mkSpin(3);
	form->addWidget(new QLabel(T("上")), 0, 2, Qt::AlignRight);
	form->addWidget(spinT, 0, 3);
	form->addWidget(new QLabel(T("左")), 1, 0, Qt::AlignRight);
	form->addWidget(spinL, 1, 1);
	form->addWidget(new QLabel(T("右")), 1, 4, Qt::AlignRight);
	form->addWidget(spinR, 1, 5);
	form->addWidget(new QLabel(T("下")), 2, 2, Qt::AlignRight);
	form->addWidget(spinB, 2, 3);
	bottom->addWidget(cropBox, 1);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &AlignDialog::reject);
	lay->addWidget(buttons);

	connect(preview, &AlignPreview::Dragged, this, [this](double nx, double ny) {
		double ax = (sw > rw) ? nx / (sw - rw) : 0.5;
		double ay = (sh > rh) ? ny / (sh - rh) : 0.5;
		SetRatios(std::clamp(ax, 0.0, 1.0), std::clamp(ay, 0.0, 1.0));
	});

	UpdateFromRow();
}

void AlignDialog::UpdateFromRow()
{
	MmpCore *core = MmpCore::Get();
	OutputRow *row = core->FindRow(rowId);
	if (!row)
		return;
	sw = core->mainW;
	sh = core->mainH;
	ow = row->w;
	oh = row->h;
	if (sw <= 0 || sh <= 0 || ow <= 0 || oh <= 0) {
		info->setText(T("請先設定主螢幕與這台螢幕的解析度。"));
		return;
	}
	RegionF r = ComputeSourceRegion(sw, sh, ow, oh, row->ax, row->ay);
	rw = r.w;
	rh = r.h;
	preview->SetFrame(sw, sh, r.x, r.y, r.w, r.h);

	QString scaleText;
	if (r.w < ow - 0.5 || r.h < oh - 0.5)
		scaleText = T("，放大 %1 倍").arg(ow / r.w, 0, 'f', 2);
	else
		scaleText = T("，1:1 原尺寸");
	info->setText(T("主螢幕 %1×%2 → 取樣 %3×%4 → 輸出 %5×%6%7。拖曳紅框、按快速對齊或輸入裁切數字調整位置，"
			"實體螢幕會即時更新。")
			      .arg(sw)
			      .arg(sh)
			      .arg(int(r.w))
			      .arg(int(r.h))
			      .arg(ow)
			      .arg(oh)
			      .arg(scaleText));

	updating = true;
	int maxX = int(sw - r.w), maxY = int(sh - r.h);
	for (QSpinBox *s : {spinL, spinR})
		s->setRange(0, std::max(0, maxX));
	for (QSpinBox *s : {spinT, spinB})
		s->setRange(0, std::max(0, maxY));
	spinL->setValue(int(r.x));
	spinR->setValue(int(sw - r.w - r.x));
	spinT->setValue(int(r.y));
	spinB->setValue(int(sh - r.h - r.y));
	updating = false;
}

void AlignDialog::SetRatios(double ax, double ay)
{
	MmpCore *core = MmpCore::Get();
	OutputRow *row = core->FindRow(rowId);
	if (!row)
		return;
	row->ax = ax;
	row->ay = ay;
	core->PushParams();
	UpdateFromRow();
}

void AlignDialog::OnSpinEdited(int which)
{
	if (updating)
		return;
	double maxX = sw - rw, maxY = sh - rh;
	OutputRow *row = MmpCore::Get()->FindRow(rowId);
	if (!row)
		return;
	double ax = row->ax, ay = row->ay;
	switch (which) {
	case 0:
		ax = maxX > 0 ? spinL->value() / maxX : 0.5;
		break;
	case 1:
		ax = maxX > 0 ? (maxX - spinR->value()) / maxX : 0.5;
		break;
	case 2:
		ay = maxY > 0 ? spinT->value() / maxY : 0.5;
		break;
	case 3:
		ay = maxY > 0 ? (maxY - spinB->value()) / maxY : 0.5;
		break;
	}
	SetRatios(std::clamp(ax, 0.0, 1.0), std::clamp(ay, 0.0, 1.0));
}

void AlignDialog::reject()
{
	MmpCore *core = MmpCore::Get();
	if (OutputRow *row = core->FindRow(rowId)) {
		row->ax = origAx;
		row->ay = origAy;
		core->PushParams();
	}
	QDialog::reject();
}
