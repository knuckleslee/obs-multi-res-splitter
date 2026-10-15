#include "mmp-dialogs.hpp"
#include "mmp-core.hpp"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

void ShowIdentifyWindow(QScreen *screen, const QString &text)
{
	auto *w = new QLabel(nullptr, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool |
					      Qt::WindowDoesNotAcceptFocus);
	w->setAttribute(Qt::WA_DeleteOnClose);
	w->setAttribute(Qt::WA_ShowWithoutActivating);
	w->setText(text);
	w->setAlignment(Qt::AlignCenter);
	QRect g = screen->geometry();
	int fontPx = qMax(24, g.height() / 9);
	w->setStyleSheet(QString("QLabel { background: rgba(0,0,0,215); color: #ffffff; border: 6px solid #ff3030;"
				 " font-size: %1px; font-weight: bold; }")
				 .arg(fontPx));
	QSize sz(g.width() * 7 / 10, g.height() * 6 / 10);
	w->setGeometry(QRect(g.center() - QPoint(sz.width() / 2, sz.height() / 2), sz));
	w->show();
	QTimer::singleShot(3000, w, &QWidget::close);
}

DetectDialog::DetectDialog(int forRowId, QWidget *parent) : QDialog(parent), rowId(forRowId)
{
	setWindowTitle(rowId == 0 ? T("選擇主螢幕") : T("選擇輸出螢幕"));
	resize(640, 320);

	auto *lay = new QVBoxLayout(this);
	auto *hint = new QLabel(T("點選一台螢幕後按「選擇」，會把它的解析度帶入。按「識別」可在該螢幕上顯示標示。"));
	hint->setWordWrap(true);
	lay->addWidget(hint);

	table = new QTableWidget(0, 5, this);
	table->setHorizontalHeaderLabels({T("型號"), T("解析度"), T("位置"), T("狀態"), T("")});
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	table->setSelectionMode(QAbstractItemView::SingleSelection);
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table->verticalHeader()->hide();
	table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	lay->addWidget(table);

	auto *buttons = new QDialogButtonBox(this);
	auto *identifyAll = buttons->addButton(T("全部識別"), QDialogButtonBox::ActionRole);
	auto *refresh = buttons->addButton(T("重新整理"), QDialogButtonBox::ActionRole);
	chooseBtn = buttons->addButton(T("選擇"), QDialogButtonBox::AcceptRole);
	buttons->addButton(QDialogButtonBox::Cancel);
	lay->addWidget(buttons);

	connect(identifyAll, &QPushButton::clicked, this, []() { MmpCore::Get()->IdentifyAll(); });
	connect(refresh, &QPushButton::clicked, this, [this]() {
		MmpCore::Get()->RefreshMonitors();
		Populate();
	});
	connect(chooseBtn, &QPushButton::clicked, this, &DetectDialog::Choose);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(table, &QTableWidget::cellDoubleClicked, this, [this](int, int) { Choose(); });
	connect(table, &QTableWidget::itemSelectionChanged, this, [this]() {
		auto sel = table->selectedItems();
		bool ok = !sel.isEmpty() && (sel.first()->flags() & Qt::ItemIsEnabled);
		chooseBtn->setEnabled(ok);
	});

	MmpCore::Get()->RefreshMonitors();
	Populate();
}

void DetectDialog::Populate()
{
	MmpCore *core = MmpCore::Get();
	const auto &mons = core->Monitors();
	const MonitorInfo *mainMon = core->MainMonitor();
	table->setRowCount(0);
	chooseBtn->setEnabled(false);

	for (size_t i = 0; i < mons.size(); i++) {
		const MonitorInfo &m = mons[i];
		QString status;
		bool disabled = false;
		if (rowId != 0) {
			if (&m == mainMon) {
				status = T("主螢幕");
				disabled = true;
			} else if (core->IsDuplicateRes(rowId, m.w, m.h)) {
				status = T("已設定");
				disabled = true;
			}
		} else if (&m == mainMon) {
			status = T("目前主螢幕");
		}

		int r = table->rowCount();
		table->insertRow(r);
		auto mk = [&](const QString &t) {
			auto *it = new QTableWidgetItem(t);
			it->setData(Qt::UserRole, QSize(m.w, m.h));
			if (disabled)
				it->setFlags(it->flags() & ~Qt::ItemIsEnabled);
			return it;
		};
		table->setItem(r, 0, mk(m.name));
		table->setItem(r, 1, mk(QString("%1 × %2").arg(m.w).arg(m.h)));
		table->setItem(r, 2, mk(QString("%1 (%2, %3)").arg(core->PositionLabel(m)).arg(m.rect.x()).arg(m.rect.y())));
		table->setItem(r, 3, mk(status));

		auto *btn = new QPushButton(T("識別"));
		QScreen *scr = m.screen;
		QString text = QString("%1 × %2\n%3").arg(m.w).arg(m.h).arg(m.name);
		connect(btn, &QPushButton::clicked, this, [scr, text]() {
			if (scr)
				ShowIdentifyWindow(scr, text);
		});
		table->setCellWidget(r, 4, btn);
	}
	table->resizeColumnsToContents();
	table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
}

void DetectDialog::Choose()
{
	auto sel = table->selectedItems();
	if (sel.isEmpty() || !(sel.first()->flags() & Qt::ItemIsEnabled))
		return;
	QSize s = sel.first()->data(Qt::UserRole).toSize();
	selW = s.width();
	selH = s.height();
	accept();
}
