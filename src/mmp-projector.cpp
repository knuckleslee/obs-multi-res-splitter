#include "mmp-projector.hpp"
#include "mmp-core.hpp"

#include <QWindow>
#include <algorithm>

ProjectorWindow::ProjectorWindow(int rowId_, QScreen *screen, obs_source_t *src)
	: MmpDisplay(nullptr, Qt::Window | Qt::FramelessWindowHint | Qt::Tool),
	  rowId(rowId_),
	  target(screen),
	  source(obs_source_get_ref(src))
{
	setAttribute(Qt::WA_DeleteOnClose, false);
	setAttribute(Qt::WA_QuitOnClose, false);
	setCursor(Qt::BlankCursor);
	setWindowTitle(T("多螢幕投影"));

	if (source)
		obs_source_inc_showing(source);

	winId(); // 建立原生視窗
	if (windowHandle()) {
		windowHandle()->setScreen(screen);
		connect(windowHandle(), &QWindow::screenChanged, this, [this](QScreen *s) {
			if (s != target) {
				hide();
				emit Misplaced();
			}
		});
	}
	setGeometry(screen->geometry());
	showFullScreen();
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

bool ProjectorWindow::IsMisplaced() const
{
	if (!target)
		return true;
	if (windowHandle() && windowHandle()->screen() != target)
		return true;
	return false;
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
