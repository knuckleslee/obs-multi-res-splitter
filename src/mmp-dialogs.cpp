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
#include <QPainter>
#include <QPointer>
#include <QWindow>
#include <algorithm>

#ifdef _WIN32
#include <Windows.h>
#endif

class IdentifyMapWindow : public QWidget {
public:
	explicit IdentifyMapWindow(QScreen *scr)
		: QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool |
					   Qt::WindowDoesNotAcceptFocus),
		  screen(scr)
	{
		setAttribute(Qt::WA_DeleteOnClose);
		setAttribute(Qt::WA_ShowWithoutActivating);
		setAttribute(Qt::WA_TranslucentBackground);
		setFocusPolicy(Qt::NoFocus);
		// Windows 排列一變動就重畫；自己這台不見了就關閉
		conn = QObject::connect(MmpCore::Get(), &MmpCore::MonitorsChanged, this, [this]() {
			if (!screen || !Self()) {
				close();
				return;
			}
			Place();
			update();
		});
		QTimer::singleShot(3000, this, &QWidget::close);
		Place();
		show();
	}
	~IdentifyMapWindow() override { QObject::disconnect(conn); }

protected:
	void paintEvent(QPaintEvent *) override
	{
		const MonitorInfo *me = Self();
		const auto &mons = MmpCore::Get()->Monitors();
		if (!me || mons.empty())
			return;
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);

		QRectF all(rect());
		p.setPen(QPen(QColor(255, 80, 80), 4));
		p.setBrush(QColor(15, 15, 15, 235));
		p.drawRoundedRect(all.adjusted(2, 2, -2, -2), 14, 14);

		// 標題：這台的名稱與解析度
		double hdr = all.height() * 0.16;
		QFont f = font();
		f.setPixelSize(int(hdr * 0.55));
		f.setBold(true);
		p.setFont(f);
		p.setPen(Qt::white);
		QString title = QString("%1　%2 × %3").arg(MmpCore::Get()->MonitorRoleName(*me)).arg(me->w).arg(me->h);
		p.drawText(QRectF(0, hdr * 0.2, all.width(), hdr), Qt::AlignCenter, title);

		// 縮圖區
		QRect bound;
		for (auto &m : mons)
			bound = bound.united(m.rect);
		QRectF area = all.adjusted(all.width() * 0.05, hdr * 1.4, -all.width() * 0.05, -all.height() * 0.06);
		double k = std::min(area.width() / bound.width(), area.height() / bound.height());
		double ox = area.left() + (area.width() - bound.width() * k) / 2;
		double oy = area.top() + (area.height() - bound.height() * k) / 2;

		for (auto &m : mons) {
			bool self = (&m == me);
			QRectF r(ox + (m.rect.x() - bound.x()) * k, oy + (m.rect.y() - bound.y()) * k, m.rect.width() * k,
				 m.rect.height() * k);
			r.adjust(2, 2, -2, -2);
			p.setPen(QPen(self ? QColor(255, 140, 140) : QColor(120, 120, 120), self ? 4 : 2));
			p.setBrush(self ? QColor(215, 55, 55) : QColor(40, 40, 40));
			p.drawRoundedRect(r, 6, 6);

			QString name = MmpCore::Get()->MonitorRoleName(m);
			QString res = QString("%1 × %2").arg(m.w).arg(m.h);
			int px = std::max(10, int(std::min(r.height() * 0.2, r.width() * 0.1)));
			QFont nf = font();
			nf.setPixelSize(px);
			nf.setBold(true);
			QFont rf = nf;
			rf.setBold(false);
			rf.setPixelSize(std::max(9, int(px * 0.8)));
			p.setPen(self ? Qt::white : QColor(200, 200, 200));
			p.setFont(nf);
			p.drawText(QRectF(r.left(), r.center().y() - px * 1.15, r.width(), px * 1.2), Qt::AlignCenter, name);
			p.setPen(self ? QColor(255, 225, 225) : QColor(150, 150, 150));
			p.setFont(rf);
			p.drawText(QRectF(r.left(), r.center().y() + px * 0.05, r.width(), px * 1.2), Qt::AlignCenter, res);
		}
	}

private:
	const MonitorInfo *Self() const
	{
		for (auto &m : MmpCore::Get()->Monitors())
			if (m.screen == screen)
				return &m;
		return nullptr;
	}

	// 以實體像素把視窗放在這台螢幕中央，大小約為螢幕的 60%
	void Place()
	{
		const MonitorInfo *me = Self();
		if (!me)
			return;
		QRect mr = me->rect;
		int w = mr.width() * 6 / 10, h = mr.height() * 6 / 10;
		QRect r(mr.x() + (mr.width() - w) / 2, mr.y() + (mr.height() - h) / 2, w, h);
		winId();
		if (windowHandle())
			windowHandle()->setScreen(screen);
		qreal dpr = screen ? screen->devicePixelRatio() : 1.0;
		setGeometry(QRect(screen->geometry().topLeft() + QPoint(int((r.x() - mr.x()) / dpr), int((r.y() - mr.y()) / dpr)),
				  QSize(int(w / dpr), int(h / dpr))));
#ifdef _WIN32
		SetWindowPos((HWND)winId(), HWND_TOPMOST, r.x(), r.y(), r.width(), r.height(),
			     SWP_NOACTIVATE | SWP_SHOWWINDOW);
#endif
	}

	QPointer<QScreen> screen;
	QMetaObject::Connection conn;
};

static QList<QPointer<IdentifyMapWindow>> g_identify;

void ShowIdentifyMap(QScreen *only)
{
	for (auto &w : g_identify)
		if (w)
			w->close();
	g_identify.clear();
	for (auto &m : MmpCore::Get()->Monitors()) {
		if (!m.screen || (only && m.screen != only))
			continue;
		g_identify.append(new IdentifyMapWindow(m.screen));
	}
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
		connect(btn, &QPushButton::clicked, this, [scr]() {
			if (scr)
				ShowIdentifyMap(scr);
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
