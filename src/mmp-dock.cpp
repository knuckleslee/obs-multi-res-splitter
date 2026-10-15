#include "mmp-dock.hpp"
#include "mmp-core.hpp"
#include "mmp-align.hpp"
#include "mmp-dialogs.hpp"

#include <obs-frontend-api.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QVBoxLayout>

static QSpinBox *make_res_spin(QWidget *parent)
{
	auto *s = new QSpinBox(parent);
	s->setRange(0, 16384);
	s->setKeyboardTracking(false);
	s->setButtonSymbols(QAbstractSpinBox::NoButtons);
	s->setMinimumWidth(56);
	s->setAlignment(Qt::AlignRight);
	return s;
}

MmpDock::MmpDock(QWidget *parent) : QWidget(parent)
{
	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(4, 4, 4, 4);

	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	gridHost = new QWidget(scroll);
	auto *hostLay = new QVBoxLayout(gridHost);
	hostLay->setContentsMargins(0, 0, 0, 0);
	grid = new QGridLayout();
	grid->setHorizontalSpacing(6);
	grid->setVerticalSpacing(4);
	hostLay->addLayout(grid);

	auto *addBtn = new QPushButton(T("＋ 追加項目"), gridHost);
	connect(addBtn, &QPushButton::clicked, this, []() { MmpCore::Get()->AddRow(); });
	auto *addLay = new QHBoxLayout();
	addLay->addWidget(addBtn);
	addLay->addStretch();
	hostLay->addLayout(addLay);
	hostLay->addStretch();
	scroll->setWidget(gridHost);
	outer->addWidget(scroll, 1);

	// 底部按鈕
	auto *btns1 = new QHBoxLayout();
	auto *reproject = new QPushButton(T("重新投放"), this);
	auto *closeAll = new QPushButton(T("關閉全部投影"), this);
	auto *identify = new QPushButton(T("識別螢幕"), this);
	btns1->addWidget(reproject);
	btns1->addWidget(closeAll);
	btns1->addWidget(identify);
	outer->addLayout(btns1);

	auto *btns2 = new QHBoxLayout();
	auto *exportBtn = new QPushButton(T("匯出設定"), this);
	auto *importBtn = new QPushButton(T("匯入設定"), this);
	btns2->addWidget(exportBtn);
	btns2->addWidget(importBtn);
	btns2->addStretch();
	outer->addLayout(btns2);

	connect(reproject, &QPushButton::clicked, this, []() { MmpCore::Get()->ProjectAll(); });
	connect(closeAll, &QPushButton::clicked, this, []() { MmpCore::Get()->CloseAll(); });
	connect(identify, &QPushButton::clicked, this, []() { MmpCore::Get()->IdentifyAll(); });
	connect(exportBtn, &QPushButton::clicked, this, [this]() {
		QString path = QFileDialog::getSaveFileName(this, T("匯出設定"), "multi-monitor-projector.json",
							    T("設定檔 (*.json)"));
		if (path.isEmpty())
			return;
		if (!MmpCore::Get()->ExportTo(path))
			QMessageBox::warning(this, T("匯出失敗"), T("無法寫入檔案。"));
	});
	connect(importBtn, &QPushButton::clicked, this, [this]() {
		QString path = QFileDialog::getOpenFileName(this, T("匯入設定"), QString(), T("設定檔 (*.json)"));
		if (path.isEmpty())
			return;
		if (!MmpCore::Get()->ImportFrom(path)) {
			QMessageBox::warning(this, T("匯入失敗"), T("無法讀取這個檔案。"));
			return;
		}
		QStringList missing;
		for (auto &r : MmpCore::Get()->rows) {
			if (r.mode != OutputMode::Scene || r.scene.isEmpty())
				continue;
			obs_source_t *s = obs_get_source_by_name(r.scene.toUtf8().constData());
			if (!s)
				missing << r.scene;
			obs_source_release(s);
		}
		if (!missing.isEmpty())
			QMessageBox::information(this, T("找不到場景"),
						 T("以下場景在目前的場景集合中不存在，請另外匯入場景集合或重新選擇：\n%1")
							 .arg(missing.join("\n")));
	});

	MmpCore *core = MmpCore::Get();
	connect(core, &MmpCore::RowsReset, this, &MmpDock::Rebuild);
	connect(core, &MmpCore::StatusChanged, this, &MmpDock::RefreshStatus);
	connect(core, &MmpCore::MonitorsChanged, this, &MmpDock::RefreshStatus);

	Rebuild();
}

