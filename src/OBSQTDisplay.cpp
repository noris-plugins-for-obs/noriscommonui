/* Common code among my plugins for OBS Studio
 * Copyright (C) 2025  Norihiro Kamae
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * - Copied from obs-studio/frontend/widgets/OBSQTDisplay.cpp
 * - Added checking display before use.
 * - See OBSQTDisplay.hpp for other modifications.
 */

#include "obs.hpp"
#include "OBSQTDisplay.hpp"

#include <display-helpers.hpp>
#include <SurfaceEventFilter.hpp>

#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

#include <QWindow>
#ifdef ENABLE_WAYLAND
#include <QApplication>
#if QT_VERSION < QT_VERSION_CHECK(6, 9, 0)
#include <qpa/qplatformnativeinterface.h>
#endif
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include "moc_OBSQTDisplay.cpp"

#define GREY_COLOR_BACKGROUND 0xFF4C4C4C

struct private_data_s {
	OBSDisplay display;
	bool destroying = false;
	uint32_t backgroundColor = GREY_COLOR_BACKGROUND;
	void UpdateDisplayBackgroundColor();
};

static inline long long color_to_int(const QColor &color)
{
	auto shift = [&](unsigned val, int shift) {
		return ((val & 0xff) << shift);
	};

	return shift(color.red(), 0) | shift(color.green(), 8) | shift(color.blue(), 16) | shift(color.alpha(), 24);
}

static inline QColor rgba_to_color(uint32_t rgba)
{
	return QColor::fromRgb(rgba & 0xFF, (rgba >> 8) & 0xFF, (rgba >> 16) & 0xFF, (rgba >> 24) & 0xFF);
}

static bool QTToGSWindow(QWindow *window, gs_window &gswindow)
{
	bool success = true;

#ifdef _WIN32
	gswindow.hwnd = (HWND)window->winId();
#elif __APPLE__
	gswindow.view = (id)window->winId();
#else
	switch (obs_get_nix_platform()) {
	case OBS_NIX_PLATFORM_X11_EGL:
		gswindow.id = window->winId();
		gswindow.display = obs_get_nix_platform_display();
		break;
#ifdef ENABLE_WAYLAND
	case OBS_NIX_PLATFORM_WAYLAND: {
#if QT_VERSION < QT_VERSION_CHECK(6, 9, 0)
		QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
		gswindow.display = native->nativeResourceForWindow("surface", window);
#else
		gswindow.display = (void *)window->winId();
#endif
		success = gswindow.display != nullptr;
		break;
	}
#endif
	default:
		success = false;
		break;
	}
#endif
	return success;
}

OBSQTDisplay::OBSQTDisplay(QWidget *parent, Qt::WindowFlags flags) : QWidget(parent, flags), priv(*new private_data_s())
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);

	auto windowVisible = [this](bool visible) {
		if (!visible) {
#if !defined(_WIN32) && !defined(__APPLE__)
			priv.display = nullptr;
#endif
			return;
		}

		if (!priv.display) {
			CreateDisplay();
		} else {
			QSize size = GetPixelSize(this);
			obs_display_resize(priv.display, size.width(), size.height());
		}
	};

	auto screenChanged = [this](QScreen *) {
		CreateDisplay();

		if (!priv.display)
			return;
		QSize size = GetPixelSize(this);
		obs_display_resize(priv.display, size.width(), size.height());
	};

	connect(windowHandle(), &QWindow::visibleChanged, windowVisible);
	connect(windowHandle(), &QWindow::screenChanged, screenChanged);

	windowHandle()->installEventFilter(new SurfaceEventFilter(this));
}

OBSQTDisplay::~OBSQTDisplay()
{
	priv.display = nullptr;
	delete &priv;
}

void OBSQTDisplay::DestroyDisplay()
{
	priv.display = nullptr;
	priv.destroying = true;
}

obs_display_t *OBSQTDisplay::GetDisplay() const
{
	return priv.display;
}

QColor OBSQTDisplay::GetDisplayBackgroundColor() const
{
	return rgba_to_color(priv.backgroundColor);
}

void OBSQTDisplay::SetDisplayBackgroundColor(const QColor &color)
{
	uint32_t newBackgroundColor = (uint32_t)color_to_int(color);

	if (newBackgroundColor != priv.backgroundColor) {
		priv.backgroundColor = newBackgroundColor;
		priv.UpdateDisplayBackgroundColor();
	}
}

void private_data_s::UpdateDisplayBackgroundColor()
{
	obs_display_set_background_color(display, backgroundColor);
}

void OBSQTDisplay::CreateDisplay()
{
	if (priv.display)
		return;

	if (priv.destroying)
		return;

	if (!windowHandle()->isExposed())
		return;

	QSize size = GetPixelSize(this);

	gs_init_data info = {};
	info.cx = size.width();
	info.cy = size.height();
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;

	if (!QTToGSWindow(windowHandle(), info.window))
		return;

	priv.display = obs_display_create(&info, priv.backgroundColor);

	emit DisplayCreated(this);
}

void OBSQTDisplay::paintEvent(QPaintEvent *event)
{
	CreateDisplay();

	QWidget::paintEvent(event);
}

void OBSQTDisplay::moveEvent(QMoveEvent *event)
{
	QWidget::moveEvent(event);

	OnMove();
}

bool OBSQTDisplay::nativeEvent(const QByteArray &, void *message, qintptr *)
{
#ifdef _WIN32
	const MSG &msg = *static_cast<MSG *>(message);
	switch (msg.message) {
	case WM_DISPLAYCHANGE:
		OnDisplayChange();
	}
#else
	UNUSED_PARAMETER(message);
#endif

	return false;
}

void OBSQTDisplay::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);

	CreateDisplay();

	if (isVisible() && priv.display) {
		QSize size = GetPixelSize(this);
		obs_display_resize(priv.display, size.width(), size.height());
	}

	emit DisplayResized();
}

QPaintEngine *OBSQTDisplay::paintEngine() const
{
	return nullptr;
}

void OBSQTDisplay::OnMove()
{
	if (priv.display)
		obs_display_update_color_space(priv.display);
}

void OBSQTDisplay::OnDisplayChange()
{
	if (priv.display)
		obs_display_update_color_space(priv.display);
}
