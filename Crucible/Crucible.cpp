// [crucible.cpp 2015-10-22 abright]
// libobs-based game capture (currently an experimental thing based on the libobs sample app)

#define NOMINMAX
#include <ShlObj.h>
#include <stdio.h>
#include <windows.h>
#include <DbgHelp.h>

#include <util/base.h>
#include <util/dstr.hpp>
#include <util/platform.h>
#include <util/profiler.hpp>
#include <obs.hpp>

#include "OBSHelpers.hpp"

#include <algorithm>
#include <atomic>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
using namespace std;

#include <boost/logic/tribool.hpp>
#include <boost/optional.hpp>

#include "../AnvilRendering/AnvilRendering.h"

#include "IPC.hpp"
#include "ProtectedObject.hpp"
#include "scopeguard.hpp"
#include "ThreadTools.hpp"

// window class borrowed from forge, remove once we've got headless mode working
#include "TestWindow.h"

//#define TEST_WINDOW

extern "C" {
	_declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
	__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

extern OBSEncoder CreateAudioEncoder(const char *name);
extern void RegisterFramebufferSource();

static IPCClient event_client, log_client;

atomic<bool> store_startup_log = false;
vector<string> startup_logs;
mutex startup_log_mutex;

#define CONCAT2(x, y) x ## y
#define CONCAT(x, y) CONCAT2(x, y)
#define LOCK(x) lock_guard<decltype(x)> CONCAT(lockGuard, __LINE__){x};

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

	if (store_startup_log) {
		LOCK(startup_log_mutex);
		startup_logs.push_back(bla);
	}

	if (log_level < LOG_WARNING && IsDebuggerPresent())
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

struct Bookmark {
	int id = 0;
	video_tracked_frame_id tracked_id = 0;
	int64_t pts = 0;
	uint32_t fps_den = 1;
	double time = 0.;

	OBSData extra_data;
};

namespace ForgeEvents {
	mutex eventMutex;
	vector<OBSData> queuedEvents;

	void SendEvent(obs_data_t *event)
	{
		if (!event)
			return;

		auto data = obs_data_get_json(event);
		if (!data)
			return;

		LOCK(eventMutex);
		if (event_client.Write(data, strlen(data) + 1))
			return;

		queuedEvents.push_back(event);
		blog(LOG_INFO, "event_client.Write failed, queueing event");
	}

	void SendQueuedEvents()
	{
		LOCK(eventMutex);
		auto i = begin(queuedEvents);
		auto end_ = end(queuedEvents);
		for (; i != end_; i++) {
			auto data = obs_data_get_json(*i);
			if (!event_client.Write(data, strlen(data) + 1))
				break;
		}

		queuedEvents.erase(begin(queuedEvents), i);
	}

	static void SendFileCompleteEvent(obs_data_t *event, const char *filename, int total_frames, double duration, const vector<double> &bookmarks,
		uint32_t width, uint32_t height, DWORD *pid=nullptr)
	{
		obs_data_set_string(event, "filename", filename);
		obs_data_set_int(event, "total_frames", total_frames);
		obs_data_set_double(event, "duration", duration);
		obs_data_set_int(event, "width", width);
		obs_data_set_int(event, "height", height);
		if (pid)
			obs_data_set_int(event, "process_id", *pid);

		auto array = OBSDataArrayCreate();
		obs_data_set_array(event, "bookmarks", array);

		for (auto bookmark : bookmarks) {
			auto tmp = OBSDataCreate();
			obs_data_set_double(tmp, "val", bookmark);
			obs_data_array_push_back(array, tmp);
		}

		SendEvent(event);
	}

	OBSData EventCreate(const char * name)
	{
		auto event = OBSDataCreate();

		obs_data_set_string(event, "event", name);
		obs_data_set_int(event, "timestamp", GetTickCount64());

		return event;
	}

	void SendRecordingStart(const char *filename)
	{
		auto event = EventCreate("started_recording");

		obs_data_set_string(event, "filename", filename);

		SendEvent(event);
	}

	void SendRecordingStop(const char *filename, int total_frames, double duration, const vector<double> &bookmarks,
		uint32_t width, uint32_t height, DWORD pid, const vector<Bookmark> &full_bookmarks)
	{
		auto event = EventCreate("stopped_recording");

		auto arr = OBSDataArrayCreate();
		for (auto &bookmark : full_bookmarks) {
			auto mark = OBSDataCreate();

			obs_data_set_int(mark, "bookmark_id", bookmark.id);
			obs_data_set_double(mark, "timestamp", bookmark.time);
			obs_data_set_obj(mark, "extra_data", bookmark.extra_data);

			obs_data_array_push_back(arr, mark);
		}

		obs_data_set_array(event, "full_bookmarks", arr);

		SendFileCompleteEvent(event, filename, total_frames, duration, bookmarks, width, height, &pid);
	}

	void SendQueryMicsResponse(obs_data_array_t *devices)
	{
		auto event = EventCreate("query_mics_response");

		obs_data_set_array(event, "devices", devices);

		SendEvent(event);
	}

	void SendBufferReady(const char *filename, int total_frames, double duration, const vector<double> &bookmarks,
		uint32_t width, uint32_t height, boost::optional<Bookmark> bookmark_info)
	{
		auto event = EventCreate("buffer_ready");

		if (bookmark_info) {
			obs_data_set_double(event, "created_at_offset", bookmark_info->time);
			obs_data_set_int(event, "bookmark_id", bookmark_info->id);
			obs_data_set_obj(event, "bookmark_extra_data", bookmark_info->extra_data);
		}

		SendFileCompleteEvent(event, filename, total_frames, duration, bookmarks, width, height);
	}

	void SendBufferFailure(const char *filename)
	{
		auto event = EventCreate("buffer_save_failed");

		obs_data_set_string(event, "filename", filename);

		SendEvent(event);
	}

	void SendInjectFailed(long *injector_exit_code)
	{
		auto event = EventCreate("inject_failed");
		
		if (injector_exit_code)
			obs_data_set_int(event, "injector_exit_code", *injector_exit_code);

		SendEvent(event);
	}

	void SendInjectRequest(bool process_is_64bit, bool anti_cheat, DWORD process_thread_id)
	{
		auto event = EventCreate("inject_request");

		obs_data_set_bool(event, "64bit", process_is_64bit);
		obs_data_set_bool(event, "anti_cheat", anti_cheat);
		obs_data_set_int(event, "id", process_thread_id);

		SendEvent(event);
	}

	void SendMonitorProcess(DWORD process_id)
	{
		auto event = EventCreate("monitor_process");

		obs_data_set_int(event, "process_id", process_id);

		SendEvent(event);
	}

	void SendCleanupComplete(const string *profiler_data, DWORD pid)
	{
		auto event = EventCreate("cleanup_complete");

		if (profiler_data)
			obs_data_set_string(event, "profiler_data", profiler_data->c_str());

		obs_data_set_int(event, "process_id", pid);

		SendEvent(event);
	}

	void SendBrowserSizeHint(uint32_t width, uint32_t height)
	{
		auto event = EventCreate("browser_size_hint");

		obs_data_set_int(event, "width", width);
		obs_data_set_int(event, "height", height);

		SendEvent(event);
	}

	void SendStreamingStart()
	{
		auto event = EventCreate("started_streaming");

		SendEvent(event);
	}

	void SendStreamingStop(long long code)
	{
#define EXPAND2(x) x
#define EXPAND(x) x, EXPAND2(#x)
		static const map<long long, const char*> known_code = {
			{EXPAND(OBS_OUTPUT_SUCCESS)},
			{EXPAND(OBS_OUTPUT_BAD_PATH)},
			{EXPAND(OBS_OUTPUT_CONNECT_FAILED)},
			{EXPAND(OBS_OUTPUT_INVALID_STREAM)},
			{EXPAND(OBS_OUTPUT_ERROR)},
			{EXPAND(OBS_OUTPUT_DISCONNECTED)},
			{EXPAND(OBS_OUTPUT_UNSUPPORTED)},
			{EXPAND(OBS_OUTPUT_NO_SPACE)},
		};
#undef EXPAND
#undef EXPAND2

		auto event = EventCreate("stopped_streaming");

		obs_data_set_int(event, "code", code);

		auto it = known_code.find(code);
		if (it != end(known_code))
			obs_data_set_string(event, "name", it->second);

		SendEvent(event);
	}

	void SendSavedGameScreenshot(const char *requested_filename, const char *actual_filename)
	{
		auto event = EventCreate("saved_game_screenshot");

		obs_data_set_string(event, "requested_filename", requested_filename);
		obs_data_set_string(event, "actual_filename", actual_filename);

		SendEvent(event);
	}

	void SendQueryWebcamsResponse(const OBSDataArray &arr)
	{
		auto event = EventCreate("query_webcams_response");

		obs_data_set_array(event, "devices", arr);

		SendEvent(event);
	}

	void SendFramebufferConnectionInfo(const char *id, const char *name)
	{
		auto event = EventCreate("framebuffer_connection_info");

		obs_data_set_string(event, "id", id);
		obs_data_set_string(event, "name", name);

		SendEvent(event);
	}

	void SendQueryDesktopAudioDevicesResponse(const OBSDataArray &arr)
	{
		auto event = EventCreate("query_desktop_audio_devices_response");

		obs_data_set_array(event, "devices", arr);

		SendEvent(event);
	}

	void SendQueryWindowsResponse(const OBSDataArray &arr)
	{
		auto event = EventCreate("query_windows_response");

		obs_data_set_array(event, "windows", arr);

		SendEvent(event);
	}

	void SendSelectSceneResult(const string &scene, const string &current_scene, bool success)
	{
		auto event = EventCreate("select_scene_result");

		obs_data_set_string(event, "scene", scene.c_str());
		obs_data_set_string(event, "current_scene", current_scene.c_str());
		obs_data_set_bool(event, "success", success);

		SendEvent(event);
	}

	void SendAudioSourceLevel(const char *source, float level, float magnitude, float peak, bool muted)
	{
		auto event = EventCreate("audio_source_level");

		obs_data_set_string(event, "source", source);
		obs_data_set_double(event, "level", level);
		obs_data_set_double(event, "magnitude", magnitude);
		obs_data_set_double(event, "peak", peak);
		obs_data_set_bool(event, "muted", muted);

		SendEvent(event);
	}
}

namespace AnvilCommands {
	IPCClient anvil_client;
	std::string current_connection;
	recursive_mutex commandMutex;

	DWORD pid;

	atomic<bool> show_welcome = false;
	atomic<bool> recording = false;
	atomic<bool> clipping  = false;
	atomic<bool> using_mic = false;
	atomic<bool> using_ptt = false;
	atomic<bool> mic_muted = false;
	atomic<bool> display_enabled_hotkey = false;
	atomic<bool> streaming = false;

	const uint64_t enabled_timeout_seconds = 10;
	atomic<uint64_t> enabled_timeout = 0;

	const uint64_t bookmark_timeout_seconds = 3;
	atomic<uint64_t> bookmark_timeout = 0;

	const uint64_t cache_limit_timeout_seconds = 10;
	atomic<uint64_t> cache_limit_timeout = 0;

	const uint64_t clip_finished_timeout_seconds = 5;
	atomic<uint64_t> clip_finished_timeout = 0;

	const uint64_t stream_timeout_seconds = 3;
	atomic<uint64_t> stream_timeout = 0;

	vector<JoiningThread> indicator_updaters;

	string forge_overlay_channel;
	OBSData bookmark_key;
	OBSData highlight_key;
	OBSData stream_key;
	OBSData cursor;

	void SendForgeInfo(const char *info=nullptr);
	void SendSettings(obs_data_t *bookmark_key_=nullptr, 
		obs_data_t *highlight_key_ = nullptr, 
		obs_data_t *stream_key_ = nullptr);
	void SendIndicator();
	void SendCursor(obs_data_t *cmd=nullptr);

	void CreateIndicatorUpdater(uint64_t timeout_seconds, atomic<uint64_t> &timeout_var)
	{
		uint64_t timeout = timeout_var = os_gettime_ns() + timeout_seconds * 1000 * 1000 * 1000;

		auto ev = shared_ptr<void>{ CreateEvent(nullptr, true, false, nullptr), HandleDeleter() };
		auto make_joinable = [ev] { SetEvent(ev.get()); };

		LOCK(commandMutex);
		indicator_updaters.emplace_back();
		auto &jt = indicator_updaters.back();

		jt.make_joinable = make_joinable;
		jt.Run([ev, timeout_seconds, timeout]
		{
			auto res = WaitForSingleObject(ev.get(), static_cast<DWORD>(timeout_seconds * 1000));
			if (res == WAIT_OBJECT_0)
				return;

			while (timeout > os_gettime_ns()) {
				res = WaitForSingleObject(ev.get(), 1000);
				if (res == WAIT_OBJECT_0)
					return;
			}

			SendIndicator();
		});

		SendIndicator();

		indicator_updaters.erase(remove_if(begin(indicator_updaters), end(indicator_updaters), [](JoiningThread &jt)
		{
			return jt.TryJoin();
		}), end(indicator_updaters));
	}

	bool Connect(DWORD pid_, bool write_failed=false)
	{
		LOCK(commandMutex);

		pid = pid_;

		auto connection_name = "AnvilRenderer" + to_string(pid_);

		if (!write_failed && current_connection == connection_name && anvil_client) {
			SendIndicator();
			return true;
		}

		if (!anvil_client.Open(connection_name))
			return false;

		current_connection = connection_name;

		SendForgeInfo();
		SendSettings();
		SendCursor();

		if (!show_welcome) {
			SendIndicator();
			return true;
		}

		CreateIndicatorUpdater(enabled_timeout_seconds, enabled_timeout);
		show_welcome = false;

		return true;
	}

	void SendCommand(obs_data_t *cmd)
	{
		if (!cmd)
			return;

		auto data = obs_data_get_json(cmd);
		if (!data)
			return;

		LOCK(commandMutex);
		for (;;) {
			if (anvil_client.Write(data, strlen(data) + 1))
				return;

			if (!Connect(pid, true))
				return;
		}

		blog(LOG_INFO, "anvil_client.Write failed");
	}

	OBSData CommandCreate(const char *cmd)
	{
		auto obj = OBSDataCreate();
		obs_data_set_string(obj, "command", cmd);
		return obj;
	}

	void SendIndicator()
	{
		auto cmd = CommandCreate("indicator");

		const char *indicator = recording ? "capturing" : "idle";
		if (recording && using_mic)
			indicator = mic_muted ? (using_ptt ? "mic_idle" : "mic_muted") : "mic_active";

		if (streaming) {
			indicator = "streaming";

			if (using_mic)
				indicator = mic_muted ? (using_ptt ? "stream_mic_idle" : "stream_mic_muted") : "stream_mic_active";
		}

		if (enabled_timeout >= os_gettime_ns())
			indicator = display_enabled_hotkey ? "enabled_hotkey" : "enabled";

		if (clipping)
			indicator = "clip_processing";

		if (bookmark_timeout >= os_gettime_ns())
			indicator = "bookmark";

		if (cache_limit_timeout >= os_gettime_ns())
			indicator = "cache_limit";

		if (clip_finished_timeout >= os_gettime_ns())
			indicator = "clip_processed";

		if (stream_timeout >= os_gettime_ns())
			indicator = streaming ? "stream_started" : "stream_stopped";

		obs_data_set_string(cmd, "indicator", indicator);

		SendCommand(cmd);
	}

	void ResetShowWelcome()
	{
		show_welcome = true;
	}

	void ShowRecording()
	{
		if (recording.exchange(true))
			return;

		SendIndicator();
	}

	void ShowIdle()
	{
		if (!recording.exchange(false))
			return;

		SendIndicator();
	}

	void ShowClipping()
	{
		if (clipping.exchange(true))
			return;

		SendIndicator();
	}

	void ClipFinished(bool success)
	{
		clipping = false;

		if (success)
			CreateIndicatorUpdater(clip_finished_timeout_seconds, clip_finished_timeout);
		else
			SendIndicator();
	}

	void ShowCacheLimitExceeded()
	{
		CreateIndicatorUpdater(cache_limit_timeout_seconds, cache_limit_timeout);
	}

	void ShowBookmark()
	{
		CreateIndicatorUpdater(bookmark_timeout_seconds, bookmark_timeout);
	}

	void HotkeyMatches(bool matches)
	{
		bool changed = display_enabled_hotkey != matches;
		display_enabled_hotkey = matches;

		if (changed)
			SendIndicator();
	}

	void MicUpdated(boost::tribool muted, boost::tribool active=boost::indeterminate, boost::tribool ptt=boost::indeterminate)
	{
		bool changed = false;
		if (!boost::indeterminate(active))
			changed = active != using_mic.exchange(active);
		if (!boost::indeterminate(muted))
			changed = (muted != mic_muted.exchange(muted)) || changed;
		if (!boost::indeterminate(ptt))
			changed = (ptt != using_ptt.exchange(ptt)) || changed;

		if (!changed)
			return;

		SendIndicator();
	}

	void SendForgeInfo(const char *info)
	{
		LOCK(commandMutex);

		if (info && info[0])
			forge_overlay_channel = info;

		auto cmd = CommandCreate("forge_info");

		obs_data_set_string(cmd, "anvil_event", forge_overlay_channel.c_str());

		SendCommand(cmd);
	}

	void SendSettings(obs_data_t *bookmark_key_, obs_data_t *highlight_key_, obs_data_t *stream_key_)
	{
		auto cmd = CommandCreate("update_settings");

		LOCK(commandMutex);

		if (bookmark_key_)
			bookmark_key = bookmark_key_;
		if (highlight_key_)
			highlight_key = highlight_key_;

		if (stream_key_)
			stream_key = stream_key_;

		if (bookmark_key)
			obs_data_set_obj(cmd, "bookmark_key", bookmark_key);
		if (highlight_key)
			obs_data_set_obj(cmd, "highlight_key", highlight_key);
		if (stream_key)
			obs_data_set_obj(cmd, "stream_key", stream_key);

		SendCommand(cmd);
	}

	void SendCursor(obs_data_t *cmd)
	{
		LOCK(commandMutex);

		if (cmd)
			cursor = cmd;

		if (cursor)
			SendCommand(cursor);
	}

	void DismissOverlay(OBSData &data)
	{
		auto cmd = CommandCreate("dismiss_overlay");

		if (obs_data_has_user_value(data, "name"))
			obs_data_set_string(cmd, "name", obs_data_get_string(data, "name"));

		LOCK(commandMutex);

		SendCommand(cmd);
	}

	void StreamStatus(bool streaming_)
	{
		auto cmd = CommandCreate("stream_status");

		streaming.exchange(streaming_);
		obs_data_set_bool(cmd, "streaming", streaming_);

		{
			LOCK(commandMutex);
			SendCommand(cmd);
		}

		CreateIndicatorUpdater(stream_timeout_seconds, stream_timeout);
	}
}

struct OutputResolution {
	uint32_t width;
	uint32_t height;

	uint32_t pixels() const { return width * height; }
};

static uint32_t AlignX264Height(uint32_t height)
{
	// We're currently using NV12, so height has to be a multiple of 2, see:
	// http://git.videolan.org/?p=x264.git;a=blob;f=encoder/encoder.c;hb=a01e33913655f983df7a4d64b0a4178abb1eb618#l502
	return (height + 1) & ~static_cast<uint32_t>(1);
}

static uint32_t gcd(uint32_t a, uint32_t b)
{
	if (!a) return b;
	if (!b) return a;

	uint32_t c = a;
	a = max(a, b);
	b = min(c, b);

	uint32_t remainder;
	do
	{
		remainder = a % b;
		a = b;
		b = remainder;
	} while (remainder);

	return a;
}

static boost::optional<OutputResolution> ScaleResolutionInteger(const OutputResolution &target, const OutputResolution &source)
{
	auto aspect_segments = gcd(source.width, source.height);
	auto aspect_width = source.width / aspect_segments;
	auto aspect_height = source.height / aspect_segments;

	auto pixel_ratio = min(target.pixels() / static_cast<double>(source.pixels()), 1.0);
	auto target_aspect_segments = static_cast<uint32_t>(floor(sqrt(pixel_ratio * aspect_segments * aspect_segments)));

	for (auto i : { 0, 1, -1 }) {
		auto target_segments = max(static_cast<uint32_t>(1), min(aspect_segments, target_aspect_segments + i));
		OutputResolution res{ aspect_width * target_segments, aspect_height * target_segments };

		if (res.width > source.width || res.height > source.height)
			continue;
		
		if (res.width % 4 == 0 && res.height % 2 == 0) //libobs enforces multiple of 4 width and multiple of 2 height
			return res;
	}

	return boost::none;
}

static OutputResolution ScaleResolution(const OutputResolution &target, const OutputResolution &source)
{
	{
		auto res = ScaleResolutionInteger(target, source);
		if (res)
			return *res;
	}

	auto pixel_ratio = min(target.pixels() / static_cast<double>(source.pixels()), 1.0);
	OutputResolution res{
		static_cast<uint32_t>(source.width * sqrt(pixel_ratio)),
		static_cast<uint32_t>(source.height * sqrt(pixel_ratio))
	};

	//libobs enforces multiple of 4 width and multiple of 2 height
	res.width &= ~3;
	res.height &= ~1;

	return res;
}

template <typename T, typename U>
static void InitRef(T &ref, const char *msg, void (*release)(U*), U *val)
{
	if (!val)
		throw msg;

	ref = val;
	release(val);
}

static decltype(ProfileSnapshotCreate()) last_session;

struct CrucibleContext {
	mutex bookmarkMutex;
	vector<Bookmark> estimatedBookmarks;
	vector<Bookmark> bookmarks;
	vector<Bookmark> estimatedBufferBookmarks;
	vector<Bookmark> bufferBookmarks;
	int next_bookmark_id = 0;

	uint64_t recordingStartTime = 0;
	bool recordingStartSent = false;
	bool sendRecordingStop = true;

	struct {
		OBSScene scene;
		OBSSceneItem game, webcam, theme;

		void MakePresentable()
		{
			obs_sceneitem_set_order(theme, OBS_ORDER_MOVE_TOP);
			obs_sceneitem_set_order(game, OBS_ORDER_MOVE_BOTTOM);
		}
	} game_and_webcam;

	struct {
		OBSScene scene;
		OBSSceneItem window, webcam, theme;
		
		void MakePresentable()
		{
			obs_sceneitem_set_order(theme, OBS_ORDER_MOVE_TOP);
			obs_sceneitem_set_order(window, OBS_ORDER_MOVE_BOTTOM);
		}
	} window_and_webcam;

	obs_video_info ovi;
	uint32_t fps_den;
	std::string webcam_device;
	OBSSource tunes, mic, gameCapture, webcam, theme, window;
	OBSSourceSignal micMuted, pttActive;
	OBSSourceSignal stopCapture, startCapture, injectFailed, injectRequest, monitorProcess, screenshotSaved;
	OBSEncoder h264, aac, stream_h264;
	string filename = "";
	string profiler_filename = "";
	string muxerSettings = "";
	OBSOutput output, buffer, stream;
	OBSOutputSignal startRecording, stopRecording;
	OBSOutputSignal sentTrackedFrame, bufferSentTrackedFrame;
	OBSOutputSignal bufferSaved, bufferSaveFailed;

	unique_ptr<obs_volmeter_t> tunesMeter;
	OBSSignal tunesLevelsUpdated;

	unique_ptr<obs_volmeter_t> micMeter;
	OBSSignal micLevelsUpdated;

	OutputResolution target = OutputResolution{ 1280, 720 }; //thanks VS2013
	uint32_t target_bitrate = 3500;

	OutputResolution game_res = OutputResolution{ 0, 0 };

	DWORD game_pid = -1;

	obs_hotkey_id ptt_hotkey_id = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id mute_hotkey_id = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id unmute_hotkey_id = OBS_INVALID_HOTKEY_ID;

	obs_hotkey_id bookmark_hotkey_id = OBS_INVALID_HOTKEY_ID;

	struct obs_service_info forge_streaming_service;
	OBSService stream_service;
	bool streaming = false;
	OutputResolution target_stream = OutputResolution{ 1280, 720 };
	uint32_t target_stream_bitrate = 3000;
	OBSOutputSignal startStreaming, stopStreaming;

	vector<pair<string, long long>> requested_screenshots;

	struct RestartThread {
		thread t;
		~RestartThread()
		{
			if (t.joinable())
				t.join();
		}
	} restartThread;

	bool ResetVideo()
	{
		return obs_reset_video(&ovi) == 0;
	}

	void InitLibobs(bool standalone)
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

		RegisterFramebufferSource();

		if (standalone)
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
		InitRef(mic, "Couldn't create audio input device source", obs_source_release,
			obs_source_create(OBS_SOURCE_TYPE_INPUT, "wasapi_input_capture", "wasapi mic", nullptr, nullptr));

		auto weak_mic = OBSGetWeakRef(mic);
		OBSEnumHotkeys([&](obs_hotkey_id id, obs_hotkey_t *key)
		{
			if (obs_hotkey_get_registerer_type(key) != OBS_HOTKEY_REGISTERER_SOURCE)
				return;

			if (obs_hotkey_get_registerer(key) != weak_mic)
				return;

			auto name = obs_hotkey_get_name(key);
			if (name == string("libobs.mute"))
				mute_hotkey_id = id;
			else if (name == string("libobs.unmute"))
				unmute_hotkey_id = id;
			else if (name == string("libobs.push-to-talk"))
				ptt_hotkey_id = id;
		});

		// create audio source
		InitRef(tunes, "Couldn't create audio input source", obs_source_release,
				obs_source_create(OBS_SOURCE_TYPE_INPUT, "wasapi_output_capture", "wasapi loopback", nullptr, nullptr));

		obs_set_output_source(1, tunes);
		
		tunesMeter = OBSVolMeterCreate(OBS_FADER_LOG);
		obs_volmeter_set_update_interval(tunesMeter.get(), 100);

		micMeter = OBSVolMeterCreate(OBS_FADER_LOG);
		obs_volmeter_set_update_interval(micMeter.get(), 100);

		InitRef(game_and_webcam.scene, "Couldn't create game_and_webcam scene", obs_scene_release,
				obs_scene_create("game_and_webcam"));

		InitRef(window_and_webcam.scene, "Couldn't create window_and_webcam scene", obs_scene_release,
				obs_scene_create("window_and_webcam"));

		InitRef(theme, "Couldn't create theme source", obs_source_release,
				obs_source_create(OBS_SOURCE_TYPE_INPUT, "FramebufferSource", "theme overlay", nullptr, nullptr));

		game_and_webcam.theme = obs_scene_add(game_and_webcam.scene, theme);
		window_and_webcam.theme = obs_scene_add(window_and_webcam.scene, theme);

		{
			auto proc = obs_source_get_proc_handler(theme);
			calldata_t data = {};
			proc_handler_call(proc, "get_server_name", &data);

			if (auto name = calldata_string(&data, "name")) {
				ForgeEvents::SendFramebufferConnectionInfo("theme", name);
			} else {
				blog(LOG_WARNING, "CrucibleContext::InitSources: failed to get framebuffer name");
			}
		}
	}

	void InitEncoders()
	{
		auto vsettings = OBSDataCreate();
		obs_data_set_int(vsettings, "bitrate", 0);
		obs_data_set_int(vsettings, "buffer_size", 2 * target_bitrate);
		obs_data_set_int(vsettings, "crf", 23);
		obs_data_set_bool(vsettings, "use_bufsize", true);
		obs_data_set_bool(vsettings, "cbr", false);
		obs_data_set_string(vsettings, "profile", "high");
		obs_data_set_string(vsettings, "preset", "veryfast");

		ostringstream os;
		os << "keyint=30 vbv-maxrate=" << target_bitrate;
		obs_data_set_string(vsettings, "x264opts", os.str().c_str());

		InitRef(h264, "Couldn't create video encoder", obs_encoder_release,
				obs_video_encoder_create("obs_x264", "x264 video", vsettings, nullptr));

		auto ssettings = OBSDataCreate();
		obs_data_set_int(ssettings, "bitrate", target_stream_bitrate);
		obs_data_set_bool(ssettings, "cbr", true);
		obs_data_set_string(ssettings, "profile", "high");
		obs_data_set_string(ssettings, "preset", "veryfast");
		obs_data_set_string(ssettings, "x264opts", "keyint=30");

		InitRef(stream_h264, "Couldn't create stream video encoder", obs_encoder_release,
			obs_video_encoder_create("obs_x264", "stream video", ssettings, nullptr));

		aac = CreateAudioEncoder("aac");
		if (!aac)
			throw "Couldn't create audio encoder";


		obs_encoder_set_video(h264, obs_get_video());
		obs_encoder_set_video(stream_h264, obs_get_video());

		obs_encoder_set_audio(aac, obs_get_audio());
	}

	void InitSignals()
	{
		micMuted
			.SetOwner(mic)
			.SetSignal("mute")
			.SetFunc([=](calldata_t *data)
		{
			AnvilCommands::MicUpdated(calldata_bool(data, "muted"));
		})
			.Connect();

		pttActive
			.SetOwner(mic)
			.SetSignal("push_to_talk_active")
			.SetFunc([=](calldata_t *data)
		{
			AnvilCommands::MicUpdated(!calldata_bool(data, "active"));
		})
			.Connect();

		stopRecording
			.SetSignal("stop")
			.SetFunc([=](calldata*)
		{
			string profiler_path;
			{
				LOCK(updateMutex);
				if (sendRecordingStop) {
					profiler_path = profiler_filename;
					auto data = OBSTransferOwned(obs_output_get_settings(output));
					decltype(bookmarks) full_bookmarks;
					{
						LOCK(bookmarkMutex);
						full_bookmarks = bookmarks;
					}
					ForgeEvents::SendRecordingStop(obs_data_get_string(data, "path"),
						obs_output_get_total_frames(output),
						obs_output_get_output_duration(output),
						BookmarkTimes(bookmarks), ovi.base_width, ovi.base_height, game_pid, full_bookmarks);
					AnvilCommands::ShowIdle();
				}
			}
			StopVideo(); // leak here!!!

			ClearBookmarks();

			auto snap = ProfileSnapshotCreate();
			auto diff = unique_ptr<profiler_snapshot_t>{profile_snapshot_diff(last_session.get(), snap.get())};

			profiler_print(diff.get());
			profiler_print_time_between_calls(diff.get());

			if (!profiler_path.empty() && !profiler_snapshot_dump_csv_gz(diff.get(), profiler_path.c_str())) {
				blog(LOG_INFO, "Failed to dump profiler data to '%s'", profiler_path.c_str());
				profiler_path = "";
			}

			last_session = move(snap);

			ForgeEvents::SendCleanupComplete(profiler_path.empty() ? nullptr : &profiler_path, game_pid);
		});

		startRecording
			.SetSignal("start")
			.SetFunc([=](calldata*)
		{
			auto data = OBSTransferOwned(obs_output_get_settings(output));
			recordingStartTime = os_gettime_ns();
			{
				LOCK(updateMutex);
				if (!recordingStartSent) {
					ForgeEvents::SendRecordingStart(obs_data_get_string(data, "path"));
					recordingStartSent = true;
				}
			}
			AnvilCommands::ShowRecording();
		});

		sentTrackedFrame
			.SetSignal("sent_tracked_frame")
			.SetFunc([=](calldata *data)
		{
			FinalizeBookmark(estimatedBookmarks, bookmarks, calldata_int(data, "id"),
				calldata_int(data, "pts"), static_cast<uint32_t>(calldata_int(data, "timebase_den")));
		});

		bufferSaved
			.SetSignal("buffer_output_finished")
			.SetFunc([=](calldata_t *data)
		{
			auto filename = calldata_string(data, "filename");
			video_tracked_frame_id tracked_id = calldata_int(data, "tracked_frame_id");
			ForgeEvents::SendBufferReady(filename, static_cast<uint32_t>(calldata_int(data, "frames")),
				calldata_float(data, "duration"), BookmarkTimes(bufferBookmarks, calldata_int(data, "start_pts")),
				ovi.base_width, ovi.base_height, FindBookmark(bookmarks, tracked_id));
		});

		bufferSaveFailed
			.SetSignal("buffer_output_failed")
			.SetFunc([=](calldata_t *data)
		{
			auto filename = calldata_string(data, "filename");
			ForgeEvents::SendBufferFailure(filename);
		});

		bufferSentTrackedFrame
			.SetSignal("sent_tracked_frame")
			.SetFunc([=](calldata *data)
		{
			FinalizeBookmark(estimatedBufferBookmarks, bufferBookmarks, calldata_int(data, "id"),
				calldata_int(data, "pts"), static_cast<uint32_t>(calldata_int(data, "timebase_den")));
		});

		stopCapture
			.SetOwner(gameCapture)
			.SetSignal("stop_capture");

		startCapture
			.SetOwner(gameCapture)
			.SetSignal("start_capture");

		injectFailed
			.SetOwner(gameCapture)
			.SetSignal("inject_failed");

		injectRequest
			.SetOwner(gameCapture)
			.SetSignal("inject_request")
			.SetFunc([](calldata_t *data)
		{
			ForgeEvents::SendInjectRequest(calldata_bool(data, "process_is_64bit"), calldata_bool(data, "anti_cheat"),
				static_cast<DWORD>(calldata_int(data, "process_thread_id")));
		});

		monitorProcess
			.SetSignal("monitor_process")
			.SetFunc([](calldata_t *data)
		{
			ForgeEvents::SendMonitorProcess(static_cast<DWORD>(calldata_int(data, "process_id")));
		});

		screenshotSaved
			.SetSignal("screenshot_saved")
			.SetFunc([=](calldata_t *data)
		{
			auto filename = calldata_string(data, "filename");
			auto id = calldata_int(data, "screenshot_id");

			LOCK(updateMutex);
			auto rs = find_if(begin(requested_screenshots), end(requested_screenshots), [&](const pair<string, long long> &p) { return p.second == id; });
			if (rs == end(requested_screenshots))
				return;

			ForgeEvents::SendSavedGameScreenshot(rs->first.c_str(), filename);
			requested_screenshots.erase(rs);
		});

		startStreaming
			.SetSignal("start")
			.SetFunc([=](calldata*)
		{
			streaming = true;
			AnvilCommands::StreamStatus(streaming);
			ForgeEvents::SendStreamingStart();
		});

		stopStreaming
			.SetSignal("stop")
			.SetFunc([=](calldata *data)
		{
			streaming = false;
			AnvilCommands::StreamStatus(streaming);
			ForgeEvents::SendStreamingStop(calldata_int(data, "code"));
		});

	}

	void InitStreamService()
	{
		memset(&forge_streaming_service, 0, sizeof(forge_streaming_service));

		forge_streaming_service.id = "forge_rtmp";
		forge_streaming_service.get_name = [](void *data)->const char *{ return "forge streaming service"; };
		forge_streaming_service.create = [](obs_data_t *settings, obs_service_t *service)->void *{ return obs_data_create(); };
		forge_streaming_service.destroy = [](void *data)->void{ obs_data_release((obs_data_t *)data); };
		forge_streaming_service.update = [](void *data, obs_data_t *settings)->void{ obs_data_apply((obs_data_t *)data, settings); };
		//forge_streaming_service.get_properties = [](void *data)->obs_properties_t *{ return obs_properties_create(); };
		forge_streaming_service.get_url = [](void *data)->const char *{ return obs_data_get_string((obs_data_t *)data, "server"); };
		forge_streaming_service.get_key = [](void *data)->const char *{ return obs_data_get_string((obs_data_t *)data, "key"); };

		obs_register_service(&forge_streaming_service);
		
		InitRef(stream_service, "Couldn't create streaming service", obs_service_release,
			obs_service_create("forge_rtmp", "forge streaming", nullptr, nullptr));
	}

	void UpdateStreamSettings()
	{
		if (game_res.width && game_res.height) {
			auto scaled = ScaleResolution(target_stream, game_res);

			blog(LOG_INFO, "setting stream output size to %ux%u", scaled.width, scaled.height);
			obs_encoder_set_scaled_size(stream_h264, scaled.width, scaled.height);
		} else {
			obs_encoder_set_scaled_size(stream_h264, target_stream.width, AlignX264Height(target_stream.height));
		}

		auto ssettings = OBSDataCreate();
		obs_data_set_int(ssettings, "bitrate", target_stream_bitrate);
		obs_data_set_bool(ssettings, "cbr", true);
		obs_data_set_string(ssettings, "profile", "high");
		obs_data_set_string(ssettings, "preset", "veryfast");
		obs_data_set_string(ssettings, "x264opts", "keyint=30");

		obs_encoder_update(stream_h264, ssettings);

		// todo: set some useful default for these?
		obs_output_set_delay(stream, 0, 0);
		obs_output_set_reconnect_settings(stream, 0, 0);
	}

	void CreateOutput()
	{
		auto osettings = OBSDataCreate();
		obs_data_set_string(osettings, "path", filename.c_str());
		obs_data_set_string(osettings, "muxer_settings", muxerSettings.c_str());

		InitRef(output, "Couldn't create output", obs_output_release,
				obs_output_create("ffmpeg_muxer", "ffmpeg recorder", osettings, nullptr));

		obs_output_set_video_encoder(output, h264);
		obs_output_set_audio_encoder(output, aac, 0);

		InitRef(buffer, "Couldn't create buffer output", obs_output_release,
				obs_output_create("ffmpeg_recordingbuffer", "ffmpeg recordingbuffer", nullptr, nullptr));

		obs_output_set_video_encoder(buffer, h264);
		obs_output_set_audio_encoder(buffer, aac, 0);
		
		
		stopRecording
			.Disconnect()
			.SetOwner(output)
			.Connect();

		startRecording
			.Disconnect()
			.SetOwner(output)
			.Connect();

		sentTrackedFrame
			.Disconnect()
			.SetOwner(output)
			.Connect();

		bufferSaved
			.Disconnect()
			.SetOwner(buffer)
			.Connect();

		bufferSaveFailed
			.Disconnect()
			.SetOwner(buffer)
			.Connect();

		bufferSentTrackedFrame
			.Disconnect()
			.SetOwner(buffer)
			.Connect();

		auto weakGameCapture = OBSGetWeakRef(gameCapture);
		auto weakOutput = OBSGetWeakRef(output);
		auto weakBuffer = OBSGetWeakRef(buffer);

		stopCapture
			.Disconnect()
			.SetOwner(gameCapture)
			.SetFunc([=](calldata_t*)
		{
			if (auto ref = OBSGetStrongRef(weakGameCapture)) {
				auto settings = OBSTransferOwned(obs_source_get_settings(ref));
				obs_data_set_int(settings, "process_id", 0);
				obs_data_set_int(settings, "thread_id", 0);
				obs_source_update(ref, settings);

				if (OBSGetOutputSource(0) == ref)
					obs_set_output_source(0, nullptr);
			}

			auto ref = OBSGetStrongRef(weakOutput);
			if (ref)
				obs_output_stop(ref);

			ref = OBSGetStrongRef(weakBuffer);
			if (ref)
				obs_output_stop(ref);
		}).Connect();

		startCapture
			.Disconnect()
			.SetOwner(gameCapture)
			.SetFunc([=](calldata_t *data)
		{
			AnvilCommands::Connect(game_pid);

			if (UpdateSize(static_cast<uint32_t>(calldata_int(data, "width")),
				       static_cast<uint32_t>(calldata_int(data, "height"))))
				return;

			auto ref = OBSGetStrongRef(weakOutput);
			if (ref)
				obs_output_start(ref);

			ref = OBSGetStrongRef(weakBuffer);
			if (ref)
				obs_output_start(ref);
		}).Connect();
	}

	void CreateStreamOutput()
	{
		InitRef(stream, "Couldn't create stream output", obs_output_release,
			obs_output_create("rtmp_output", "rtmp streaming", nullptr, nullptr));

		obs_output_set_video_encoder(stream, stream_h264);
		obs_output_set_audio_encoder(stream, aac, 0);
		obs_output_set_service(stream, stream_service);

		stopStreaming
			.Disconnect()
			.SetOwner(stream)
			.Connect();

		startStreaming
			.Disconnect()
			.SetOwner(stream)
			.Connect();
	}

	void ClearBookmarks()
	{
		LOCK(bookmarkMutex);
		estimatedBookmarks.clear();
		bookmarks.clear();

		estimatedBufferBookmarks.clear();
		bufferBookmarks.clear();

		next_bookmark_id = 0;
	}

	vector<double> BookmarkTimes(vector<Bookmark> &bookmarks, int64_t start_pts = 0)
	{
		vector<double> res;
		{
			LOCK(bookmarkMutex);

			res.reserve(bookmarks.size());
			for (auto &bookmark : bookmarks) {
				if (bookmark.pts < start_pts)
					continue;

				res.push_back((bookmark.pts - start_pts) / static_cast<double>(bookmark.fps_den));
			}
		}

		return res;
	}

	boost::optional<Bookmark> FindBookmark(const vector<Bookmark> &bookmarks, video_tracked_frame_id id)
	{
		LOCK(bookmarkMutex);

		for (auto &bookmark : bookmarks) {
			if (bookmark.tracked_id == id)
				return bookmark;
		}

		return boost::none;
	}

	void FinalizeBookmark(vector<Bookmark> &estimates, vector<Bookmark> &bookmarks, video_tracked_frame_id tracked_id, int64_t pts, uint32_t fps_den)
	{
		LOCK(bookmarkMutex);

		auto it = find_if(begin(estimates), end(estimates), [&](const Bookmark &bookmark)
		{
			return bookmark.tracked_id == tracked_id;
		});
		if (it == end(estimates))
			return;

		auto new_time = pts / static_cast<double>(fps_den);

		blog(LOG_INFO, "Updated bookmark from %g s to %g s (tracked frame %lld)", it->time, new_time, tracked_id);

		it->fps_den = fps_den;
		it->pts = pts;
		it->time = new_time;

		bookmarks.push_back(*it);
		estimates.erase(it);
	}

	void CreateBookmark(OBSData &obj)
	{
		if (!output || !obs_output_active(output))
			return;

		LOCK(bookmarkMutex);
		estimatedBookmarks.emplace_back();
		auto &bookmark = estimatedBookmarks.back();

		estimatedBufferBookmarks.emplace_back();
		auto &bufferBookmark = estimatedBufferBookmarks.back();

		bookmark.id = bufferBookmark.id = ++next_bookmark_id;

		bookmark.time = bufferBookmark.time = (os_gettime_ns() - recordingStartTime) / 1000000000.;

		bookmark.extra_data = bufferBookmark.extra_data = OBSDataGetObj(obj, "extra_data");

		video_tracked_frame_id tracked_id = 0;
		if (!SaveRecordingBuffer(obj, &tracked_id))
			tracked_id = obs_track_next_frame();

		bookmark.tracked_id = bufferBookmark.tracked_id = tracked_id;

		blog(LOG_INFO, "Created bookmark at offset %g s (estimated, tracking frame %lld)", bookmark.time, tracked_id);

		{
			const char *filename = obs_data_get_string(obj, "screenshot");
			if (filename && *filename)
				SaveGameScreenshot(filename);
		}

		if (!obs_data_get_bool(obj, "suppress_indicator"))
			AnvilCommands::ShowBookmark();
	}

	recursive_mutex updateMutex;

	bool SaveRecordingBuffer(obs_data_t *settings, video_tracked_frame_id *tracked_id=nullptr)
	{
		if (!settings)
			return false;

		const char *filename = obs_data_get_string(settings, "filename");
		if (!filename || !*filename)
			return false;

		if (!buffer || !obs_output_active(buffer))
			return false;

		calldata_t param{};
		calldata_init(&param);
		calldata_set_string(&param, "filename", filename);

		{
			LOCK(updateMutex);
			auto proc = obs_output_get_proc_handler(buffer);
			proc_handler_call(proc, "output_precise_buffer", &param);
		}

		if (tracked_id)
			*tracked_id = calldata_int(&param, "tracked_frame_id");

		calldata_free(&param);
		return true;
	}

	void ForwardInjectorResult(obs_data_t *res)
	{
		if (!res)
			return;

		DWORD code = obs_data_has_user_value(res, "code") ? static_cast<DWORD>(obs_data_get_int(res, "code")) : -1;

		calldata_t param{};
		calldata_init(&param);
		calldata_set_int(&param, "code", code);

		{
			LOCK(updateMutex);
			auto proc = obs_source_get_proc_handler(gameCapture);
			proc_handler_call(proc, "injector_result", &param);
		}

		calldata_free(&param);
	}

	void ForwardMonitoredProcessExit(obs_data_t *res)
	{
		if (!res)
			return;

		auto pid = static_cast<DWORD>(obs_data_get_int(res, "process_id"));
		auto code = static_cast<DWORD>(obs_data_has_user_value(res, "code") ? obs_data_get_int(res, "process_id") : -1);

		calldata_t param{};
		calldata_init(&param);
		calldata_set_int(&param, "process_id", pid);
		calldata_set_int(&param, "code", code);

		{
			LOCK(updateMutex);
			auto proc = obs_source_get_proc_handler(gameCapture);
			proc_handler_call(proc, "monitored_process_exit", &param);
		}

		calldata_free(&param);
	}

	void DeleteGameCapture()
	{
		LOCK(updateMutex);
		auto source = OBSGetOutputSource(0);
		if (source == gameCapture)
			obs_set_output_source(0, nullptr);

		gameCapture = nullptr;

		obs_sceneitem_remove(game_and_webcam.game);
		game_and_webcam.game = nullptr;
	}

	void CreateGameCapture(obs_data_t *settings)
	{
		if (!settings)
			return;

		LOCK(updateMutex);
		game_pid = static_cast<DWORD>(obs_data_get_int(settings, "process_id"));

		auto path = GetModulePath(nullptr);
		DStr path64;
		dstr_printf(path64, "%sAnvilRendering64.dll", path->array);
		dstr_cat(path, "AnvilRendering.dll");

		obs_data_set_string(settings, "overlay_dll", path);
		obs_data_set_string(settings, "overlay_dll64", path64);
		//obs_data_set_bool(settings, "allow_ipc_injector", true);

		InitRef(gameCapture, "Couldn't create game capture source", obs_source_release,
			obs_source_create(OBS_SOURCE_TYPE_INPUT, "game_capture", "game capture", settings, nullptr));

		injectFailed
			.Disconnect()
			.SetOwner(gameCapture)
			.SetFunc([=](calldata_t *data)
		{
			ForgeEvents::SendInjectFailed(static_cast<long*>(calldata_ptr(data, "injector_exit_code")));
		}).Connect();

		injectRequest
			.Disconnect()
			.SetOwner(gameCapture)
			.Connect();

		monitorProcess
			.Disconnect()
			.SetOwner(gameCapture)
			.Connect();

		screenshotSaved
			.Disconnect()
			.SetOwner(gameCapture)
			.Connect();

		if (game_and_webcam.game)
			obs_sceneitem_remove(game_and_webcam.game);
		game_and_webcam.game = obs_scene_add(game_and_webcam.scene, gameCapture);
		game_and_webcam.MakePresentable();

		if (!OBSGetOutputSource(0))
			obs_set_output_source(0, gameCapture);
	}

	void ResetWindowCapture(obs_data_t *settings)
	{
		if (!settings) {
			obs_sceneitem_remove(window_and_webcam.window);
			window_and_webcam.window = nullptr;
			window = nullptr;

			return;
		}

		if (window) {
			obs_source_update(window, settings);
			return;
		}

		InitRef(window, "Couldn't create window capture source", obs_source_release,
			obs_source_create(OBS_SOURCE_TYPE_INPUT, "window_capture", "window capture", settings, nullptr));

		if (!window)
			return;

		window_and_webcam.window = obs_scene_add(window_and_webcam.scene, window);
		obs_sceneitem_set_order(window_and_webcam.window, OBS_ORDER_MOVE_BOTTOM);

		obs_sceneitem_set_bounds_type(window_and_webcam.window, OBS_BOUNDS_MAX_ONLY);
		obs_sceneitem_set_bounds_alignment(window_and_webcam.window, OBS_ALIGN_CENTER);

		UpdateSourceBounds();

		window_and_webcam.MakePresentable();
	}

	void SetOutputScene(string scene_name)
	{
		obs_source_t *source = nullptr;

		auto source_from_scene = [&](auto &container)
		{
			container.MakePresentable();
			return obs_scene_get_source(container.scene);
		};

		auto current_scene_name = [&]()
		{
			auto cur = OBSGetOutputSource(0);
			if (!cur || cur == gameCapture)
				return "game_only";
			if (cur == source_from_scene(game_and_webcam))
				return "game";
			if (cur == source_from_scene(window_and_webcam))
				return "window";

			return "unknown";
		};

		if (scene_name == "game_only") {
			source = gameCapture;
		} else if (scene_name == "game") {
			source = source_from_scene(game_and_webcam);
		} else if (scene_name == "window") {
			source = source_from_scene(window_and_webcam);
		} else {
			ForgeEvents::SendSelectSceneResult(scene_name, current_scene_name(), false);
			return;
		}

		obs_set_output_source(0, source);
		ForgeEvents::SendSelectSceneResult(scene_name, scene_name, true);
	}

	void SetSourceVolume(const string source_name, double volume, bool mute)
	{
		const map<string, OBSSource &> sources = 
		{
			{"desktop", tunes},
			{"microphone", mic}
		};

		auto elem = sources.find(source_name);
		if (elem == sources.end())
		{
			blog(LOG_INFO, "SetSourceVolume: source '%s' not found", source_name.c_str());
			return;
		}

		obs_source_set_volume(elem->second, volume);
		obs_source_set_muted(elem->second, mute);
	}

	void EnableSourceLevelMeters(bool enabled)
	{	
		tunesLevelsUpdated.Disconnect();
		micLevelsUpdated.Disconnect();

		obs_volmeter_detach_source(tunesMeter.get());
		obs_volmeter_detach_source(micMeter.get());

		if (enabled)
		{
			auto handler = [](void *param, calldata_t *calldata)
			{
				const char *source = (const char *)param;

				float level = calldata_float(calldata, "level");
				float mag = calldata_float(calldata, "magnitude");
				float peak = calldata_float(calldata, "peak");
				bool  muted = calldata_bool(calldata, "muted");
				//blog(LOG_INFO, "levels_updated for %s: level %.3f, magnitude %.3f, peak %.3f, muted: %s", source, mag, level, peak, muted ? "true" : "false");
				ForgeEvents::SendAudioSourceLevel(source, level, mag, peak, muted);
			};

			tunesLevelsUpdated.Connect(obs_volmeter_get_signal_handler(tunesMeter.get()), "levels_updated", handler, "desktop");
			micLevelsUpdated.Connect(obs_volmeter_get_signal_handler(micMeter.get()), "levels_updated", handler, "microphone");

			obs_volmeter_attach_source(tunesMeter.get(), tunes);
			obs_volmeter_attach_source(micMeter.get(), mic);
		}
	}

	void StartStreaming(const char *server, const char *key, const char *version)
	{
		auto settings = OBSDataCreate();
		obs_data_set_string(settings, "server", server);
		obs_data_set_string(settings, "key", key);
		obs_service_update(stream_service, settings);

		DStr encoder_name;
		dstr_printf(encoder_name, "Crucible (%s)", version);

		auto ssettings = OBSDataCreate();
		obs_data_set_string(ssettings, "encoder_name", encoder_name->array);
		obs_output_update(stream, ssettings);

		UpdateStreamSettings();
		obs_output_start(stream);
	}

	void StopStreaming()
	{
		obs_output_stop(stream);
	}

	void UpdateSettings(obs_data_t *settings)
	{
		if (!settings)
			return;

		DStr str;

		auto bookmark_key = OBSDataGetObj(settings, "bookmark_key");
		obs_key_combination bookmark_combo = {
			(obs_data_get_bool(bookmark_key, "shift") ? INTERACT_SHIFT_KEY : 0u) |
			(obs_data_get_bool(bookmark_key, "meta") ? INTERACT_COMMAND_KEY : 0u) |
			(obs_data_get_bool(bookmark_key, "ctrl") ? INTERACT_CONTROL_KEY : 0u) |
			(obs_data_get_bool(bookmark_key, "alt") ? INTERACT_ALT_KEY : 0u),
			obs_key_from_virtual_key(static_cast<int>(obs_data_get_int(bookmark_key, "keycode")))
		};

		AnvilCommands::HotkeyMatches(bookmark_combo.key == OBS_KEY_F5 && !bookmark_combo.modifiers);

#ifdef ANVIL_HOTKEYS
		AnvilCommands::SendSettings(bookmark_key,
			OBSDataGetObj(settings, "highlight_key"),
			OBSDataGetObj(settings, "stream_key"));
#else
		obs_key_combination_to_str(bookmark_combo, str);
		blog(LOG_INFO, "bookmark hotkey uses '%s'", str->array);

		obs_hotkey_load_bindings(bookmark_hotkey_id, &bookmark_combo, 1);
#endif

		auto ptt_key = OBSDataGetObj(settings, "ptt_key");
		auto microphone = OBSDataGetObj(settings, "microphone");
		if (!microphone) {
			blog(LOG_WARNING, "no microphone data in settings");
			return;
		}

		auto enabled = obs_data_get_bool(microphone, "enabled");
		auto ptt = obs_data_get_bool(microphone, "ptt_mode");
		auto source_settings = OBSDataGetObj(microphone, "source_settings");
		
		auto continuous = enabled && !ptt;
		ptt = enabled && ptt;

		obs_key_combination combo = {
			(obs_data_get_bool(ptt_key, "shift") ? INTERACT_SHIFT_KEY : 0u) |
			(obs_data_get_bool(ptt_key, "meta")  ? INTERACT_COMMAND_KEY : 0u) |
			(obs_data_get_bool(ptt_key, "ctrl")  ? INTERACT_CONTROL_KEY : 0u) |
			(obs_data_get_bool(ptt_key, "alt")   ? INTERACT_ALT_KEY : 0u),
			obs_key_from_virtual_key(static_cast<int>(obs_data_get_int(ptt_key, "keycode")))
		};

		obs_key_combination_to_str(combo, str);
		blog(LOG_INFO, "mic hotkey uses '%s'", str->array);

		auto desktop_audio_settings = OBSDataGetObj(settings, "desktop_audio");

		LOCK(updateMutex);
		obs_source_update(tunes, desktop_audio_settings);
		obs_source_update(mic, source_settings);
		obs_source_set_muted(mic, false);
		obs_source_enable_push_to_talk(mic, ptt);
		AnvilCommands::MicUpdated(ptt, enabled, ptt);
		obs_hotkey_load_bindings(ptt_hotkey_id, &combo, ptt ? 1 : 0);
		obs_hotkey_load_bindings(mute_hotkey_id, &combo, continuous ? 1 : 0);
		obs_hotkey_load_bindings(unmute_hotkey_id, &combo, continuous ? 1 : 0);
		obs_set_output_source(2, enabled ? mic : nullptr);

		auto webcam_ = OBSDataGetObj(settings, "webcam");

		auto remove_from_scene = [&](auto &container)
		{
			obs_sceneitem_remove(container.webcam);
			container.webcam = nullptr;
		};

		DEFER {
			game_and_webcam.MakePresentable();
			window_and_webcam.MakePresentable();
		};

		if (!obs_data_has_user_value(webcam_, "device")) {
			remove_from_scene(game_and_webcam);
			remove_from_scene(window_and_webcam);

			webcam = nullptr;

			return;
		}

		{
			auto dev = obs_data_get_string(webcam_, "device");

			auto webcam_settings = OBSDataCreate();
			obs_data_set_string(webcam_settings, "video_device_id", dev);

			if (webcam && webcam_device == dev)
				obs_source_update(webcam, webcam_settings);
			else {
				remove_from_scene(game_and_webcam);
				remove_from_scene(window_and_webcam);

				InitRef(webcam, "Couldn't create webcam source", obs_source_release,
					obs_source_create(OBS_SOURCE_TYPE_INPUT, "dshow_input", "webcam", webcam_settings, nullptr));
			}

			webcam_device = dev;

			obs_source_set_muted(webcam, true); // webcams can have mics attached to them that dshow_input will pick up sometimes
		}

		auto add_to_scene = [&](auto &container)
		{
			if (container.webcam)
				return;

			container.webcam = obs_scene_add(container.scene, webcam);

			obs_sceneitem_set_bounds_type(container.webcam, OBS_BOUNDS_SCALE_INNER);
			obs_sceneitem_set_bounds_alignment(container.webcam, OBS_ALIGN_BOTTOM);
			obs_sceneitem_set_alignment(container.webcam, OBS_ALIGN_BOTTOM | OBS_ALIGN_LEFT);
		};

		add_to_scene(game_and_webcam);
		add_to_scene(window_and_webcam);

		UpdateSourceBounds();
	}

	void UpdateSourceBounds()
	{
		if (ovi.base_height && ovi.base_width) {
			auto vec = vec2{ ovi.base_width / 6.f, ovi.base_height / 6.f };
			auto pos = vec2{ 0.f, static_cast<float>(ovi.base_height) };

			auto update_scene = [&](auto &container)
			{
				obs_sceneitem_set_bounds(container.webcam, &vec);
				obs_sceneitem_set_pos(container.webcam, &pos);
			};

			update_scene(game_and_webcam);
			update_scene(window_and_webcam);

			auto full = vec2{ ovi.base_width / 1.f, ovi.base_height / 1.f };
			obs_sceneitem_set_bounds(window_and_webcam.window, &full);
		}
	}

	void UpdateEncoder(obs_data_t *settings)
	{
		if (!settings)
			return;

		obs_encoder_update(h264, settings);
	}

	void UpdateFilenames(const char *path, const char *profiler_path)
	{
		if (!path)
			return;

		LOCK(updateMutex);
		filename = path;
		profiler_filename = profiler_path;
	}

	void UpdateMuxerSettings(const char *settings)
	{
		if (!settings)
			return;

		LOCK(updateMutex);
		muxerSettings = settings;
	}

	bool UpdateSize(uint32_t width, uint32_t height)
	{
		LOCK(updateMutex);

		game_res.width = width;
		game_res.height = height;

		ForgeEvents::SendBrowserSizeHint(width, height);

		auto scaled = ScaleResolution(target, game_res);

		bool output_dimensions_changed = scaled.width != ovi.output_width || scaled.height != ovi.output_height;

		if (width == ovi.base_width && height == ovi.base_height && !output_dimensions_changed)
			return false;

		ovi.base_width = width;
		ovi.base_height = height;
		ovi.output_width = scaled.width;
		ovi.output_height = scaled.height;

		// TODO: this is probably not really safe, should introduce a command queue soon
		if (restartThread.t.joinable())
			restartThread.t.join();

		bool streaming = obs_output_active(stream);

		restartThread.t = thread{[=]()
		{
			{
				LOCK(updateMutex);
				sendRecordingStop = false;
			}

			if (output_dimensions_changed) {
				obs_output_stop(stream);
				StopVideo();
			}
			
			StartVideo();

			obs_output_start(this->output);
			obs_output_start(buffer);
			if (streaming)
				obs_output_start(stream);

			{
				LOCK(updateMutex);
				sendRecordingStop = true;
			}
		}};

		return true;
	}

	void UpdateVideoSettings(obs_data_t *settings)
	{
		if (!settings)
			return;

		OutputResolution new_res{
			static_cast<uint32_t>(obs_data_get_int(settings, "width")),
			static_cast<uint32_t>(obs_data_get_int(settings, "height"))
		};
		uint32_t max_rate = static_cast<uint32_t>(obs_data_get_int(settings, "max_rate"));

		OutputResolution new_stream_res{
			static_cast<uint32_t>(obs_data_get_int(settings, "stream_width")),
			static_cast<uint32_t>(obs_data_get_int(settings, "stream_height"))
		};
		uint32_t stream_rate = static_cast<uint32_t>(obs_data_get_int(settings, "stream_rate"));

		LOCK(updateMutex);
		if (max_rate) {
			target_bitrate = max_rate;

			if (h264) {
				auto vsettings = OBSDataCreate();
				obs_data_set_int(vsettings, "bitrate", 0);
				obs_data_set_int(vsettings, "buffer_size", 2 * max_rate);
				obs_data_set_int(vsettings, "crf", 23);
				obs_data_set_bool(vsettings, "use_bufsize", true);
				obs_data_set_bool(vsettings, "cbr", false);
				obs_data_set_string(vsettings, "profile", "high");
				obs_data_set_string(vsettings, "preset", "veryfast");

				ostringstream os;
				os << "keyint=30 vbv-maxrate=" << max_rate;
				obs_data_set_string(vsettings, "x264opts", os.str().c_str());

				obs_encoder_update(h264, vsettings);
			}
		}

		if (stream_rate) {
			target_stream_bitrate = stream_rate;

			target_stream = new_stream_res;
			
			if (stream_h264) {
				auto vsettings = OBSDataCreate();
				obs_data_set_int(vsettings, "bitrate", stream_rate);
				obs_data_set_bool(vsettings, "cbr", true);
				obs_data_set_string(vsettings, "profile", "high");
				obs_data_set_string(vsettings, "preset", "veryfast");
				obs_data_set_string(vsettings, "x264opts", "keyint=30");

				if (game_res.width && game_res.height) {
					auto scaled = ScaleResolution(target_stream, game_res);

					obs_encoder_set_scaled_size(stream_h264, scaled.width, scaled.height);
				} else {
					obs_encoder_set_scaled_size(stream_h264, target_stream.width, AlignX264Height(target_stream.height));
				}

				obs_encoder_update(stream_h264, vsettings);
			}
		}

		if (new_res.width && new_res.height) {
			target = new_res;

			if (obs_output_active(output))
				UpdateSize(game_res.width, game_res.height);
		}
	}

	void SaveGameScreenshot(const char *filename)
	{
		calldata_t data;
		calldata_init(&data);

		calldata_set_string(&data, "filename", filename);

		{
			LOCK(updateMutex);
			auto proc = obs_source_get_proc_handler(gameCapture);
			proc_handler_call(proc, "save_screenshot", &data);

			auto id = calldata_int(&data, "screenshot_id");
			requested_screenshots.emplace_back(filename, id);
		}

		calldata_free(&data);
	}
	
	bool stopping = false;
	void StopVideo()
	{
		LOCK(updateMutex);
		if (stopping)
			return;

		ProfileScope(profile_store_name(obs_get_profiler_name_store(), "StopVideo()"));

		stopping = true;
		if (obs_output_active(output))
			obs_output_stop(output);
		if (obs_output_active(buffer))
			obs_output_stop(buffer);

		output = nullptr;
		buffer = nullptr;

		game_res.width = 0;
		game_res.height = 0;

		stopping = false;
	}

	void StartVideo()
	{
		LOCK(updateMutex);
		auto name = profile_store_name(obs_get_profiler_name_store(),
			"StartVideo(%s)", filename.c_str());
		profile_register_root(name, 0);

		ProfileScope(name);

		ovi.fps_den = fps_den;
		ResetVideo();

		UpdateSourceBounds();

		obs_encoder_set_video(h264, obs_get_video());
		obs_encoder_set_video(stream_h264, obs_get_video());

		obs_encoder_set_audio(aac, obs_get_audio());
		
		CreateOutput();
	}

	void StartVideoCapture()
	{
		LOCK(updateMutex);
		recordingStartSent = false;
		sendRecordingStop = true;

		StartVideo();
	}
};

