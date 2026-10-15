#include "mmp-display.hpp"

#include <graphics/vec4.h>

#include <QWindow>
#include <QResizeEvent>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

MmpDisplay::MmpDisplay(QWidget *parent, Qt::WindowFlags flags) : QWidget(parent, flags)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);
}

MmpDisplay::~MmpDisplay()
{
	DestroyDisplay();
}

void MmpDisplay::DestroyDisplay()
{
	if (display) {
		obs_display_remove_draw_callback(display, DrawCallback, this);
		obs_display_destroy(display);
		display = nullptr;
	}
}

QSize MmpDisplay::PixelSize() const
{
	qreal dpr = devicePixelRatioF();
	return QSize(qMax(1, int(width() * dpr + 0.5)), qMax(1, int(height() * dpr + 0.5)));
}

void MmpDisplay::CreateDisplay()
{
	if (display)
		return;
	QWindow *win = windowHandle();
	if (!win || !win->isExposed())
		return;

	QSize size = PixelSize();
	gs_init_data info = {};
	info.cx = size.width();
	info.cy = size.height();
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
#ifdef _WIN32
	info.window.hwnd = (HWND)win->winId();
#else
	return;
#endif

	display = obs_display_create(&info, 0x000000);
	if (display)
		obs_display_add_draw_callback(display, DrawCallback, this);
}

void MmpDisplay::DrawCallback(void *data, uint32_t cx, uint32_t cy)
{
	static_cast<MmpDisplay *>(data)->Render(cx, cy);
}

void MmpDisplay::paintEvent(QPaintEvent *event)
{
	CreateDisplay();
	QWidget::paintEvent(event);
}

void MmpDisplay::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	CreateDisplay();
}

void MmpDisplay::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	CreateDisplay();
	if (display) {
		QSize size = PixelSize();
		obs_display_resize(display, size.width(), size.height());
	}
}

void MmpStartRegion(int vx, int vy, int vcx, int vcy, float l, float r, float t, float b)
{
	gs_projection_push();
	gs_viewport_push();
	gs_set_viewport(vx, vy, vcx, vcy);
	gs_ortho(l, r, t, b, -100.0f, 100.0f);
}

void MmpEndRegion()
{
	gs_viewport_pop();
	gs_projection_pop();
}

static void quad(float x, float y, float w, float h)
{
	gs_vertex2f(x, y);
	gs_vertex2f(x + w, y);
	gs_vertex2f(x, y + h);
	gs_vertex2f(x + w, y);
	gs_vertex2f(x + w, y + h);
	gs_vertex2f(x, y + h);
}

static void set_solid_color(gs_effect_t *solid, uint32_t rgba)
{
	struct vec4 c;
	vec4_from_rgba(&c, rgba);
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &c);
}

void MmpDrawRectOutline(float x, float y, float w, float h, float t, uint32_t rgba)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	set_solid_color(solid, rgba);
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(true);
		quad(x, y, w, t);
		quad(x, y + h - t, w, t);
		quad(x, y + t, t, h - 2 * t);
		quad(x + w - t, y + t, t, h - 2 * t);
		gs_render_stop(GS_TRIS);
	}
	gs_blend_state_pop();
}

void MmpDrawRectFill(float x, float y, float w, float h, uint32_t rgba)
{
	if (w <= 0 || h <= 0)
		return;
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	set_solid_color(solid, rgba);
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(true);
		quad(x, y, w, h);
		gs_render_stop(GS_TRIS);
	}
	gs_blend_state_pop();
}
