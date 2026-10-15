/*
Multi Monitor Projector for OBS
以解析度辨識螢幕，將主螢幕畫面裁切投放到多台副螢幕，並在螢幕斷線重接後自動恢復。

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include "mmp-core.hpp"
#include "mmp-dock.hpp"

#include <QMainWindow>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static MmpDock *g_dock = nullptr;

static void frontend_event(enum obs_frontend_event event, void *)
{
	MmpCore *core = MmpCore::Get();
	if (!core)
		return;

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		core->Startup();
		if (g_dock)
			g_dock->RefreshScenes();
		break;
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		if (g_dock)
			g_dock->RefreshScenes();
		core->Reconcile(false);
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		// 場景集合要換掉了，先關閉場景模式的投影（釋放場景參照）
		core->ReleaseScenes();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		core->Shutdown();
		break;
	default:
		break;
	}
}

bool obs_module_load(void)
{
	MmpCore::Create();

	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	g_dock = new MmpDock(main);
	obs_frontend_add_dock_by_id("multi_monitor_projector", "多螢幕投影", g_dock);

	obs_frontend_add_event_callback(frontend_event, nullptr);
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontend_event, nullptr);
	MmpCore::Destroy();
	obs_log(LOG_INFO, "plugin unloaded");
}