static void HandleConnectCommand(CrucibleContext &cc, OBSData &obj)
{
	const char *str = nullptr;

	if ((str = obs_data_get_string(obj, "log"))) {
		if (log_client.Open(str)) {
			vector<string> buffered_logs;
			if (store_startup_log) {
				LOCK(startup_log_mutex);
				swap(buffered_logs, startup_logs);
				store_startup_log = false;
			}

			blog(LOG_INFO, "Connected log to '%s'", str);

			if (buffered_logs.size()) {
				blog(LOG_INFO, "Replaying startup log to '%s':", str);
				for (auto &log : buffered_logs)
					log_client.Write(log);
				blog(LOG_INFO, "Done replaying startup log to '%s'", str);
			}
		}
	}

	if ((str = obs_data_get_string(obj, "event"))) {
		if (event_client.Open(str)) {
			blog(LOG_INFO, "Connected event to '%s'", str);

			ForgeEvents::SendQueuedEvents();
		}
	}

	if ((str = obs_data_get_string(obj, "anvil_event"))) {
		AnvilCommands::SendForgeInfo(str);
	}
}

static void HandleCaptureCommand(CrucibleContext &cc, OBSData &obj)
{
	cc.StopVideo();

	last_session = ProfileSnapshotCreate();

	cc.CreateGameCapture(OBSDataGetObj(obj, "game_capture"));
	cc.UpdateEncoder(OBSDataGetObj(obj, "encoder"));
	cc.UpdateStreamSettings();
	cc.UpdateFilenames(obs_data_get_string(obj, "filename"), obs_data_get_string(obj, "profiler_data"));
	cc.UpdateMuxerSettings(obs_data_get_string(obj, "muxer_settings"));

	blog(LOG_INFO, "Starting new capture");

	AnvilCommands::ResetShowWelcome();
	cc.StartVideoCapture();
}

