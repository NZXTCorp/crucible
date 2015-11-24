// [crucible.cpp 2015-10-22 abright]
// libobs-based game capture (currently an experimental thing based on the libobs sample app)

#include <stdio.h>
#include <windows.h>

#include <util/base.h>
#include <util/dstr.hpp>
#include <obs.hpp>

#include <iostream>
using namespace std;

// window class borrowed from forge, remove once we've got headless mode working
#include "TestWindow.h"

// logging lifted straight out of the test app
void do_log(int log_level, const char *msg, va_list args, void *param)
{
	char bla[4096];
	vsnprintf(bla, 4095, msg, args);

	OutputDebugStringA(bla);
	OutputDebugStringA("\n");

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

void TestVideoRecording(TestWindow &window)
{
	try
	{
		struct obs_video_info ovi;
		ovi.adapter = 0;
		ovi.base_width = 1280;
		ovi.base_height = 720;
		ovi.fps_num = 30000;
		ovi.fps_den = 1001;
		ovi.graphics_module = "libobs-d3d11.dll";
		ovi.output_format = VIDEO_FORMAT_RGBA;
		ovi.output_width = 1280;
		ovi.output_height = 720;

		if (obs_reset_video(&ovi) != 0)
			throw "Couldn't initialize video";

		struct obs_audio_info ai;
		ai.samples_per_sec = 44100;
		ai.speakers = SPEAKERS_STEREO;
		ai.buffer_ms = 1000;
		if (!obs_reset_audio(&ai))
			throw "Couldn't initialize audio";

		// todo: find/create a way to do this without rendering to a window (maybe a 'headless' swap chain option? so calls to present and stuff are silently ignored but querying its width/height works ok and such)
		gs_init_data dinfo = {};
		dinfo.cx = 800;
		dinfo.cy = 480;
		dinfo.format = GS_RGBA;
		dinfo.zsformat = GS_ZS_NONE;
		dinfo.window.hwnd = window.GetHandle();

		OBSDisplay display(obs_display_create(&dinfo));
		if (!display)
			throw "Couldn't create display";

		{
			DStr obs_path = GetModulePath(/*&obs_startup*/ "obs.dll");
			DStr bin_path, data_path;
			dstr_printf(bin_path, "%s../../obs-plugins/" BIT_STRING, obs_path->array);
			dstr_printf(data_path, "%s../../data/obs-plugins/%%module%%", obs_path->array);
			obs_add_module_path(bin_path, data_path);
		}

		obs_load_all_modules();

		// create audio source
		OBSSource tunes(obs_source_create(OBS_SOURCE_TYPE_INPUT, "wasapi_output_capture", "wasapi loopback", nullptr, nullptr));
		if (!tunes)
			throw "Couldn't create audio input source";

		// create game capture video source
		OBSSource source(obs_source_create(OBS_SOURCE_TYPE_INPUT, "game_capture", "game capture", nullptr, nullptr));
		if (!source)
			throw "Couldn't create game capture test source";
		
		// create scene
		OBSScene scene(obs_scene_create("test scene"));
		if (!scene)
			throw "Couldn't create scene";

		// add the source to the scene
		OBSSceneItem item = nullptr;
		item = obs_scene_add(scene, source);
		
		// add audio to scene
		item = obs_scene_add(scene, tunes);

		// update source settings - tell game_capture to try and capture hl2: lost coast
		OBSData csettings(obs_data_create());
		obs_data_set_bool(csettings, "capture_any_fullscreen", false);
		obs_data_set_bool(csettings, "capture_cursor", true);
		obs_data_set_string(csettings, "window", "Half-Life 2#3A Lost Coast:Valve001:hl2.exe");
		obs_source_update(source, csettings);

		// set up encoder
		OBSData vsettings(obs_data_create());
		obs_data_set_int(vsettings, "bitrate", 2500);
		obs_data_set_int(vsettings, "buffer_size", 0);
		obs_data_set_int(vsettings, "crf", 23);
		obs_data_set_bool(vsettings, "use_bufsize", true);
		obs_data_set_bool(vsettings, "cbr", false);
		obs_data_set_string(vsettings, "profile", "high");
		obs_data_set_string(vsettings, "preset", "veryfast");

		OBSEncoder h264encoder(obs_video_encoder_create("obs_x264", "x264 video", vsettings, nullptr));
		if (!h264encoder)
			throw "Couldn't create video encoder";

		//OBSData asettings = obs_data_create();
		//obs_data_set_int(asettings, "bitrate", 128000);
		// TODO: settings for audio?
		OBSEncoder aacEncoder(obs_audio_encoder_create("CoreAudio_AAC", "coreaudio aac", nullptr, 0, nullptr));
		if (!aacEncoder)
			throw "Couldn't create audio encoder";

		// set up output to file
		OBSData osettings = obs_data_create();
		obs_data_set_string(osettings, "path", "test.mp4");

		OBSOutput output = obs_output_create("ffmpeg_muxer", "ffmpeg recorder", osettings, nullptr);
		if (!output)
			throw "Couldn't create output";

		OBSSignal startRecording, stopRecording;
		startRecording.Connect(obs_output_get_signal_handler(output), "start", OBSStartRecording, nullptr);
		stopRecording.Connect(obs_output_get_signal_handler(output), "stop", OBSStopRecording, nullptr);

		// set the scene as the primary output source
		obs_set_output_source(0, obs_scene_get_source(scene));

		// connect video and audio to encoders
		obs_encoder_set_video(h264encoder, obs_get_video());
		obs_encoder_set_audio(aacEncoder, obs_get_audio());
		// connect those encoders to the video output
		obs_output_set_video_encoder(output, h264encoder);
		obs_output_set_audio_encoder(output, aacEncoder, 0);

		obs_display_add_draw_callback(display, RenderWindow, nullptr);

		if (!obs_output_start(output))
			throw "Can't start recording";

		MSG msg;
		while (GetMessage(&msg, NULL, 0, 0))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}

		// stop recording
		obs_output_stop(output);
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