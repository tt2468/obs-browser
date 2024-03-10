/******************************************************************************
 Copyright (C) 2014 by John R. Bradley <jrb@turrettech.com>
 Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************/

#include <util/threading.h>
#include <util/platform.h>
#include <util/util.hpp>
#include <util/dstr.hpp>
#include <obs-module.h>
#include <obs.hpp>
#include <functional>
#include <sstream>
#include <thread>
#include <mutex>

#include "obs-browser-source.hpp"
#include "browser-scheme.hpp"
#include "browser-app.hpp"
#include "browser-version.h"
#include "browser-config.h"

#include "signal-restore.hpp"
#include "obs-websocket-api/obs-websocket-api.h"
#include "cef-headers.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-browser", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "CEF-based web browser source & panels";
}

using namespace std;

static thread manager_thread;
static bool manager_initialized = false;
os_event_t *cef_started_event = nullptr;

static std::wstring deviceId;

bool hwaccel = false;

/* ========================================================================= */

class BrowserTask : public CefTask {
public:
	std::function<void()> task;

	inline BrowserTask(std::function<void()> task_) : task(task_) {}
	virtual void Execute() override
	{
		task();
	}

	IMPLEMENT_REFCOUNTING(BrowserTask);
};

bool QueueCEFTask(std::function<void()> task)
{
	return CefPostTask(TID_UI,
			   CefRefPtr<BrowserTask>(new BrowserTask(task)));
}

/* ========================================================================= */

static const char *default_css = "\
body { \
background-color: rgba(0, 0, 0, 0); \
margin: 0px auto; \
overflow: hidden; \
}";

static void browser_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "url", "https://irltoolkit.com");
	obs_data_set_default_int(settings, "width", 800);
	obs_data_set_default_int(settings, "height", 600);
	obs_data_set_default_int(settings, "fps", 30);
	obs_data_set_default_bool(settings, "fps_custom", true);
	obs_data_set_default_bool(settings, "shutdown", false);
	obs_data_set_default_bool(settings, "restart_when_active", false);
	obs_data_set_default_string(settings, "css", default_css);
}

static bool is_local_file_modified(obs_properties_t *props, obs_property_t *,
				   obs_data_t *settings)
{
	bool enabled = obs_data_get_bool(settings, "is_local_file");
	obs_property_t *url = obs_properties_get(props, "url");
	obs_property_t *local_file = obs_properties_get(props, "local_file");
	obs_property_set_visible(url, !enabled);
	obs_property_set_visible(local_file, enabled);

	return true;
}

static bool is_fps_custom(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	bool enabled = obs_data_get_bool(settings, "fps_custom");
	obs_property_t *fps = obs_properties_get(props, "fps");
	obs_property_set_visible(fps, enabled);

	return true;
}

static obs_properties_t *browser_source_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	BrowserSource *bs = static_cast<BrowserSource *>(data);
	DStr path;

	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);
	obs_property_t *prop = obs_properties_add_bool(props, "is_local_file", obs_module_text("LocalFile"));

	if (bs && !bs->url.empty()) {
		const char *slash;

		dstr_copy(path, bs->url.c_str());
		dstr_replace(path, "\\", "/");
		slash = strrchr(path->array, '/');
		if (slash)
			dstr_resize(path, slash - path->array + 1);
	}

	obs_property_set_modified_callback(prop, is_local_file_modified);
	obs_properties_add_path(props, "local_file", obs_module_text("LocalFile"), OBS_PATH_FILE, "*.*", path->array);
	obs_properties_add_text(props, "url", obs_module_text("URL"), OBS_TEXT_DEFAULT);

	obs_properties_add_int(props, "width", obs_module_text("Width"), 1, 8192, 1);
	obs_properties_add_int(props, "height", obs_module_text("Height"), 1, 8192, 1);

	obs_properties_add_bool(props, "reroute_audio", obs_module_text("RerouteAudio"));

	obs_property_t *fps_set = obs_properties_add_bool(props, "fps_custom", obs_module_text("CustomFrameRate"));
	obs_property_set_modified_callback(fps_set, is_fps_custom);

	obs_property_set_enabled(fps_set, false);

	obs_properties_add_int(props, "fps", obs_module_text("FPS"), 1, 60, 1);

	obs_property_t *p = obs_properties_add_text(props, "css", obs_module_text("CSS"), OBS_TEXT_MULTILINE);
	obs_property_text_set_monospace(p, true);
	obs_properties_add_bool(props, "shutdown", obs_module_text("ShutdownSourceNotVisible"));
	obs_properties_add_bool(props, "restart_when_active", obs_module_text("RefreshBrowserActive"));

	obs_properties_add_button(
		props, "refreshnocache", obs_module_text("RefreshNoCache"),
		[](obs_properties_t *, obs_property_t *, void *data) {
			static_cast<BrowserSource *>(data)->Refresh();
			return false;
		});

	return props;
}