static void HandleQueryMicsCommand(CrucibleContext&, OBSData&)
{
	unique_ptr<obs_properties_t> props{obs_get_source_properties(OBS_SOURCE_TYPE_INPUT, "wasapi_input_capture")};

	auto devices = OBSDataArrayCreate();

	auto prop = obs_properties_get(props.get(), "device_id");

	for (size_t i = 0, c = obs_property_list_item_count(prop); i < c; i++) {
		auto device = OBSDataCreate();
		obs_data_set_string(device, "name", obs_property_list_item_name(prop, i));
		obs_data_set_string(device, "device", obs_property_list_item_string(prop, i));
		obs_data_array_push_back(devices, device);
	}

	ForgeEvents::SendQueryMicsResponse(devices);
}

static void HandleUpdateSettingsCommand(CrucibleContext &cc, OBSData &obj)
{
	cc.UpdateSettings(OBSDataGetObj(obj, "settings"));
}

static void HandleSaveRecordingBuffer(CrucibleContext &cc, OBSData &obj)
{
	cc.SaveRecordingBuffer(obj);
}

static void HandleCreateBookmark(CrucibleContext &cc, OBSData &obj)
{
	cc.CreateBookmark(obj);
}

static void HandleStopRecording(CrucibleContext &cc, OBSData &)
{
	AnvilCommands::ShowCacheLimitExceeded();

	cc.StopVideo();
}

