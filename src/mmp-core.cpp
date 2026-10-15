#include "mmp-core.hpp"
#include "mmp-projector.hpp"
#include "mmp-dialogs.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>
#include <plugin-support.h>

#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <QtGui/qscreen_platform.h>
#endif

static MmpCore *g_core = nullptr;

// 繪圖執行緒讀取用的參數快照
struct RenderParams {
	int ow = 0, oh = 0;
	double ax = 0.5, ay = 1.0;
	OutputMode mode = OutputMode::Source;
};
static std::mutex g_paramMutex;
static std::map<int, RenderParams> g_params;
static int g_mainW = 0, g_mainH = 0;

/* ------------------------------------------------------------------ */

RegionF ComputeSourceRegion(int sw, int sh, int ow, int oh, double ax, double ay)
{
	RegionF r;
	if (sw <= 0 || sh <= 0 || ow <= 0 || oh <= 0)
		return r;
	if (ow <= sw && oh <= sh) {
		r.w = ow;
		r.h = oh;
	} else {
		// 規則 A：取與輸出同比例、能放進主螢幕的最大範圍，再放大
		double scale = std::max(double(ow) / sw, double(oh) / sh);
		r.w = std::min<double>(sw, std::floor(ow / scale + 0.5));
		r.h = std::min<double>(sh, std::floor(oh / scale + 0.5));
	}
	ax = std::clamp(ax, 0.0, 1.0);
	ay = std::clamp(ay, 0.0, 1.0);
	r.x = std::floor((sw - r.w) * ax + 0.5);
	r.y = std::floor((sh - r.h) * ay + 0.5);
	return r;
}

bool ResMatches(int aw, int ah, int bw, int bh)
{
	if (aw <= 0 || ah <= 0 || bw <= 0 || bh <= 0)
		return false;
	return std::abs(aw - bw) <= 2 && std::abs(ah - bh) <= 2;
}

/* ------------------------------------------------------------------ */

MmpCore *MmpCore::Get()
{
	return g_core;
}

void MmpCore::Create()
{
	if (!g_core)
		g_core = new MmpCore();
}

void MmpCore::Destroy()
{
	delete g_core;
	g_core = nullptr;
}

MmpCore::MmpCore()
{
	debounce.setSingleShot(true);
	debounce.setInterval(2500);
	connect(&debounce, &QTimer::timeout, this, [this]() {
		RefreshMonitors();
		UpdateCaptureTarget();
		Reconcile(true);
	});

	poll.setInterval(4000);
	connect(&poll, &QTimer::timeout, this, [this]() {
		if (MonitorSignature() != lastSignature && !debounce.isActive())
			OnScreensChanged();
	});

	saveTimer.setSingleShot(true);
	saveTimer.setInterval(800);
	connect(&saveTimer, &QTimer::timeout, this, &MmpCore::Save);

	connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen *s) {
		ConnectScreen(s);
		OnScreensChanged();
	});
	connect(qApp, &QGuiApplication::screenRemoved, this, [this](QScreen *) { OnScreensChanged(); });
	connect(qApp, &QGuiApplication::primaryScreenChanged, this, [this](QScreen *) { OnScreensChanged(); });
	for (QScreen *s : QGuiApplication::screens())
		ConnectScreen(s);

	hkClose = obs_hotkey_register_frontend("mmp_close_all", "多螢幕投影：關閉全部投影", HotkeyCloseAll, this);
	hkReproject = obs_hotkey_register_frontend("mmp_reproject", "多螢幕投影：重新投放", HotkeyReproject, this);

	Load();
	RefreshMonitors();
}

