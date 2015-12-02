// [crucible.cpp 2015-10-22 abright]
// libobs-based game capture (currently an experimental thing based on the libobs sample app)

#include <stdio.h>
#include <windows.h>

#include <util/base.h>
#include <util/dstr.hpp>
#include <obs.hpp>

#include <iostream>
#include <mutex>
using namespace std;

#include "IPC.hpp"

// window class borrowed from forge, remove once we've got headless mode working
#include "TestWindow.h"

namespace std {

template <>
struct default_delete<obs_data_item_t> {
	void operator()(obs_data_item_t *item)
	{
		obs_data_item_release(&item);
	}
};

}

static IPCClient event_client, log_client;

// logging lifted straight out of the test app
void do_log(int log_level, const char *msg, va_list args, void *param)
{
	char bla[4096];
	size_t n = vsnprintf(bla, 4095, msg, args);

	OutputDebugStringA(bla);
	OutputDebugStringA("\n");

	if (log_client)
		log_client.Write(bla, n + 1);

	//cout << bla << endl;

	if (log_level < LOG_WARNING)
		__debugbreak();

	UNUSED_PARAMETER(param);
}

void RenderWindow(void *data, uint32_t cx, uint32_t cy)
{
	obs_render_main_view();

	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(cx);
	UNUSED_PARAMETER(cy);
}

void OBSStartRecording(void *data, calldata_t *params)
{
	blog(LOG_INFO, "Recording started");
}

void OBSStopRecording(void *data, calldata_t *params)
{
	int code = (int)calldata_int(params, "code");
	blog(LOG_INFO, "Recording stopped, code %d", code);
}

static OBSData OBSDataCreate(const string &json={})
{
	OBSData data = json.empty() ? obs_data_create() : obs_data_create_from_json(json.c_str());
	obs_data_release(data);
	return data;
}

static OBSData OBSDataGetObj(obs_data_t *data, const char *name)
{
	OBSData obj = obs_data_get_obj(data, name);
	obs_data_release(obj);
	return obj;
}