static void HandleInjectorResult(CrucibleContext &cc, OBSData &data)
{
	cc.ForwardInjectorResult(data);
}

static void HandleMonitoredProcessExit(CrucibleContext &cc, OBSData &data)
{
	cc.ForwardMonitoredProcessExit(data);
}

static void HandleUpdateVideoSettingsCommand(CrucibleContext &cc, OBSData &obj)
{
	cc.UpdateVideoSettings(OBSDataGetObj(obj, "settings"));
}

static void HandleSetCursor(CrucibleContext&, OBSData &obj)
{
	AnvilCommands::SendCursor(obj);
}

static void HandleClipFinished(CrucibleContext &cc, OBSData &obj)
{
	AnvilCommands::ClipFinished(obs_data_get_bool(obj, "success"));
}

static void HandleForgeWillClose(CrucibleContext &cc, OBSData&)
{
	cc.StopVideo();
}

static void HandleStartStreaming(CrucibleContext &cc, OBSData& obj)
{
	cc.CreateStreamOutput();
	cc.StartStreaming(obs_data_get_string(obj, "server"), obs_data_get_string(obj, "key"), obs_data_get_string(obj, "version"));
}

static void HandleStopStreaming(CrucibleContext &cc, OBSData&)
{
	cc.StopStreaming();
}

