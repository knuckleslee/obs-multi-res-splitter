#include "mmp-projector.hpp"
#include "mmp-core.hpp"

#include <QTimer>
#include <QWindow>
#include <algorithm>

#ifdef _WIN32
#include <Windows.h>
#endif

ProjectorWindow::ProjectorWindow(int rowId_, const MonitorInfo &m, obs_source_t *src)
	: MmpDisplay(nullptr, Qt::Window | Qt::FramelessWindowHint | Qt::Tool | Qt::WindowDoesNotAcceptFocus),
	  rowId(rowId_),
	  target(m.screen),
	  hmon(m.hmon),
	  rect(m.rect),
	  source(obs_source_get_ref(src))
{
	setAttribute(Qt::WA_DeleteOnClose, false);
	setAttribute(Qt::WA_QuitOnClose, false);
	setAttribute(Qt::WA_ShowWithoutActivating);
	setFocusPolicy(Qt::NoFocus);
	setCursor(Qt::BlankCursor);
	setWindowTitle(T("多螢幕投影"));

	if (source)
		obs_source_inc_showing(source);

	winId(); // 建立原生視窗
#ifdef _WIN32
	HWND hwnd = (HWND)winId();
	LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
#endif
	if (target) {
		if (windowHandle())
			windowHandle()->setScreen(target);
		setGeometry(target->geometry());
	}
	show();
	ApplyNativePlacement();
	// Qt 可能在螢幕縮放比例切換後再調整一次大小，稍後再校正
	QTimer::singleShot(300, this, [this]() { ApplyNativePlacement(); });
}

ProjectorWindow::~ProjectorWindow()
{
	DestroyDisplay();
	if (source) {
		obs_source_dec_showing(source);
		obs_source_release(source);
		source = nullptr;
	}
}

void ProjectorWindow::ApplyNativePlacement()
{
#ifdef _WIN32
	if (rect.isEmpty())
		return;
	HWND hwnd = (HWND)winId();
	RECT cur;
	GetWindowRect(hwnd, &cur);
	if (cur.left == rect.x() && cur.top == rect.y() && cur.right - cur.left == rect.width() &&
	    cur.bottom - cur.top == rect.height() && IsWindowVisible(hwnd))
		return;
	SetWindowPos(hwnd, HWND_TOP, rect.x(), rect.y(), rect.width(), rect.height(),
		     SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
#endif
}

void ProjectorWindow::Place(const MonitorInfo &m)
{
	target = m.screen;
	hmon = m.hmon;
	rect = m.rect;
	if (isHidden()) {
		setAttribute(Qt::WA_ShowWithoutActivating);
		show();
	}
	ApplyNativePlacement();
}

void *ProjectorWindow::CurrentMonitor() const
{
#ifdef _WIN32
	return MonitorFromWindow((HWND)winId(), MONITOR_DEFAULTTONULL);
#else
	return nullptr;
#endif
}

void ProjectorWindow::Render(uint32_t cx, uint32_t cy)
{
	if (!source || cx == 0 || cy == 0)
		return;

	uint32_t srcW = obs_source_get_width(source);
	uint32_t srcH = obs_source_get_height(source);
	if (!srcW || !srcH)
		return;

	int sw, sh, ow, oh;
	double ax, ay;
	OutputMode mode;
	MmpCore::Get()->GetRenderParams(rowId, sw, sh, ow, oh, ax, ay, mode);
	if (ow <= 0 || oh <= 0)
		return;

	double rx, ry, rw, rh;
	if (mode == OutputMode::Source) {
		if (sw <= 0 || sh <= 0) {
			sw = (int)srcW;
			sh = (int)srcH;
		}
		RegionF r = ComputeSourceRegion(sw, sh, ow, oh, ax, ay);
		double kx = double(srcW) / sw;
		double ky = double(srcH) / sh;
		rx = r.x * kx;
		ry = r.y * ky;
		rw = r.w * kx;
		rh = r.h * ky;
	} else {
		// 場景模式：取畫布左上角與螢幕同尺寸的範圍
		rx = 0;
		ry = 0;
		rw = std::min<double>(ow, srcW);
		rh = std::min<double>(oh, srcH);
	}
	if (rw <= 0 || rh <= 0)
		return;

	double scale = std::min(double(cx) / rw, double(cy) / rh);
	int vw = int(rw * scale + 0.5);
	int vh = int(rh * scale + 0.5);
	int vx = (int(cx) - vw) / 2;
	int vy = (int(cy) - vh) / 2;

	MmpStartRegion(vx, vy, vw, vh, float(rx), float(rx + rw), float(ry), float(ry + rh));
	obs_source_video_render(source);
	MmpEndRegion();
}
