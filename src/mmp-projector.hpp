#pragma once

#include "mmp-display.hpp"

#include <QPointer>
#include <QScreen>

// 全螢幕投影視窗：依所屬列的設定，從來源或場景取樣後輸出
class ProjectorWindow : public MmpDisplay {
	Q_OBJECT

public:
	ProjectorWindow(int rowId, QScreen *screen, obs_source_t *source);
	~ProjectorWindow() override;

	int RowId() const { return rowId; }
	QScreen *TargetScreen() const { return target; }
	obs_source_t *Source() const { return source; }

	// 視窗目前是否在錯誤的螢幕上（例如目標螢幕斷線後被 Windows 搬走）
	bool IsMisplaced() const;

signals:
	void Misplaced();

protected:
	void Render(uint32_t cx, uint32_t cy) override;

private:
	int rowId;
	QPointer<QScreen> target;
	obs_source_t *source = nullptr;
};