static void HandleGameScreenshot(CrucibleContext &cc, OBSData &obj)
{
	cc.SaveGameScreenshot(obs_data_get_string(obj, "filename"));
}

static void HandleQueryWebcams(CrucibleContext&, OBSData&)
{
	auto props = obs_get_source_properties(OBS_SOURCE_TYPE_INPUT, "dshow_input");

	DEFER {
		obs_properties_destroy(props);
	};

	auto result = OBSDataArrayCreate();

	DEFER {
		ForgeEvents::SendQueryWebcamsResponse(result);
	};

	auto prop = obs_properties_get(props, "video_device_id");
	if (prop) {
		auto count = obs_property_list_item_count(prop);
		for (decltype(count) i = 0; i < count; i++) {
			if (obs_property_list_item_disabled(prop, i))
				continue;

			auto data = OBSDataCreate();
			obs_data_set_string(data, "name", obs_property_list_item_name(prop, i));
			obs_data_set_string(data, "device", obs_property_list_item_string(prop, i));

			obs_data_array_push_back(result, data);
		}
	}
}

static void HandleQueryDesktopAudioDevices(CrucibleContext&, OBSData&)
{
	auto props = obs_get_source_properties(OBS_SOURCE_TYPE_INPUT, "wasapi_output_capture");

	DEFER {
		obs_properties_destroy(props);
	};

	auto result = OBSDataArrayCreate();

	DEFER {
		ForgeEvents::SendQueryDesktopAudioDevicesResponse(result);
	};

	auto prop = obs_properties_get(props, "device_id");
	if (prop) {
		auto count = obs_property_list_item_count(prop);
		for (decltype(count) i = 0; i < count; i++) {
			if (obs_property_list_item_disabled(prop, i))
				continue;

			auto data = OBSDataCreate();
			obs_data_set_string(data, "name", obs_property_list_item_name(prop, i));
			obs_data_set_string(data, "device", obs_property_list_item_string(prop, i));

			obs_data_array_push_back(result, data);
		}
	}
}