MmpCore::~MmpCore()
{
	Shutdown();
	if (hkClose != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(hkClose);
	if (hkReproject != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(hkReproject);
}

void MmpCore::ConnectScreen(QScreen *s)
{
	connect(s, &QScreen::geometryChanged, this, [this](const QRect &) { OnScreensChanged(); });
}

/* ------------------------------ 列管理 ------------------------------ */

OutputRow *MmpCore::FindRow(int id)
{
	for (auto &r : rows)
		if (r.id == id)
			return &r;
	return nullptr;
}

int MmpCore::AddRow()
{
	OutputRow r;
	r.id = nextId++;
	r.name = T("螢幕 %1").arg(QChar('A' + int(rows.size() % 26)));
	rows.push_back(r);
	ConfigEdited(true);
	return r.id;
}

void MmpCore::RemoveRow(int id)
{
	auto it = windows.find(id);
	if (it != windows.end()) {
		for (auto &w : it->second)
			delete w.data();
		windows.erase(it);
	}
	rows.erase(std::remove_if(rows.begin(), rows.end(), [id](const OutputRow &r) { return r.id == id; }),
		   rows.end());
	ConfigEdited(true);
}

bool MmpCore::IsDuplicateRes(int rowId, int w, int h) const
{
	if (w <= 0 || h <= 0)
		return false;
	for (auto &r : rows)
		if (r.id != rowId && r.w == w && r.h == h)
			return true;
	return false;
}

/* ------------------------------ 螢幕偵測 ------------------------------ */

#ifdef _WIN32
static QString monitor_friendly_name(const wchar_t *gdiName)
{
	UINT32 numPath = 0, numMode = 0;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &numPath, &numMode) != ERROR_SUCCESS || !numPath)
		return QString();
	std::vector<DISPLAYCONFIG_PATH_INFO> paths(numPath);
	std::vector<DISPLAYCONFIG_MODE_INFO> modes(numMode);
	if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &numPath, paths.data(), &numMode, modes.data(), nullptr) !=
	    ERROR_SUCCESS)
		return QString();
	for (UINT32 i = 0; i < numPath; i++) {
		DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
		src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		src.header.size = sizeof(src);
		src.header.adapterId = paths[i].sourceInfo.adapterId;
		src.header.id = paths[i].sourceInfo.id;
		if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || wcscmp(gdiName, src.viewGdiDeviceName))
			continue;
		DISPLAYCONFIG_TARGET_DEVICE_NAME tgt = {};
		tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
		tgt.header.size = sizeof(tgt);
		tgt.header.adapterId = paths[i].sourceInfo.adapterId;
		tgt.header.id = paths[i].targetInfo.id;
		if (DisplayConfigGetDeviceInfo(&tgt.header) == ERROR_SUCCESS)
			return QString::fromWCharArray(tgt.monitorFriendlyDeviceName);
	}
	return QString();
}
#endif

void MmpCore::RefreshMonitors()
{
	monitors.clear();
	for (QScreen *s : QGuiApplication::screens()) {
		MonitorInfo m;
		m.screen = s;
		qreal dpr = s->devicePixelRatio();
		m.w = int(s->geometry().width() * dpr + 0.5);
		m.h = int(s->geometry().height() * dpr + 0.5);
		m.rect = QRect(s->geometry().topLeft(), QSize(m.w, m.h));
		m.name = s->model().isEmpty() ? s->name() : s->model();

#ifdef _WIN32
		auto *native = s->nativeInterface<QNativeInterface::QWindowsScreen>();
		HMONITOR hmon = native ? native->handle() : nullptr;
		MONITORINFOEXW mi = {};
		mi.cbSize = sizeof(mi);
		if (hmon && GetMonitorInfoW(hmon, (LPMONITORINFO)&mi)) {
			m.rect = QRect(mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
				       mi.rcMonitor.bottom - mi.rcMonitor.top);
			DEVMODEW dm = {};
			dm.dmSize = sizeof(dm);
			if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
				m.w = (int)dm.dmPelsWidth;
				m.h = (int)dm.dmPelsHeight;
			} else {
				m.w = m.rect.width();
				m.h = m.rect.height();
			}
			QString friendly = monitor_friendly_name(mi.szDevice);
			if (!friendly.isEmpty())
				m.name = friendly;

			DISPLAY_DEVICEW dev = {};
			dev.cb = sizeof(dev);
			if (EnumDisplayDevicesW(mi.szDevice, 0, &dev, EDD_GET_DEVICE_INTERFACE_NAME) && dev.DeviceID[0])
				m.deviceId = QString::fromWCharArray(dev.DeviceID);
			else
				m.deviceId = QString::fromWCharArray(mi.szDevice);
		}
#endif
		if (m.name.isEmpty())
			m.name = T("未知螢幕");
		m.key = QString("%1|%2x%3|%4,%5").arg(m.deviceId).arg(m.w).arg(m.h).arg(m.rect.x()).arg(m.rect.y());
		monitors.push_back(m);
	}
	// 依位置排序，讓「第一台」較穩定
	std::sort(monitors.begin(), monitors.end(), [](const MonitorInfo &a, const MonitorInfo &b) {
		if (a.rect.y() != b.rect.y())
			return a.rect.y() < b.rect.y();
		return a.rect.x() < b.rect.x();
	});
	lastSignature = MonitorSignature();
	emit MonitorsChanged();
	emit StatusChanged();
}

