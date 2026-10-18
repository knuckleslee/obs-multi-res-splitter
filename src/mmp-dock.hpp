#pragma once

#include <QWidget>
#include <map>

class QGridLayout;
class QLabel;
class QSpinBox;
class QComboBox;
class QPushButton;
class QCheckBox;

class MmpDock : public QWidget {
	Q_OBJECT

public:
	explicit MmpDock(QWidget *parent = nullptr);

	void RefreshScenes();

private:
	struct RowWidgets {
		QSpinBox *w = nullptr;
		QSpinBox *h = nullptr;
		QComboBox *mode = nullptr;
		QPushButton *align = nullptr;
		QLabel *status = nullptr;
	};

	void Rebuild();
	void RefreshStatus();
	void UpdateRowVisibility(int rowId);
	void EditRes(int rowId, bool isW);

	QGridLayout *grid = nullptr;
	QWidget *gridHost = nullptr;
	QSpinBox *mainW = nullptr;
	QSpinBox *mainH = nullptr;
	QLabel *mainStatus = nullptr;
	std::map<int, RowWidgets> rowWidgets;
};