static CefRefPtr<BrowserApp> app;

static void BrowserInit(void)
{
	string path = obs_get_module_binary_path(obs_current_module());
	path = path.substr(0, path.find_last_of('/') + 1);
	path += "//obs-browser-page";

	/* On non-windows platforms, ie macOS, we'll want to pass thru flags to
	 * CEF */
	struct obs_cmdline_args cmdline_args = obs_get_cmdline_args();
	CefMainArgs args(cmdline_args.argc, cmdline_args.argv);

	BPtr<char> conf_path = obs_module_config_path("");
	os_mkdir(conf_path);

	CefSettings settings;
	settings.log_severity = LOGSEVERITY_DISABLE;
	BPtr<char> log_path = obs_module_config_path("debug.log");
	BPtr<char> log_path_abs = os_get_abs_path_ptr(log_path);
	CefString(&settings.log_file) = log_path_abs;
	settings.windowless_rendering_enabled = true;
	settings.no_sandbox = true;

	uint32_t obs_ver = obs_get_version();
	uint32_t obs_maj = obs_ver >> 24;
	uint32_t obs_min = (obs_ver >> 16) & 0xFF;
	uint32_t obs_pat = obs_ver & 0xFFFF;

	/* This allows servers the ability to determine that browser panels and
	 * browser sources are coming from OBS. */
	std::stringstream prod_ver;
	prod_ver << "Chrome/";
	prod_ver << std::to_string(cef_version_info(4)) << "."
		 << std::to_string(cef_version_info(5)) << "."
		 << std::to_string(cef_version_info(6)) << "."
		 << std::to_string(cef_version_info(7));
	prod_ver << " OBS/";
	prod_ver << std::to_string(obs_maj) << "." << std::to_string(obs_min)
		 << "." << std::to_string(obs_pat);

	CefString(&settings.user_agent_product) = prod_ver.str();

	// Override locale path from OBS binary path to plugin binary path
	string locales = obs_get_module_binary_path(obs_current_module());
	locales = locales.substr(0, locales.find_last_of('/') + 1);
	locales += "locales";
	BPtr<char> abs_locales = os_get_abs_path_ptr(locales.c_str());
	CefString(&settings.locales_dir_path) = abs_locales;

	std::string obs_locale = obs_get_locale();
	std::string accepted_languages;
	if (obs_locale != "en-US") {
		accepted_languages = obs_locale;
		accepted_languages += ",";
		accepted_languages += "en-US,en";
	} else {
		accepted_languages = "en-US,en";
	}

	BPtr<char> conf_path_abs = os_get_abs_path_ptr(conf_path);
	CefString(&settings.locale) = obs_get_locale();
	CefString(&settings.accept_language_list) = accepted_languages;
	settings.persist_user_preferences = 1;
	CefString(&settings.cache_path) = conf_path_abs;
	char *abs_path = os_get_abs_path_ptr(path.c_str());
	CefString(&settings.browser_subprocess_path) = abs_path;
	bfree(abs_path);

	bool tex_sharing_avail = false;

	app = new BrowserApp(tex_sharing_avail);

	/* Back up then restore configured signal handlers since
	 * CefInitialize wipes them for some ungodly reason */
	BackupSignalHandlers();
	CefInitialize(args, settings, app, nullptr);
	RestoreSignalHandlers();

	/* Register http://absolute/ scheme handler for older
	 * CEF builds which do not support file:// URLs */
	CefRegisterSchemeHandlerFactory("http", "absolute", new BrowserSchemeHandlerFactory());

	os_event_signal(cef_started_event);
}

static void BrowserShutdown(void)
{
	CefClearSchemeHandlerFactories();

	CefShutdown();
	app = nullptr;
}

static void BrowserManagerThread(void)
{
	BrowserInit();
	CefRunMessageLoop();
	BrowserShutdown();
}

extern "C" EXPORT void obs_browser_initialize(void)
{
	if (!os_atomic_set_bool(&manager_initialized, true)) {
		manager_thread = thread(BrowserManagerThread);
	}
}