QString MmpCore::MonitorSignature() const
{
	QStringList parts;
	for (QScreen *s : QGuiApplication::screens()) {
		qreal dpr = s->devicePixelRatio();
		parts << QString("%1:%2x%3@%4,%5")
				 .arg(s->name())
				 .arg(int(s->geometry().width() * dpr + 0.5))
				 .arg(int(s->geometry().height() * dpr + 0.5))
				 .arg(s->geometry().x())
				 .arg(s->geometry().y());
	}
	parts.sort();
	return parts.join(';');
}

const MonitorInfo *MmpCore::MainMonitor() const
{
	for (auto &m : monitors)
		if (ResMatches(m.w, m.h, mainW, mainH))
			return &m;
	return nullptr;
}

std::vector<const MonitorInfo *> MmpCore::MatchRow(const OutputRow &row) const
{
	std::vector<const MonitorInfo *> out;
	const MonitorInfo *mainMon = MainMonitor();
	for (auto &m : monitors) {
		if (&m == mainMon)
			continue;
		if (!ResMatches(m.w, m.h, row.w, row.h))
			continue;
		out.push_back(&m);
		if (!row.applySame)
			break;
	}
	return out;
}

QString MmpCore::PositionLabel(const MonitorInfo &m) const
{
	if (monitors.size() <= 1)
		return T("唯一");
	QRect bound;
	for (auto &o : monitors)
		bound = bound.united(o.rect);
	double cx = (m.rect.center().x() - bound.left()) / double(std::max(1, bound.width()));
	double cy = (m.rect.center().y() - bound.top()) / double(std::max(1, bound.height()));
	static const char *names[3][3] = {{"左上", "中上", "右上"}, {"左中", "置中", "右中"}, {"左下", "中下", "右下"}};
	int col = cx < 0.34 ? 0 : (cx > 0.66 ? 2 : 1);
	int row = cy < 0.34 ? 0 : (cy > 0.66 ? 2 : 1);
	return T(names[row][col]);
}

/* ------------------------------ 狀態 ------------------------------ */

QString MmpCore::MainStatus() const
{
	if (mainW <= 0 || mainH <= 0)
		return T("未設定解析度");
	const MonitorInfo *m = MainMonitor();
	if (!m)
		return T("未接上");
	return T("已接上：%1").arg(m->name);
}

QString MmpCore::RowStatus(const OutputRow &row) const
{
	if (!row.enabled)
		return T("停用");
	if (row.w <= 0 || row.h <= 0)
		return T("未設定解析度");
	QString extra;
	if (row.mode == OutputMode::Scene) {
		if (row.scene.isEmpty())
			return T("未選場景");
		obs_source_t *sc = obs_get_source_by_name(row.scene.toUtf8().constData());
		if (!sc)
			return T("找不到場景");
		obs_source_release(sc);
		obs_video_info ovi;
		if (obs_get_video_info(&ovi) && ((int)ovi.base_width < row.w || (int)ovi.base_height < row.h))
			extra = T("（畫布不足，需 %1×%2 以上）").arg(row.w).arg(row.h);
	}
	auto matches = MatchRow(row);
	if (matches.empty())
		return T("未接上") + extra;
	int n = 0;
	auto it = windows.find(row.id);
	if (it != windows.end())
		for (auto &w : it->second)
			if (w && w->isVisible())
				n++;
	if (n == 0)
		return T("已接上（未投放）") + extra;
	if (n == 1)
		return T("投影中") + extra;
	return T("投影中 ×%1").arg(n) + extra;
}

/* ------------------------------ 擷取來源 ------------------------------ */

