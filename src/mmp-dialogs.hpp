#pragma once

#include <QDialog>
#include <QString>

class QScreen;
class QTableWidget;
class QPushButton;

// 在每台螢幕（或只在 only 這台）顯示 Windows 螢幕排列縮圖，並標出自己是哪一台
void ShowIdentifyMap(QScreen *only);

// 偵測螢幕清單：選擇後回傳解析度
class DetectDialog : public QDialog {
	Q_OBJECT

public:
	// forRowId = 0 代表主螢幕列
	DetectDialog(int forRowId, QWidget *parent = nullptr);

	int SelectedW() const { return selW; }
	int SelectedH() const { return selH; }

private:
	void Populate();
	void Choose();

	int rowId;
	int selW = 0;
	int selH = 0;
	QTableWidget *table = nullptr;
	QPushButton *chooseBtn = nullptr;
};