void RegisterBrowserSource()
{
	struct obs_source_info info = {};
	info.id = "browser_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_INTERACTION | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_SRGB;
	info.get_properties = browser_source_get_properties;
	info.get_defaults = browser_source_get_defaults;
	info.icon_type = OBS_ICON_TYPE_BROWSER;

	info.get_name = [](void *) { return obs_module_text("BrowserSource"); };
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		obs_browser_initialize();
		return new BrowserSource(settings, source);
	};
	info.destroy = [](void *data) {
		static_cast<BrowserSource *>(data)->Destroy();
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<BrowserSource *>(data)->Update(settings);
	};
	info.get_width = [](void *data) {
		return (uint32_t) static_cast<BrowserSource *>(data)->width;
	};
	info.get_height = [](void *data) {
		return (uint32_t) static_cast<BrowserSource *>(data)->height;
	};
	info.video_tick = [](void *data, float) {
		static_cast<BrowserSource *>(data)->Tick();
	};
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<BrowserSource *>(data)->Render();
	};
	info.mouse_click = [](void *data, const struct obs_mouse_event *event,
			      int32_t type, bool mouse_up,
			      uint32_t click_count) {
		static_cast<BrowserSource *>(data)->SendMouseClick(
			event, type, mouse_up, click_count);
	};
	info.mouse_move = [](void *data, const struct obs_mouse_event *event,
			     bool mouse_leave) {
		static_cast<BrowserSource *>(data)->SendMouseMove(event,
								  mouse_leave);
	};
	info.mouse_wheel = [](void *data, const struct obs_mouse_event *event,
			      int x_delta, int y_delta) {
		static_cast<BrowserSource *>(data)->SendMouseWheel(
			event, x_delta, y_delta);
	};
	info.focus = [](void *data, bool focus) {
		static_cast<BrowserSource *>(data)->SendFocus(focus);
	};
	info.key_click = [](void *data, const struct obs_key_event *event,
			    bool key_up) {
		static_cast<BrowserSource *>(data)->SendKeyClick(event, key_up);
	};
	info.show = [](void *data) {
		static_cast<BrowserSource *>(data)->SetShowing(true);
	};
	info.hide = [](void *data) {
		static_cast<BrowserSource *>(data)->SetShowing(false);
	};
	info.activate = [](void *data) {
		BrowserSource *bs = static_cast<BrowserSource *>(data);
		if (bs->restart)
			bs->Refresh();
		bs->SetActive(true);
	};
	info.deactivate = [](void *data) {
		static_cast<BrowserSource *>(data)->SetActive(false);
	};

	obs_register_source(&info);
}

/* ========================================================================= */

extern void DispatchJSEvent(std::string eventName, std::string jsonString, BrowserSource *browser = nullptr);

bool obs_module_load(void)
{
	os_event_init(&cef_started_event, OS_EVENT_TYPE_MANUAL);

	blog(LOG_INFO, "[obs-browser]: Version %s", OBS_BROWSER_VERSION_STRING);
	blog(LOG_INFO,
	     "[obs-browser]: CEF Version %i.%i.%i.%i (runtime), %s (compiled)",
	     cef_version_info(4), cef_version_info(5), cef_version_info(6),
	     cef_version_info(7), CEF_VERSION);

	RegisterBrowserSource();

	return true;
}

void obs_module_post_load(void)
{
	auto vendor = obs_websocket_register_vendor("obs-browser");
	if (!vendor)
		return;

	auto emit_event_request_cb = [](obs_data_t *request_data, obs_data_t *,
					void *) {
		const char *event_name =
			obs_data_get_string(request_data, "event_name");
		if (!event_name)
			return;

		OBSDataAutoRelease event_data =
			obs_data_get_obj(request_data, "event_data");
		const char *event_data_string =
			event_data ? obs_data_get_json(event_data) : "{}";

		DispatchJSEvent(event_name, event_data_string, nullptr);
	};

	if (!obs_websocket_vendor_register_request(
		    vendor, "emit_event", emit_event_request_cb, nullptr))
		blog(LOG_WARNING,
		     "[obs-browser]: Failed to register obs-websocket request emit_event");
}

void obs_module_unload(void)
{
	if (manager_thread.joinable()) {
		while (!QueueCEFTask([]() { CefQuitMessageLoop(); }))
			os_sleep_ms(5);

		manager_thread.join();
	}

	os_event_destroy(cef_started_event);
}