static void HandleQueryWindows(CrucibleContext&, OBSData&)
{
	auto props = obs_get_source_properties(OBS_SOURCE_TYPE_INPUT, "window_capture");

	DEFER{
		obs_properties_destroy(props);
	};

	auto result = OBSDataArrayCreate();

	DEFER{
		ForgeEvents::SendQueryWindowsResponse(result);
	};

	auto prop = obs_properties_get(props, "window");
	if (prop) {
		auto count = obs_property_list_item_count(prop);
		for (decltype(count) i = 0; i < count; i++) {
			if (obs_property_list_item_disabled(prop, i))
				continue;

			auto data = OBSDataCreate();
			obs_data_set_string(data, "name", obs_property_list_item_name(prop, i));
			obs_data_set_string(data, "window", obs_property_list_item_string(prop, i));

			obs_data_array_push_back(result, data);
		}
	}
}

static void HandleCaptureWindow(CrucibleContext &cc, OBSData &data)
{
	cc.ResetWindowCapture(OBSDataGetObj(data, "window"));
}

static void HandleSelectScene(CrucibleContext &cc, OBSData &data)
{
	cc.SetOutputScene(obs_data_get_string(data, "scene"));
}

static void HandleSetSourceVolume(CrucibleContext &cc, OBSData &data)
{
	cc.SetSourceVolume(obs_data_get_string(data, "source"), obs_data_get_double(data, "volume"), obs_data_get_bool(data, "mute"));
}

