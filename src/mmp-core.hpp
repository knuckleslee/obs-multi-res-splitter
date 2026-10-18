#pragma once

#include <obs.h>
#include <obs-hotkey.h>

#include <QObject>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QTimer>

#include <map>
#include <vector>

class QScreen;
class ProjectorWindow;

inline QString T(const char *utf8)
{
	return QString::fromUtf8(utf8);
}

enum class OutputMode { Source = 0, Scene = 1 };

struct OutputRow {
	int id = 0;
	bool enabled = true;
	QString name;
	int w = 0;
	int h = 0;
	OutputMode mode = OutputMode::Source;
	QString scene;
	bool applySame = false;
	bool autoProject = true;
	double ax = 0.5; // 水平位置比例 0=左 0.5=中 1=右
	double ay = 1.0; // 垂直位置比例 0=上 0.5=中 1=下

	// runtime only
	bool wanted = true;
};

struct MonitorInfo {
	QScreen *screen = nullptr;
	void *hmon = nullptr; // HMONITOR
	int w = 0;
	int h = 0;
	QRect rect;       // 桌面座標（實體像素）
	QString name;     // 型號名稱
	QString deviceId; // OBS 顯示器擷取用的 monitor_id
	QString gdiName;  // Windows 裝置名稱，例如 DISPLAY1
	QString adapter;  // 接在哪張顯示卡
	QString key;      // 用於比對變動的識別字串
};

struct RegionF {
	double x = 0, y = 0, w = 0, h = 0;
};

// 來源模式的取樣範圍計算（主螢幕座標）
RegionF ComputeSourceRegion(int sw, int sh, int ow, int oh, double ax, double ay);

bool ResMatches(int aw, int ah, int bw, int bh);

class MmpCore : public QObject {
	Q_OBJECT

public:
	static MmpCore *Get();
	static void Create();
	static void Destroy();

	// 設定
	int mainW = 0;
	int mainH = 0;
	std::vector<OutputRow> rows;

	OutputRow *FindRow(int id);
	int AddRow();
	void RemoveRow(int id);
	bool IsDuplicateRes(int rowId, int w, int h) const;

	// 螢幕
	const std::vector<MonitorInfo> &Monitors() const { return monitors; }
	void RefreshMonitors();
	const MonitorInfo *MainMonitor() const;
	std::vector<const MonitorInfo *> MatchRow(const OutputRow &row) const;
	QString PositionLabel(const MonitorInfo &m) const;
	QString MonitorRoleName(const MonitorInfo &m) const; // 主螢幕／該列名稱／未設定

	obs_source_t *CaptureSource() const { return capture; }

	// 狀態
	QString RowStatus(const OutputRow &row) const;
	QString MainStatus() const;

	// 動作
	void Startup();
	void Shutdown();
	void ProjectAll();
	void CloseAll();
	void ConfigEdited(bool structural = false);
	void Reconcile(bool recovery);
	void PushParams();
	void ReleaseScenes();
	void IdentifyAll();
	void IdentifyOne(const MonitorInfo &m);

	// 存檔
	void Load();
	void Save();
	bool ExportTo(const QString &path);
	bool ImportFrom(const QString &path);

	void GetRenderParams(int rowId, int &sw, int &sh, int &ow, int &oh, double &ax, double &ay, OutputMode &mode);

signals:
	void MonitorsChanged();
	void StatusChanged();
	void RowsReset();

private:
	MmpCore();
	~MmpCore();

	void OnScreensChanged();
	void ImmediateGuard();
	void UpdateCaptureTarget();
	void ConnectScreen(QScreen *s);
	void LoadFromData(obs_data_t *data, bool withHotkeys);
	obs_data_t *SaveToData(bool withHotkeys);
	QString MonitorSignature() const;

	static void SourceRenamed(void *data, calldata_t *cd);
	static void HotkeyCloseAll(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed);
	static void HotkeyReproject(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed);

	std::vector<MonitorInfo> monitors;
	obs_source_t *capture = nullptr;
	QString captureDeviceId;
	int nextId = 1;
	bool started = false;

	std::map<int, std::vector<QPointer<ProjectorWindow>>> windows;

	QTimer debounce;
	QTimer poll;
	QTimer saveTimer;
	QString lastSignature;

	obs_hotkey_id hkClose = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hkReproject = OBS_INVALID_HOTKEY_ID;
};