void MmpDock::Rebuild()
{
	MmpCore *core = MmpCore::Get();

	// 清空
	while (QLayoutItem *item = grid->takeAt(0)) {
		if (item->widget())
			item->widget()->deleteLater();
		delete item;
	}
	rowWidgets.clear();

	int r = 0;
	auto header = [&](int col, const QString &text) {
		auto *l = new QLabel(QString("<b>%1</b>").arg(text), gridHost);
		grid->addWidget(l, r, col);
	};

	// 主螢幕列
	header(1, T("主螢幕"));
	header(2, T("寬"));
	header(3, T("高"));
	r++;
	{
		auto *lbl = new QLabel(T("訊源"), gridHost);
		grid->addWidget(lbl, r, 1);
		mainW = make_res_spin(gridHost);
		mainH = make_res_spin(gridHost);
		mainW->setValue(core->mainW);
		mainH->setValue(core->mainH);
		grid->addWidget(mainW, r, 2);
		grid->addWidget(mainH, r, 3);
		auto *det = new QPushButton(T("偵測"), gridHost);
		grid->addWidget(det, r, 4);
		mainStatus = new QLabel(gridHost);
		grid->addWidget(mainStatus, r, 5, 1, 6);

		auto apply = [this]() {
			MmpCore *c = MmpCore::Get();
			if (c->mainW == mainW->value() && c->mainH == mainH->value())
				return;
			c->mainW = mainW->value();
			c->mainH = mainH->value();
			c->ConfigEdited();
		};
		connect(mainW, &QSpinBox::editingFinished, this, apply);
		connect(mainH, &QSpinBox::editingFinished, this, apply);
		connect(det, &QPushButton::clicked, this, [this]() {
			DetectDialog dlg(0, this);
			if (dlg.exec() != QDialog::Accepted)
				return;
			MmpCore *c = MmpCore::Get();
			c->mainW = dlg.SelectedW();
			c->mainH = dlg.SelectedH();
			QSignalBlocker b1(mainW), b2(mainH);
			mainW->setValue(c->mainW);
			mainH->setValue(c->mainH);
			c->ConfigEdited();
		});
	}
	r++;

	auto *line = new QFrame(gridHost);
	line->setFrameShape(QFrame::HLine);
	line->setFrameShadow(QFrame::Sunken);
	grid->addWidget(line, r, 0, 1, 12);
	r++;

	header(0, T("啟用"));
	header(1, T("名稱"));
	header(2, T("寬"));
	header(3, T("高"));
	header(5, T("模式"));
	header(6, T("對齊／場景"));
	header(7, T("套用相同解析度"));
	header(8, T("自動投放"));
	header(9, T("狀態"));
	r++;

	for (auto &row : core->rows) {
		int id = row.id;
		RowWidgets rw;

		auto *en = new QCheckBox(gridHost);
		en->setChecked(row.enabled);
		grid->addWidget(en, r, 0, Qt::AlignCenter);
		connect(en, &QCheckBox::toggled, this, [id](bool on) {
			MmpCore *c = MmpCore::Get();
			if (OutputRow *o = c->FindRow(id)) {
				o->enabled = on;
				if (on)
					o->wanted = true;
				c->ConfigEdited();
			}
		});

		auto *name = new QLineEdit(row.name, gridHost);
		name->setMinimumWidth(80);
		grid->addWidget(name, r, 1);
		connect(name, &QLineEdit::editingFinished, this, [id, name]() {
			MmpCore *c = MmpCore::Get();
			if (OutputRow *o = c->FindRow(id)) {
				if (o->name != name->text()) {
					o->name = name->text();
					c->ConfigEdited();
				}
			}
		});

		rw.w = make_res_spin(gridHost);
		rw.h = make_res_spin(gridHost);
		rw.w->setValue(row.w);
		rw.h->setValue(row.h);
		grid->addWidget(rw.w, r, 2);
		grid->addWidget(rw.h, r, 3);
		connect(rw.w, &QSpinBox::editingFinished, this, [this, id]() { EditRes(id, true); });
		connect(rw.h, &QSpinBox::editingFinished, this, [this, id]() { EditRes(id, false); });

		auto *det = new QPushButton(T("偵測"), gridHost);
		grid->addWidget(det, r, 4);
		connect(det, &QPushButton::clicked, this, [this, id]() {
			DetectDialog dlg(id, this);
			if (dlg.exec() != QDialog::Accepted)
				return;
			MmpCore *c = MmpCore::Get();
			OutputRow *o = c->FindRow(id);
			if (!o)
				return;
			o->w = dlg.SelectedW();
			o->h = dlg.SelectedH();
			o->wanted = true;
			auto &wid = rowWidgets[id];
			QSignalBlocker b1(wid.w), b2(wid.h);
			wid.w->setValue(o->w);
			wid.h->setValue(o->h);
			c->ConfigEdited();
		});

		rw.mode = new QComboBox(gridHost);
		rw.mode->addItem(T("來源"));
		rw.mode->addItem(T("場景"));
		rw.mode->setCurrentIndex(row.mode == OutputMode::Scene ? 1 : 0);
		grid->addWidget(rw.mode, r, 5);
		connect(rw.mode, &QComboBox::currentIndexChanged, this, [this, id](int idx) {
			MmpCore *c = MmpCore::Get();
			if (OutputRow *o = c->FindRow(id)) {
				o->mode = idx == 1 ? OutputMode::Scene : OutputMode::Source;
				UpdateRowVisibility(id);
				c->ConfigEdited();
			}
		});

		auto *cell = new QWidget(gridHost);
		auto *cellLay = new QHBoxLayout(cell);
		cellLay->setContentsMargins(0, 0, 0, 0);
		rw.align = new QPushButton(T("對齊"), cell);
		rw.scene = new QComboBox(cell);
		rw.scene->setMinimumWidth(100);
		cellLay->addWidget(rw.align);
		cellLay->addWidget(rw.scene);
		grid->addWidget(cell, r, 6);
		connect(rw.align, &QPushButton::clicked, this, [this, id]() {
			MmpCore *c = MmpCore::Get();
			AlignDialog dlg(id, this);
			if (dlg.exec() == QDialog::Accepted)
				c->ConfigEdited();
		});
		connect(rw.scene, &QComboBox::activated, this, [this, id](int) {
			MmpCore *c = MmpCore::Get();
			if (OutputRow *o = c->FindRow(id)) {
				o->scene = rowWidgets[id].scene->currentData().toString();
				c->ConfigEdited();
			}
		});

		auto *same = new QCheckBox(gridHost);
		same->setChecked(row.applySame);
		grid->addWidget(same, r, 7, Qt::AlignCenter);
		connect(same, &QCheckBox::toggled, this, [id](bool on) {
			MmpCore *c = MmpCore::Get();
			if (OutputRow *o = c->FindRow(id)) {
				o->applySame = on;
				c->ConfigEdited();
			}
		});

		auto *autoP = new QCheckBox(gridHost);
		autoP->setChecked(row.autoProject);
		grid->addWidget(autoP, r, 8, Qt::AlignCenter);
		connect(autoP, &QCheckBox::toggled, this, [id](bool on) {
			MmpCore *c = MmpCore::Get();
			if (OutputRow *o = c->FindRow(id)) {
				o->autoProject = on;
				c->ConfigEdited();
			}
		});

		rw.status = new QLabel(gridHost);
		rw.status->setMinimumWidth(90);
		grid->addWidget(rw.status, r, 9);

		auto *del = new QPushButton(T("刪除"), gridHost);
		grid->addWidget(del, r, 10);
		connect(del, &QPushButton::clicked, this, [this, id]() {
			MmpCore *c = MmpCore::Get();
			OutputRow *o = c->FindRow(id);
			if (!o)
				return;
			if (QMessageBox::question(this, T("刪除"), T("確定刪除「%1」？").arg(o->name)) !=
			    QMessageBox::Yes)
				return;
			// 延後刪除，避免刪到正在處理點擊事件的按鈕
			QMetaObject::invokeMethod(c, [c, id]() { c->RemoveRow(id); }, Qt::QueuedConnection);
		});

		rowWidgets[id] = rw;
		r++;
	}
	grid->setColumnStretch(9, 1);

	RefreshScenes();
	for (auto &row : core->rows)
		UpdateRowVisibility(row.id);
	RefreshStatus();
}