void MmpCore::UpdateCaptureTarget()
{
	if (!capture)
		return;
	const MonitorInfo *m = MainMonitor();
	if (!m || m->deviceId.isEmpty() || m->deviceId == captureDeviceId)
		return;
	captureDeviceId = m->deviceId;
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "monitor_id", captureDeviceId.toUtf8().constData());
	obs_data_set_int(s, "method", 1); // DXGI
	obs_data_set_bool(s, "capture_cursor", false);
	obs_source_update(capture, s);
	obs_data_release(s);
	obs_log(LOG_INFO, "capture -> %s (%dx%d)", m->name.toUtf8().constData(), m->w, m->h);
}

/* ------------------------------ 投影管理 ------------------------------ */

void MmpCore::Startup()
{
	if (started)
		return;
	started = true;

	obs_data_t *s = obs_data_create();
	obs_data_set_int(s, "method", 1);
	obs_data_set_bool(s, "capture_cursor", false);
	capture = obs_source_create_private("monitor_capture", "MMP 主螢幕擷取", s);
	obs_data_release(s);
	if (!capture)
		obs_log(LOG_WARNING, "無法建立顯示器擷取來源");

	RefreshMonitors();
	UpdateCaptureTarget();
	for (auto &r : rows)
		r.wanted = r.autoProject;
	PushParams();
	Reconcile(false);
	poll.start();
}

void MmpCore::Shutdown()
{
	poll.stop();
	debounce.stop();
	saveTimer.stop();
	if (started)
		Save();
	for (auto &kv : windows)
		for (auto &w : kv.second)
			delete w.data();
	windows.clear();
	if (capture) {
		obs_source_release(capture);
		capture = nullptr;
	}
	started = false;
}

void MmpCore::ReleaseScenes()
{
	for (auto &kv : windows) {
		OutputRow *row = FindRow(kv.first);
		if (!row || row->mode != OutputMode::Scene)
			continue;
		for (auto &w : kv.second)
			delete w.data();
		kv.second.clear();
	}
	emit StatusChanged();
}

void MmpCore::ProjectAll()
{
	for (auto &r : rows)
		r.wanted = true;
	RefreshMonitors();
	UpdateCaptureTarget();
	Reconcile(false);
}

void MmpCore::CloseAll()
{
	for (auto &r : rows)
		r.wanted = false;
	Reconcile(false);
}

void MmpCore::PushParams()
{
	std::lock_guard<std::mutex> lock(g_paramMutex);
	g_params.clear();
	g_mainW = mainW;
	g_mainH = mainH;
	for (auto &r : rows) {
		RenderParams p;
		p.ow = r.w;
		p.oh = r.h;
		p.ax = r.ax;
		p.ay = r.ay;
		p.mode = r.mode;
		g_params[r.id] = p;
	}
}

void MmpCore::GetRenderParams(int rowId, int &sw, int &sh, int &ow, int &oh, double &ax, double &ay,
			      OutputMode &mode)
{
	std::lock_guard<std::mutex> lock(g_paramMutex);
	sw = g_mainW;
	sh = g_mainH;
	auto it = g_params.find(rowId);
	if (it == g_params.end()) {
		ow = oh = 0;
		ax = 0.5;
		ay = 1.0;
		mode = OutputMode::Source;
		return;
	}
	ow = it->second.ow;
	oh = it->second.oh;
	ax = it->second.ax;
	ay = it->second.ay;
	mode = it->second.mode;
}

void MmpCore::ConfigEdited(bool structural)
{
	PushParams();
	saveTimer.start();
	if (structural)
		emit RowsReset();
	if (started) {
		UpdateCaptureTarget();
		Reconcile(false);
	} else {
		emit StatusChanged();
	}
}

