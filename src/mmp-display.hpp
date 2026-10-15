#pragma once

#include <obs.h>

#include <QWidget>

// 以 libobs 繪製畫面的 Qt 視窗基底（簡化自 OBS 的 OBSQTDisplay）
class MmpDisplay : public QWidget {
	Q_OBJECT

public:
	explicit MmpDisplay(QWidget *parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags());
	~MmpDisplay() override;

	QPaintEngine *paintEngine() const override { return nullptr; }

protected:
	// 在繪圖執行緒呼叫，cx/cy 為實體像素
	virtual void Render(uint32_t cx, uint32_t cy) = 0;

	void DestroyDisplay();

	void paintEvent(QPaintEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

	QSize PixelSize() const;

private:
	void CreateDisplay();
	static void DrawCallback(void *data, uint32_t cx, uint32_t cy);

	obs_display_t *display = nullptr;
};

// 繪圖輔助
void MmpStartRegion(int vx, int vy, int vcx, int vcy, float l, float r, float t, float b);
void MmpEndRegion();
void MmpDrawRectOutline(float x, float y, float w, float h, float thickness, uint32_t rgba);
void MmpDrawRectFill(float x, float y, float w, float h, uint32_t rgba);