void MmpDock::EditRes(int rowId, bool isW)
{
	MmpCore *c = MmpCore::Get();
	OutputRow *o = c->FindRow(rowId);
	auto it = rowWidgets.find(rowId);
	if (!o || it == rowWidgets.end())
		return;
	int nw = it->second.w->value();
	int nh = it->second.h->value();
	if (nw == o->w && nh == o->h)
		return;
	if (c->IsDuplicateRes(rowId, nw, nh)) {
		QSignalBlocker b1(it->second.w), b2(it->second.h);
		it->second.w->setValue(o->w);
		it->second.h->setValue(o->h);
		QMessageBox::warning(this, T("解析度重複"),
				     T("已經有其他項目設定為 %1×%2，同一個解析度只能有一列。").arg(nw).arg(nh));
		return;
	}
	(void)isW;
	o->w = nw;
	o->h = nh;
	o->wanted = true;
	c->ConfigEdited();
}

void MmpDock::UpdateRowVisibility(int rowId)
{
	OutputRow *o = MmpCore::Get()->FindRow(rowId);
	auto it = rowWidgets.find(rowId);
	if (!o || it == rowWidgets.end())
		return;
	bool scene = o->mode == OutputMode::Scene;
	it->second.align->setVisible(!scene);
	it->second.scene->setVisible(scene);
}