void MmpCore::Reconcile(bool recovery)
{
	if (!started) {
		emit StatusChanged();
		return;
	}

	// 清掉已失效的列
	for (auto it = windows.begin(); it != windows.end();) {
		if (!FindRow(it->first)) {
			for (auto &w : it->second)
				delete w.data();
			it = windows.erase(it);
		} else {
			++it;
		}
	}

	for (auto &row : rows) {
		auto &list = windows[row.id];
		list.erase(std::remove_if(list.begin(), list.end(), [](const QPointer<ProjectorWindow> &w) { return !w; }),
			   list.end());

		std::vector<QScreen *> desired;
		obs_source_t *src = nullptr;
		bool valid = row.enabled && row.wanted && row.w > 0 && row.h > 0;
		if (valid) {
			if (row.mode == OutputMode::Source) {
				src = obs_source_get_ref(capture);
			} else if (!row.scene.isEmpty()) {
				src = obs_get_source_by_name(row.scene.toUtf8().constData());
			}
			if (!src)
				valid = false;
		}
		if (valid) {
			for (auto *m : MatchRow(row))
				desired.push_back(m->screen);
			if (recovery && !row.autoProject) {
				// 未勾自動投放：只保留原本還在的投影，不自動補回
				std::vector<QScreen *> keep;
				for (QScreen *s : desired)
					for (auto &w : list)
						if (w && w->TargetScreen() == s)
							keep.push_back(s);
				desired = keep;
			}
		}

		// 移除不需要的、放錯位置的、來源不同的
		for (auto &w : list) {
			if (!w)
				continue;
			bool keep = std::find(desired.begin(), desired.end(), w->TargetScreen()) != desired.end() &&
				    w->Source() == src && !w->IsMisplaced();
			if (!keep) {
				delete w.data();
				w = nullptr;
			}
		}
		list.erase(std::remove_if(list.begin(), list.end(), [](const QPointer<ProjectorWindow> &w) { return !w; }),
			   list.end());

		// 建立缺少的
		for (QScreen *s : desired) {
			bool exists = false;
			for (auto &w : list)
				if (w->TargetScreen() == s)
					exists = true;
			if (exists)
				continue;
			auto *w = new ProjectorWindow(row.id, s, src);
			connect(w, &ProjectorWindow::Misplaced, this, &MmpCore::OnScreensChanged);
			list.push_back(w);
		}
		obs_source_release(src);
	}
	emit StatusChanged();
}

void MmpCore::ImmediateGuard()
{
	// 螢幕一有變動，先把跑到錯誤螢幕（例如主螢幕）上的投影藏起來
	QScreen *mainScreen = nullptr;
	if (const MonitorInfo *m = MainMonitor())
		mainScreen = m->screen;
	const auto screens = QGuiApplication::screens();
	for (auto &kv : windows) {
		for (auto &w : kv.second) {
			if (!w)
				continue;
			QScreen *t = w->TargetScreen();
			bool gone = !t || !screens.contains(t);
			bool onMain = w->windowHandle() && mainScreen && w->windowHandle()->screen() == mainScreen &&
				      t != mainScreen;
			if (gone || onMain || w->IsMisplaced())
				w->hide();
		}
	}
}

void MmpCore::OnScreensChanged()
{
	ImmediateGuard();
	debounce.start();
	emit StatusChanged();
}

/* ------------------------------ 識別螢幕 ------------------------------ */

void MmpCore::IdentifyAll()
{
	RefreshMonitors();
	for (auto &m : monitors)
		IdentifyOne(m);
}

void MmpCore::IdentifyOne(const MonitorInfo &m)
{
	if (!m.screen)
		return;
	QString text = QString("%1 × %2\n%3").arg(m.w).arg(m.h).arg(m.name);
	if (MainMonitor() == &m)
		text += T("\n（主螢幕）");
	ShowIdentifyWindow(m.screen, text);
}

/* ------------------------------ 快速鍵 ------------------------------ */

void MmpCore::HotkeyCloseAll(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	auto *core = static_cast<MmpCore *>(data);
	QMetaObject::invokeMethod(core, [core]() { core->CloseAll(); }, Qt::QueuedConnection);
}

void MmpCore::HotkeyReproject(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	auto *core = static_cast<MmpCore *>(data);
	QMetaObject::invokeMethod(core, [core]() { core->ProjectAll(); }, Qt::QueuedConnection);
}

/* ------------------------------ 存檔 ------------------------------ */

