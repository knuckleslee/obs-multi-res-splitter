#include "mmp-align.hpp"
#include "mmp-core.hpp"

#include <QButtonGroup>
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

	setWindowTitle(row ? T("對齊：%1（%2×%3）").arg(row->name).arg(row->w).arg(row->h) : T("對齊"));
	resize(900, 480);

	auto *lay = new QVBoxLayout(this);
	preview = new AlignPreview(this);
	lay->addWidget(preview, 1);

	info = new QLabel(this);
	info->setWordWrap(true);
	info->setVisible(false);
	lay->addWidget(info);

	auto mkSpin = [&](QWidget *parentW, int which) {
		auto *sp = new QSpinBox(parentW);
		sp->setRange(0, 16384);
		sp->setSuffix(" px");
		sp->setKeyboardTracking(false);
		sp->setMinimumWidth(110);
		connect(sp, &QSpinBox::valueChanged, this, [this, which](int) { OnSpinEdited(which); });
		return sp;
	};

	// 位置 + 對齊
	auto *posBox = new QGroupBox(this);
	auto *posLay = new QGridLayout(posBox);
	posLay->addWidget(new QLabel(T("<b>位置</b>")), 0, 0, 1, 2);
	posLay->addWidget(new QLabel("X"), 1, 0, Qt::AlignRight);
	spinX = mkSpin(posBox, 4);
	posLay->addWidget(spinX, 1, 1);
	posLay->addWidget(new QLabel("Y"), 2, 0, Qt::AlignRight);
	spinY = mkSpin(posBox, 5);
	posLay->addWidget(spinY, 2, 1);
	posLay->setColumnMinimumWidth(2, 32);

	// 裁剪（放在位置與對齊之間）
	posLay->addWidget(new QLabel(T("<b>裁剪</b>")), 0, 3, 1, 4);
	spinL = mkSpin(posBox, 0);
	spinR = mkSpin(posBox, 1);
	spinT = mkSpin(posBox, 2);
	spinB = mkSpin(posBox, 3);
	posLay->addWidget(new QLabel(T("左")), 1, 3, Qt::AlignRight);
	posLay->addWidget(spinL, 1, 4);
	posLay->addWidget(new QLabel(T("右")), 1, 5, Qt::AlignRight);
	posLay->addWidget(spinR, 1, 6);
	posLay->addWidget(new QLabel(T("上")), 2, 3, Qt::AlignRight);
	posLay->addWidget(spinT, 2, 4);
	posLay->addWidget(new QLabel(T("下")), 2, 5, Qt::AlignRight);
	posLay->addWidget(spinB, 2, 6);
	posLay->setColumnMinimumWidth(7, 32);

	posLay->addWidget(new QLabel(T("<b>對齊</b>")), 0, 8);
	auto *gridHost = new QWidget(posBox);
	auto *grid = new QGridLayout(gridHost);
	grid->setContentsMargins(0, 0, 0, 0);
	grid->setSpacing(3);
	gridGroup = new QButtonGroup(this);
	gridGroup->setExclusive(false);
	static const char *tips[3][3] = {{"左上", "中上", "右上"}, {"左中", "置中", "右中"}, {"左下", "中下", "右下"}};
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++) {
			auto *b = new QPushButton(gridHost);
			b->setCheckable(true);
			b->setFixedSize(18, 18);
			b->setToolTip(T(tips[r][c]));
			b->setStyleSheet("QPushButton{border:1px solid palette(mid);border-radius:2px;background:palette(base);}"
					 "QPushButton:checked{background:#378ADD;border-color:#185FA5;}");
			double ax = c * 0.5, ay = r * 0.5;
			connect(b, &QPushButton::clicked, this, [this, ax, ay]() { SetRatios(ax, ay); });
			grid->addWidget(b, r, c);
			gridGroup->addButton(b, r * 3 + c);
		}
	}
	posLay->addWidget(gridHost, 1, 8, 2, 1, Qt::AlignTop | Qt::AlignLeft);
	posLay->setColumnStretch(9, 1);
	lay->addWidget(posBox);

	// 按鈕
	auto *btnLay = new QHBoxLayout();
	auto *reset = new QPushButton(T("重設"), this);
	connect(reset, &QPushButton::clicked, this, [this]() { SetRatios(0.5, 1.0); });
	btnLay->addWidget(reset);
	btnLay->addStretch();
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &AlignDialog::reject);
	btnLay->addWidget(buttons);
	lay->addLayout(btnLay);

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
		info->setVisible(true);
		return;
	}
	RegionF r = ComputeSourceRegion(sw, sh, ow, oh, row->ax, row->ay);
	rw = r.w;
	rh = r.h;
	preview->SetFrame(sw, sh, r.x, r.y, r.w, r.h);

	updating = true;
	int maxX = std::max(0, int(sw - r.w)), maxY = std::max(0, int(sh - r.h));
	for (QSpinBox *sp : {spinX, spinL, spinR})
		sp->setRange(0, maxX);
	for (QSpinBox *sp : {spinY, spinT, spinB})
		sp->setRange(0, maxY);
	spinX->setValue(int(r.x));
	spinY->setValue(int(r.y));
	spinL->setValue(int(r.x));
	spinR->setValue(int(sw - r.w - r.x));
	spinT->setValue(int(r.y));
	spinB->setValue(int(sh - r.h - r.y));
	for (int i = 0; i < 9; i++) {
		double gx = (i % 3) * 0.5, gy = (i / 3) * 0.5;
		bool on = std::abs(row->ax - gx) < 1e-6 && std::abs(row->ay - gy) < 1e-6;
		if (auto *b = gridGroup->button(i))
			b->setChecked(on);
	}
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
	case 0: // 左
	case 4: // X
		ax = maxX > 0 ? (which == 0 ? spinL : spinX)->value() / maxX : 0.5;
		break;
	case 1: // 右
		ax = maxX > 0 ? (maxX - spinR->value()) / maxX : 0.5;
		break;
	case 2: // 上
	case 5: // Y
		ay = maxY > 0 ? (which == 2 ? spinT : spinY)->value() / maxY : 0.5;
		break;
	case 3: // 下
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
