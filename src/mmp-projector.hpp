#pragma once

#include "mmp-display.hpp"

#include <QPointer>
#include <QRect>
#include <QScreen>

struct MonitorInfo;

// 全螢幕投影視窗：依所屬列的設定，從來源或場景取樣後輸出
class ProjectorWindow : public MmpDisplay {
	Q_OBJECT

public:
	ProjectorWindow(int rowId, const MonitorInfo &monitor, obs_source_t *source);
	~ProjectorWindow() override;

	int RowId() const { return rowId; }
	QScreen *TargetScreen() const { return target; }
	void *TargetMonitor() const { return hmon; }
	void *CurrentMonitor() const;
	obs_source_t *Source() const { return source; }

	// 以實體像素座標把視窗放到目標螢幕（不搶焦點）
	void Place(const MonitorInfo &monitor);

protected:
	void Render(uint32_t cx, uint32_t cy) override;

private:
	void ApplyNativePlacement();

	int rowId;
	QPointer<QScreen> target;
	void *hmon = nullptr;
	QRect rect; // 實體像素
	obs_source_t *source = nullptr;
};