obs_data_t *MmpCore::SaveToData(bool withHotkeys)
{
	obs_data_t *d = obs_data_create();
	obs_data_set_int(d, "version", 1);
	obs_data_set_int(d, "main_w", mainW);
	obs_data_set_int(d, "main_h", mainH);
	obs_data_array_t *arr = obs_data_array_create();
	for (auto &r : rows) {
		obs_data_t *o = obs_data_create();
		obs_data_set_bool(o, "enabled", r.enabled);
		obs_data_set_string(o, "name", r.name.toUtf8().constData());
		obs_data_set_int(o, "w", r.w);
		obs_data_set_int(o, "h", r.h);
		obs_data_set_int(o, "mode", (int)r.mode);
		obs_data_set_string(o, "scene", r.scene.toUtf8().constData());
		obs_data_set_bool(o, "apply_same", r.applySame);
		obs_data_set_bool(o, "auto", r.autoProject);
		obs_data_set_double(o, "ax", r.ax);
		obs_data_set_double(o, "ay", r.ay);
		obs_data_array_push_back(arr, o);
		obs_data_release(o);
	}
	obs_data_set_array(d, "rows", arr);
	obs_data_array_release(arr);

	if (withHotkeys) {
		obs_data_array_t *a = obs_hotkey_save(hkClose);
		obs_data_set_array(d, "hotkey_close_all", a);
		obs_data_array_release(a);
		a = obs_hotkey_save(hkReproject);
		obs_data_set_array(d, "hotkey_reproject", a);
		obs_data_array_release(a);
	}
	return d;
}

void MmpCore::LoadFromData(obs_data_t *d, bool withHotkeys)
{
	mainW = (int)obs_data_get_int(d, "main_w");
	mainH = (int)obs_data_get_int(d, "main_h");
	rows.clear();
	obs_data_array_t *arr = obs_data_get_array(d, "rows");
	size_t n = obs_data_array_count(arr);
	for (size_t i = 0; i < n; i++) {
		obs_data_t *o = obs_data_array_item(arr, i);
		obs_data_set_default_bool(o, "enabled", true);
		obs_data_set_default_bool(o, "auto", true);
		obs_data_set_default_double(o, "ax", 0.5);
		obs_data_set_default_double(o, "ay", 1.0);
		OutputRow r;
		r.id = nextId++;
		r.enabled = obs_data_get_bool(o, "enabled");
		r.name = QString::fromUtf8(obs_data_get_string(o, "name"));
		r.w = (int)obs_data_get_int(o, "w");
		r.h = (int)obs_data_get_int(o, "h");
		r.mode = obs_data_get_int(o, "mode") == 1 ? OutputMode::Scene : OutputMode::Source;
		r.scene = QString::fromUtf8(obs_data_get_string(o, "scene"));
		r.applySame = obs_data_get_bool(o, "apply_same");
		r.autoProject = obs_data_get_bool(o, "auto");
		r.ax = obs_data_get_double(o, "ax");
		r.ay = obs_data_get_double(o, "ay");
		r.wanted = r.autoProject;
		rows.push_back(r);
		obs_data_release(o);
	}
	obs_data_array_release(arr);

	if (withHotkeys) {
		obs_data_array_t *a = obs_data_get_array(d, "hotkey_close_all");
		if (a) {
			obs_hotkey_load(hkClose, a);
			obs_data_array_release(a);
		}
		a = obs_data_get_array(d, "hotkey_reproject");
		if (a) {
			obs_hotkey_load(hkReproject, a);
			obs_data_array_release(a);
		}
	}
}

static QString config_file()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *p = obs_module_config_path("settings.json");
	QString s = QString::fromUtf8(p ? p : "");
	bfree(p);
	return s;
}

void MmpCore::Load()
{
	obs_data_t *d = obs_data_create_from_json_file_safe(config_file().toUtf8().constData(), "bak");
	if (!d) {
		// 預設一列
		OutputRow r;
		r.id = nextId++;
		r.name = T("螢幕 A");
		rows.push_back(r);
		PushParams();
		return;
	}
	LoadFromData(d, true);
	obs_data_release(d);
	PushParams();
}

void MmpCore::Save()
{
	obs_data_t *d = SaveToData(true);
	obs_data_save_json_safe(d, config_file().toUtf8().constData(), "tmp", "bak");
	obs_data_release(d);
}

bool MmpCore::ExportTo(const QString &path)
{
	obs_data_t *d = SaveToData(false);
	bool ok = obs_data_save_json_pretty_safe(d, path.toUtf8().constData(), "tmp", "bak");
	obs_data_release(d);
	return ok;
}

bool MmpCore::ImportFrom(const QString &path)
{
	obs_data_t *d = obs_data_create_from_json_file(path.toUtf8().constData());
	if (!d)
		return false;
	for (auto &kv : windows)
		for (auto &w : kv.second)
			delete w.data();
	windows.clear();
	LoadFromData(d, false);
	obs_data_release(d);
	for (auto &r : rows)
		r.wanted = r.autoProject;
	ConfigEdited(true);
	return true;
}