/*template <typename T>
static DStr GetModulePath(T *sym)*/
static DStr GetModulePath(const char *name)
{
	DStr res;

	HMODULE module;
	if (!GetModuleHandleEx(
			//GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			/*(LPCTSTR)sym*/ name, &module)) {
		blog(LOG_ERROR, "module handle ex: %d", GetLastError());
		return res;
	}

	char filename[MAX_PATH];
	if (!GetModuleFileNameA(module, filename, MAX_PATH))
		return res;

	filename[MAX_PATH - 1] = 0;

	char drive[_MAX_DRIVE] = "";
	char dir[_MAX_DIR] = "";
	if (_splitpath_s(filename, drive, _MAX_DRIVE, dir, _MAX_DIR,
			NULL, 0, NULL, 0))
		return res;

	dstr_printf(res, "%s%s", drive, dir);
	return res;
}

#ifdef _WIN64
#define BIT_STRING "64bit"
#else
#define BIT_STRING "32bit"
#endif

template <typename T, typename U>
static void InitRef(T &ref, const char *msg, void (*release)(U*), U *val)
{
	if (!val)
		throw msg;

	ref = val;
	release(val);
}

struct CrucibleContext {
	obs_video_info ovi;
	uint32_t fps_den;
	OBSSource tunes, gameCapture;
	OBSEncoder h264, aac;
	OBSOutput output;
	OBSSignal startRecording, stopRecording;

	bool ResetVideo()
	{
		return obs_reset_video(&ovi) == 0;
	}

	void InitLibobs()
	{
		ovi.adapter = 0;
		ovi.base_width = 1280;
		ovi.base_height = 720;
		ovi.fps_num = 30;
		ovi.fps_den = fps_den = 1;
		ovi.graphics_module = "libobs-d3d11.dll";
		ovi.output_format = VIDEO_FORMAT_NV12;
		ovi.output_width = 1280;
		ovi.output_height = 720;
		ovi.scale_type = OBS_SCALE_BICUBIC;
		ovi.range = VIDEO_RANGE_PARTIAL;
		ovi.gpu_conversion = true;
		ovi.colorspace = VIDEO_CS_601;
		if (ovi.output_width >= 1280 || ovi.output_height >= 720)
			ovi.colorspace = VIDEO_CS_709;

		if (!ResetVideo())
			throw "Couldn't initialize video";

		obs_audio_info ai;
		ai.samples_per_sec = 44100;
		ai.speakers = SPEAKERS_STEREO;
		ai.buffer_ms = 1000;
		if (!obs_reset_audio(&ai))
			throw "Couldn't initialize audio";

		{
			DStr obs_path = GetModulePath(/*&obs_startup*/ "obs.dll");
			DStr bin_path, data_path;
			dstr_printf(bin_path, "%s../../obs-plugins/" BIT_STRING, obs_path->array);
			dstr_printf(data_path, "%s../../data/obs-plugins/%%module%%", obs_path->array);
			obs_add_module_path(bin_path, data_path);
		}

		obs_load_all_modules();
	}

	void InitSources()
	{
		// create audio source
		InitRef(tunes, "Couldn't create audio input source", obs_source_release,
				obs_source_create(OBS_SOURCE_TYPE_INPUT, "wasapi_output_capture", "wasapi loopback", nullptr, nullptr));

		// create game capture video source
		InitRef(gameCapture, "Couldn't create game capture source", obs_source_release,
				obs_source_create(OBS_SOURCE_TYPE_INPUT, "game_capture", "game capture", nullptr, nullptr));

		obs_set_output_source(0, gameCapture);
		obs_set_output_source(1, tunes);
	}

	void InitEncoders()
	{
		auto vsettings = OBSDataCreate();
		obs_data_set_int(vsettings, "bitrate", 2500);
		obs_data_set_int(vsettings, "buffer_size", 0);
		obs_data_set_int(vsettings, "crf", 23);
		obs_data_set_bool(vsettings, "use_bufsize", true);
		obs_data_set_bool(vsettings, "cbr", false);
		obs_data_set_string(vsettings, "profile", "high");
		obs_data_set_string(vsettings, "preset", "veryfast");

		InitRef(h264, "Couldn't create video encoder", obs_encoder_release,
				obs_video_encoder_create("obs_x264", "x264 video", vsettings, nullptr));


		auto asettings = OBSDataCreate();
		obs_data_set_int(asettings, "bitrate", 128);

		InitRef(aac, "Couldn't create audio encoder", obs_encoder_release,
				obs_audio_encoder_create("CoreAudio_AAC", "coreaudio aac", asettings, 0, nullptr));


		obs_encoder_set_video(h264, obs_get_video());
		obs_encoder_set_audio(aac, obs_get_audio());
	}

	void InitOutput()
	{
		auto osettings = OBSDataCreate();
		obs_data_set_string(osettings, "path", "test.mp4");

		InitRef(output, "Couldn't create output", obs_output_release,
				obs_output_create("ffmpeg_muxer", "ffmpeg recorder", osettings, nullptr));

		obs_output_set_video_encoder(output, h264);
		obs_output_set_audio_encoder(output, aac, 0);
	}

	void InitSignals()
	{
		startRecording.Connect(obs_output_get_signal_handler(output), "start", OBSStartRecording, nullptr);
		stopRecording.Connect(obs_output_get_signal_handler(output), "stop", OBSStopRecording, nullptr);
	}

#define CONCAT2(x, y) x ## y
#define CONCAT(x, y) CONCAT2(x, y)
#define LOCK auto CONCAT(lockGuard, __LINE__) = lock_guard<mutex>{updateMutex};

	mutex updateMutex;

	void UpdateGameCapture(obs_data_t *settings)
	{
		LOCK;
		obs_source_update(gameCapture, settings);
	}

	void UpdateEncoder(obs_data_t *settings)
	{
		obs_encoder_update(h264, settings);
	}

	void StopVideo()
	{
		LOCK;
		if (obs_output_active(output))
			obs_output_stop(output);

		ovi.fps_den = 0;
		ResetVideo();
	}

	bool StartVideo()
	{
		LOCK;
		ovi.fps_den = fps_den;
		ResetVideo();

		return obs_output_start(output);
	}

};

static void HandleConnectCommand(CrucibleContext &cc, OBSData &obj)
{
	const char *str = nullptr;

	if ((str = obs_data_get_string(obj, "log"))) {
		if (log_client.Open(str))
			blog(LOG_INFO, "Connected log to '%s'", str);
	}

	if ((str = obs_data_get_string(obj, "event"))) {
		if (event_client.Open(str))
			blog(LOG_INFO, "Connected event to '%s'", str);
	}
}

static void HandleCaptureCommand(CrucibleContext &cc, OBSData &obj)
{
	cc.StopVideo();
	cc.UpdateGameCapture(OBSDataGetObj(obj, "game_capture"));
	cc.UpdateEncoder(OBSDataGetObj(obj, "encoder"));
	blog(LOG_INFO, "Starting new capture");
	cc.StartVideo();
}

static void HandleCommand(CrucibleContext &cc, const uint8_t *data, size_t size)
{
	if (!data)
		return;

	auto obj = OBSDataCreate({data, data+size});

	blog(LOG_INFO, "got: %s", data);

	unique_ptr<obs_data_item_t> item{obs_data_item_byname(obj, "command")};
	if (!item) {
		blog(LOG_WARNING, "Missing command element on command channel");
		return;
	}

	const char *str = obs_data_item_get_string(item.get());
	if (!str) {
		blog(LOG_WARNING, "Invalid command element");
		return;
	}

	if (string("connect") == str) {
		HandleConnectCommand(cc, obj);
		return;
	} else if (string("capture_new_process") == str) {
		HandleCaptureCommand(cc, obj);
		return;
	}

	blog(LOG_WARNING, "Unknown command: %s", str);


	// TODO: Handle changes to frame rate, target resolution, encoder type,
	//       ...
}

void TestVideoRecording(TestWindow &window)
{
	try
	{
		CrucibleContext crucibleContext;

		crucibleContext.InitLibobs();
		crucibleContext.InitSources();
		crucibleContext.InitEncoders();
		crucibleContext.InitOutput();
		crucibleContext.StopVideo();

		// TODO: remove once we're done debugging
		gs_init_data dinfo = {};
		dinfo.cx = 800;
		dinfo.cy = 480;
		dinfo.format = GS_RGBA;
		dinfo.zsformat = GS_ZS_NONE;
		dinfo.window.hwnd = window.GetHandle();

		OBSDisplay display(obs_display_create(&dinfo));
		if (!display)
			throw "Couldn't create display";

		obs_display_add_draw_callback(display, RenderWindow, nullptr);

		// update source settings - tell game_capture to try and capture hl2: lost coast
		auto csettings = OBSDataCreate();
		obs_data_set_bool(csettings, "capture_any_fullscreen", false);
		obs_data_set_bool(csettings, "capture_cursor", true);
		obs_data_set_string(csettings, "window", "Half-Life 2#3A Lost Coast:Valve001:hl2.exe");
		crucibleContext.UpdateGameCapture(csettings);

		auto handleCommand = [&](const uint8_t *data, size_t size)
		{
			HandleCommand(crucibleContext, data, size);
		};

		IPCServer remote{"ForgeCrucible", handleCommand};

		MSG msg;
		while (GetMessage(&msg, NULL, 0, 0))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}

		crucibleContext.StopVideo();
	}
	catch (const char *err)
	{
		blog(LOG_ERROR, "Error: %s", err);
	}

}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmd)
{
	base_set_log_handler(do_log, nullptr);
	
	try
	{	
		if (!obs_startup("en-US", "module-config", nullptr))
			throw "Couldn't init OBS";

		TestWindow window(hInstance);

		TestWindow::RegisterWindowClass(hInstance);

		if (!window.Create(800, 480, "libobs test"))
			throw "Couldn't create test window";

		window.Show();

		TestVideoRecording(window);
	}
	catch (const char *err)
	{
		blog(LOG_ERROR, "Error: %s", err);
	}

	obs_shutdown();

	blog(LOG_INFO, "Number of memory leaks: %ld", bnum_allocs());

	UNUSED_PARAMETER(hPrevInstance);
	UNUSED_PARAMETER(lpCmdLine);
	UNUSED_PARAMETER(nCmd);

	return 0;
}
