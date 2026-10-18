#pragma once

#include "mmp-display.hpp"

#include <QDialog>
#include <mutex>

class QSpinBox;
class QLabel;

// 對齊視窗裡的即時預覽：主螢幕畫面 + 紅框
class AlignPreview : public MmpDisplay {
	Q_OBJECT

public:
	AlignPreview(QWidget *parent);
	~AlignPreview() override;

	// 以主螢幕座標設定（sw/sh 為主螢幕解析度）
	void SetFrame(int sw, int sh, double rx, double ry, double rw, double rh);

signals:
	void Dragged(double rx, double ry); // 新的紅框左上角（主螢幕座標）

protected:
	void Render(uint32_t cx, uint32_t cy) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	QSize sizeHint() const override { return QSize(640, 240); }

private:
	bool MapToMain(const QPointF &widgetPos, double &x, double &y);

	obs_source_t *source = nullptr;
	std::mutex mtx;
	int sw = 0, sh = 0;
	double rx = 0, ry = 0, rw = 0, rh = 0;
	// 上一次繪製的映射（實體像素）
	double viewX = 0, viewY = 0, viewScale = 1;

	bool dragging = false;
	double grabDX = 0, grabDY = 0;
};

class QButtonGroup;

class AlignDialog : public QDialog {
	Q_OBJECT

public:
	AlignDialog(int rowId, QWidget *parent = nullptr);

	void reject() override;

private:
	void SetRatios(double ax, double ay);
	void UpdateFromRow();
	void OnSpinEdited(int which);

	int rowId;
	double origAx, origAy;
	int sw = 0, sh = 0, ow = 0, oh = 0;
	double rw = 0, rh = 0;
	bool updating = false;

	AlignPreview *preview = nullptr;
	QSpinBox *spinX = nullptr, *spinY = nullptr;
	QSpinBox *spinL = nullptr, *spinR = nullptr, *spinT = nullptr, *spinB = nullptr;
	QButtonGroup *gridGroup = nullptr;
	QLabel *info = nullptr;
};