static void HandleEnableSourceLevelMeters(CrucibleContext &cc, OBSData &data)
{
	cc.EnableSourceLevelMeters(obs_data_get_bool(data, "enabled"));
}

static void HandleCommand(CrucibleContext &cc, const uint8_t *data, size_t size)
{
	static const map<string, void(*)(CrucibleContext&, OBSData&)> known_commands = {
		{ "connect", HandleConnectCommand },
		{ "capture_new_process", HandleCaptureCommand },
		{ "query_mics", HandleQueryMicsCommand },
		{ "update_settings", HandleUpdateSettingsCommand },
		{ "save_recording_buffer", HandleSaveRecordingBuffer },
		{ "create_bookmark", HandleCreateBookmark },
		{ "stop_recording", HandleStopRecording },
		{ "injector_result", HandleInjectorResult },
		{ "monitored_process_exit", HandleMonitoredProcessExit },
		{ "update_video_settings", HandleUpdateVideoSettingsCommand },
		{ "set_cursor", HandleSetCursor },
		{ "dismiss_overlay", [](CrucibleContext&, OBSData &data) { AnvilCommands::DismissOverlay(data); } },
		{ "clip_accepted", [](CrucibleContext&, OBSData&) { AnvilCommands::ShowClipping(); } },
		{ "clip_finished", HandleClipFinished },
		{ "forge_will_close", HandleForgeWillClose },
		{ "start_streaming", HandleStartStreaming },
		{ "stop_streaming", HandleStopStreaming },
		{ "save_game_screenshot", HandleGameScreenshot },
		{ "query_webcams", HandleQueryWebcams },
		{ "query_desktop_audio_devices", HandleQueryDesktopAudioDevices },
		{ "query_windows", HandleQueryWindows },
		{ "capture_window", HandleCaptureWindow },
		{ "select_scene", HandleSelectScene },
		{ "set_source_volume", HandleSetSourceVolume },
		{ "enable_source_level_meters", HandleEnableSourceLevelMeters },
	};
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

	auto elem = known_commands.find(str);
	if (elem == cend(known_commands))
		return blog(LOG_WARNING, "Unknown command: %s", str);

	elem->second(cc, obj);

	// TODO: Handle changes to frame rate, target resolution, encoder type,
	//       ...
}