void MmpDock::RefreshScenes()
{
	QStringList names;
	char **list = obs_frontend_get_scene_names();
	if (list) {
		for (char **p = list; *p; p++)
			names << QString::fromUtf8(*p);
		bfree(list);
	}
	MmpCore *core = MmpCore::Get();
	for (auto &kv : rowWidgets) {
		OutputRow *o = core->FindRow(kv.first);
		QComboBox *cb = kv.second.scene;
		if (!o || !cb)
			continue;
		QSignalBlocker block(cb);
		cb->clear();
		cb->addItem(T("（選擇場景）"), QString());
		for (auto &n : names)
			cb->addItem(n, n);
		int idx = cb->findData(o->scene);
		if (idx < 0 && !o->scene.isEmpty()) {
			cb->addItem(T("%1（不存在）").arg(o->scene), o->scene);
			idx = cb->count() - 1;
		}
		cb->setCurrentIndex(idx < 0 ? 0 : idx);
	}
}

void MmpDock::RefreshStatus()
{
	MmpCore *core = MmpCore::Get();
	if (mainStatus)
		mainStatus->setText(core->MainStatus());
	for (auto &kv : rowWidgets) {
		OutputRow *o = core->FindRow(kv.first);
		if (o && kv.second.status)
			kv.second.status->setText(core->RowStatus(*o));
	}
}