struct FreeHandle
{
	void operator()(HANDLE h)
	{
		CloseHandle(h);
	}
};
using ProcessHandle = unique_ptr<void, FreeHandle>;

void TestVideoRecording(TestWindow &window, ProcessHandle &forge, HANDLE start_event)
{
	try
	{
		CrucibleContext crucibleContext;

		{
			ProfileScope("CrucibleContext Init");

			crucibleContext.InitLibobs(!forge);
			crucibleContext.InitStreamService();
			crucibleContext.InitSources();
			crucibleContext.InitEncoders();
			crucibleContext.InitSignals();
			crucibleContext.StopVideo();
		}

#ifdef TEST_WINDOW
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
#endif

		auto path = GetModulePath(nullptr);
		DStr path64;
		dstr_printf(path64, "%sAnvilRendering64.dll", path->array);
		dstr_cat(path, "AnvilRendering.dll");

		// update source settings - tell game_capture to try and capture hl2: lost coast
		/*auto csettings = OBSDataCreate();
		obs_data_set_bool(csettings, "capture_any_fullscreen", false);
		obs_data_set_bool(csettings, "capture_cursor", true);
		obs_data_set_string(csettings, "overlay_dll", path);
		obs_data_set_string(csettings, "overlay_dll64", path64);
		obs_data_set_string(csettings, "window", "Half-Life 2#3A Lost Coast:Valve001:hl2.exe");
		crucibleContext.UpdateGameCapture(csettings);*/

		crucibleContext.bookmark_hotkey_id = obs_hotkey_register_frontend("bookmark hotkey", "bookmark hotkey",
			[](void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
		{
			if (pressed)
				static_cast<CrucibleContext*>(data)->CreateBookmark(OBSDataCreate());
		}, &crucibleContext);

		auto handleCommand = [&](const uint8_t *data, size_t size)
		{
			HandleCommand(crucibleContext, data, size);
		};

		IPCServer remote{"ForgeCrucible", handleCommand};

		last_session = ProfileSnapshotCreate();
		profiler_print(last_session.get()); // print startup stats

		if (start_event)
			SetEvent(start_event);

		MSG msg;

		if (forge) {
			DWORD reason = WAIT_TIMEOUT;
			HANDLE h = forge.get();
			while (WAIT_OBJECT_0 != reason)
			{
				switch (reason = MsgWaitForMultipleObjects(1, &h, false, INFINITE, QS_ALLINPUT)) {
				case WAIT_OBJECT_0:
					blog(LOG_INFO, "Forge exited, exiting");
					break;

				case WAIT_OBJECT_0 + 1:
					while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
					{
						TranslateMessage(&msg);
						DispatchMessage(&msg);
					}
					break;

				default:
					throw "Unexpected value from MsgWaitForMultipleObjects";
				}
			}

		} else {
			while (GetMessage(&msg, nullptr, 0, 0))
			{
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
		}

		crucibleContext.StopVideo();
	}
	catch (const char *err)
	{
		blog(LOG_ERROR, "Error: %s", err);
	}

}

static ProcessHandle HandleCLIArgs(HANDLE &start_event)
{
	auto argvFree = [](wchar_t *argv[]) { LocalFree(argv); };
	int argc = 0;
	auto argv = unique_ptr<wchar_t*[], decltype(argvFree)>{CommandLineToArgvW(GetCommandLineW(), &argc), argvFree};

	if (!argv || argc <= 1)
		throw make_pair("Started without arguments, exiting", -1);

	if (wstring(L"-standalone") == argv[1]) {
		blog(LOG_INFO, "Running standalone");
		return {};
	}

	if (argc <= 2)
		throw make_pair("Not enough arguments for non-standalone", -4);

	DWORD pid;
	wistringstream ss(argv[1]);
	if (!(ss >> pid))
		throw make_pair("Couldn't read PID from argv", -2);

	ss = wistringstream(argv[2]);
	if (!(ss >> start_event))
		throw make_pair("Couldn't read event id from argv", -3);

	return ProcessHandle{ OpenProcess(SYNCHRONIZE, false, pid) };
}

static DStr GetConfigDirectory(const char *subdir)
{
	wchar_t *fpath;

	SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &fpath);
	DStr path;
	dstr_from_wcs(path, fpath);

	CoTaskMemFree(fpath);

	dstr_catf(path, "/Forge/%s", subdir);

	return path;
}

LONG WINAPI SaveCrashDump(__in struct _EXCEPTION_POINTERS *ExceptionInfo);
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmd)
{
	base_set_log_handler(do_log, nullptr);

	unique_ptr<profiler_name_store_t> profiler_names{profiler_name_store_create()};
	profiler_start();

	ProcessHandle forge;
	HANDLE start_event = nullptr;
	try
	{
		forge = HandleCLIArgs(start_event);
	}
	catch (std::pair<const char*, int> &err)
	{
		blog(LOG_ERROR, "ERROR: %s (%#x)", err.first, GetLastError());
		return err.second;
	}

	if (forge)
		store_startup_log = true;

	SetUnhandledExceptionFilter(SaveCrashDump);

	try
	{
		if (!obs_startup("en-US", GetConfigDirectory("obs-module-config"), profiler_names.get()))
			throw "Couldn't init OBS";

		TestWindow window(hInstance);

#ifdef TEST_WINDOW
		TestWindow::RegisterWindowClass(hInstance);

		if (!window.Create(800, 480, "libobs test"))
			throw "Couldn't create test window";

		window.Show();
#endif

		TestVideoRecording(window, forge, start_event);
	}
	catch (const char *err)
	{
		blog(LOG_ERROR, "Error: %s", err);
	}

	obs_shutdown();

	{
		auto snap = ProfileSnapshotCreate();
		profiler_print(snap.get());
	}

	profiler_stop();
	profiler_names.reset();

	blog(LOG_INFO, "Number of memory leaks: %ld", bnum_allocs());

	UNUSED_PARAMETER(hPrevInstance);
	UNUSED_PARAMETER(lpCmdLine);
	UNUSED_PARAMETER(nCmd);

	return 0;
}

LONG WINAPI SaveCrashDump(__in struct _EXCEPTION_POINTERS *ExceptionInfo)
{
	auto t = time(nullptr);
	auto utc = *gmtime(&t);
	ostringstream dump_name;
	dump_name << "crucible-crash-" << put_time(&utc, "%Y%m%dT%H%M%SZ") << "-" << GetCurrentProcessId() << ".dmp";
	auto dump_path = GetConfigDirectory("logs");
	
	auto dump_path_w = dstr_to_wcs(dump_path);
	SHCreateDirectoryExW(nullptr, dump_path_w, nullptr);

	dstr_catf(dump_path, "/%s", dump_name.str().c_str());
	dump_path_w = dstr_to_wcs(dump_path);

	auto file = CreateFileW(dump_path_w, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	if (file && (file != INVALID_HANDLE_VALUE))
	{
		MINIDUMP_EXCEPTION_INFORMATION mdei;

		mdei.ThreadId = GetCurrentThreadId();
		mdei.ExceptionPointers = ExceptionInfo;
		mdei.ClientPointers = FALSE;

		MINIDUMP_TYPE mdt = (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData | MiniDumpWithHandleData);

		auto rv = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, mdt, (ExceptionInfo != nullptr) ? &mdei : nullptr, nullptr, nullptr);

		if (!rv)
			blog(LOG_WARNING, "MiniDumpWriteDump failed. Error: %#08x\n", GetLastError());
		else
			blog(LOG_INFO, "Minidump created.\n");

		CloseHandle(file);
	}
	else
		blog(LOG_INFO, "Unable to create Minidump: CreateFile failed. Error: %#08x\n", GetLastError());

	return EXCEPTION_EXECUTE_HANDLER;
}
